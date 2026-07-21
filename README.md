# ashford

Ashford is a statically typed, contract oriented language that sits between other languages. A program declares contracts, other languages sign and fulfill them across the C ABI, and the runtime enforces the terms. No protocol definitions, no serialization boilerplate, no hand written FFI bindings. Any language that can call into a shared C library can talk through an Ashford contract. It also works as a standalone language for writing contract libraries and contract driven programs.

The compiler for Ashford is `ashc`. Source files for Ashford end in `.ash`. The compiler itself is written in a new systems programming language I made called [dusk](https://dusk-lang.org), which emits C that links against a small C runtime. I love programming language design and compilers, and Ashford is the interop half of that obsession: dusk answers how a language should own memory, Ashford answers how two languages should trust each other.

## Core ideas

- Everything is a contract. The contract is the unit of the language, not functions.
- Nothing is implicit. Ownership is always clear, visibility is always stated, and every fallible pledge returns a `Result` or an `Option`.
- Errors are values. No exceptions, and nothing throws across a language boundary.
- Interop is the primary goal of the language. The runtime owns every heap allocation that crosses a boundary, data passes by value in both directions, and a reference is copied on entry and written back on return, never held by Ashford.

## A taste of the language

```text
provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

contract PaymentService {
    incorporate Loggable

    vow currency: String = "USD"

    subcontract Validation {
        pledge validate_card(card: Card) -> Result<Bool, ValidationError>
        pledge validate_amount(amount: Float) -> Result<Bool, ValidationError>
    }

    subcontract Processing {
        pledge charge(card: Card, amount: Float) -> Result<Receipt, PaymentError>
    }

    pledge notify_user(receipt: Receipt) -> Result<Unit, NotifyError>

    requirements {
        fulfill: Validation && Processing && notify_user
        partial: Validation || Processing
        break: !Validation && !Processing && !notify_user
    }

    clause log(message: String) -> Result<Unit, LogError> {
        // log implementation
    }
}
```

A `vow` is an immutable field locked when the contract is signed. A `pledge` is a callable commitment, first class element, with a declared return type. A `clause` is an internal method that never leaves its contract. A `subcontract` groups pledges so a contract can be partially fulfilled, and the `requirements` block writes the fulfillment policy as boolean logic over those groups.

## The lifecycle

A contract moves through five states.

```text
Unsigned -> Signed -> Fulfilled
                   -> PartiallyFulfilled
                   -> Broken
```

`sign()` validates the contract shape, locks the vows, and activates it. Fulfilling a pledge runs it and records the outcome. `break()` tears the contract down and frees every allocation the runtime owns for it. A partially fulfilled contract reports exactly which pledges landed, which are pending, and which broke, with the errors attached.

## How the interop works

The runtime is a shared C library. A compiled `.ash` module carries its contract descriptors, and the runtime registers them in the iname table, a queryable registry of every contract and pledge signature with the language of origin, the type signature hash, and the version baked into each mangled name. A version mismatch is a descriptive error at link time instead of silent corruption at run time. A C program, or anything that can load a C library, looks up a contract, signs it, fulfills pledges, and reads results, all through a handful of runtime calls.

## How to use

New to Ashford, start with [docs/tutorial.md](docs/tutorial.md): it walks the whole language and then drives one contract from C and from Python with no generated bindings. [spec.md](spec.md) is the full language reference, and [CHANGELOG.md](CHANGELOG.md) carries the history version by version.

The toolchain is early, but the whole pipeline already runs: `ashc` compiles a module, the runtime loads it, and a C host signs and fulfills a contract end to end.

```sh
# build everything and run the end to end check
make smoke

# the same check under the address and leak sanitizers
make smoke-asan
```

`make smoke` builds four things in order: the runtime shared library `libashrt.so` from `runtime/`, the `ashc` binary from `compiler/` through the dusk toolchain, the compiled contract module `libhello.ash.so` from `skeleton/hello.ash`, and the C host from `skeleton/host.c`. Then it runs the host, which loads the module, signs `Greeter`, fulfills `greet("world")`, checks the result is `Ok("hello, world")`, exercises the error paths, and breaks the contract. Everything lands under `target/`.

`ashc` builds modules, standalone executables, and the C header a foreign host compiles against:

```sh
target/dusk-out/ashc version
target/dusk-out/ashc build skeleton/hello.ash
target/dusk-out/ashc build --bin skeleton/main_demo.ash
target/dusk-out/ashc emit-header skeleton/hello.ash
target/dusk-out/ashc emit-proto skeleton/payment.ash
```

`build` reads the source, emits the module C into `target/ashc-out/`, and links it with cc into a loadable `.ash.so`. Run it from the repository root, since it hands cc the relative include path, or set `ASH_ROOT` to a checkout and run `ashc` from anywhere: `ASH_ROOT` points cc at the runtime headers and `libashrt`, and `ASH_HOME` at the standard library for imports.

Ashford is a standalone language too. A program declares one `Main` contract with a `run(args: List<String>) -> Result<Int, E>` pledge, and `build --bin` links it into a real executable: `Ok(n)` becomes the exit code, `Err` prints itself to stderr and exits 1.

```sh
make test-bin
./target/ashc-out/main_demo a b c; echo $?   # counts its args, exits 3
./target/ashc-out/main_demo fail             # takes the Err path, exits 1
```

`emit-header` writes `target/ashc-out/hello.ash.h`, which spells the shape hash and every mangled pledge name as defines, so a C host resolves and signs against generated names instead of hardcoded strings. `make test-header` pins the header against its golden and compiles a host with it.

`emit-proto` writes the contract's gRPC surface: `target/ashc-out/payment.proto`, where every pledge is its own typed rpc and signing is a stream whose lifetime is the instance's, and the session wrappers protoc cannot write, `payment_session.go` for a Go module and `payment_session.ts` for a Node host over `@grpc/proto-loader`, no protoc step and no native code, the shape an editor extension ships. A consumer in any gRPC language builds against the `.proto` with stock tooling; `make test-proto` pins the emitted files against their goldens, and `make test-grpc-go` and `make test-grpc-node` each drive the whole payment lifecycle from a client built from nothing but the emitted artifacts.

The Makefile finds the dusk compiler as `dusk` on your path. A dusk older than 1.5 predates the two parameter `std.map` the compiler is written against; point `DUSK` at a current build when the installed one lags.

```sh
make smoke DUSK=~/projects/cool-lang/target/release/dusk
```

The runtime binds from anything that can load a C library, and the repository proves it with Python. `interop/python/ashford.py` is a ctypes binding written against [docs/abi.md](docs/abi.md) alone, no header parsed, no code generated, and `interop/python/demo_payment.py` walks the payment contract from Python: it binds a Python function over the abstract `charge` pledge, signs with a vow override, drives the partial, fulfilled, and broken paths, and takes a by-reference argument back through the write back.

```sh
# the payment walk from Python
make test-python
```

The same contracts run across processes, and the wire is gRPC. Ashford does not carry a transport of its own: the compiler emits the wire surface and the whole gRPC ecosystem, every language, every load balancer, every proxy, is the transport. A server is ordinary host code holding the runtime through the C ABI and serving the emitted surface; a client in any gRPC language builds from the emitted artifacts with stock tooling and never learns Ashford exists. Signing is a stream whose lifetime is the instance's, a pledge's `Err` crosses as a value on an OK rpc while an Ashford status crosses as a gRPC code, and the shape hash pins both ends to one contract. [docs/bridge.md](docs/bridge.md) is the normative surface.

```sh
# the payment contract served over gRPC, driven from Python
make test-grpc-bridge

# the same lifecycle driven from Go and from Node, built from emitted artifacts
make test-grpc-go
make test-grpc-node
```

A session survives more than its connection. A server started with a park store writes an instance down when its stream ends, the vows, the latches, the error payloads, and the transactional fates in one row, and a `Resume` call stands it back up under the token the signature carried, one shot. The gate kills the server between the park and the resume and finishes the contract on a fresh server holding the same store, which is the partition and replica story in one run:

```sh
# park, kill the server, resume on a fresh one, finish the contract
make test-grpc-resume
```

The same contracts run backed by a database. A contract declares the table shape it needs as a `schema`, a vow like any other that locks at sign, binds a live database through a `dsn` vow, and its pledges read and write rows through the store standard library. A group of writes that must all land or all vanish is a `transactional` subcontract, the unit the language already had for all-or-nothing, so a transfer is a modifier and not a new idea. The database sits behind the contract, not in front of the host: the store is invisible across the C ABI, so a host signs, fulfills, and breaks a store-backed contract with the same calls it always used, and the storage never shows through. The reference backend is SQLite, vendored as the amalgamation and compiled into `libashrt`, so the store gates are hermetic and need no server. [docs/database.md](docs/database.md) is the normative store design.

```sh
# the Ledger skeleton over a temp SQLite file, from C
make test-store

# the transactional Transfer subcontract, commit and rollback
make test-store-txn

# the same ledger walk from Python, sign, transfer, rollback, over ctypes
make test-store-python
```

`make test-store` signs the `Ledger` against a temp file, reconciles the `Accounts` schema into it, and drives the loose store pledges: `open` writes a row, `balance` reads one back, and `set_balance` rewrites one, each a `Result` whose `Err` is the ledger's own business and never a store status. `make test-store-txn` drives the transactional `Transfer`, a good transfer that commits both writes and a bad one that rolls the whole episode back, reopening the file each time so the file is the witness. `make test-store-python` runs `interop/python/demo_ledger.py`, the store twin of the payment demos, which asserts the same outcomes from Python that the C hosts assert, proving the store stays invisible in the second language too.

## Status

The core language runs end to end. `ashc` carries the whole pipeline, lexer through type checker through codegen, over the full grammar in [docs/grammar.md](docs/grammar.md): contracts with vows, pledges, clauses, subcontracts, and requirements policies, records and sums, match, loops, propagation, imports with package visibility, and a first standard library under `lib/ashstd`. The C runtime enforces the lifecycle with per pledge latching, runs fulfillments on a worker pool, owns every allocation an instance makes, and reclaims it all at break. Contracts are discovered through the iname table by mangled name and shape hash, a C host drives everything through [docs/abi.md](docs/abi.md), Python does the same through ctypes with no C written, and `build --bin` links a standalone executable. Contracts run backed by a database: a `schema` is a vow, a `transactional` subcontract is a transaction, and a SQLite backend vendored into the runtime persists the rows, the store invisible across the boundary. An instance's durable state parks into a store row and resumes in another process, from the C surface and from inside the language alike. A golden and compile fail suite, sanitizer gates, a determinism gate, thread sanitizer gates over the pool, and store gates that sign a contract against a real SQLite file hold the line on every build.

The cross process story is the bridge. `ashc emit-proto` writes the gRPC surface and its session wrappers, a reference server holds the runtime over the C ABI and serves it, and clients in Python, Go, and Node drive the whole lifecycle from the emitted artifacts alone. The session is a stream, the park store makes it survive the stream, the server, and the process, and the gates prove each claim including a server killed between a park and a resume. Ashford keeps no wire of its own: in one process the languages share memory through `libashrt`, and across processes they share gRPC, which every language already speaks.

## Requirements

- The dusk toolchain, 1.5.3 or newer, to build `ashc`.
- cc and clang, to compile the emitted C and the runtime.
- make.

## License

TBD.
