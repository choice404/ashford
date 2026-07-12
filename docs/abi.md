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

Strings are a fat value: a pointer and a byte length, UTF-8, no terminator. The bytes live wherever the value's owner put them; a compiled module points string constants into its own mapped image, and everything built at run time lives on a contract instance.

`tag` carries the variant for the sum shaped types and is 0 for everything else:

| type | tag 0 | tag 1 |
|---|---|---|
| Option | None | Some |
| Result | Ok | Err |

An Option or Result payload rides in `as.box` as a pointer to a single `AshValue` the instance owns. `None` carries a null box.

A list carries its elements as a contiguous `AshValue` array behind `as.list.data`, `len` of them live, `cap` allocated, `elem_ty` the declared element tag. A tuple rides the same union arm: the data pointer is the `AshValue` array, `len` is the arity, `cap` equals `len`, and `elem_ty` is 0 because a tuple's elements need not agree. Maps and records have no settled runtime representation yet; a deep copy that meets one reports `ASH_ERR_TYPE` rather than freeze a representation by accident.

The runtime exposes deep value helpers to hosts and thunks alike: `ash_list_new`, `ash_list_push`, `ash_list_get`, `ash_tuple_new`, and `ash_value_deep_copy`, which recursively copies any supported value into instance owned memory, string bytes, list and tuple elements, and boxed payloads included. After a deep copy, nothing in the destination aliases memory the instance does not own.

## The thunk frame

Every pledge crosses the boundary in one shape, compiled bodies and host bound implementations alike:

```c
typedef AshStatus (*AshPledgeFn)(void* ctx, const AshValue* args, size_t nargs,
                                 AshValue* out);
```

`ctx` is the signed contract instance the fulfillment runs against. The runtime zeroes `out` before the call. The thunk validates `nargs` and each argument's type tag before touching a union arm, reports `ASH_ERR_TYPE` on any mismatch, and writes the pledge's declared value through `out` on `ASH_OK`. Everything a thunk allocates goes through `ash_bytes`, `ash_box`, or a helper built on them, so it is owned by the instance in `ctx` and dies with it.

`args` is the full frame, one slot per declared parameter, and every slot is an instance owned deep copy the runtime built at fulfillment entry. A pledge body never sees host memory. When the caller passed trailing parameters by reference, those slots are mutable on purpose: an implementation that updates a by-reference parameter casts away the const on its own slot and writes the new value there, and the runtime carries the final slot values back to the host at delivery.

A pledge that succeeds with an `Err` result still returns `ASH_OK`; the status word reports whether the thunk ran, the value reports what the pledge decided. The runtime latches the outcome either way.

## Descriptor tables

A compiled module publishes its contracts as static const data. The runtime keeps the pointers rather than copying, so the tables live exactly as long as the module, which stays mapped until runtime shutdown.

```c
typedef struct AshPledgeDesc {
    const char* name;      /* the pledge's declared name */
    const char* mangled;   /* the mangled symbol, see below */
    uint32_t    nargs;
    AshPledgeFn fn;        /* NULL marks an abstract pledge awaiting a bind */
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
} AshContractDesc;
```

Pledges and vows appear in declaration order. A vow whose declaration carried an initializer has `has_default` set and the literal encoded in `default_value`; a string default points at bytes inside the module. Signing copies every vow value onto the instance, so nothing an instance reads aliases the descriptor after sign.

## The registrar

A module exports exactly one undecorated symbol:

```c
AshStatus ash_module_register(AshRuntime* rt);
```

`ash_module_load` dlopens the module, resolves the registrar, and calls it. The registrar hands each contract descriptor to `ash_register_contract` and stops at the first failure. Registering a contract name the runtime already holds is `ASH_ERR_NAME`; a module that fails to register is closed and reported as `ASH_ERR_LOAD` or the registrar's own status.

## Signing and vows

`ash_contract_sign` finds the contract by name, checks the shape hash when the caller supplied one, and builds the instance's vow storage: declared defaults first, then the caller's `AshVowBinding` overrides. An override naming no vow is `ASH_ERR_NAME`, an override whose value carries the wrong type tag is `ASH_ERR_TYPE`, and a vow with neither a default nor an override is `ASH_ERR_UNBOUND`. A contract with any abstract pledge, an `fn` of NULL nothing has bound, refuses to sign with `ASH_ERR_UNBOUND`. No failure leaves an instance behind.

## Binding host implementations

An abstract pledge, a declaration with no body, compiles to a descriptor entry whose `fn` is NULL. The host supplies the implementation:

```c
AshStatus ash_pledge_bind(AshRuntime* rt, const char* pledge_name,
                          AshPledgeFn fn);
```

`pledge_name` is either `"Contract.pledge"` or the pledge's mangled symbol; `ASH_ERR_NAME` when no registered contract carries it. The bound function runs in the uniform thunk frame with `ctx` = the signed instance, exactly like a compiled body, and everything it builds through the allocation helpers is instance owned. The descriptor tables are const data inside the module image, so the binding lives in an overlay the runtime keeps.

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

A future delivers exactly once. The first `ash_future_wait` blocks until the fulfillment completes, writes any ref slots back to host memory on the waiting thread, then copies the value out and returns the fulfillment's status; every later wait on the same future is `ASH_ERR_STATE`. The value's contents are owned by the instance that produced it, so wait before you break. `ash_pledge_fulfill_sync` remains as the two calls fused, on the same internal path, its write back happening before it returns.

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

## Threading

The runtime owns a worker pool. `ash_runtime_init` takes an `AshRuntimeConfig` whose `max_threads` sizes it, 0 or a NULL config selecting the default of 4 workers; a request beyond 256 is refused with `ASH_ERR_TYPE`. The pool drains one unbounded queue, intrusive through the future itself so queuing allocates nothing. `ash_runtime_shutdown` stops intake, drains what is queued, joins every worker, and only then frees instances, futures, and modules; nothing else may be calling into the runtime by then.

The moments a fulfillment touches host memory are unchanged from the single threaded ABI, and that is the whole point of the copy boundary: copy-in happens inside `ash_pledge_fulfill` on the caller's thread, write back happens inside the delivering `ash_future_wait` on the waiting thread, and the pool worker in between only ever sees instance owned memory.

**Serialization.** Every instance carries one recursive mutex. It covers fulfillment validation and copy-in, the whole thunk run, the outcome latch, every block list allocation, and break, so fulfillments against one instance serialize in queue order while distinct instances run truly in parallel. The allocation helpers, `ash_bytes` and everything built on it, take the instance lock themselves: recursive acquisition makes them safe inside a thunk, where the worker already holds the lock, and cold acquisition makes them safe from a host thread outside any fulfillment. A single `AshValue` is still not a shared object; two threads mutating the same value is a host bug. `ash_vow_ref` is safe from any thread, with the standing caveat that the pointer it returns is instance owned and dangles after break.

**Wait semantics.** `ash_future_wait` blocks on the future's condition variable until the outcome exists, delivers exactly once, and performs the ref write back under the future's mutex on the waiting thread. A second wait is `ASH_ERR_STATE`. The future struct itself is heap memory the runtime tracks per instance and frees at shutdown, which is what makes a late wait safe; everything the delivered value points at is instance owned and dies at break, which is what keeps wait-before-break the rule for a host that wants the bytes.

**Break against in-flight work.** `ash_contract_break` takes the instance lock, so a thunk mid-run finishes and latches before the break proceeds; a task still queued finds the state already Broken when its worker arrives and never touches the freed heap. Before freeing anything the break forfeits every future not yet waited, delivered or not, to `ASH_ERR_STATE` and clears its pointers into the instance heap. A fulfillment racing a break therefore resolves to exactly one of two outcomes: delivered before the break won, or `ASH_ERR_STATE`; never a crash, never freed memory. An unwaited outcome is forfeited even if the pledge ran, so a host that wants a result waits before it breaks.

**Lock ordering.** Three lock levels, always taken downward, never upward: the runtime lock over the descriptor, instance, and binding tables (sign, register, bind, load); the per-instance mutex; the per-future mutex. The pool's queue lock is a leaf taken with no other lock held. `ash_future_wait` takes only the future mutex, so a waiter can never hold up an instance. v1 ships no deadlock detector and `ASH_ERR_DEADLOCK` stays reserved: a thunk cannot start a fulfillment (emitted C has no such path), the instance lock is only taken by the runtime itself, and no path holds two instance locks at once, so a lock cycle cannot be constructed.

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

## Status codes

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
