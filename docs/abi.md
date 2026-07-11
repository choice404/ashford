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

## The thunk frame

Every pledge crosses the boundary in one shape, compiled bodies and host bound implementations alike:

```c
typedef AshStatus (*AshPledgeFn)(void* ctx, const AshValue* args, size_t nargs,
                                 AshValue* out);
```

`ctx` is the signed contract instance the fulfillment runs against. The runtime zeroes `out` before the call. The thunk validates `nargs` and each argument's type tag before touching a union arm, reports `ASH_ERR_TYPE` on any mismatch, and writes the pledge's declared value through `out` on `ASH_OK`. Everything a thunk allocates goes through `ash_bytes`, `ash_box`, or a helper built on them, so it is owned by the instance in `ctx` and dies with it.

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

A signed instance carries a hidden signature: the shape hash it was signed under and the Unix time the signing happened, readable through `ash_contract_hash` and `ash_contract_signed_at`. Thunks and hosts read vow values through `ash_vow_ref`, which returns a pointer the instance owns.

## Futures

Fulfillment has one real shape, the future:

```c
AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs);
AshStatus  ash_future_wait(AshFuture* f, AshValue* out);
```

`ash_pledge_fulfill` starts a fulfillment and hands back its receipt. Today the pledge runs to completion inside the call and the future only carries the outcome; when the threading milestone arrives the same two calls bracket real concurrency, which is why every fulfillment error, wrong state, unknown pledge, argument mismatch, is delivered by the wait rather than the fulfill. The fulfill returns NULL only when its own arguments are null or the future cannot be allocated.

A future delivers exactly once. The first `ash_future_wait` copies the value out and returns the fulfillment's status; every later wait on the same future is `ASH_ERR_STATE`. The future is owned by the instance that produced it, so wait before you break. `ash_pledge_fulfill_sync` remains as the two calls fused, on the same internal path.

## Ownership

One rule, stated once: everything a fulfillment builds hangs off the contract instance it ran against, and `ash_contract_break` frees all of it in one walk. That covers thunk allocations, boxed payloads, concatenated strings, copied vow values, and futures. A host that wants to keep a result copies it out before breaking. Arguments are host owned and the runtime never keeps them. The instance struct itself stays valid for state queries until runtime shutdown; only its heap is gone.

Breaking latches the state at Broken and every later fulfillment on the instance reports `ASH_ERR_STATE` through its future or its synchronous return.

## Mangling

Every pledge a module compiles gets a mangled symbol:

```text
__ash_ash_{contract}_{pledge}_{sighash}_v{version}
```

`{contract}` and `{pledge}` are the declared names, which are identifiers and therefore valid symbol fragments. `{sighash}` is the 16 lowercase hex digits of the FNV-1a 64 hash of the pledge's canonical signature spelling. `{version}` is the contract's `version` attribute, 1 when the declaration carries none. The sig hash keeps two revisions of a pledge apart even when nothing about the surrounding contract moved; the version keeps two shipped generations apart on purpose.

The mangled name appears twice on purpose: as the thunk's C identifier inside the module and as the `mangled` string in the pledge descriptor, so a debugger and the runtime agree about what they are looking at.

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
| `ASH_ERR_STATE` | illegal in the contract's current state, or a second wait |
| `ASH_ERR_TYPE` | argument count or type mismatch, at a thunk or at sign |
| `ASH_ERR_VERSION` | shape hash disagreed at sign |
| `ASH_ERR_UNBOUND` | an abstract pledge or an unsupplied vow at sign |
| `ASH_ERR_NAME` | no contract, pledge, or vow under that name |
| `ASH_ERR_DEADLOCK` | reserved for the threading milestone |
| `ASH_ERR_OOM` | allocation failed |
| `ASH_ERR_LOAD` | dlopen or registrar failure on a module |
