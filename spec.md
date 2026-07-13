# Ashford Language Specification

## Status

This is the language reference for Ashford, a statically typed, contract
oriented language that sits between other languages. A program declares
contracts, other languages sign and fulfill them across the C ABI, and a shared
runtime enforces the terms. This document describes the language as it stands
today: contracts with vows, pledges, clauses, subcontracts, and a requirements
policy, records and sums, the full statement and expression core with exhaustive
match and error propagation, value semantics with one named exception for a
signed instance, a package aware module system, a first standard library, and
the interop surface that is the point of the language, a foreign host driving a
contract through the ABI with no generated bindings. Where this spec states a
rule, the rule is the one `ashc` enforces and the runtime holds.

Ashford is built in three layers. Layer 1, the core language and the in process
runtime, is complete. Layer 2, the network runtime, runs: the same contracts
served over a socket and fulfilled across a machine boundary with the host code
unchanged. Layer 3, the store runtime, contracts backed by a database, is in
progress; the store surface is described here and marked where it is still
landing. The compiler is `ashc`, source files end in `.ash`, a compiled module
ends in `.ash.so`, and the runtime is a shared C library, `libashrt`.

---

## Table of Contents

1. [Core Philosophy](#core-philosophy)
2. [Source Files, Imports, Packages](#source-files-imports-packages)
3. [Contracts](#contracts)
4. [The Lifecycle](#the-lifecycle)
5. [The Requirements Policy](#the-requirements-policy)
6. [Type System](#type-system)
7. [Statements and Expressions](#statements-and-expressions)
8. [Provisional Clauses](#provisional-clauses)
9. [Cross Contract Calls](#cross-contract-calls)
10. [Standalone Programs](#standalone-programs)
11. [The Standard Library](#the-standard-library)
12. [Interop and the ABI](#interop-and-the-abi)
13. [The Store Layer](#the-store-layer)
14. [The Network Layer](#the-network-layer)

---

## Core Philosophy

- Everything is a contract. The contract is the unit of the language, not the
  function. A program is a set of contracts other languages sign and fulfill.
- Nothing is implicit. Ownership is always clear, visibility is always stated,
  numeric widths never convert on their own, and every fallible pledge returns a
  `Result` or an `Option`.
- Errors are values. There are no exceptions, and nothing throws across a
  language boundary. A failure is a value the caller matches on.
- Interop is the primary goal. The runtime owns every heap allocation that
  crosses a boundary, data passes by value in both directions, and a reference
  is copied on entry and written back on return, never held by Ashford.
- The runtime enforces the terms. A contract's shape, its vows, its fulfillment
  policy, and its version are checked at the boundary, so a mismatch is a
  descriptive error and never silent corruption.

---

## Source Files, Imports, Packages

A source file is a list of top level declarations: contracts, records, sums,
provisional clauses, and imports. A file is a module, and a directory of files
is a package.

An `import` names a module by a dotted path:

```text
import ashstd.math
import ashstd.collections
```

A dotted path resolves beside the root file first and then under `ASH_HOME`. A
directory is a package and a file is a module within it. Every imported file
parses into one shared tree, with each file's spans mapped back to its own
source so a diagnostic names the right place. A cycle in the import graph and a
duplicate import are named errors, and two builds of one source emit byte
identical module C.

Visibility is by package. A declaration is exported from its package by default
and visible to any importer. A declaration marked internal to its package is
invisible outside its own directory: an internal contract or provisional clause
cannot be incorporated, signed, or named in a type annotation from another
package, and an internal contract never reaches the descriptor tables or the
iname table a foreign host discovers. The boundary holds at every position,
including nested ones, so an internal type cannot leak through a public
signature.

---

## Contracts

A contract is the unit of Ashford. It groups the state a signing locks, the
commitments a host fulfills, the internal methods those commitments share, and
the policy that decides when the whole is fulfilled, partially fulfilled, or
broken.

```text
contract PaymentService {
    incorporate Loggable

    vow currency: String = "USD"

    subcontract Validation {
        pledge validate_card(card: String) -> Result<Bool, Int> {
            return Ok(true)
        }
        pledge validate_amount(amount: Float) -> Result<Bool, Int> {
            if amount > 0.0 { return Ok(true) }
            return Err(1)
        }
    }

    subcontract Processing {
        pledge charge(card: String, amount: Float) -> Result<Bool, Int>
    }

    pledge notify_user(ok: Bool) -> Result<Bool, Int> {
        return Ok(true)
    }

    requirements {
        fulfill: Validation && Processing && notify_user
        partial: Validation || Processing
        break: !Validation && !Processing && !notify_user
    }
}
```

### Vows

A `vow` is an immutable field, locked the moment the contract is signed. A vow
carries a type and an optional default:

```text
vow currency: String = "USD"
```

A vow default is a constant of the vow's type. Nothing inside the contract may
assign to a vow, and a host may override a vow at sign, supplying a value of the
declared type. A vow with no default and no override at sign is an unbound
signing and is refused. A pledge body reads a vow by name.

### Pledges

A `pledge` is a callable commitment with a declared return type, and it is a
first class element of the language. Its return type is a `Result`, an `Option`,
or `Unit`, never a bare fallible value:

```text
pledge charge(card: String, amount: Float) -> Result<Bool, Int>
```

A pledge with a body is concrete. A pledge with no body is abstract: the
contract cannot be signed until a host binds an implementation for it, which is
how a foreign language plugs its own behavior into a contract. A pledge is
fulfilled through method syntax on a signed instance, `instance.charge(card,
amount)`, and the runtime also exposes a dynamic by name fulfillment for a
foreign host.

A pledge may take an argument by reference, a trailing declared parameter the
runtime copies in on entry and writes back to the host's pointer on return, so
host memory is touched only while the host is blocked in the call. See
[Interop and the ABI](#interop-and-the-abi).

### Clauses

A `clause` is an internal method. It never leaves its contract, has no place in
the lifecycle, and exists so pledges share logic:

```text
clause compare(a: Int, b: Int) -> Int {
    if a < b { return -1 }
    if a > b { return 1 }
    return 0
}
```

A clause is not a first class value and is contract scoped only. Clauses may
call each other in any order.

### Subcontracts

A `subcontract` groups pledges so a contract can be partially fulfilled and so
the requirements policy can name a group as a unit:

```text
subcontract Validation {
    pledge validate_card(card: String) -> Result<Bool, Int> { ... }
    pledge validate_amount(amount: Float) -> Result<Bool, Int> { ... }
}
```

A subcontract is satisfied when every pledge inside it has latched fulfilled. A
pledge declared outside any subcontract is a loose pledge and stands on its own
in the policy.

### Attributes

A contract carries an optional version attribute, `[version: N]`, default 1. The
version is baked into every mangled name, so a host that signs against the wrong
version is refused at discovery rather than corrupted at call time.

---

## The Lifecycle

A contract moves through five states:

```text
Unsigned -> Signed -> Fulfilled
                   -> PartiallyFulfilled
                   -> Broken
```

`sign()` validates the contract's shape, locks its vows, and activates it. It
takes named vow overrides and an optional expected shape hash, and it refuses an
abstract pledge that nothing has bound, an override naming no vow, an override of
the wrong type, and a hash that disagrees.

Fulfilling a pledge runs it and records the outcome. A pledge latches on its
first `Ok`: it becomes fulfilled and stays fulfilled, and later calls run and
return results but never change the latched state. An `Err` before any `Ok`
latches the pledge broken with the error payload kept readable. Contract state
never regresses.

`break()` tears the contract down and reclaims every allocation the runtime owns
for it, in one walk. Every later fulfillment on a broken contract is a state
error.

The partial surface reports, at any moment, which subcontracts and loose pledges
are fulfilled, which are pending, and which are broken, with the latched errors
attached by name.

---

## The Requirements Policy

The `requirements` block writes the contract's fulfillment as boolean logic over
its subcontracts and loose pledges:

```text
requirements {
    fulfill: Validation && Processing && notify_user
    partial: Validation || Processing
    break: !Validation && !Processing && !notify_user
}
```

A bare name means that item is fulfilled. `&&`, `||`, and `!` combine the
conditions. Three lines are defined:

- **fulfill**: the condition under which the contract is fully fulfilled.
- **partial**: the condition under which it is partially fulfilled.
- **break**: the condition under which it breaks.

The runtime evaluates the policy after every outcome, under the instance's lock,
in the priority break, then fulfill, then partial. When the break line holds the
contract transitions to broken by itself, keeping its heap alive so the error
payloads stay readable until an explicit break reclaims them. The break line is
armed only once something has actually broken, so an empty contract does not
break on the vacuous truth of its negations.

The compiler proves at build time that no single state satisfies both the
fulfill and the break line, naming a witness if one exists, and rejects a policy
no state can satisfy. The requirements block compiles to a shared table the
static checker and the runtime evaluator both read, so the two can never
diverge. When a contract writes no requirements block, the grammar's defaults are
synthesized into the same table form.

---

## Type System

### Primitive types

| type | representation |
|---|---|
| `Unit` | the empty value |
| `Int` | 64 bit signed |
| `UInt` | 64 bit unsigned |
| `Float` | IEEE 754 double |
| `Bool` | one byte, true or false |
| `Byte` | one byte |
| `Char` | a 32 bit Unicode scalar value |
| `String` | UTF-8 bytes, a pointer and a byte length, no terminator |

Numeric widths are fixed and never convert implicitly. A widening or narrowing
is a type error unless written explicitly, and there is no implicit conversion
anywhere in the language, widths included.

### Composite types

- **`List<T>`**: a homogeneous, ordered sequence, written `[a, b, c]`.
- **Tuples**: a fixed heterogeneous group whose elements need not agree in type.
- **`Map<K, V>`**: an ordered map, constructed `Map<K, V>()`, with a keyable
  scalar key. An index read answers `Option<V>`, `None` on a miss, and an index
  assignment inserts or updates. Entries keep insertion order, and that order is
  what serialization sees and what equality compares.

### Records

A record is a product type, a fixed set of named fields:

```text
record Card {
    number: String
    amount: Float
}
```

A record is built with a brace literal on the type's name, its fields read by
name, and it carries value semantics like every composite.

### Sums

A sum is a choice between variants, each of which may carry a payload, written
`is either`:

```text
DemoError is either BadMood(reason: String) or Empty
```

A variant is built by naming it, `BadMood("asked to fail")` or `Empty`, and
taken apart with `match`. A sum's representation carries the variant's
declaration index as its tag.

### Option and Result

`Option<T>` is `Some(T)` or `None`. `Result<T, E>` is `Ok(T)` or `Err(E)`. They
are how nullability and failure are expressed, and the checker holds a `match`
on either to both arms.

### The contract type at the boundary

A contract type is forbidden at every ABI position: a pledge signature, a vow, a
record or variant field, at any nesting depth. A signed instance is a value only
inside a pledge body, never a value that crosses the boundary, so nothing a
foreign host receives can carry an instance it cannot own. See
[Cross Contract Calls](#cross-contract-calls).

---

## Statements and Expressions

A pledge body, a clause body, and a subcontract pledge body are ordinary
imperative code. A statement is terminated by a newline at the top group depth,
so a block inside an argument list keeps its statement semantics.

### Bindings

`let` binds an immutable name, `let mut` a reassignable one:

```text
let n = 0
let mut xs = [a, b, c]
```

An unused binding is not silently accepted; the language names deferred and
unsupported constructs rather than accepting them quietly.

### Control flow

- `if` and `else`, with `else if` chaining as ordinary nested `if`s. A bare
  record literal is banned in an `if`, `while`, `for`, or `match` head so the
  brace is never ambiguous.
- `while cond { }` loops on a condition.
- `for x in list { }` walks a list.
- `break` and `continue` inside a loop.
- `return` leaves a pledge or clause with a value of its declared type.

### Match

`match` is an exhaustive expression over sums, `Option`, `Result`, and `Bool`.
It must cover every variant, and the checker rejects it otherwise. Each arm binds
the payload it matches:

```text
match r {
    Ok(v) -> { return Ok(v * 2) }
    Err(_) -> { return Err(-1) }
}
```

### Propagation

`?` propagates an error. `f(x)?` runs a fallible call, returns its `Err` from the
enclosing pledge immediately, and unwraps its `Ok` otherwise. The callee's error
type must be the enclosing return type's error type.

### Blocks as values

A block is an expression, and its value is its last expression, so a `match` arm
or an `if` branch can produce a value directly.

### Operators

The language has an eight level precedence ladder. `String + String`
concatenates. The comparison and equality operators work over the primitive
types. The logical operators `&&` and `||` short circuit: the right operand is
evaluated only when the left does not already decide the result, so a bounds
check like `i < n && xs[i] == 0` never touches `xs[i]` when `i < n` is false.

### Value semantics

Composites carry value semantics at every boundary. A `let` of a composite, an
assignment, and a composite read all deep copy, so mutation through one binding
is never visible through another. The one place a write lands in place is an
assignment target, `xs[0] = y` or `record.field = y` or `map[k] = y`, which walks
to the named slot and writes there while the right side still copies. An out of
range list index or a missing map key mid chain is a clean type error, never a
fault, and the fault checks run before the right side evaluates.

The single exception to value semantics is a signed instance, described next.

---

## Provisional Clauses

A provisional clause is Ashford's trait: a named set of clause signatures a
contract can promise to satisfy.

```text
provisional clause Comparable {
    clause compare(a: Int, b: Int) -> Int
}
```

A contract `incorporate`s a provisional clause and implements its clauses:

```text
contract StdUser {
    incorporate Comparable
    incorporate Loggable

    clause compare(a: Int, b: Int) -> Int { ... }
    clause log_line(msg: String) -> String { ... }
}
```

An incorporated signature must match its clause implementation exactly. Once
incorporated, the contract is of that provisional clause, and its pledges use the
implemented clauses like any internal method.

---

## Cross Contract Calls

A pledge body can sign another contract, fulfill its pledges, and break it, which
is how contracts compose:

```text
pledge compute(x: Int) -> Result<Int, Int> {
    let ops = MathOps.sign(epsilon: 0.5)
    let r = ops.abs(x)
    ops.break()
    match r {
        Ok(v) -> {
            let gate = IntGate.sign()
            let d = gate.double_pos(v)?
            gate.break()
            return Ok(d)
        }
        Err(_) -> { return Err(-1) }
    }
}
```

`MathOps.sign(epsilon: 0.5)` signs with a vow override, `ops.abs(x)` fulfills a
pledge through method syntax, and `ops.break()` tears it down. A cross contract
call copies the callee's result home before use, so the result outlives the
callee breaking, which is why `r` is safe to read after `ops.break()`.

A signed instance is the one value in the language with reference semantics: a
copy of the handle aliases the same instance, and equality is identity. It is
also the one value that can never cross the C boundary; only its results can. A
fulfillment issued from inside another fulfillment runs inline on the same
worker, so nested calls cannot starve the pool.

---

## Standalone Programs

Ashford is a language you can compile straight to an executable. A program
declares exactly one `Main` contract with a `run` pledge over `List<String>`
returning `Result<Int, E>`:

```text
contract Main {
    vow greeting: String = "counted"

    pledge run(args: List<String>) -> Result<Int, DemoError> {
        let mut n = 0
        for a in args {
            if a == "fail" { return Err(BadMood("asked to fail")) }
            n = n + 1
        }
        return Ok(n)
    }
}
```

`ashc build --bin` links it into a real program: the generated wrapper signs
`Main`, hands `argv` to `run` as a `List<String>`, and maps the result onto the
process exit code. `Ok(n)` becomes the exit code, and an `Err` renders itself to
stderr and exits 1. A `--bin` module still emits every contract in the file, not
`Main` alone.

---

## The Standard Library

A first standard library lives under `lib/ashstd` and is pulled in with `import`:

- **`ashstd.math`**: overflow checked arithmetic pledges and an epsilon vow.
- **`ashstd.strings`** and **`ashstd.collections`**: string and list helpers
  shaped to what the language expresses today.
- **`ashstd.errors`**: common error sums for the `E` slot.
- **`ashstd.traits`**: the provisional clauses, `Comparable`, `Loggable`, and
  `Serializable`.

A module lends another its record and sum shapes, its error sums, its provisional
clauses, and its contracts themselves. A pledge in one module can sign a contract
declared in another, fulfill its pledges, and break it.

---

## Interop and the ABI

This is the reason the language exists. A compiled `.ash.so` module carries
descriptor tables that spell every contract, every pledge and its signature, and
every vow and its default. When the runtime loads the module it registers all of
it in the iname table, and a foreign host drives a contract through a handful of
C calls with no binding generated for it.

### The iname table

The iname table is a sorted registry of every contract and pledge, keyed by a
mangled name that bakes in the contract, the pledge, a 64 bit FNV-1a hash of the
type signature, and the version. A host looks a contract up by that name, and
because the name carries the shape hash, a version mismatch is a clean error at
discovery time. The table supports lookup by exact mangled name, ordered
enumeration, and a canonical one line per entry dump. `ash_runtime_freeze` makes
the table immutable: after it, loading, registering, and binding refuse, while
signing and fulfilling continue.

The mangled name has the form
`__ash_ash_{contract}_{symbol}_{sighash}_v{n}`, and `ashc emit-header` writes the
shape hash and every mangled name as C defines so a host resolves against
generated names instead of string literals.

### Values

Every value that crosses the boundary is an `AshValue`, a tagged union whose
layout is fixed and documented. Integers, floats, strings, lists, tuples, maps,
records, sums, options, and results each have a pinned representation. A string
is a fat value, a pointer and a byte length, UTF-8, no terminator. A list, a
tuple, a record, and a map all ride one array arm with different rules.

### Ownership

The runtime owns every heap allocation that crosses the line. Arguments are deep
copied onto the instance on entry, on the caller's thread, so a host buffer
rewritten after the call changes nothing. Results are owned by the instance and
die at its break. A host never frees what Ashford returns, and Ashford never
holds a pointer into host memory after a call returns.

### The pledge frame

Every pledge crosses the boundary in one uniform frame, compiled bodies and host
bound implementations alike:

```c
AshStatus (*AshPledgeFn)(void* ctx, const AshValue* args, size_t nargs,
                         AshValue* out);
```

`ctx` is the signed instance the fulfillment runs against, and its allocation
helpers make everything the pledge builds instance owned. The runtime marshals
the frame and dispatches, and the network layer serializes the same frame.

### Futures

Fulfillment is a future. `ash_pledge_fulfill` returns a handle and
`ash_future_wait` collects the outcome exactly once; a synchronous form fuses the
two. The ABI never promises where the body runs, on the caller's thread, on a
worker pool, or across a socket, so a host that waits on a future is unaffected
when the origin of the answer changes.

### Threading

Fulfillments run on a worker pool sized by the runtime configuration. Each
instance is serialized behind one recursive mutex covering the thunk run, the
latch, and every arena allocation, so fulfillments against one instance serialize
while distinct instances run in parallel. The lock order is instance, then
runtime, then future. A break racing an in flight fulfillment resolves
deterministically: the pledge delivered before the break wins, or the break
forfeits the future to a state error, and nothing touches freed memory.

### By reference and write back

A host can pass an argument by reference and receive data back through its own
pointer. The runtime copies the value in at fulfill, the body mutates its
instance owned slot, and the write back lands in host memory only at delivery,
while the host is still blocked in the call, through a default for scalars and
strings or a caller supplied callback.

### Binding an abstract pledge

A host implements an abstract pledge by binding a function of the uniform frame
over its name. The binding is resolved into a fixed dispatch table at sign, beats
a compiled body, and satisfies the abstract pledge so the contract can sign.

### Two languages, one contract

The repository proves the claim with C and with Python. `skeleton/host.c` links
`libashrt` and speaks the ABI header. `interop/python/ashford.py` is a ctypes
binding written against the ABI reference alone, no header parsed and no stubs
generated. Both hosts sign the same contracts, override the same vows, bind the
same abstract pledges, and read the same partial surface. The contract is the
shared artifact, and every language meets at it.

---

## The Store Layer

Layer 3 backs a contract with a database. It is in progress; the surface below is
its normative design, and the parts still landing are marked.

A store backed contract declares the shape of a table as a schema, beside its
vows:

```text
contract Ledger {
    vow dsn: String = "file:ledger.db"

    schema Accounts {
        id: Int
        balance: Float
        owner: String
    }

    subcontract Transfer transactional {
        pledge debit(id: Int, amount: Float) -> Result<Unit, LedgerError>
        pledge credit(id: Int, amount: Float) -> Result<Unit, LedgerError>
    }
}
```

### Schema as vows

A `schema` names a table and its columns, each a scalar type, and it is vow
shaped: immutable, locked at sign, and validated at sign. When the contract
signs, the runtime reconciles every schema with the live database on the bound
`dsn`: an absent table is created to match, a present table is validated column
for column, and a divergent table fails the sign. The schema joins the contract's
shape hash, so a changed column fails a stale host's sign the way a changed
pledge does. A schema also stands up a record, so a row a pledge reads is an
ordinary record. Column types are the seven scalars, `Int`, `UInt`, `Float`,
`Bool`, `Byte`, `Char`, and `String`; a composite column is a type error.

### The store surface

A pledge body reads and writes through the standard library store module,
naming a schema in its own contract:

```text
pledge debit(id: Int, amount: Float) -> Result<Unit, LedgerError> {
    match Store.find(Accounts, id) {
        Err(_) -> { return Err(NoSuchAccount(id)) }
        Ok(None) -> { return Err(NoSuchAccount(id)) }
        Ok(Some(row)) -> {
            Store.update(Accounts, id, Accounts { balance: row.balance - amount })
            return Ok(Unit)
        }
    }
}
```

`Store.find`, `Store.insert`, `Store.update`, and `Store.delete` name a schema
and bind every value positionally, never by concatenation, so a value carrying
SQL is a value and nothing else. The first column of a schema is its primary key.
A backend failure that the runtime cannot complete, a lost connection, a full
disk, a refused constraint, is the status `ASH_ERR_STORE` delivered through the
fulfillment; a contract's own rules stay values in its own error type.
`Store.query` and a partial record form of `update` are still landing.

### Transactions as subcontracts

A `transactional` subcontract runs its pledges as one all or nothing episode. The
transaction opens on the first fulfillment of a pledge in the subcontract,
buffers its writes, and commits the moment the subcontract completes, its last
pledge latching `Ok`, or rolls back the instant any pledge in it returns `Err` or
the contract breaks first. Because a transaction is one episode and not a latch, a
pledge in a transactional subcontract is fulfilled at most once, and a repeat call
to a resolved one is a state error. Loose pledges outside a transactional
subcontract run in autocommit, each store operation its own implicit transaction.
The `transactional` modifier is allowed only on a subcontract in a store backed
contract.

### The lifecycle over storage

`sign` opens the connection and reconciles the schema; `break` rolls back any
open transaction, closes the connection, and reclaims the heap, in that order, so
no uncommitted write survives a break and no connection outlives its instance.

---

## The Network Layer

Layer 2 serves the same contracts over a socket. A daemon, `ashd`, loads modules,
freezes so its table is immutable, and serves their contracts on a TCP address. A
client links the same `libashrt` and calls `ash_runtime_connect`, and every
contract the daemon serves appears in the client's iname table beside the local
ones. Signing and fulfilling a remote contract is the same code as a local one,
so a host changes by a single line, a module load turned into a connect, when the
contract moves across the wire. A shared token guards the connection, and a
dropped connection surfaces as one new status, `ASH_ERR_NET`, through the wait a
host already reads. The wire is a fixed frame header and a canonical little
endian value encoding, and by reference arguments are refused across the network
rather than bent. The network layer adds no language surface; it is the same
contracts, one call wider.
