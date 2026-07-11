/* runtime.c: the M0 intermediary runtime. One translation unit on purpose;
 * the split into contract.c, iname.c, and friends happens when there is more
 * than one contract's worth of machinery to split. What exists today is the
 * whole walking skeleton path: load a module, register its descriptors, sign
 * a contract, dispatch a synchronous fulfillment through the uniform thunk
 * frame, latch the pledge outcome, and reclaim everything at break.
 *
 * Memory follows one rule. Every allocation a pledge makes goes through the
 * instance's block list, so ash_contract_break frees the lot in one walk and
 * valgrind stays clean whatever the pledge did. */

#include <ash/ash.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

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
    uint32_t               fulfilled; /* pledges latched Fulfilled, by count */
    uint32_t               broken;    /* pledges latched Broken, by count */
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

AshStatus ash_contract_sign(AshRuntime* rt, const char* contract_name,
                            const void* vows, size_t nvows,
                            uint64_t expected_hash, AshContract** out) {
    (void)vows;
    (void)nvows;
    if (!rt || !contract_name || !out) return ASH_ERR_TYPE;
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
    c->state = ASH_SIGNED;
    rt->instances[rt->ninstances++] = c;
    *out = c;
    return ASH_OK;
}

AshContractState ash_contract_state(const AshContract* c) {
    return c ? c->state : ASH_UNSIGNED;
}

AshStatus ash_contract_break(AshContract* c) {
    if (!c) return ASH_ERR_TYPE;
    if (c->state == ASH_UNSIGNED) return ASH_ERR_STATE;
    contract_free_owned(c);
    c->state = ASH_BROKEN;
    return ASH_OK;
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

AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
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
