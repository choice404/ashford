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
```

`build` reads the source, emits the module C into `target/ashc-out/`, and links it with cc into a loadable `.ash.so`. Run it from the repository root, since it hands cc the relative include path, or set `ASH_ROOT` to a checkout and run `ashc` from anywhere: `ASH_ROOT` points cc at the runtime headers and `libashrt`, and `ASH_HOME` at the standard library for imports.

Ashford is a standalone language too. A program declares one `Main` contract with a `run(args: List<String>) -> Result<Int, E>` pledge, and `build --bin` links it into a real executable: `Ok(n)` becomes the exit code, `Err` prints itself to stderr and exits 1.

```sh
make test-bin
./target/ashc-out/main_demo a b c; echo $?   # counts its args, exits 3
./target/ashc-out/main_demo fail             # takes the Err path, exits 1
```

`emit-header` writes `target/ashc-out/hello.ash.h`, which spells the shape hash and every mangled pledge name as defines, so a C host resolves and signs against generated names instead of hardcoded strings. `make test-header` pins the header against its golden and compiles a host with it.

The Makefile finds the dusk compiler as `dusk` on your path. A dusk older than 0.6.0 predates `std.os` and cannot build `ashc`; point `DUSK` at a newer build when the installed one lags.

```sh
make smoke DUSK=~/projects/cool-lang/target/release/dusk
```

The runtime binds from anything that can load a C library, and the repository proves it with Python. `interop/python/ashford.py` is a ctypes binding written against [docs/abi.md](docs/abi.md) alone, no header parsed, no code generated, and `interop/python/demo_payment.py` walks the payment contract from Python: it binds a Python function over the abstract `charge` pledge, signs with a vow override, drives the partial, fulfilled, and broken paths, and takes a by-reference argument back through the write back.

```sh
# the payment walk from Python
make test-python
```

The same contracts run across a network. `ashd` is a small daemon over the runtime that loads modules, freezes so its table is immutable, and serves their contracts on a TCP address. A client links the same `libashrt` and calls `ash_runtime_connect`, and every contract the daemon serves lands in the client's iname table beside the local ones. Signing and fulfilling a remote contract is the same code as a local one, so a host changes by a single line, a module load turned into a connect, when the contract moves across the wire. A shared token read from a file guards the daemon and the client presents it at the handshake; [docs/network.md](docs/network.md) is the normative wire.

```sh
# a C client drives a remote Greeter over loopback, token and refusal paths
make test-net

# the Python payment walk, run in process and over the wire side by side
make test-net-python
```

`make test-net-python` stands up two daemons on loopback, one under a token and one without, and runs `interop/python/demo_remote.py`, the remote twin of the local payment demo. It drives the same sign, fulfill, partial, and break sequence once from a module loaded in process and once over a connection, proves the two agree outcome for outcome, then walks the token matrix and kills the daemon mid fulfillment to watch the network's one new failure reach an in flight wait.

Those same contracts also weave into a mesh. Serving is a library call, `ash_runtime_serve`, the far side of connect lifted out of `ashd` into `libashrt`, so a node is an ordinary runtime that serves the contracts it holds and connects to the peers whose contracts it wants at the same time. A node loads and binds, serves, and then opens an edge to each peer, and a lookup, a sign, and a fulfill read one table without caring which peer, or the node itself, owns the name. The two directions of a conversation are two one directional edges, each a plain connection, so two nodes that each serve and connect form a full duplex pair, and three form a mesh, every sign one hop to the owner with no relay and no routing. `ashd` does not go away; it becomes the thin case, a node that only serves. `ash_runtime_connect_all` wires a node to a list of peer addresses in one call, still explicit addresses and still no registry. Because serving is a library call the provider side is any language: a Python host binds a Python function over an abstract pledge and serves it, a C peer connects and fulfills it, and the result the C peer reads was computed by Python, live, in Python's process. Each edge carries its own token, presented at the handshake and compared in constant time, the Layer 2 posture per edge. [docs/mesh.md](docs/mesh.md) is the normative design and wire.

```sh
# a plain C host stands up a server, a client drives it, under ASan
make test-mesh-serve

# two symmetric nodes, each serving and connecting, both directions asserted
make test-mesh-pair

# a Python provider serves a bound Python pledge, a C consumer fulfills it
make test-mesh-python

# three nodes under a fulfillment storm, a per edge kill, memory flat
make test-mesh-stress
```

`make test-mesh-pair` brings up node A serving Greeter and node B serving PaymentService, opens the two edges after both serve, and drives both directions at once in a loop, each thread signing, fulfilling, checking, and breaking every pass. `make test-mesh-python` stands up a Python node that binds a Python `charge` over the abstract pledge and serves it, connects a C client, and asserts the value the client reads equals the same computation run in process. `make test-mesh-stress` wires three nodes into one mesh, storms every edge with concurrent fulfillments and demands each resolves to the right owner, then drops a node mid mesh to watch its peers land `ASH_ERR_NET` on exactly the dead edge's waits and stand otherwise, memory flat across the churn.

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

The core language runs end to end. `ashc` carries the whole pipeline, lexer through type checker through codegen, over the full grammar in [docs/grammar.md](docs/grammar.md): contracts with vows, pledges, clauses, subcontracts, and requirements policies, records and sums, match, loops, propagation, imports with package visibility, and a first standard library under `lib/ashstd`. The C runtime enforces the lifecycle with per pledge latching, runs fulfillments on a worker pool, owns every allocation an instance makes, and reclaims it all at break. Contracts are discovered through the iname table by mangled name and shape hash, a C host drives everything through [docs/abi.md](docs/abi.md), Python does the same through ctypes with no C written, and `build --bin` links a standalone executable. An `ashd` daemon serves those same contracts over TCP, and a client, C or Python, connects with `ash_runtime_connect` and signs, fulfills, and breaks across the wire with its host code unchanged, a shared token guarding the connection and a dropped connection surfacing as one new status through the wait a host already reads. Contracts run backed by a database too: a `schema` is a vow, a `transactional` subcontract is a transaction, and a SQLite backend vendored into the runtime persists the rows, the store invisible across the boundary. A golden and compile fail suite, sanitizer gates, a determinism gate, thread sanitizer gates over both the pool and the socket, network gates that stand up a real daemon, and store gates that sign a contract against a real SQLite file hold the line on every build.

Layer 1 is complete, from the language surface through the memory model, threading, discovery, and a second language over the boundary; Layer 2, the network runtime, runs; Layer 3, the store runtime, runs, contracts backed by a database with schemas as vows and transactions as subcontracts, driven from C and from Python alike; and Layer 4, the mesh, runs. Serving is a library call, so a node serves the contracts it holds and connects to the peers whose contracts it wants at once, symmetric halves of one process rather than a daemon and a client. A node's provider side is any language that reaches the ABI, so a Python node and a C node exchange results through one contract across the wire, and three nodes wire by address into one mesh, every sign one hop to the owner with no relay. Mesh gates stand up a plain C server, a symmetric pair driving both directions at once, a Python provider a C consumer reads, and a three node mesh under a fulfillment storm and a per edge kill, beside the network and store gates on every build.

## Requirements

- The dusk toolchain, 0.6.0 or newer, to build `ashc`.
- cc and clang, to compile the emitted C and the runtime.
- make.

## License

TBD.
