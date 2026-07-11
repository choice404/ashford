/* runtime.c: the M4 intermediary runtime. One translation unit on purpose;
 * the split into contract.c, iname.c, and friends happens when there is more
 * than one contract's worth of machinery to split. What exists today is the
 * whole path a compiled module and a foreign host share: load a module,
 * register its descriptors, bind host implementations over abstract or
 * compiled pledges, sign a contract with vow overrides over the declared
 * defaults, dispatch fulfillments through the uniform thunk frame,
 * synchronously or through a future that carries the outcome, latch the
 * pledge outcome, and reclaim everything at break.
 *
 * Memory follows one rule. Every allocation a pledge makes goes through the
 * instance's block list, vow values and futures included, so
 * ash_contract_break frees the lot in one walk and valgrind stays clean
 * whatever the pledge did. The corollary a host lives by: wait a future
 * before breaking the instance that owns it.
 *
 * M4 sharpens the boundary itself. Every argument is deep copied onto the
 * instance at fulfillment entry, on the caller's thread, so a pledge body
 * only ever sees instance owned memory and host argument lifetimes end when
 * the fulfill call returns. By-reference parameters ride in as AshRefs:
 * copied in at entry the same way, mutated as instance owned slots, and
 * written back to host memory at delivery, inside the wait or before the
 * synchronous return, again on the caller's thread. Host memory is only
 * ever touched while the host is blocked inside an ash call, which is the
 * whole story when the threading milestone makes fulfillment concurrent. */

#include <ash/ash.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASH_MAX_CONTRACT_TYPES 64
#define ASH_MAX_MODULES        64
#define ASH_MAX_INSTANCES      256
#define ASH_MAX_BINDINGS       128

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
    AshPledgeFn*           fns;       /* dispatch table resolved at sign */
    uint64_t               shape_hash;
    int64_t                signed_at;
    uint32_t               fulfilled; /* pledges latched Fulfilled, by count */
    uint32_t               broken;    /* pledges latched Broken, by count */
};

/* A future is the receipt of one fulfillment. Today the work is already done
 * when the future exists; the struct remembers the outcome until the one
 * wait that delivers it, and it carries the refs whose slots that wait must
 * write back to host memory. It lives on the instance's block list. */
struct AshFuture {
    AshStatus status;
    uint32_t  waited;
    AshValue  value;
    AshRef*   refs;      /* instance owned copy of the caller's refs */
    AshValue* ref_slots; /* the mutable trailing slots of the frame */
    size_t    nrefs;
};

/* A host implementation bound over one pledge descriptor. The overlay lives
 * on the runtime because the descriptor tables are const data inside the
 * module image. */
typedef struct AshBinding {
    const AshPledgeDesc* pd;
    AshPledgeFn          fn;
} AshBinding;

struct AshRuntime {
    const AshContractDesc* descs[ASH_MAX_CONTRACT_TYPES];
    size_t                 ndescs;
    void*                  modules[ASH_MAX_MODULES];
    size_t                 nmodules;
    AshContract*           instances[ASH_MAX_INSTANCES];
    size_t                 ninstances;
    AshBinding             bindings[ASH_MAX_BINDINGS];
    size_t                 nbindings;
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
    c->fns = NULL;
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

/* The host binding over a pledge descriptor, or NULL when nothing bound. */
static AshPledgeFn find_binding(const AshRuntime* rt, const AshPledgeDesc* pd) {
    for (size_t i = 0; i < rt->nbindings; i++) {
        if (rt->bindings[i].pd == pd) return rt->bindings[i].fn;
    }
    return NULL;
}

/* Resolves the instance's dispatch table at sign, one fn per pledge, the
 * host binding beating the compiled body. A pledge with neither refuses the
 * whole sign. */
static AshStatus resolve_dispatch(const AshRuntime* rt, AshContract* c) {
    const AshContractDesc* desc = c->desc;
    if (desc->npledges == 0) return ASH_OK;
    c->fns = (AshPledgeFn*)ash_bytes(c, desc->npledges * sizeof(AshPledgeFn));
    if (!c->fns) return ASH_ERR_OOM;
    for (uint32_t i = 0; i < desc->npledges; i++) {
        AshPledgeFn fn = find_binding(rt, &desc->pledges[i]);
        if (!fn) fn = desc->pledges[i].fn;
        if (!fn) return ASH_ERR_UNBOUND;
        c->fns[i] = fn;
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
    AshContract* c = calloc(1, sizeof(AshContract));
    if (!c) return ASH_ERR_OOM;
    c->desc = desc;
    AshStatus st = resolve_dispatch(rt, c);
    if (st == ASH_OK) st = bind_vows(c, vows, nvows);
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

/* Resolves "Contract.pledge" or a mangled symbol to its descriptor entry. */
static const AshPledgeDesc* resolve_pledge_name(const AshRuntime* rt,
                                                const char* name) {
    const char* dot = strchr(name, '.');
    if (dot && dot != name && dot[1] != '\0') {
        size_t clen = (size_t)(dot - name);
        for (size_t i = 0; i < rt->ndescs; i++) {
            const AshContractDesc* d = rt->descs[i];
            if (strncmp(d->name, name, clen) != 0 || d->name[clen] != '\0')
                continue;
            return find_pledge(d, dot + 1);
        }
        return NULL;
    }
    for (size_t i = 0; i < rt->ndescs; i++) {
        const AshContractDesc* d = rt->descs[i];
        for (uint32_t j = 0; j < d->npledges; j++) {
            if (d->pledges[j].mangled &&
                strcmp(d->pledges[j].mangled, name) == 0)
                return &d->pledges[j];
        }
    }
    return NULL;
}

AshStatus ash_pledge_bind(AshRuntime* rt, const char* pledge_name,
                          AshPledgeFn fn) {
    if (!rt || !pledge_name || !fn) return ASH_ERR_TYPE;
    const AshPledgeDesc* pd = resolve_pledge_name(rt, pledge_name);
    if (!pd) return ASH_ERR_NAME;
    for (size_t i = 0; i < rt->nbindings; i++) {
        if (rt->bindings[i].pd == pd) {
            rt->bindings[i].fn = fn;
            return ASH_OK;
        }
    }
    if (rt->nbindings == ASH_MAX_BINDINGS) return ASH_ERR_OOM;
    rt->bindings[rt->nbindings].pd = pd;
    rt->bindings[rt->nbindings].fn = fn;
    rt->nbindings++;
    return ASH_OK;
}

/* Copies the host value a ref points at into an instance owned frame slot.
 * v1 passes scalars and strings by reference; the composite types wait for a
 * host repr worth standardizing. */
static AshStatus ref_copy_in(AshContract* c, const AshRef* r, AshValue* slot) {
    if (!r->host_ptr) return ASH_ERR_TYPE;
    memset(slot, 0, sizeof(*slot));
    slot->ty = r->ty;
    switch ((AshTypeTag)r->ty) {
    case ASH_TY_INT:   slot->as.i  = *(const int64_t*)r->host_ptr;  return ASH_OK;
    case ASH_TY_UINT:  slot->as.u  = *(const uint64_t*)r->host_ptr; return ASH_OK;
    case ASH_TY_FLOAT: slot->as.f  = *(const double*)r->host_ptr;   return ASH_OK;
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:  slot->as.b  = *(const uint8_t*)r->host_ptr;  return ASH_OK;
    case ASH_TY_CHAR:  slot->as.ch = *(const uint32_t*)r->host_ptr; return ASH_OK;
    case ASH_TY_STRING: {
        const AshString* hs = (const AshString*)r->host_ptr;
        *slot = ash_string_copy(c, hs->ptr, hs->len);
        if (hs->len && !slot->as.s.ptr) return ASH_ERR_OOM;
        return ASH_OK;
    }
    default:
        return ASH_ERR_TYPE;
    }
}

/* Writes one slot's final value back to host memory, the default protocol
 * when the ref carries no callback: scalars in place, strings as a whole
 * AshString struct whose bytes stay instance owned. Runs on the thread that
 * collects the outcome, while the host is blocked in the ash call. */
static AshStatus ref_write_back(const AshRef* r, const AshValue* slot) {
    if (slot->ty != r->ty) return ASH_ERR_TYPE;
    if (r->write_back) {
        r->write_back(r->host_ptr, slot, r->user);
        return ASH_OK;
    }
    switch ((AshTypeTag)r->ty) {
    case ASH_TY_INT:   *(int64_t*)r->host_ptr   = slot->as.i;  return ASH_OK;
    case ASH_TY_UINT:  *(uint64_t*)r->host_ptr  = slot->as.u;  return ASH_OK;
    case ASH_TY_FLOAT: *(double*)r->host_ptr    = slot->as.f;  return ASH_OK;
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:  *(uint8_t*)r->host_ptr   = slot->as.b;  return ASH_OK;
    case ASH_TY_CHAR:  *(uint32_t*)r->host_ptr  = slot->as.ch; return ASH_OK;
    case ASH_TY_STRING: *(AshString*)r->host_ptr = slot->as.s; return ASH_OK;
    default:
        return ASH_ERR_TYPE;
    }
}

/* Applies every write back of a delivered fulfillment. Slot types are
 * checked first so a pledge that broke the protocol writes nothing at all. */
static AshStatus write_back_refs(const AshRef* refs, const AshValue* slots,
                                 size_t nrefs) {
    for (size_t i = 0; i < nrefs; i++) {
        if (slots[i].ty != refs[i].ty) return ASH_ERR_TYPE;
    }
    for (size_t i = 0; i < nrefs; i++) {
        AshStatus st = ref_write_back(&refs[i], &slots[i]);
        if (st != ASH_OK) return st;
    }
    return ASH_OK;
}

/* Builds the frame a thunk sees: one instance owned slot per declared
 * parameter, the value arguments deep copied first, the refs copied in
 * behind them. Everything happens on the caller's thread, so no host memory
 * is ever read after the fulfill call returns. */
static AshStatus prepare_frame(AshContract* c, const AshValue* args,
                               size_t nargs, const AshRef* refs, size_t nrefs,
                               AshValue** frame_out) {
    size_t total = nargs + nrefs;
    *frame_out = NULL;
    if (total == 0) return ASH_OK;
    AshValue* frame = (AshValue*)ash_bytes(c, total * sizeof(AshValue));
    if (!frame) return ASH_ERR_OOM;
    for (size_t i = 0; i < nargs; i++) {
        AshStatus st = ash_value_deep_copy(c, &args[i], &frame[i]);
        if (st != ASH_OK) return st;
    }
    for (size_t i = 0; i < nrefs; i++) {
        AshStatus st = ref_copy_in(c, &refs[i], &frame[nargs + i]);
        if (st != ASH_OK) return st;
    }
    *frame_out = frame;
    return ASH_OK;
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
 * come through here, so the state rules and the latch cannot drift apart.
 * On ASH_OK *slots_out points at the frame's mutable ref slots, ready for
 * the write back the caller owes at delivery. */
static AshStatus run_pledge(AshContract* c, const char* pledge_name,
                            const AshValue* args, size_t nargs,
                            const AshRef* refs, size_t nrefs,
                            AshValue* out, AshValue** slots_out) {
    *slots_out = NULL;
    if (!c || !pledge_name || !out) return ASH_ERR_TYPE;
    if (nargs > 0 && !args) return ASH_ERR_TYPE;
    if (nrefs > 0 && !refs) return ASH_ERR_TYPE;
    if (c->state != ASH_SIGNED && c->state != ASH_PARTIAL &&
        c->state != ASH_FULFILLED)
        return ASH_ERR_STATE;
    const AshPledgeDesc* p = find_pledge(c->desc, pledge_name);
    if (!p) return ASH_ERR_NAME;
    if (nargs + nrefs != p->nargs) return ASH_ERR_TYPE;
    AshValue* frame = NULL;
    AshStatus st = prepare_frame(c, args, nargs, refs, nrefs, &frame);
    if (st != ASH_OK) return st;
    memset(out, 0, sizeof(*out));
    AshPledgeFn fn = c->fns[p - c->desc->pledges];
    st = fn(c, frame, p->nargs, out);
    if (st != ASH_OK) return st;
    latch(c, out);
    *slots_out = frame ? frame + nargs : NULL;
    return ASH_OK;
}

AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs,
                              const AshRef* refs, size_t nrefs) {
    if (!c || !pledge_name) return NULL;
    AshFuture* f = (AshFuture*)ash_bytes(c, sizeof(AshFuture));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    if (nrefs > 0 && refs) {
        f->refs = (AshRef*)ash_bytes(c, nrefs * sizeof(AshRef));
        if (!f->refs) {
            f->status = ASH_ERR_OOM;
            return f;
        }
        memcpy(f->refs, refs, nrefs * sizeof(AshRef));
        f->nrefs = nrefs;
    }
    f->status = run_pledge(c, pledge_name, args, nargs, refs, nrefs,
                           &f->value, &f->ref_slots);
    return f;
}

AshStatus ash_future_wait(AshFuture* f, AshValue* out) {
    if (!f || !out) return ASH_ERR_TYPE;
    if (f->waited) return ASH_ERR_STATE;
    f->waited = 1;
    if (f->status == ASH_OK && f->nrefs > 0 && f->ref_slots) {
        AshStatus wb = write_back_refs(f->refs, f->ref_slots, f->nrefs);
        if (wb != ASH_OK) return wb;
    }
    *out = f->value;
    return f->status;
}

AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
                                  const AshValue* args, size_t nargs,
                                  const AshRef* refs, size_t nrefs,
                                  AshValue* out) {
    AshValue* slots = NULL;
    AshStatus st = run_pledge(c, pledge_name, args, nargs, refs, nrefs,
                              out, &slots);
    if (st != ASH_OK) return st;
    if (nrefs > 0 && slots) return write_back_refs(refs, slots, nrefs);
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

/* ---- deep values ---- */

AshStatus ash_list_new(AshContract* c, uint32_t elem_ty, uint64_t cap,
                       AshValue* out) {
    if (!c || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_LIST;
    out->as.list.elem_ty = elem_ty;
    if (cap == 0) return ASH_OK;
    AshValue* data = (AshValue*)ash_bytes(c, cap * sizeof(AshValue));
    if (!data) return ASH_ERR_OOM;
    memset(data, 0, cap * sizeof(AshValue));
    out->as.list.data = data;
    out->as.list.cap = cap;
    return ASH_OK;
}

AshStatus ash_list_push(AshContract* c, AshValue* list, const AshValue* elem) {
    if (!c || !list || !elem) return ASH_ERR_TYPE;
    if (list->ty != ASH_TY_LIST) return ASH_ERR_TYPE;
    if (elem->ty != list->as.list.elem_ty) return ASH_ERR_TYPE;
    if (list->as.list.len == list->as.list.cap) {
        uint64_t cap = list->as.list.cap ? list->as.list.cap * 2 : 4;
        AshValue* data = (AshValue*)ash_bytes(c, cap * sizeof(AshValue));
        if (!data) return ASH_ERR_OOM;
        if (list->as.list.len) {
            memcpy(data, list->as.list.data,
                   list->as.list.len * sizeof(AshValue));
        }
        list->as.list.data = data;
        list->as.list.cap = cap;
    }
    ((AshValue*)list->as.list.data)[list->as.list.len++] = *elem;
    return ASH_OK;
}

const AshValue* ash_list_get(const AshValue* v, uint64_t idx) {
    if (!v) return NULL;
    if (v->ty != ASH_TY_LIST && v->ty != ASH_TY_TUPLE) return NULL;
    if (idx >= v->as.list.len) return NULL;
    return (const AshValue*)v->as.list.data + idx;
}

AshStatus ash_tuple_new(AshContract* c, uint64_t count, AshValue* out) {
    if (!c || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_TUPLE;
    if (count == 0) return ASH_OK;
    AshValue* data = (AshValue*)ash_bytes(c, count * sizeof(AshValue));
    if (!data) return ASH_ERR_OOM;
    memset(data, 0, count * sizeof(AshValue));
    out->as.list.data = data;
    out->as.list.len = count;
    out->as.list.cap = count;
    return ASH_OK;
}

/* The recursive workhorse behind copy-in. Scalars are the struct copy, a
 * string copies its bytes, list and tuple copy element by element, Option
 * and Result rebox their payload. Map and record wait for a settled repr;
 * copying them today would freeze one by accident. */
AshStatus ash_value_deep_copy(AshContract* c, const AshValue* src,
                              AshValue* dst) {
    if (!c || !src || !dst) return ASH_ERR_TYPE;
    switch ((AshTypeTag)src->ty) {
    case ASH_TY_UNIT:
    case ASH_TY_INT:
    case ASH_TY_UINT:
    case ASH_TY_FLOAT:
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:
    case ASH_TY_CHAR:
    case ASH_TY_PLEDGE_REF:
        *dst = *src;
        return ASH_OK;
    case ASH_TY_STRING:
        *dst = ash_string_copy(c, src->as.s.ptr, src->as.s.len);
        if (src->as.s.len && !dst->as.s.ptr) return ASH_ERR_OOM;
        return ASH_OK;
    case ASH_TY_LIST:
    case ASH_TY_TUPLE: {
        uint64_t n = src->as.list.len;
        memset(dst, 0, sizeof(*dst));
        dst->ty = src->ty;
        dst->as.list.elem_ty = src->as.list.elem_ty;
        dst->as.list.len = n;
        dst->as.list.cap = n;
        if (n == 0) return ASH_OK;
        AshValue* data = (AshValue*)ash_bytes(c, n * sizeof(AshValue));
        if (!data) return ASH_ERR_OOM;
        const AshValue* from = (const AshValue*)src->as.list.data;
        for (uint64_t i = 0; i < n; i++) {
            AshStatus st = ash_value_deep_copy(c, &from[i], &data[i]);
            if (st != ASH_OK) return st;
        }
        dst->as.list.data = data;
        return ASH_OK;
    }
    case ASH_TY_OPTION:
    case ASH_TY_RESULT: {
        memset(dst, 0, sizeof(*dst));
        dst->ty = src->ty;
        dst->tag = src->tag;
        if (!src->as.box) return ASH_OK;
        AshValue* boxed = ash_box(c);
        if (!boxed) return ASH_ERR_OOM;
        AshStatus st = ash_value_deep_copy(c, (const AshValue*)src->as.box,
                                           boxed);
        if (st != ASH_OK) return st;
        dst->as.box = boxed;
        return ASH_OK;
    }
    case ASH_TY_MAP:
    case ASH_TY_RECORD:
    default:
        return ASH_ERR_TYPE;
    }
}
