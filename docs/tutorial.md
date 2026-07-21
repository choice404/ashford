# An Ashford Tutorial

Ashford is a language for the space between languages. You write a contract
once, and any language that can call a C library can sign it, fulfill its
pledges, and read the results, with the runtime holding every allocation that
crosses the line. This tutorial walks the whole language surface and then the
part that is the point of it: driving one contract from C and from Python with
no generated bindings on either side.

The bridge, contracts served over gRPC to other processes, is a real part of
Ashford and is out of scope here on purpose. Everything below is the in
process story: the language, and two languages meeting through it in one
address space. The bridge is [docs/bridge.md](bridge.md) when you want it.

Every example in this tutorial is a real file under `skeleton/` or `lib/`, and
every command is a real `make` target. Nothing here is pseudocode.

## Before you start

You need the dusk toolchain to build the compiler, and `cc` and `clang` to
compile the emitted C and the runtime. Build everything and run the walking
skeleton:

```sh
make smoke
```

That compiles the runtime into `libashrt.so`, builds `ashc` through dusk,
compiles a contract module, and runs a C host that signs it and fulfills a
pledge end to end. If the installed dusk lags, point `DUSK` at a newer build:

```sh
make smoke DUSK=~/projects/cool-lang/target/release/dusk
```

The compiler is `ashc`, source files end in `.ash`, and a compiled module ends
in `.ash.so`. Everything lands under `target/`.

## 1. Your first contract

A contract is the unit of Ashford the way a function is the unit of most
languages. Here is the whole of `skeleton/hello.ash`:

```text
GreetError is either Empty

contract Greeter {
    vow prefix: String = "hello, "

    pledge greet(name: String) -> Result<String, GreetError> {
        return Ok(prefix + name)
    }

    pledge shout(name: String) -> Result<String, GreetError>
}
```

Three ideas are already here.

A **vow** is an immutable field. `prefix` is locked the moment the contract is
signed and never changes for the life of that signature. A vow may carry a
default, and a host may override it at sign, but nothing inside the contract can
assign to it.

A **pledge** is a callable commitment with a declared return type. `greet` has a
body and returns a `Result`. Every fallible pledge returns a `Result` or an
`Option`, never a bare value and never an exception, because errors in Ashford
are values that cross the boundary like any other.

`shout` is an **abstract pledge**: a commitment with no body. The contract
cannot be signed until a host binds an implementation for it, which is how a
foreign language plugs its own behavior into an Ashford contract. We come back
to it in the interop section.

`String + String` concatenates, and `Ok(...)` wraps a success. `GreetError is
either Empty` declares a sum type with a single variant, the error slot of the
`Result`.

## 2. The lifecycle

A contract moves through five states, and the whole runtime is built around this
progression:

```text
Unsigned -> Signed -> Fulfilled
                   -> PartiallyFulfilled
                   -> Broken
```

Signing validates the contract's shape, locks its vows, and activates it.
Fulfilling a pledge runs it and records the outcome. A pledge latches on its
first `Ok`: once fulfilled it stays fulfilled, and an `Err` before any `Ok`
latches it broken with the error kept readable. Breaking a contract tears it
down and reclaims every allocation the runtime made for it. A contract that got
partway reports exactly which pledges landed, which are pending, and which
broke, with the errors attached.

You never call these transitions from inside `.ash` code for your own contract;
a host drives them across the boundary. That is the next half of the tutorial.
First, the rest of the language.

## 3. Data: records and sums

Ashford has two ways to shape your own data.

A **record** is a product, a fixed set of named fields:

```text
DemoError is either BadMood(reason: String) or Empty
```

A **sum**, written `is either`, is a choice between variants, each of which may
carry a payload. `DemoError` above is a sum with two variants, one carrying a
`String` and one carrying nothing. You build a variant by naming it,
`BadMood("asked to fail")` or `Empty`, and you take it apart with `match`.

Two sums are built in and everywhere. `Option<T>` is `Some(T)` or `None`.
`Result<T, E>` is `Ok(T)` or `Err(E)`. They are how a pledge says "a value or
nothing" and "a value or a failure", and the type checker holds you to handling
both arms.

## 4. Inside a pledge body

A pledge body is ordinary imperative code. Here is `Main.run` from
`skeleton/main_demo.ash`, which counts its arguments:

```text
pledge run(args: List<String>) -> Result<Int, DemoError> {
    let mut n = 0
    for a in args {
        if a == "fail" {
            return Err(BadMood("asked to fail"))
        }
        n = n + 1
    }
    return Ok(n)
}
```

The pieces:

- `let` binds an immutable name, `let mut` a reassignable one. `n` is `mut`
  because the loop grows it.
- `if` and `else` are the conditional. A record literal is banned in an `if`
  head so the brace is never ambiguous, a rule worth knowing before it surprises
  you.
- `for x in list` walks a list, `while cond { }` loops on a condition, and
  `break` and `continue` do what you expect.
- `return` leaves the pledge with a value of its declared type.

The two operators that make Ashford feel like Ashford are `match` and `?`.

**`match`** is an exhaustive expression. It must cover every variant, and the
checker rejects it if it does not:

```text
match r {
    Ok(v) -> {
        let gate = IntGate.sign()
        let d = gate.double_pos(v)?
        gate.break()
        return Ok(d)
    }
    Err(_) -> {
        return Err(-1)
    }
}
```

Each arm binds the payload it matches, `v` for the `Ok`, `_` to ignore the
`Err`. A `match` is an expression, so it can produce a value directly, and a
block is a value too: the last expression in a `{ }` is what it evaluates to.

**`?`** is propagation. `gate.double_pos(v)?` runs the pledge, and if it returns
`Err`, the whole enclosing pledge returns that `Err` immediately; on `Ok` it
unwraps to the inner value. The error types have to agree: `?` only works when
the callee's error type is the enclosing pledge's error type, which is why the
example around it hand translates errors when they do not match.

One safety rule to know: the logical operators short circuit, so a bounds check
like `i < n && xs[i] == 0` never evaluates `xs[i]` when `i < n` is false, and an
out of range index returns a type error rather than faulting.

## 5. Clauses and traits

A **clause** is an internal method. It never leaves its contract, has no place
in the lifecycle, and exists so pledges can share logic. From
`skeleton/std_user.ash`:

```text
clause compare(a: Int, b: Int) -> Int {
    if a < b { return -1 }
    if a > b { return 1 }
    return 0
}
```

A **provisional clause** is Ashford's trait: a named set of clause signatures a
contract can promise to satisfy. A contract `incorporate`s one and implements
its clauses:

```text
contract StdUser {
    incorporate Comparable
    incorporate Loggable

    clause compare(a: Int, b: Int) -> Int { ... }
    clause log_line(msg: String) -> String { ... }
}
```

Now `StdUser` is `Comparable`, and a pledge can sort through its own comparator:

```text
pledge sort3(a: Int, b: Int, c: Int) -> Result<List<Int>, Int> {
    let mut xs = [a, b, c]
    if compare(xs[0], xs[1]) > 0 {
        let t = xs[0]
        xs[0] = xs[1]
        xs[1] = t
    }
    // two more passes
    return Ok(xs)
}
```

`xs[0] = xs[1]` is an index assignment, and it writes in place. Composites carry
value semantics everywhere else: a `let` of a list, an assignment, and a
composite read all copy, so mutating one binding is never visible through
another. The one place a write lands in place is an assignment target like
`xs[0]`.

## 6. Subcontracts and the fulfillment policy

A **subcontract** groups pledges, and the **requirements** block writes the
contract's success and failure as boolean logic over those groups. This is the
heart of the language, and `skeleton/payment.ash` shows it whole:

```text
contract PaymentService {
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

Read the policy out loud. The contract is **fulfilled** when Validation and
Processing and notify_user all land. It is **partially fulfilled** when either
Validation or Processing lands. It **breaks** when none of the three has landed
and something has failed. A subcontract counts as satisfied when its own pledges
have latched.

The runtime evaluates this policy after every outcome, in the priority break,
then fulfill, then partial, and moves the contract's state automatically. When
the break line holds, the contract breaks itself and keeps its heap alive so the
error payloads stay readable until an explicit break reclaims them. The checker
proves at compile time that no state satisfies both fulfill and break at once,
so a policy that contradicts itself never ships.

The payoff is the partial surface: at any moment a host can ask which
subcontracts and pledges are fulfilled, which are pending, and which broke, and
read the latched errors by name. We watch that happen from Python below.

## 7. Calling a contract from a contract

A pledge body can sign another contract, fulfill it, and break it, which is how
Ashford composes. From `StdUser.compute`:

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

`MathOps.sign(epsilon: 0.5)` signs another contract with a vow override,
`ops.abs(x)` fulfills a pledge on it through method syntax, and `ops.break()`
tears it down. Notice the instance breaks before `r` is read: that is safe
because a cross contract call copies the result home before the callee dies. A
signed instance is the one value in the language with reference semantics,
copies of the handle alias the same instance and equality is identity, and it is
also the one value that can never cross the C boundary, only its results can.

## 8. A standalone program

Everything so far is a library other languages drive. Ashford is also a language
you can compile straight to an executable. Declare one `Main` contract with a
`run` pledge over `List<String>` returning `Result<Int, E>`:

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

`build --bin` links it into a real program. `Ok(n)` becomes the exit code, and
an `Err` renders itself to stderr and exits 1:

```sh
make test-bin
./target/ashc-out/main_demo a b c ; echo $?   # counts its args, exits 3
./target/ashc-out/main_demo fail              # takes the Err path, exits 1
```

## 9. The standard library

A first standard library lives under `lib/ashstd`: `math` with overflow checked
pledges, `strings` and `collections` shaped to what the language can express,
common `errors`, and the `traits` that hold the provisional clauses like
`Comparable` and `Loggable`. You pull a module in with `import`:

```text
import ashstd.math
import ashstd.collections
import ashstd.traits
import ashstd.errors
```

Imports resolve beside your root file and then under `ASH_HOME`, every file
parses into one shared tree, and cycles and duplicate imports are named errors.
A module lends another its record and sum shapes, its error sums for the `E`
slot, its provisional clauses, and its contracts themselves. `make test-std`
builds a module that imports four ashstd modules and drives them from C.

That is the language. Now the reason it exists.

## 10. The boundary

Here is what makes Ashford an interop language and not just another small
language.

A compiled `.ash.so` module carries **descriptors**, tables that spell every
contract, every pledge and its signature, and every vow and its default. When
the runtime loads the module, it registers all of that in the **iname table**, a
sorted registry keyed by a **mangled name** that bakes in the contract, the
pledge, a 64 bit hash of the type signature, and a version. A host looks a
contract up by that name, and because the name carries the shape hash, a version
mismatch is a clean error at discovery time instead of silent corruption at call
time.

Every value that crosses the boundary is an `AshValue`, a small tagged union
whose layout is fixed and documented in [docs/abi.md](abi.md). Integers, floats,
strings, lists, tuples, records, sums, options, and results all have a pinned
representation. The one iron rule is ownership: the runtime owns every heap
allocation that crosses the line. Arguments are copied onto the instance on the
way in, results are owned by the instance and die when it breaks, and a value
passed by reference is copied in on entry and written back on return, never held
by Ashford past the call. A host never has to free what Ashford gave it, and
Ashford never holds a pointer into host memory after a call returns.

A host drives a contract through a handful of runtime calls: initialize the
runtime, load a module, sign a contract, fulfill its pledges, read the partial
surface, break it. Those calls are the same in every language, because they are
just C. Let us make two languages speak them.

## 11. Interop with C

`skeleton/host.c` is a foreign program. It knows nothing about `ashc`; it links
`libashrt` and speaks the ABI header `ash/ash.h`. The shape of a host is always
the same five moves.

**Initialize and load.**

```c
AshRuntime* rt = NULL;
ash_runtime_init(NULL, &rt);
ash_module_load(rt, "target/ashc-out/libhello.ash.so");
```

**Sign.** A host builds an argument as an `AshValue` and hands the runtime a
value it owns; the runtime never keeps it:

```c
AshContract* c = NULL;
ash_contract_sign(rt, "Greeter", NULL, 0, 0, &c);
```

Pass a vow override to change a locked field at sign, exactly what
`MathOps.sign(epsilon: 0.5)` did from inside the language:

```c
AshVowBinding prefix;
prefix.name = "prefix";
prefix.value = str_arg("hey, ");
ash_contract_sign(rt, "Greeter", &prefix, 1, 0, &c3);
```

**Fulfill.** A pledge runs through a future: `ash_pledge_fulfill` returns a
handle and `ash_future_wait` collects the outcome exactly once. The synchronous
form fuses the two:

```c
AshValue out;
AshValue name = str_arg("world");
ash_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &out);
// out is Ok("hello, world")
```

The future is not decoration. A wait blocks until the outcome exists, and the
runtime never promised where the body runs, on the caller's thread, on a worker
pool, or, in the network layer, across a socket. Writing to the future is how
the host stays the same when the answer's origin changes.

**Bind an abstract pledge.** `Greeter.shout` had no body. The host supplies one,
written in the same uniform thunk frame every compiled pledge uses, and only
then can the contract sign:

```c
static AshStatus host_shout(void* ctx, const AshValue* args, size_t nargs,
                            AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    // uppercase args[0], write the result into *out as Ok(...)
    return ASH_OK;
}

ash_pledge_bind(rt, "Greeter.shout", host_shout);
```

**By reference and write back.** A pledge can take an argument by reference and
hand data back through the host's own pointer. The host owns the storage, the
runtime copies the value in at fulfill, the body mutates its instance owned slot,
and the write back lands in host memory only at delivery, while the host is
still blocked in the call:

```c
AshString by_ref;
by_ref.ptr = (uint8_t*)"whisper";
by_ref.len = 7;
AshRef ref;
memset(&ref, 0, sizeof(ref));
ref.host_ptr = &by_ref;
ref.ty = ASH_TY_STRING;
ash_pledge_fulfill_sync(c, "shout", NULL, 0, &ref, 1, &out);
// by_ref now holds "WHISPER", written back into host storage
```

**Break.** Tearing the contract down reclaims everything, and every later
fulfillment on it is a clean state error:

```c
ash_contract_break(c);
```

Run the whole C walk, sign, bind, fulfill, by reference write back, futures,
discovery, and break, under the address and leak sanitizers:

```sh
make smoke-asan
```

**Resolving names without hardcoding them.** A host can spell mangled names by
hand, but `ashc` will generate the header for you:

```sh
target/dusk-out/ashc emit-header skeleton/hello.ash
```

That writes the shape hash and every mangled pledge name as C defines, read from
the same tables the module emitter hashes, so a host resolves and signs against
generated names instead of string literals.

## 12. Interop with Python

The claim Ashford makes is that any language which can load a C library can
speak a contract, with no code generated for the binding. The repository proves
it with Python and nothing but `ctypes`. `interop/python/ashford.py` is written
against [docs/abi.md](abi.md) alone, no header parsed and no stubs generated, and
`interop/python/demo_payment.py` drives the payment contract with it.

The same five moves, now in Python:

```python
from ashford import Runtime, Ok, Err

with Runtime(OUT / "libashrt.so") as rt:
    rt.load(OUT / "libpayment.ash.so")
    rt.bind("PaymentService.charge", charge)   # a Python function is the pledge
    rt.freeze()

    c1 = rt.sign("PaymentService", vows={"currency": "EUR"})
    assert c1.vow("currency") == "EUR"
```

The abstract `charge` pledge is implemented by an ordinary Python function,
called back from the runtime when a pledge fulfills:

```python
def charge(inst, args):
    card, amount = args
    if amount > 0:
        return Ok(True)
    return Err(41)
```

Now walk the fulfillment policy from Python and watch the partial surface move.
Validation lands, and the contract goes partial:

```python
c1.fulfill_sync("validate_card", "4111 1111")
c1.fulfill("validate_amount", 25.0).wait()

p = c1.partial()
# p.fulfilled == ["Validation"], p.pending == ["Processing", "notify_user"]
```

Drive `charge` and `notify_user`, and the policy declares the contract
fulfilled. Or send a bad amount, and the break line fires by itself:

```python
c2 = rt.sign("PaymentService")
out = c2.fulfill_sync("charge", "4111 1111", -2.0)
# out == Err(41), and c2 broke automatically
p = c2.partial()
# p.broken == ["Processing"], p.errors == [("charge", 41)]
```

By reference works from Python too, the write back landing back in a host owned
object:

```python
g = rt.sign("Greeter")
ref = StringRef("whisper")
out = g.fulfill_sync("shout", refs=[ref])
# out == Ok("WHISPER"), and ref.value == "WHISPER"
```

Run it:

```sh
make test-python
```

Read the two demos side by side and the point lands. The C host and the Python
host drive the same contracts, sign the same vows, bind the same abstract
pledges, and read the same partial surface. Neither generated a line of binding
code. That is the whole thesis of the language: the contract is the shared
artifact, and every language meets at it.

## Where to go next

- [docs/grammar.md](grammar.md) is the normative language grammar, every rule
  this tutorial showed and the ones it did not.
- [docs/abi.md](abi.md) is the ABI a foreign host compiles against, the value
  layouts, the thunk frame, ownership, mangling, and the hash.
- [docs/bridge.md](bridge.md) is the layer this tutorial skipped: the same
  contracts served over gRPC, the wire surface emitted by the compiler and
  driven from any language with stock tooling.

The fastest way to learn the rest is to read `skeleton/` beside `docs/abi.md`
and change something. Every file there is compiled and driven by a gate, so a
broken change fails loudly and a working one runs.
