# The Ashford ABI

This is the normative ABI for Ashford. It covers everything that crosses the boundary between a compiled module, the intermediary runtime, and a foreign host: the value representation, the thunk frame every pledge is called through, the descriptor tables a module publishes, the registrar protocol, the symbol mangling, the shape hash, and who owns what. The single header `runtime/include/ash/ash_abi.h` is the source of truth for every layout named here; a change to the wire lands in that header first and this document moves with it.

## Values

Everything crosses the boundary by value in an `AshValue`, one struct with a type tag, a variant tag, and a union.

```c
typedef struct AshValue {
    uint32_t ty;   /* AshTypeTag, picks the union arm */
    uint32_t tag;  /* variant for the sum shaped types, 0 otherwise */
    union {
        int64_t   i;    /* Int */
        uint64_t  u;    /* UInt */
        double    f;    /* Float */
        uint8_t   b;    /* Bool and Byte */
        uint32_t  ch;   /* Char, a Unicode scalar value */
        AshString s;    /* String */
        AshList   list; /* List */
        void*     box;  /* Option and Result payloads */
    } as;
} AshValue;
```

The numeric widths are fixed forever: `Int` is 64 bit signed, `UInt` 64 bit unsigned, `Float` is IEEE double, `Bool` and `Byte` one byte, `Char` a 32 bit Unicode scalar value. There is no implicit conversion at the boundary because there is none in the language.

The type tags are numbers on the wire and the numbers are fixed, declaration order in the enum:

```c
typedef enum AshTypeTag {
    ASH_TY_UNIT = 0, ASH_TY_INT, ASH_TY_UINT, ASH_TY_FLOAT, ASH_TY_BOOL,
    ASH_TY_BYTE, ASH_TY_CHAR, ASH_TY_STRING, ASH_TY_LIST, ASH_TY_MAP,
    ASH_TY_TUPLE, ASH_TY_OPTION, ASH_TY_RESULT, ASH_TY_RECORD,
    ASH_TY_PLEDGE_REF, ASH_TY_SUM
} AshTypeTag;
```

Strings are a fat value: a pointer and a byte length, UTF-8, no terminator. The bytes live wherever the value's owner put them; a compiled module points string constants into its own mapped image, and everything built at run time lives on a contract instance.

```c
typedef struct AshString {
    uint8_t* ptr;
    uint64_t len;
} AshString;

typedef struct AshList {
    void*    data;   /* a contiguous AshValue array */
    uint64_t len;
    uint64_t cap;
    uint32_t elem_ty;
} AshList;
```

Every layout in this document uses the platform's natural C alignment and nothing is packed. On LP64 an `AshString` is 16 bytes, an `AshList` 32 with four bytes of tail padding after `elem_ty`, and an `AshValue` 40: two 32 bit tags and a union whose widest arm is the list. A foreign host that reconstructs these structs by hand, an FFI without the header, builds exactly these fields in exactly this order.

`tag` carries the variant for the sum shaped types and is 0 for everything else:

| type | tag 0 | tag 1 |
|---|---|---|
| Option | None | Some |
| Result | Ok | Err |

A declared sum is `ASH_TY_SUM` and its `tag` is the variant's declaration index, 0 for the first variant the source names, counting up in source order.

An Option or Result payload rides in `as.box` as a pointer to a single `AshValue` the instance owns. `None` carries a null box.

A list carries its elements as a contiguous `AshValue` array behind `as.list.data`, `len` of them live, `cap` allocated, `elem_ty` the declared element tag. A tuple rides the same union arm: the data pointer is the `AshValue` array, `len` is the arity, `cap` equals `len`, and `elem_ty` is 0 because a tuple's elements need not agree.

A record rides the list arm too: `ASH_TY_RECORD`, the data pointer an `AshValue` array holding the fields in declaration order, `len` the field count, `cap` equal to `len`, `elem_ty` 0. A sum variant's payload is the same shape, `ASH_TY_SUM` with the variant's fields in declaration order behind the data pointer; a variant with no payload carries an empty arm, data NULL and `len` 0. Field access in compiled code is a slot read by declaration index, so the order is normative.

**The map representation.** A map, `ASH_TY_MAP`, rides the list arm as well, and this layout is canonical: the data pointer holds interleaved key, value pairs in insertion order, the key of pair i at slot 2i and its value at slot 2i+1, `len` counts slots so it is always twice the pair count, `cap` is allocated slots, and `elem_ty` is the key's tag alone, one of the keyable scalars `Int`, `UInt`, `Bool`, `Byte`, `Char`, `String`. The value type is the checker's knowledge; the runtime never records or checks it. Insertion order is normative: it is the order any serialization sees, the order equality compares, and updating an existing key replaces its value without moving its pair. Lookup and insert scan the keys linearly through `ash_value_eq`, O(n) in the pair count, the v1 tradeoff that keeps the representation one union arm deep.

**The internal instance tag.** `ASH_TY_INSTANCE`, the tag after `ASH_TY_SUM`, is internal to compiled modules and never crosses the ABI. It is the signed instance handle a cross-contract `Contract.sign(...)` returns inside a pledge body: `as.box` holds the `AshContract*`. The language already rejects a contract type at every boundary position, pledge parameters and returns, vows, record and variant fields, and provisional clause signatures, so no thunk frame, vow storage, or descriptor value ever carries this tag, and a foreign host never meets it. Its helper arms are the reference-handle semantics the language pins: `ash_value_deep_copy` copies the handle, not the contract, the one deliberate exception to value semantics, and `ash_value_eq` is handle identity. The debug renderer spells it `<instance>`.

The runtime exposes deep value helpers to hosts and thunks alike: `ash_list_new`, `ash_list_push`, `ash_list_get`, `ash_list_set`, `ash_map_new`, `ash_map_set`, `ash_map_get`, `ash_tuple_new`, `ash_value_eq`, and `ash_value_deep_copy`, which recursively copies any supported value into instance owned memory, string bytes, list, map, tuple, record, and sum elements, and boxed payloads included. After a deep copy, nothing in the destination aliases memory the instance does not own. `ash_value_eq` is the structural equality the language's `==` lowers onto for the deep shapes: type tags first, the sum shaped `tag` next, then elements recursively. Two maps compare pair by pair in insertion order, keys and values both, so the same pairs written in another order read unequal, which keeps the answer deterministic and matches what serialization sees.

The value and allocation helpers cross the boundary with these shapes, every allocating one hanging its memory off the instance it was handed:

```c
uint8_t*  ash_bytes(AshContract* c, uint64_t n);
AshValue* ash_box(AshContract* c);
AshValue  ash_string_copy(AshContract* c, const uint8_t* utf8, uint64_t len);
AshStatus ash_string_concat(AshContract* c, const AshValue* a,
                            const AshValue* b, AshValue* out);
AshStatus ash_list_new(AshContract* c, uint32_t elem_ty, uint64_t cap,
                       AshValue* out);
AshStatus ash_list_push(AshContract* c, AshValue* list, const AshValue* elem);
const AshValue* ash_list_get(const AshValue* v, uint64_t idx);
AshStatus ash_list_set(AshValue* list, uint64_t idx, const AshValue* elem);
AshValue  ash_map_new(AshContract* c, uint32_t key_ty);
AshStatus ash_map_set(AshContract* c, AshValue* m, const AshValue* k,
                      const AshValue* v);
int       ash_map_get(const AshValue* m, const AshValue* k,
                      const AshValue** out);
AshStatus ash_tuple_new(AshContract* c, uint64_t count, AshValue* out);
int       ash_value_eq(const AshValue* a, const AshValue* b);
AshStatus ash_value_deep_copy(AshContract* c, const AshValue* src,
                              AshValue* dst);
```

**Indexing out of bounds.** Compiled code bounds checks every list element read and write, reads through `ash_list_get` and writes through `ash_list_set`. An index outside the list, a negative one included through the unsigned cast, makes the pledge return `ASH_ERR_TYPE` from its thunk: the status reports the fulfillment never produced a value, no latch moves, and host memory is never touched. This rule is normative for every compiled module.

**Indexing a map.** A map read is never a fault: compiled code lowers `m[k]` onto `ash_map_get` and wraps the answer in the `Option` the language promises, `Some` around an instance owned deep copy of the value on a hit, `None` on a miss. `m[k] = v` lowers onto `ash_map_set`, which deep copies the key and the value onto the instance before committing, replaces in place when the key exists, and appends in insertion order when it does not. `ash_map_new` builds the empty map; it allocates nothing, so it returns the value directly. `ash_map_get` hands back a borrow into the map's own pair storage, which a later `ash_map_set` on the same map may move, so a host copies it out before the next write.

**The debug renderer.** The runtime renders any supported value into its canonical debug spelling, the text a standalone executable prints for a `Main.run` error and a convenience for any host that wants a value it can log:

```c
AshStatus ash_value_render(const AshValue* v, char* buf, size_t cap,
                           size_t* need);
```

The spelling: `Int`, `UInt`, and `Float` as C would print them, `Bool` as `true` or `false`, `Byte` as its decimal value, `Char` as `U+0041`, `Unit` as `()`, strings quoted with `"` and `\` escaped and control bytes as `\xNN`, lists as `[a, b]`, tuples as `(a, b)`, records as `{a, b}` with the fields in declaration order, a sum as `#tag` with its payload in parens when it carries one, and `None`, `Some(v)`, `Ok(v)`, `Err(v)` through their boxes. A map renders as `{k: v, ...}` with its pairs in insertion order, the only order a map has, and an empty map as `{}`. Nesting deeper than eight levels renders as `...`, so a cyclic or absurd value cannot run the renderer away. The size protocol is `ash_iname_dump`'s: `*need` receives the full size including the terminating NUL, a `cap` at least that writes the text and returns `ASH_OK`, anything smaller writes nothing and returns `ASH_ERR_OOM`, so a NULL `buf` with `cap` 0 sizes the buffer.

## The thunk frame

Every pledge crosses the boundary in one shape, compiled bodies and host bound implementations alike:

```c
typedef AshStatus (*AshPledgeFn)(void* ctx, const AshValue* args, size_t nargs,
                                 AshValue* out);
```

`ctx` is the signed contract instance the fulfillment runs against. The runtime zeroes `out` before the call. The thunk validates `nargs` and each argument's type tag before touching a union arm, reports `ASH_ERR_TYPE` on any mismatch, and writes the pledge's declared value through `out` on `ASH_OK`. Everything a thunk allocates goes through `ash_bytes`, `ash_box`, or a helper built on them, so it is owned by the instance in `ctx` and dies with it.

`args` is the full frame, one slot per declared parameter, and every slot is an instance owned deep copy the runtime built at fulfillment entry. A pledge body never sees host memory. When the caller passed trailing parameters by reference, those slots are mutable on purpose: an implementation that updates a by-reference parameter casts away the const on its own slot and writes the new value there, and the runtime carries the final slot values back to the host at delivery.

A pledge that succeeds with an `Err` result still returns `ASH_OK`; the status word reports whether the thunk ran, the value reports what the pledge decided. The runtime latches the outcome either way.

The frame is validated on the way in, not on the way out. The runtime checks the count and every argument tag before the body runs, and it trusts what the body wrote through `out`: v1 does not police a host bound implementation that writes a value of a different shape than the pledge declared. Compiled bodies cannot do this, the compiler already checked them; a host that does has broken its side of the contract, and what a compiled caller makes of the value is its own misfortune.

## Runtime lifecycle

```c
typedef struct AshRuntimeConfig {
    uint32_t max_threads;   /* 0 selects the default pool of 4 workers */
} AshRuntimeConfig;

AshStatus ash_runtime_init(const AshRuntimeConfig* cfg, AshRuntime** out);
void      ash_runtime_shutdown(AshRuntime* rt);
AshStatus ash_module_load(AshRuntime* rt, const char* so_path);
```

A NULL `cfg` means all defaults. `ash_module_load` hands `so_path` to dlopen as it stands, so a relative path resolves by the loader's rules from the process working directory; shutdown is described under Threading and frees everything the runtime tracked.

**Loading the runtime dynamically.** A compiled module carries undefined references to the runtime's exported symbols, `ash_register_contract` and the allocation helpers its thunks call, and no library dependency of its own; the dynamic linker resolves them against the process global scope when the runtime dlopens the module. A C host that links libashrt into its executable gets this for free, because an executable's startup dependencies join the global scope. A host that opens libashrt dynamically itself, dlopen from C or ctypes from Python, must open it with `RTLD_GLOBAL`, or every module load fails to resolve and reports `ASH_ERR_LOAD`.

## Descriptor tables

A compiled module publishes its contracts as static const data. The runtime keeps the pointers rather than copying, so the tables live exactly as long as the module, which stays mapped until runtime shutdown.

```c
typedef struct AshPledgeDesc {
    const char* name;      /* the pledge's declared name */
    const char* mangled;   /* the mangled symbol, see below */
    uint32_t    nargs;
    AshPledgeFn fn;        /* NULL marks an abstract pledge awaiting a bind */
    int32_t     sub;       /* subs table index, or -1 for a loose pledge */
} AshPledgeDesc;

typedef struct AshVowDesc {
    const char* name;
    uint32_t    ty;           /* AshTypeTag of the vow's value */
    uint32_t    has_default;
    AshValue    default_value;
} AshVowDesc;

typedef struct AshContractDesc {
    const char*          name;
    uint64_t             shape_hash;
    uint32_t             version;
    uint32_t             npledges;
    const AshPledgeDesc* pledges;
    uint32_t             nvows;
    const AshVowDesc*    vows;
    uint32_t             nsubs;
    const char* const*   subs;         /* NULL entry = anonymous subcontract */
    uint32_t             natoms;
    const AshReqAtom*    atoms;
    uint32_t             has_reqs;     /* 1 when the source wrote the block */
    uint32_t             nfulfill;
    uint32_t             npartial;
    uint32_t             nbreak;
    const AshReqOp*      req_fulfill;
    const AshReqOp*      req_partial;
    const AshReqOp*      req_break;
} AshContractDesc;
```

Pledges and vows appear in declaration order. A vow whose declaration carried an initializer has `has_default` set and the literal encoded in `default_value`; a string default points at bytes inside the module. Signing copies every vow value onto the instance, so nothing an instance reads aliases the descriptor after sign.

`subs` names every subcontract in declaration order, an anonymous one as a NULL entry, and each pledge's `sub` indexes it, -1 for a pledge declared outside any subcontract. A `sub` outside `[0, nsubs)` reads as loose, which is what a zero-filled handwritten descriptor gets. The atom table, the `has_reqs` flag, and the three postfix policy programs are the requirements surface, described in their own section below; a handwritten descriptor that carries none of them gets the structural default policy.

## The registrar

A module exports exactly one undecorated symbol:

```c
AshStatus ash_module_register(AshRuntime* rt);
```

`ash_module_load` dlopens the module, resolves the registrar, and calls it. The registrar hands each contract descriptor to `ash_register_contract` and stops at the first failure. Registering a contract name the runtime already holds is `ASH_ERR_NAME`; a module that fails to register is closed and reported as `ASH_ERR_LOAD` or the registrar's own status.

## Signing and vows

`ash_contract_sign` finds the contract by name, checks the shape hash when the caller supplied one, and builds the instance's vow storage: declared defaults first, then the caller's `AshVowBinding` overrides. An override naming no vow is `ASH_ERR_NAME`, an override whose value carries the wrong type tag is `ASH_ERR_TYPE`, and a vow with neither a default nor an override is `ASH_ERR_UNBOUND`. A contract with any abstract pledge, an `fn` of NULL nothing has bound, refuses to sign with `ASH_ERR_UNBOUND`. No failure leaves an instance behind.

```c
typedef struct AshVowBinding {
    const char* name;
    AshValue    value;   /* copied onto the instance at sign */
} AshVowBinding;

AshStatus ash_contract_sign(AshRuntime* rt, const char* contract_name,
                            const AshVowBinding* vows, size_t nvows,
                            uint64_t expected_hash, AshContract** out);
```

NULL and 0 sign on the declared defaults alone, and an `expected_hash` of 0 skips the shape check. The instance's state and signature read back through calls whose enum values are as fixed as the type tags, `ASH_FULFILLED` sitting below `ASH_PARTIAL` because the enum declares it first:

```c
typedef enum AshContractState {
    ASH_UNSIGNED = 0, ASH_SIGNED, ASH_FULFILLED, ASH_PARTIAL, ASH_BROKEN
} AshContractState;

AshContractState ash_contract_state(const AshContract* c);
uint64_t         ash_contract_hash(const AshContract* c);
int64_t          ash_contract_signed_at(const AshContract* c);
const AshValue*  ash_vow_ref(AshContract* c, const char* name);
AshStatus        ash_contract_break(AshContract* c);
```

## Binding host implementations

An abstract pledge, a declaration with no body, compiles to a descriptor entry whose `fn` is NULL. The host supplies the implementation:

```c
AshStatus ash_pledge_bind(AshRuntime* rt, const char* pledge_name,
                          AshPledgeFn fn);
```

`pledge_name` is either `"Contract.pledge"` or the pledge's mangled symbol; `ASH_ERR_NAME` when no registered contract carries it. The bound function runs in the uniform thunk frame with `ctx` = the signed instance, exactly like a compiled body, and everything it builds through the allocation helpers is instance owned. The descriptor tables are const data inside the module image, so the binding lives in an overlay the runtime keeps.

The overlay holds the raw function pointer and nothing else. A host whose implementation is a generated trampoline, a ctypes callback or any FFI-made thunk, keeps that trampoline alive itself for as long as the runtime can dispatch it; the runtime takes no reference and cannot see a garbage collector coming. The simple rule for such a host is that a binding lives as long as the runtime does.

Bindings resolve at sign. Signing walks the contract's pledges and fixes the instance's dispatch table then and there, the host binding beating the compiled body, a pledge with neither refusing the sign with `ASH_ERR_UNBOUND`. An instance signed before a bind keeps dispatching what it was signed with; a rebind replaces the binding for instances signed after it. A per-binding userdata is deferred; a bound implementation that needs state reaches it through its own globals for now, and reads the contract's vows through `ash_vow_ref` on `ctx` like any thunk.

A signed instance carries a hidden signature: the shape hash it was signed under and the Unix time the signing happened, readable through `ash_contract_hash` and `ash_contract_signed_at`. Thunks and hosts read vow values through `ash_vow_ref`, which returns a pointer the instance owns.

## Futures

Fulfillment has one real shape, the future:

```c
AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs,
                              const AshRef* refs, size_t nrefs);
AshStatus  ash_future_wait(AshFuture* f, AshValue* out);
```

`ash_pledge_fulfill` starts a fulfillment and hands back its receipt. The two calls bracket real concurrency: the fulfill validates and copies in on the caller's thread, a pool worker runs the pledge body, and the wait blocks until the outcome exists. Every fulfillment error, wrong state, unknown pledge, argument mismatch, is delivered by the wait rather than the fulfill, so a host's error handling has one place to live. The fulfill returns NULL only when its own arguments are null or the future cannot be allocated.

Every value argument is deep copied onto the instance at the fulfill call, on the caller's thread, so host argument lifetimes end when the call returns; the pledge body sees only the instance owned frame. `refs` pass trailing parameters by reference, `NULL` and 0 for none; the count rule is `nargs + nrefs == ` the pledge's declared parameter count, with the refs occupying the trailing slots.

A future delivers exactly once. The first `ash_future_wait` blocks until the fulfillment completes, writes any ref slots back to host memory on the waiting thread, then copies the value out and returns the fulfillment's status; every later wait on the same future is `ASH_ERR_STATE`. The value's contents are owned by the instance that produced it, so wait before you break. `ash_pledge_fulfill_sync` remains as the two calls fused, on the same internal path, its write back happening before it returns:

```c
AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
                                  const AshValue* args, size_t nargs,
                                  const AshRef* refs, size_t nrefs,
                                  AshValue* out);
```

## Cross-contract calls

A pledge body may sign another contract, fulfill its pledges, and break it. The runtime the body signs against is its own instance's, reached through the backref every instance carries:

```c
AshRuntime* ash_instance_runtime(const AshContract* c);
```

Compiled code lowers `Contract.sign(overrides)` onto `ash_contract_sign(ash_instance_runtime(ctx), ...)` with the overrides in an `AshVowBinding` array, `instance.pledge(args)` onto `ash_pledge_fulfill_sync`, and `instance.break()` onto `ash_contract_break`. A sign or fulfill status other than `ASH_OK` is a fault of the enclosing pledge: the thunk returns that status and the enclosing fulfillment reports it, no latch moving.

**Reentrancy.** `ash_pledge_fulfill_sync` called on a pool worker thread, which is what a call from inside a pledge body is, runs the fulfillment inline on that worker: the same validate, copy in, run, latch, and write back walk, under the callee instance's lock, with no future and no queue. Queueing it would park the worker on work only the pool can run, and a pool whose every worker is parked that way deadlocks; the inline rule is what makes nested synchronous calls safe at any pool size. Called from a host thread it is the queued path, unchanged. `ash_pledge_fulfill`, the future form, always queues; a pledge body that queues work and waits on it from the worker can still starve the pool, so a body that must call synchronously uses the sync form, which is the only form the language emits.

**Callee result lifetime.** A fulfillment's value is owned by the callee instance and dies with its heap. Compiled callers therefore deep copy the result onto their own instance immediately after the call returns, before anything else runs, so the value survives the callee's `break()` the way value semantics promise. A host bound body that calls across contracts must do the same, `ash_value_deep_copy` against its own ctx, before breaking the callee.

**Instances a body leaves behind.** A signed instance nothing breaks, the path where propagation exits a body between a sign and its break, stays on the runtime's instance registry and is reclaimed at `ash_runtime_shutdown` like every instance. Nothing leaks past shutdown; the instance slot stays occupied until then.

## By-reference arguments

```c
typedef void (*AshWriteBackFn)(void* host_ptr, const AshValue* v, void* user);

typedef struct AshRef {
    void*          host_ptr;
    uint32_t       ty;          /* AshTypeTag of the referenced value */
    uint64_t       cap;         /* byte capacity at host_ptr, for callbacks */
    AshWriteBackFn write_back;  /* NULL selects the default write back */
    void*          user;        /* passed through to write_back untouched */
} AshRef;
```

`host_ptr` addresses the host's storage for a value of type `ty`: the raw scalar for the numeric and character types, an `AshString` struct for strings. Those are the types that cross by reference in v1; a ref carrying anything else is refused with `ASH_ERR_TYPE`, as is a null `host_ptr`.

The protocol has exactly two moments, both on a thread the host is blocked in an ash call with, so the runtime never touches host memory behind the host's back:

1. Copy in, at fulfillment entry, inside `ash_pledge_fulfill` or `ash_pledge_fulfill_sync` on the caller's thread. The referenced value is deep copied onto the instance as a mutable slot appended after the value arguments. From here the pledge body mutates instance memory only.
2. Write back, at delivery, inside the `ash_future_wait` that collects the outcome or before `ash_pledge_fulfill_sync` returns, on the waiting thread. Each slot's final value goes back to host memory: through `write_back` when the host supplied one, `user` passed through untouched, or by the default otherwise.

The default write back writes scalars in place and writes a whole `AshString` struct for strings. The string bytes it points at are instance owned and die at break; a host that wants the shouted bytes to outlive the instance supplies a `write_back` that copies them out, which is also where a capacity declared in `cap` gets honored. Write back happens only when the fulfillment itself reported `ASH_OK`; a thunk error leaves host memory untouched. Every slot's type is checked against its ref before anything is written, so a pledge that swapped a slot's type writes nothing and the wait reports `ASH_ERR_TYPE`.

## Requirements and the contract state

Every pledge carries a latch: pending until its first outcome, fulfilled on its first Ok, broken on an Err that lands before any Ok, and immutable after either. Later fulfillments still run and still return their results to the caller; the latch never moves, so the contract state never regresses from a pledge's point of view. A pledge that returns Option or Unit latches fulfilled on any delivered outcome; only a Result with the Err tag latches broken, and its payload, the boxed value behind the Err, is kept beside the latch for the partial surface.

A subcontract is fulfilled when every pledge inside it has latched fulfilled and broken when every pledge inside it has latched broken. An empty subcontract holds no latch to test and reads neither, constant false on both sides.

The policy rides in the descriptor as an atom table and three postfix programs:

```c
enum { ASH_ATOM_FULFILLED = 0, ASH_ATOM_BROKEN = 1 };

typedef struct AshReqAtom {
    const char* name;    /* informational; NULL for an anonymous sub */
    uint32_t    kind;    /* which latch the atom tests */
    int32_t     sub;     /* subs table index, -1 when the atom is a pledge */
    int32_t     pledge;  /* pledges table index, -1 when the atom is a sub */
} AshReqAtom;

enum { ASH_REQ_ATOM = 0, ASH_REQ_NOT = 1, ASH_REQ_AND = 2, ASH_REQ_OR = 3 };

typedef struct AshReqOp {
    uint8_t op;
    uint8_t atom;        /* atoms index, meaningful for ASH_REQ_ATOM only */
} AshReqOp;
```

A source atom is always the fulfilled test, the grammar's rule that a bare name means the item is fulfilled and `!` negates it, so a false atom covers pending and broken alike. `ASH_ATOM_BROKEN` exists for the synthesized default break line, everything broken, which a fulfilled atom cannot spell. A program of length 0 is a line the source did not write and never fires. Evaluation is a small stack machine under the instance lock; a malformed program that would over- or underflow reads false rather than anything worse.

The compiler serializes the source block's lines from the same AST its satisfiability check walked, atoms in the same first appearance order, so what `ashc check` proved about fulfill and break never co-holding is exactly what the runtime evaluates. When the source wrote no block the compiler synthesizes the grammar's defaults as trees: fulfill when every subcontract and every loose pledge is fulfilled, partial when at least one subcontract is, break when everything is broken. `has_reqs` records which case it was, informational either way. A descriptor with no requirements data at all, the handwritten case, gets the same structural default computed from the descriptor shape.

**Evaluation.** After every fulfillment outcome, under the same instance lock the thunk ran in, the runtime latches the pledge and recomputes the state in priority order: break, then fulfill, then partial, the first line that matches setting the state, and Signed when none does. A state satisfying both fulfill and partial is therefore Fulfilled.

**The arming rule.** The break line matches only once at least one pledge has latched broken. A break line written over negated atoms, the README's `!Validation && !Processing && !notify_user` shape, is true the moment the contract signs, since nothing is fulfilled yet; firing it on the contract's first Ok would tear down a contract nothing broke. Arming it on the first broken pledge keeps a written break line and the synthesized default, which already implies a broken pledge, reading the same way.

**Automatic break.** A break line that fires latches the contract Broken exactly as `ash_contract_break` does, every later fulfillment reporting `ASH_ERR_STATE`, with one deliberate difference: the instance heap is kept alive, because the Err payloads the partial surface hands out live there. Only an explicit `ash_contract_break` reclaims the heap, and the runtime shutdown reclaims it regardless.

## The partial result

The PartialResult surface reports which items landed, which are pending, which broke, and the errors attached, the language's `instance.partial()` seen from C:

```c
typedef enum AshItemState {
    ASH_ITEM_PENDING   = 0,
    ASH_ITEM_FULFILLED = 1,
    ASH_ITEM_BROKEN    = 2
} AshItemState;

size_t      ash_partial_count(AshContract* c, AshItemState k);
const char* ash_partial_name(AshContract* c, AshItemState k, size_t i);
size_t      ash_partial_nerrors(AshContract* c);
AshStatus   ash_partial_error(AshContract* c, size_t i,
                              const char** pledge_name, const AshValue** err);
```

An item is a named subcontract or a loose pledge, the same names a requirements atom can test; an anonymous subcontract groups its pledges for the policy but has no name a result could report. Enumeration order is descriptor order, named subcontracts in declaration order first, then loose pledges in declaration order, deterministic across runs of the same module. A named subcontract reads fulfilled when every pledge inside it latched Ok, broken when every one latched Err, and pending otherwise; a loose pledge reads its own latch.

The errors walk every pledge, subcontract members included, in declaration order: `ash_partial_nerrors` counts the pledges latched broken, and `ash_partial_error` hands out the i-th one's declared name and a pointer to the payload its first Err carried. The payload pointer is runtime owned and stays valid until shutdown, but what it points into follows the instance heap: it survives an automatic break, whose whole point is handing the errors over, and reads as a zeroed Unit value after an explicit break reclaimed the heap.

Every call takes the instance lock and reads the latches as they stand at that moment, so two calls bracket a snapshot only when no fulfillment can land between them; a host that wants a coherent picture reads it after its waits complete.

## Threading

The runtime owns a worker pool. `ash_runtime_init` takes an `AshRuntimeConfig` whose `max_threads` sizes it, 0 or a NULL config selecting the default of 4 workers; a request beyond 256 is refused with `ASH_ERR_TYPE`. The pool drains one unbounded queue, intrusive through the future itself so queuing allocates nothing. `ash_runtime_shutdown` stops intake, drains what is queued, joins every worker, and only then frees instances, futures, and modules; nothing else may be calling into the runtime by then.

The moments a fulfillment touches host memory are unchanged from the single threaded ABI, and that is the whole point of the copy boundary: copy-in happens inside `ash_pledge_fulfill` on the caller's thread, write back happens inside the delivering `ash_future_wait` on the waiting thread, and the pool worker in between only ever sees instance owned memory.

**Serialization.** Every instance carries one recursive mutex. It covers fulfillment validation and copy-in, the whole thunk run, the outcome latch, every block list allocation, and break, so fulfillments against one instance serialize in queue order while distinct instances run truly in parallel. The allocation helpers, `ash_bytes` and everything built on it, take the instance lock themselves: recursive acquisition makes them safe inside a thunk, where the worker already holds the lock, and cold acquisition makes them safe from a host thread outside any fulfillment. A single `AshValue` is still not a shared object; two threads mutating the same value is a host bug. `ash_vow_ref` is safe from any thread, with the standing caveat that the pointer it returns is instance owned and dangles after break.

**Wait semantics.** `ash_future_wait` blocks on the future's condition variable until the outcome exists, delivers exactly once, and performs the ref write back under the future's mutex on the waiting thread. A second wait is `ASH_ERR_STATE`. The future struct itself is heap memory the runtime tracks per instance and frees at shutdown, which is what makes a late wait safe; everything the delivered value points at is instance owned and dies at break, which is what keeps wait-before-break the rule for a host that wants the bytes.

**Break against in-flight work.** `ash_contract_break` takes the instance lock, so a thunk mid-run finishes and latches before the break proceeds; a task still queued finds the state already Broken when its worker arrives and never touches the freed heap. Before freeing anything the break forfeits every future not yet waited, delivered or not, to `ASH_ERR_STATE` and clears its pointers into the instance heap. A fulfillment racing a break therefore resolves to exactly one of two outcomes: delivered before the break won, or `ASH_ERR_STATE`; never a crash, never freed memory. An unwaited outcome is forfeited even if the pledge ran, so a host that wants a result waits before it breaks.

**Lock ordering.** The hierarchy runs instance above runtime, then future, taken downward and never upward: a caller instance's mutex, then any callee instance mutexes a cross-contract call nests below it, then the runtime lock over the descriptor, instance, and binding tables, then the per-future mutex. The pool's queue lock is a leaf taken with no other lock held. The runtime lock never sits above an instance mutex: `ash_contract_sign` resolves the descriptor and dispatch table under the runtime lock alone, builds the instance with no locks held, the allocation helpers touching only the fresh, unpublished instance's own mutex, and publishes it under the runtime lock again, which is what lets a thunk holding its instance lock sign through `ash_instance_runtime`. `ash_future_wait` takes only the future mutex, so a waiter can never hold up an instance.

Instance-to-instance nesting is safe for compiled code: a body only ever holds instances it signed itself, fresh and unshared, so the per-thread lock graph is a tree over recursive mutexes and a cycle cannot be constructed. The one deadlock v1 leaves possible is host made: two host bound bodies on different threads fulfilling against each other's shared instances in opposite orders. That needs instance pointers handed around outside the language, and v1 documents it instead of detecting it; `ASH_ERR_DEADLOCK` stays reserved.

## Ownership

One rule, stated once: everything a fulfillment builds hangs off the contract instance it ran against, and `ash_contract_break` frees all of it in one walk. That covers thunk allocations, boxed payloads, concatenated strings, copied vow values, argument frames, and ref slots. A host that wants to keep a result copies it out before breaking; `ash_value_deep_copy` against another instance is one way. Arguments are deep copied at fulfillment entry, so the runtime holds nothing the host owns once the fulfill call returns, and the host may free or reuse its argument memory immediately. The instance struct itself stays valid for state queries until runtime shutdown; only its heap is gone. The future struct rides the same rule as the instance: tracked per instance, valid for its one wait even after break, freed at shutdown, while the value inside it follows the instance heap.

Breaking latches the state at Broken, forfeits every unwaited future to `ASH_ERR_STATE`, and every later fulfillment on the instance reports `ASH_ERR_STATE` through its future or its synchronous return.

## Mangling

Every pledge a module compiles gets a mangled symbol:

```text
__ash_ash_{contract}_{pledge}_{sighash}_v{version}
```

`{contract}` and `{pledge}` are the declared names, which are identifiers and therefore valid symbol fragments. `{sighash}` is the 16 lowercase hex digits of the FNV-1a 64 hash of the pledge's canonical signature spelling. `{version}` is the contract's `version` attribute, 1 when the declaration carries none. The sig hash keeps two revisions of a pledge apart even when nothing about the surrounding contract moved; the version keeps two shipped generations apart on purpose.

The mangled name appears twice on purpose: as the thunk's C identifier inside the module and as the `mangled` string in the pledge descriptor, so a debugger and the runtime agree about what they are looking at.

The contract type itself has a mangled name too, in the same grammar with an empty symbol slot and the shape hash where a pledge carries its signature hash:

```text
__ash_ash_{contract}__{shapehash}_v{version}
```

The compiler does not emit this one; the runtime synthesizes it from the contract descriptor at registration, which it can do byte for byte because every piece is already in the descriptor. It exists so the iname table has one keyspace: every discoverable thing, contract type or pledge, is one mangled string.

## The iname table

The iname table is the runtime's registry of contract types, the discovery surface a foreign host resolves mangled names against. `ash_register_contract` fills it, one entry for the contract and one per pledge that carries a mangled name, each keyed by that mangled string; a handwritten descriptor whose pledges carry no mangled names contributes only its contract entry. The table is a type registry, not an instance registry: signing adds nothing to it, and after `ash_runtime_freeze` it never changes again.

```c
typedef enum AshInameKind {
    ASH_INAME_CONTRACT = 0,
    ASH_INAME_PLEDGE   = 1
} AshInameKind;

typedef struct AshInameEntry {
    const char* mangled;
    uint32_t    kind;        /* AshInameKind */
    const char* contract;    /* owning contract name */
    const char* symbol;      /* pledge name, NULL for a contract entry */
    uint64_t    shape_hash;
    uint32_t    version;
    uint32_t    nargs;       /* pledge only, 0 for a contract entry */
} AshInameEntry;
```

`shape_hash` is the owning contract's shape hash for both kinds, so a pledge entry hands a host exactly the pair it signs under: `contract` names the contract and `shape_hash` is what `ash_contract_sign` checks. `version` is the contract's version attribute. The strings live until shutdown: `contract`, `symbol`, and a pledge entry's `mangled` point into the module image, a contract entry's `mangled` is runtime owned heap.

The surface is four calls, every one taking the runtime lock, so the table is safe from any thread by the same discipline as registration itself:

```c
AshStatus ash_iname_lookup(AshRuntime* rt, const char* mangled, AshInameEntry* out);
size_t    ash_iname_count(AshRuntime* rt);
AshStatus ash_iname_at(AshRuntime* rt, size_t i, AshInameEntry* out);
AshStatus ash_iname_dump(AshRuntime* rt, char* buf, size_t cap, size_t* need);
```

Lookup is exact: the whole mangled name or `ASH_ERR_NAME`. A host holding yesterday's header therefore misses today's module instead of resolving to the wrong shape; the version mismatch is a clean name error before any hash comparison happens. Enumeration through `ash_iname_count` and `ash_iname_at` walks in strict mangled name order, deterministic for a given set of registered contracts; an index out of range is `ASH_ERR_NAME`.

`ash_iname_dump` renders the whole table in its canonical text form, the Layer 2 discovery payload. One entry per line in mangled name order, and this format is normative, a change to it is a wire change:

```text
{mangled} {kind} {shape_hash} v{version}\n
```

`{kind}` is spelled `contract` or `pledge`, `{shape_hash}` is 16 lowercase hex digits. The text is byte stable for a given set of registered contracts. `*need` receives the full size including the terminating NUL; when `cap` is at least that, the text and its NUL are written to `buf` and the call returns `ASH_OK`, otherwise nothing is written and the call returns `ASH_ERR_OOM`, so a NULL `buf` with `cap` 0 sizes the buffer. Loading the two skeleton modules dumps:

```text
__ash_ash_Greeter2__26a068304bf801f2_v2 contract 26a068304bf801f2 v2
__ash_ash_Greeter2_greet_8d9bbc95f1afbbec_v2 pledge 26a068304bf801f2 v2
__ash_ash_Greeter__80464bf23398ab38_v1 contract 80464bf23398ab38 v1
__ash_ash_Greeter_greet_17cef80f14421b9b_v1 pledge 80464bf23398ab38 v1
__ash_ash_Greeter_shout_17cef80f14421b9b_v1 pledge 80464bf23398ab38 v1
```

## The freeze

`ash_runtime_freeze` latches the registration surface shut. After it returns, `ash_module_load`, `ash_register_contract`, and `ash_pledge_bind` report `ASH_ERR_STATE`; `ash_contract_sign`, fulfillment, waits, vow reads, breaks, and every iname read continue unchanged. The call is idempotent and safe from any thread. The point is the discovery contract: once a host has frozen the runtime, the iname table it enumerates is the table every later sign resolves against, byte for byte, and no module loaded behind its back can move a name.

## The shape hash

A contract's shape hash is FNV-1a 64, offset basis `0xcbf29ce484222325` and prime `0x100000001b3`, over the contract's canonical signature string:

```text
contract {name};vow {name}:{type};...pledge {name}:{sig};...
```

Vows first, then pledges, each in declaration order, each type spelled in the checker's canonical form, the same spelling signature comparison uses. The string is deterministic by construction, no table iteration order touches it, so two builds of the same header agree byte for byte and any change to a vow type or a pledge signature moves the hash.

A host that passes the expected hash to `ash_contract_sign` gets `ASH_ERR_VERSION` when the loaded module's shape disagrees, which is the whole defense against fulfilling yesterday's contract through today's module. Passing 0 skips the check.

## The standalone entry

`ashc build --bin` wraps a module whose program declares the grammar's entry point, exactly one `Main` contract carrying a concrete `run(args: List<String>) -> Result<Int, E>` pledge, `E` any declared error type. The compiler emits the module C as always, plus a small `main()` wrapper beside it, and links the pair against libashrt into an executable instead of a shared library. The module is inside the executable, so the wrapper calls `ash_module_register` directly and no dlopen happens.

The wrapper's walk is fixed: `ash_runtime_init`, `ash_module_register`, `ash_runtime_freeze`, then `ash_contract_sign` on `"Main"`. Only then does it build the argument list, `argv[1..]` deep copied into a `List<String>` through the signed instance, because the allocation helpers need an instance to own the bytes. It fulfills `run` synchronously and maps the outcome onto the process:

- `Ok(n)` exits with the low eight bits of `n`, the shell's own truncation made explicit.
- `Err(e)` renders `e` through `ash_value_render` to stderr, one line naming the program, and exits 1.
- A fulfillment status other than `ASH_OK`, and every setup failure before it, is a diagnostic on stderr naming the step and the status, and exit 1.

The instance is broken and the runtime shut down on every path, so the sanitizers hold the wrapper to the same standard as any host. stdout belongs to the program alone; the wrapper never writes it.

## The generated header

`ashc emit-header file.ash` runs the whole front end and writes `target/ashc-out/<stem>.ash.h`, the C header a foreign host compiles against instead of hardcoding mangled strings. The header carries no layouts, those stay in `ash_abi.h`; it spells names and hashes alone, two defines per surface:

```c
/* contract Greeter, version 1
 * vow prefix: String (default)
 */
#define ASH_HASH_Greeter 0x80464bf23398ab38ULL

/* pledge greet(name: String) -> Result<String, GreetError> */
#define ASH_MANGLED_Greeter_greet "__ash_ash_Greeter_greet_17cef80f14421b9b_v1"
```

One `ASH_HASH_{Contract}` per public contract, the shape hash `ash_contract_sign` checks, and one `ASH_MANGLED_{Contract}_{pledge}` per pledge, the exact string the iname table keys, each under a comment spelling the signature the way the source wrote it, an abstract pledge marked so the host knows to bind before signing. Internal contracts publish no descriptor and appear in no header. The hashes and mangled names come from the same canonical spellings the module emitter hashes, so a header and its module cannot disagree, and the text is byte stable for a given source, which is what lets `tests/golden/hello.ash.h` pin it.

## Status codes

The numbers are part of the wire: `ASH_OK` is 0 and the rest count up in the order of this table, `ASH_ERR_STATE` 1 through `ASH_ERR_NET` 9.

| status | meaning |
|---|---|
| `ASH_OK` | the operation ran; a pledge's Err is still `ASH_OK` |
| `ASH_ERR_STATE` | illegal in the contract's current state, a second wait, a future a break forfeited, or load, register, and bind after the freeze |
| `ASH_ERR_TYPE` | argument count or type mismatch, at a thunk, at sign, at a ref, or an oversized pool request |
| `ASH_ERR_VERSION` | shape hash disagreed at sign |
| `ASH_ERR_UNBOUND` | an abstract pledge or an unsupplied vow at sign |
| `ASH_ERR_NAME` | no contract, pledge, vow, or iname entry under that name |
| `ASH_ERR_DEADLOCK` | reserved; v1 constructs no lock cycles and ships no detector |
| `ASH_ERR_OOM` | allocation failed |
| `ASH_ERR_LOAD` | dlopen or registrar failure on a module |
| `ASH_ERR_NET` | the connection carrying this operation failed |
