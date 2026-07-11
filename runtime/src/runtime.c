/* runtime.c: the M3 intermediary runtime. One translation unit on purpose;
 * the split into contract.c, iname.c, and friends happens when there is more
 * than one contract's worth of machinery to split. What exists today is the
 * whole path a compiled module and a foreign host share: load a module,
 * register its descriptors, sign a contract with vow overrides over the
 * declared defaults, dispatch fulfillments through the uniform thunk frame,
 * synchronously or through a future that carries the outcome, latch the
 * pledge outcome, and reclaim everything at break.
 *
 * Memory follows one rule. Every allocation a pledge makes goes through the
 * instance's block list, vow values and futures included, so
 * ash_contract_break frees the lot in one walk and valgrind stays clean
 * whatever the pledge did. The corollary a host lives by: wait a future
 * before breaking the instance that owns it. */

#include <ash/ash.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASH_MAX_CONTRACT_TYPES 64
#define ASH_MAX_MODULES        64
#define ASH_MAX_INSTANCES      256

/* A block the instance owns. Blocks form a singly linked list headed in the
 * instance; the header rides in front of the caller's bytes. */
typedef struct AshBlock {
    struct AshBlock* next;
} AshBlock;

struct AshContract {
    const AshContractDesc* desc;
    AshContractState       state;
    AshBlock*              owned;
    AshValue*              vow_vals;  /* one per desc vow, instance owned */
    uint64_t               shape_hash;
    int64_t                signed_at;
    uint32_t               fulfilled; /* pledges latched Fulfilled, by count */
    uint32_t               broken;    /* pledges latched Broken, by count */
};

/* A future is the receipt of one fulfillment. Today the work is already done
 * when the future exists; the struct remembers the outcome until the one
 * wait that delivers it. It lives on the instance's block list. */
struct AshFuture {
    AshStatus status;
    uint32_t  waited;
    AshValue  value;
};

struct AshRuntime {
    const AshContractDesc* descs[ASH_MAX_CONTRACT_TYPES];
    size_t                 ndescs;
    void*                  modules[ASH_MAX_MODULES];
    size_t                 nmodules;
    AshContract*           instances[ASH_MAX_INSTANCES];
    size_t                 ninstances;
};

typedef AshStatus (*AshRegisterFn)(AshRuntime*);

/* ---- runtime lifecycle ---- */

AshStatus ash_runtime_init(const void* cfg, AshRuntime** out) {
    (void)cfg;
    if (!out) return ASH_ERR_TYPE;
    AshRuntime* rt = calloc(1, sizeof(AshRuntime));
    if (!rt) return ASH_ERR_OOM;
    *out = rt;
    return ASH_OK;
}

static void contract_free_owned(AshContract* c) {
    AshBlock* b = c->owned;
    while (b) {
        AshBlock* next = b->next;
        free(b);
        b = next;
    }
    c->owned = NULL;
    c->vow_vals = NULL;
}

void ash_runtime_shutdown(AshRuntime* rt) {
    if (!rt) return;
    for (size_t i = 0; i < rt->ninstances; i++) {
        contract_free_owned(rt->instances[i]);
        free(rt->instances[i]);
    }
    for (size_t i = 0; i < rt->nmodules; i++) {
        dlclose(rt->modules[i]);
    }
    free(rt);
}

AshStatus ash_module_load(AshRuntime* rt, const char* so_path) {
    if (!rt || !so_path) return ASH_ERR_TYPE;
    if (rt->nmodules == ASH_MAX_MODULES) return ASH_ERR_OOM;
    void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return ASH_ERR_LOAD;
    AshRegisterFn reg = (AshRegisterFn)dlsym(handle, "ash_module_register");
    if (!reg) {
        dlclose(handle);
        return ASH_ERR_LOAD;
    }
    AshStatus st = reg(rt);
    if (st != ASH_OK) {
        dlclose(handle);
        return st;
    }
    rt->modules[rt->nmodules++] = handle;
    return ASH_OK;
}

AshStatus ash_register_contract(AshRuntime* rt, const AshContractDesc* desc) {
    if (!rt || !desc || !desc->name) return ASH_ERR_TYPE;
    if (rt->ndescs == ASH_MAX_CONTRACT_TYPES) return ASH_ERR_OOM;
    for (size_t i = 0; i < rt->ndescs; i++) {
        if (strcmp(rt->descs[i]->name, desc->name) == 0) return ASH_ERR_NAME;
    }
    rt->descs[rt->ndescs++] = desc;
    return ASH_OK;
}

/* ---- contracts ---- */

static const AshContractDesc* find_desc(const AshRuntime* rt, const char* name) {
    for (size_t i = 0; i < rt->ndescs; i++) {
        if (strcmp(rt->descs[i]->name, name) == 0) return rt->descs[i];
    }
    return NULL;
}

static const AshVowDesc* find_vow_desc(const AshContractDesc* desc,
                                       const char* name) {
    for (uint32_t i = 0; i < desc->nvows; i++) {
        if (strcmp(desc->vows[i].name, name) == 0) return &desc->vows[i];
    }
    return NULL;
}

/* Copies one vow value onto the instance. Strings deep copy so the instance
 * never aliases host memory or another instance's heap; everything else is a
 * plain value. */
static AshStatus copy_vow_value(AshContract* c, const AshValue* src,
                                AshValue* dst) {
    if (src->ty == ASH_TY_STRING) {
        *dst = ash_string_copy(c, src->as.s.ptr, src->as.s.len);
        if (src->as.s.len && !dst->as.s.ptr) return ASH_ERR_OOM;
        return ASH_OK;
    }
    *dst = *src;
    return ASH_OK;
}

/* Fills the instance's vow storage: defaults first, then the overrides, each
 * override checked by name and by type, and every vow accounted for. */
static AshStatus bind_vows(AshContract* c, const AshVowBinding* vows,
                           size_t nvows) {
    const AshContractDesc* desc = c->desc;
    if (desc->nvows == 0) {
        return (nvows == 0) ? ASH_OK : ASH_ERR_NAME;
    }
    c->vow_vals = (AshValue*)ash_bytes(c, desc->nvows * sizeof(AshValue));
    if (!c->vow_vals) return ASH_ERR_OOM;
    memset(c->vow_vals, 0, desc->nvows * sizeof(AshValue));

    uint8_t bound[ASH_MAX_CONTRACT_TYPES] = {0};
    if (desc->nvows > ASH_MAX_CONTRACT_TYPES) return ASH_ERR_OOM;

    for (size_t i = 0; i < nvows; i++) {
        if (!vows[i].name) return ASH_ERR_NAME;
        const AshVowDesc* vd = find_vow_desc(desc, vows[i].name);
        if (!vd) return ASH_ERR_NAME;
        if (vows[i].value.ty != vd->ty) return ASH_ERR_TYPE;
        size_t slot = (size_t)(vd - desc->vows);
        AshStatus st = copy_vow_value(c, &vows[i].value, &c->vow_vals[slot]);
        if (st != ASH_OK) return st;
        bound[slot] = 1;
    }
    for (uint32_t j = 0; j < desc->nvows; j++) {
        if (bound[j]) continue;
        if (!desc->vows[j].has_default) return ASH_ERR_UNBOUND;
        AshStatus st = copy_vow_value(c, &desc->vows[j].default_value,
                                      &c->vow_vals[j]);
        if (st != ASH_OK) return st;
    }
    return ASH_OK;
}

AshStatus ash_contract_sign(AshRuntime* rt, const char* contract_name,
                            const AshVowBinding* vows, size_t nvows,
                            uint64_t expected_hash, AshContract** out) {
    if (!rt || !contract_name || !out) return ASH_ERR_TYPE;
    if (nvows > 0 && !vows) return ASH_ERR_TYPE;
    if (rt->ninstances == ASH_MAX_INSTANCES) return ASH_ERR_OOM;
    const AshContractDesc* desc = find_desc(rt, contract_name);
    if (!desc) return ASH_ERR_NAME;
    if (expected_hash != 0 && expected_hash != desc->shape_hash)
        return ASH_ERR_VERSION;
    for (uint32_t i = 0; i < desc->npledges; i++) {
        if (desc->pledges[i].fn == NULL) return ASH_ERR_UNBOUND;
    }
    AshContract* c = calloc(1, sizeof(AshContract));
    if (!c) return ASH_ERR_OOM;
    c->desc = desc;
    AshStatus st = bind_vows(c, vows, nvows);
    if (st != ASH_OK) {
        contract_free_owned(c);
        free(c);
        return st;
    }
    c->state = ASH_SIGNED;
    c->shape_hash = desc->shape_hash;
    c->signed_at = (int64_t)time(NULL);
    rt->instances[rt->ninstances++] = c;
    *out = c;
    return ASH_OK;
}

AshContractState ash_contract_state(const AshContract* c) {
    return c ? c->state : ASH_UNSIGNED;
}

uint64_t ash_contract_hash(const AshContract* c) {
    return c ? c->shape_hash : 0;
}

int64_t ash_contract_signed_at(const AshContract* c) {
    return c ? c->signed_at : 0;
}

AshStatus ash_contract_break(AshContract* c) {
    if (!c) return ASH_ERR_TYPE;
    if (c->state == ASH_UNSIGNED) return ASH_ERR_STATE;
    contract_free_owned(c);
    c->state = ASH_BROKEN;
    return ASH_OK;
}

/* ---- vows ---- */

const AshValue* ash_vow_ref(AshContract* c, const char* name) {
    if (!c || !name || !c->vow_vals) return NULL;
    const AshVowDesc* vd = find_vow_desc(c->desc, name);
    if (!vd) return NULL;
    return &c->vow_vals[vd - c->desc->vows];
}

/* ---- pledges ---- */

static const AshPledgeDesc* find_pledge(const AshContractDesc* desc,
                                        const char* name) {
    for (uint32_t i = 0; i < desc->npledges; i++) {
        if (strcmp(desc->pledges[i].name, name) == 0) return &desc->pledges[i];
    }
    return NULL;
}

/* The M0 state rule, one contract wide until subcontracts land: a pledge
 * outcome latches, an Ok result counts toward Fulfilled and an Err toward
 * Broken. The contract reads Fulfilled once every pledge has latched Ok and
 * Broken once every pledge has latched Err, the spec's default policy over a
 * subcontract free contract. An automatic Broken keeps the owned heap alive,
 * since the Err payload just handed to the host lives there; only an explicit
 * break() reclaims. The full requirements evaluator replaces this in M7. */
static void latch(AshContract* c, const AshValue* out) {
    if (out->ty == ASH_TY_RESULT && out->tag == 1) {
        c->broken += 1;
        if (c->broken >= c->desc->npledges) c->state = ASH_BROKEN;
    } else {
        c->fulfilled += 1;
        if (c->fulfilled >= c->desc->npledges) c->state = ASH_FULFILLED;
    }
}

/* The one fulfillment path. Both the synchronous form and the future form
 * come through here, so the state rules and the latch cannot drift apart. */
static AshStatus run_pledge(AshContract* c, const char* pledge_name,
                            const AshValue* args, size_t nargs,
                            AshValue* out) {
    if (!c || !pledge_name || !out) return ASH_ERR_TYPE;
    if (c->state != ASH_SIGNED && c->state != ASH_PARTIAL &&
        c->state != ASH_FULFILLED)
        return ASH_ERR_STATE;
    const AshPledgeDesc* p = find_pledge(c->desc, pledge_name);
    if (!p) return ASH_ERR_NAME;
    if (nargs != p->nargs) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    AshStatus st = p->fn(c, args, nargs, out);
    if (st != ASH_OK) return st;
    latch(c, out);
    return ASH_OK;
}

AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs) {
    if (!c || !pledge_name) return NULL;
    AshFuture* f = (AshFuture*)ash_bytes(c, sizeof(AshFuture));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->status = run_pledge(c, pledge_name, args, nargs, &f->value);
    return f;
}

AshStatus ash_future_wait(AshFuture* f, AshValue* out) {
    if (!f || !out) return ASH_ERR_TYPE;
    if (f->waited) return ASH_ERR_STATE;
    f->waited = 1;
    *out = f->value;
    return f->status;
}

AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
                                  const AshValue* args, size_t nargs,
                                  AshValue* out) {
    return run_pledge(c, pledge_name, args, nargs, out);
}

/* ---- allocation helpers ---- */

uint8_t* ash_bytes(AshContract* c, uint64_t n) {
    if (!c) return NULL;
    AshBlock* b = malloc(sizeof(AshBlock) + n);
    if (!b) return NULL;
    b->next = c->owned;
    c->owned = b;
    return (uint8_t*)(b + 1);
}

AshValue* ash_box(AshContract* c) {
    AshValue* v = (AshValue*)ash_bytes(c, sizeof(AshValue));
    if (v) memset(v, 0, sizeof(*v));
    return v;
}

AshValue ash_string_copy(AshContract* c, const uint8_t* utf8, uint64_t len) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    uint8_t* dst = ash_bytes(c, len);
    if (dst && len) memcpy(dst, utf8, len);
    v.as.s.ptr = dst;
    v.as.s.len = dst ? len : 0;
    return v;
}

AshStatus ash_string_concat(AshContract* c, const AshValue* a,
                            const AshValue* b, AshValue* out) {
    if (!c || !a || !b || !out) return ASH_ERR_TYPE;
    if (a->ty != ASH_TY_STRING || b->ty != ASH_TY_STRING) return ASH_ERR_TYPE;
    uint64_t n = a->as.s.len + b->as.s.len;
    uint8_t* buf = ash_bytes(c, n);
    if (!buf) return ASH_ERR_OOM;
    if (a->as.s.len) memcpy(buf, a->as.s.ptr, a->as.s.len);
    if (b->as.s.len) memcpy(buf + a->as.s.len, b->as.s.ptr, b->as.s.len);
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_STRING;
    out->as.s.ptr = buf;
    out->as.s.len = n;
    return ASH_OK;
}

int ash_string_eq(const AshValue* a, const AshValue* b) {
    if (!a || !b) return 0;
    if (a->ty != ASH_TY_STRING || b->ty != ASH_TY_STRING) return 0;
    if (a->as.s.len != b->as.s.len) return 0;
    if (a->as.s.len == 0) return 1;
    return memcmp(a->as.s.ptr, b->as.s.ptr, a->as.s.len) == 0;
}
