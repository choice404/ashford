# Changelog

Every released version of Ashford, newest first. A commit message carries the
short bulleted shape of a change; this file carries the whole of it, the design
notes and the reasons a bullet has no room for. Versions are the `v` tags on the
history, one per milestone.

## [v0.2.3] the all or nothing

The store layer's headline. A transactional subcontract runs its pledges as one
episode that commits or vanishes whole, the language's all or nothing written on
the grouping it already had.

- add the transactional modifier: a lexer keyword, a subcontract flag the parser
  records after the name and before the body, and a type error when it is written
  on a subcontract of a contract that declares no schema and so has no database to
  be durable against
- carry the flag as a byte array parallel to the subs table, appended at the tail
  of the descriptor so every earlier descriptor stays byte identical and the shape
  hash never moves
- open the episode lazily on the first fulfillment of a transactional pledge,
  commit the instant the subcontract completes, and roll back on the first Err, on
  a backend failure, or on a break before the commit
- key the commit on the same subcontract fulfilled predicate the requirements
  evaluator reads, never a parallel counter, so the commit point cannot drift from
  the subcontract's own fulfillment under partial fulfillment
- refuse a pledge whose episode already resolved with ASH_ERR_STATE through the
  wait, since a committed transaction is closed and there is nothing left to join;
  loose pledges stay autocommit, untouched, and the connection stays single
  threaded under the instance lock with no new lock
- give the Ledger a transactional Transfer subcontract and add make test-store-txn:
  a good transfer commits both writes, a failed one rolls the debit back byte
  identical, a resolved pledge re-call is a state error, and a break mid
  transaction leaves no write durable, ASan, LSan, and TSan clean
- teach the parse dump to render a schema member, closing a crash where ashc parse
  walked a schema as a requirements block and ran off the end

### Notes

The episode state is one byte per subcontract on the instance, beside the pledge
latches and under the same lock, moving from none to open on the lazy begin to done
on the one commit or rollback. Because the commit test is the evaluator's own
predicate, a transaction that spans debit and credit commits on whichever pledge
completes the pair, order independent and independent of the overall contract
state. One SQLite connection holds one transaction, so two transactional
subcontracts open at once on one instance is not attempted in v1: while one is
open, a pledge of another is refused ASH_ERR_STORE and stays unopened until the
first resolves, a sound consequence of the single connection model rather than a
corruption. The break order from the store layer's first milestone, rollback then
close then reclaim, already covers a teardown mid transaction, so nothing durable
survives a broken contract.

## [v0.2.2] the schema takes the sign

The first ashc change since Layer 1. A contract can now declare the shape of a
table it operates against, sign against a live database, and read and write rows
through a small store surface, all with the schema locked and checked the way a
vow and a shape hash are.

- add the schema block to ashc: a lexer keyword, a parser node, a row record the
  schema stands up so a pledge reads a row as an ordinary record, the seven
  scalar column rule as a named type error, and the schema folded into the
  contract's shape hash so a changed column fails the sign like a changed pledge
- lower the store surface: Store.find, insert, update, and delete name a schema
  in the same contract and lower onto runtime primitives that bind every value
  positionally and land an Ok result the instance owns
- open and reconcile at sign: a store backed contract binds its dsn vow, opens
  the connection, and reconciles every schema against the live database, create
  if absent and validate column for column if present, ASH_ERR_TYPE on
  divergence and ASH_ERR_UNBOUND with no dsn; break rolls back, closes, then
  reclaims the heap, in that order
- emit the schema descriptor as a pure tail on the contract descriptor, one cols
  array and one schemas array, so every pre store module reads as not store
  backed and nothing else in the table moves
- add the Ledger skeleton and make test-store: sign against a temp file, write a
  row, read it back, update it, prove a missing account is the contract's own
  error and an injection string is a value the table survives, both refusal
  signs, ASan and LSan clean
- fold the store driver and its sqlite into every gate that links the runtime,
  since the runtime now speaks to the store

### Notes

A schema stands up a real record, so a row a pledge reads is an ordinary record
and field access, record literals, and match all reuse the machinery already in
the language. The cost is that a schema name shares the global type namespace, so
a clash reads as a duplicate. `Store` is a compiler namespace that turns magic
only when nothing local or a contract owns the name, so a real contract named
Store keeps its sign path. The first column of a schema is its primary key, the
key find, update, and delete write against. A backend failure is the
`ASH_ERR_STORE` status delivered through the fulfillment, not an `Err` value; the
`Err(StoreError)` arm exists so the surface types and matches exhaustively, and
the primitive builds only the `Ok` arm.

Sign opens the connection on the instance before it is published to the runtime,
so no lock is held across the open and the reconcile, and a sign that fails to
open or reconcile never leaves a half open connection behind. Break holds the
instance lock every fulfillment holds, so the rollback and close never race a
store operation.

Left for the milestones above this one: `Store.query`, transactional
subcontracts, and the partial record form of `update` are S2, and the schema
descriptor structs are documented in the ABI reference at the layer's closing
docs pass. The parse and lex dumps do not yet render a schema member, so nothing
regresses but `ashc parse` omits it from its tree.

## [v0.2.1] the store surface

The first executable piece of Layer 3, and it holds no contract yet. Layer 3
starts where the wire layer started, as a library proven on its own before
anything above it can hide a bug behind it.

- add the `AshStore` vtable in `runtime/include/ash/ash_store.h`: open a
  connection, exec a statement, exec with a bound parameter frame, query rows
  back, and begin, commit, and rollback a transaction, with an `AshStoreAlloc`
  hook so the driver builds rows through a caller supplied allocator and never
  names an `AshContract`
- write the SQLite driver in `runtime/src/store.c`, the one translation unit
  that calls `sqlite3_*`: the scalar type map of `docs/database.md`, Int and
  UInt as one 64 bit integer with the UInt riding by its bit pattern, Float as
  REAL, Bool and Byte and Char as integers of their width, and String as raw
  bytes bound and read as a blob into a column declared TEXT, no UTF-8 check
- bind every parameter positionally through SQLite, never by concatenation, so
  a value carrying SQL is a value and nothing else; a composite parameter or
  column has no place in a flat row and is `ASH_ERR_TYPE` before a byte moves
- keep the driver owning no result heap: a query counts its rows, resets, and
  reads them into one backbone sized once from the hook, the same discipline
  the wire decoder kept over its owner, so there is nothing to leak and one
  break reclaims the lot
- map failure honestly, a backend that fails at what it was told is
  `ASH_ERR_STORE`, an allocation the hook refuses is `ASH_ERR_OOM`, and a
  contract's own rules are never folded into either
- vendor the SQLite amalgamation 3.46.1 under `runtime/third_party`, compiled
  into `libashrt.so` as one object with its own flags and no sanitizer, so the
  store is hermetic and versioned in tree with no system dependency
- gate it with `make test-store-unit` under the address and leak sanitizers: a
  byte golden of a queried row set, a round trip for every scalar with the UInt
  high bit and an embedded NUL both pinned, a transaction that commits and one
  that rolls back, and a negative corpus that refuses a dead dsn, a broken
  statement, a wrong parameter count, and a non scalar value without a crash
- append `ASH_ERR_STORE` to the status enum and the ABI reference ahead of its
  first contract level use, kept out of the `all` gate until the layers above
  it exist to fold it in

### Notes

The `col_names` array rides the query API for S1, which will map a schema's
columns onto a row. The record representation the ABI pins is strictly
positional and carries no names of its own, so the driver takes the array as the
seam and does not fold it into the value; names surface only in the test's
golden dumper. The row allocator is the whole reason the driver is portable
across the milestones: S0 hands it a bump arena the test frees wholesale, and S1
will hand it `ash_bytes` on the signing instance, and the driver cannot tell the
difference. A `sqlite3_busy_timeout` is set at open so a writer that must wait on
another connection surfaces as a store error rather than a silent stall, the
concurrency behavior `docs/database.md` pins for the two instance case that S3
will exercise.

## [v0.2.0] the durable boundary

- open Layer 3 with the store design: a contract backed by a database, its
  schema locked as vows and reconciled against the live table at sign, its
  transactions written as subcontracts that commit when the group completes and
  roll back on any failure or on break
- pin the one new fact storage adds, `ASH_ERR_STORE`, the backend failing the
  runtime and nothing more, with the contract's own rules kept in the contract's
  own error type
- choose the ground: SQLite vendored into the runtime as the reference backend,
  a small store vtable behind it for a second backend later, a stdlib store
  surface the pledge bodies call with parameters and never concatenation, and
  relational shape only for v1
- lay the roadmap for the layer, the store driver proven under raw C before the
  first emitted schema depends on it, the first layer since the language surface
  to move the compiler

## [v0.1.3] the resilience pass

- bound the connect: a non blocking connect and poll capped by the handshake
  timeout, with a send deadline over the handshake writes, both cleared once
  served so a running pledge still blocks as long as it runs; a dead peer now
  fails in the timeout instead of parking on the kernel default
- cover every in flight state across a disconnect: a fulfillment awaiting its
  result, an interrupted round trip, a half read frame, and any call after
  teardown all resolve to a network error through an idempotent shutdown that
  touches no wire
- keep the daemon standing through client death, a dead socket write, and a
  malformed frame, draining an instance's waiters before it breaks and staying
  synced on a payload the codec consumes whole
- add a stress gate: load across connections and threads, disconnect churn while
  serving, and a kill storm with hundreds of fulfillments in flight, every wait
  landing a network error or a clean result, no crash and no leak, clean under
  both sanitizers with a hang failing loud
- pin live reconnect out of v1: a dropped connection is final for its runtime,
  and a reconnect is a fresh one

## [v0.1.2] the remote fulfillment

- serve sign, fulfill, break, and partial from ashd: a sign returns the instance
  and its effective vows, a fulfillment runs on the pool and a detached waiter
  sends its result, and a dead connection breaks the instances it signed
- route the client transparently: sign, fulfill, wait, break, and the partial
  surface follow a remote origin proxy over the wire and the local path
  everywhere else, so the same host code drives a contract local or remote,
  proven by one test asserting identically on both
- carry one reader thread and a writer lock per connection, the request id
  doubling as the future id so every answer routes by id alone
- refuse references over the wire and keep binding local, and deliver a
  disconnect as a network error through every waiting future
- hold the instance lock across every runtime owned write on the remote partial
  path, closing a use after free an external review caught, and reclaim a
  proxy's heap on break so the invariant carries its weight
- correct the wire notes: no argument count crosses, and a fulfillment names a
  pledge the way the local call does

## [v0.1.1] the handshake

- add ashd, a daemon over the runtime that loads modules, freezes, and serves
  their contracts thread per connection, snapshotting the iname dump and its
  hash once at startup
- speak the handshake: a version checked HELLO, a constant time token compare
  that folds length in and never returns early, a HELLO_OK carrying the dump
  hash, and the whole iname table in one INAME_TABLE
- connect a client with ash_runtime_connect, which dials, verifies the received
  table against the HELLO_OK hash, and merges every remote entry into the local
  table under the freeze law, rejecting a name collision without merging anything
- factor the socket plumbing into net.c: full transfer reads and writes that
  retry on EINTR, frame send and receive over the wire codec, and dial and
  listen with keepalive
- gate the handshake with a raw frame client covering the good path, a refused
  token that merges nothing, a forged version, a dead address, and connect after
  freeze, clean under the thread sanitizer
- pin the timeout surface and the argument count handoff in the network design

## [v0.1.0] the wire

- open the network layer with its design: an ashd daemon serves the contracts
  its modules carry, a client connects the same runtime it always linked, and
  every remote contract appears in the local iname table, so the host that moves
  a contract across the network changes no code
- define the wire: a fixed twenty byte frame, thirteen message kinds, and a
  canonical little endian value encoding where decoding and re-encoding any
  payload reproduces it byte for byte
- ship the codec inside the runtime with byte goldens for every representation
  and a negative corpus that sweeps every strict prefix, lies about every
  length, bombs the nesting cap, and puts the two untransportable tags on the
  wire, all refused without a crash
- reserve out of memory for the payload cap alone: wrong magic, an unknown kind,
  a duplicate map key, and a domain breaking scalar are malformed and typed as
  such, encoder and decoder holding the same rules
- append the network status to the ABI ahead of its first use

## [v0.0.15] the map and the place

- give maps their surface: the constructor form with spelled type arguments, an
  index read that answers Option with None on a miss, and an index assignment
  that inserts or updates, over a pinned representation of interleaved pairs in
  insertion order
- deep copy both halves of every entry on the way in and the value on the way
  out, compare maps order sensitively, and render them whole
- teach the parser that Map before an angle bracket opens type arguments in
  expression position rather than a comparison
- fix a silent lost write: an assignment through a chain, a record field's list
  element or a map slot behind a field, wrote to a read copy and vanished;
  targets now lower through an in place lvalue walk at any depth while the right
  side keeps value semantics
- rule the lvalue map index: it names the value slot and types as the value,
  final position never faults, and a missing key mid chain faults like an out of
  range index, all pinned in the grammar
- run the fault checks before the right side evaluates, and pin the whole
  surface with gauntlet pledges, reject twins, and runtime unit tests under the
  sanitizers

## [v0.0.14] the call across the boundary

- call one contract from another inside a pledge body: sign with vow overrides,
  fulfill, and break lower onto the runtime, and a callee's result is copied
  home before use so it outlives the callee breaking
- make a signed instance a reference handle, the one exception to value
  semantics, pinned in the grammar: copies alias, equality is identity, and
  nothing can move one across the boundary
- run a fulfillment issued from a pool worker inline on that worker, so nested
  calls cannot starve the pool, proven by a two worker stress with nested sign,
  fulfill, and break from thunks
- restructure sign into three phases after the thread sanitizer caught it
  holding the runtime lock across a fresh instance mutex; the lock hierarchy is
  now instance, then runtime, then future
- deep copy every vow override at sign, closing a hole an external review
  confirmed where a composite override aliased caller heap into a signed vow and
  dangled after break
- expose the owning runtime of an instance, and gate the whole surface under the
  sanitizers beside the existing suite

## [v0.0.13] the standalone program

- link a real executable from build --bin: exactly one Main contract with a
  bodied run over List<String> returning Result<Int, E>, a generated main that
  signs, marshals argv onto the instance, fulfills, and maps Ok to the exit code
  and Err to a rendered diagnostic
- render any value for diagnostics through ash_value_render, recursive with a
  depth cap and the same sizing protocol as the iname dump
- emit the C header a foreign host compiles against: the shape hash and every
  mangled pledge name as defines, read from the same tables the module emitter
  hashes, pinned against a golden and proven by a host that uses nothing else
- reject the bin builds the grammar forbids, no Main, a missing or abstract run,
  and a wrong signature, each with a named diagnostic
- pin the entry point rules in the grammar and bring the README status up to
  what the toolchain actually does

## [v0.0.12] the second language

- bind the runtime from Python with ctypes against the ABI reference alone, no
  header parsed and no code generated, which is the product claim made runnable
- walk the payment contract from Python end to end: a Python function bound over
  the abstract charge pledge and called back from a pool worker, sign with a vow
  override, the partial, fulfilled, and broken paths with the error payloads
  read back, and a by reference argument written back through the host buffer
- record what the binding taught the ABI reference: a host that opens the
  runtime dynamically must load it into the global scope or every module resolve
  fails, the concrete LP64 layouts for a header less foreign function interface,
  the callback lifetime rule, and the stability of every enum value
- gate the walk behind make test-python beside the other gates

## [v0.0.11] the standard library and the boundary

- load imports for real: dotted paths resolve beside the root then under
  ASH_HOME, every file parses into one shared tree with per file span bases
  mapping diagnostics back to their sources, cycles and duplicate imports are
  named errors, and two builds stay byte identical
- enforce the package boundary: an internal contract or provisional clause is
  invisible outside its directory through incorporate, sign, and type
  annotations alike, and an internal contract never reaches the descriptor
  tables or the iname
- close an unchecked ABI hole an external review confirmed with a repro: a
  contract type in a pledge signature emitted no runtime check at all, so a host
  could pass anything; contract types are now rejected at every boundary
  position at any nesting depth, pinned in the grammar
- lower clauses as static functions sharing the thunk's exit convention, so
  returns and propagation work unchanged and mutual clause calls need no ordering
- write the first ashstd: math with overflow checked pledges and an epsilon vow,
  strings and collections shaped honestly to what the language can express,
  common errors, and the comparable, loggable, and serializable provisional
  clauses
- drive the library end to end from the C host, one module registering three
  contracts, sorting through an incorporated compare clause

## [v0.0.10] the whole body

- lower the rest of the surface: while, for over a list, all three assignment
  targets, break and continue, match in every form with nested patterns and
  payload bindings, record and variant construction, field and index access,
  list and tuple literals, and propagation
- give sums their representation, the variant index as the tag over the shared
  payload arm, and teach the deep copy and the new recursive equality to walk
  records and sums
- short circuit the logical operators by lowering the right arm inside a guard,
  so a bounds check like i < n && xs[i] never touches the index it just refused
- give composites value semantics at every boundary, let, assignment, and
  composite reads all deep copy, so mutation through one binding is never visible
  through another, both rules pinned in the grammar
- return a type error from an out of range index instead of faulting, with the
  outcome never latched
- gauntlet the whole set behind a sanitized gate and fold the new module into
  the determinism check

## [v0.0.9] the fulfillment policy

- replace the counting latch with true per pledge state: first Ok latches
  fulfilled, an Err before any Ok latches broken with its payload kept readable,
  and neither ever changes again
- serialize the requirements block from the exact tree the checker verified,
  atoms in the checker's own enumeration order, with the grammar's defaults
  synthesized into the same postfix form when the block is absent
- evaluate the policy under the instance lock after every outcome, in break,
  fulfill, partial priority, with the break line armed only once something has
  actually broken, a rule now pinned in the grammar
- transition to broken automatically when the break line holds, keeping the
  instance heap alive so the error payloads stay readable until an explicit break
  reclaims them
- expose the partial result: item counts and names over named subcontracts and
  loose pledges, and the latched errors by pledge name
- drive the spec's payment contract end to end through partial, fulfilled, and
  both broken paths under the sanitizers

## [v0.0.8] the iname table

- register every contract and pledge in a sorted runtime table keyed by mangled
  name, inserted with full rollback so a failed registration leaves nothing
  behind
- add freeze: after it, loading, registering, and binding refuse with a state
  error while signing and fulfilling continue, and the table reads are immutable
  from then on
- expose lookup by exact mangled name, ordered enumeration, and a canonical
  dump, one line per entry in mangled order, sized or refused whole
- synthesize the contract level mangled name from the descriptor and carry the
  owning contract's shape hash on every pledge entry, so a discovered entry is
  exactly the pair a host signs under
- walk the host through real discovery, lookup to sign to fulfill, and pin a
  second generation module whose stale mangled names miss at every version stamp
- gate determinism, two builds byte compared, and the table against a golden
  dump with the real hashes

## [v0.0.7] the pool

- run every fulfillment on a worker pool sized by the runtime config, with copy
  in still on the caller's thread and write back still at the wait, so the memory
  model's promises survive real concurrency
- queue tasks through an intrusive link the future itself carries, so enqueueing
  allocates nothing and shutdown drains, joins, and frees single threaded
- serialize each instance behind one recursive mutex covering the thunk run, the
  latch, and every arena allocation, with distinct instances genuinely parallel
  and a documented runtime, instance, future lock order
- resolve the break race deterministically: a running thunk finishes, a queued
  task finds the contract broken and touches nothing, and an unwaited future is
  forfeited to a state error with its pointers cleared before the heap goes
- refcount futures between the receipt and the pool, fixing a use after free the
  thread sanitizer caught on the sync path against a broken instance
- pin the model with a stress gate, hundreds of fulfillments across threads and
  instances plus break hammering, run under the address and thread sanitizers in
  the default build, both silent
- document the threading rules in the ABI reference

## [v0.0.6] the memory model

- deep copy every argument onto the instance at fulfill entry, on the caller's
  thread, so a thunk only ever sees instance owned values and a host buffer
  rewritten after the call changes nothing
- add by reference arguments: a ref occupies a trailing declared parameter as a
  mutable instance owned slot, and its final value is written back to the host
  pointer at delivery only, inside the wait or before the synchronous return, so
  host memory is touched only while the host is blocked in an ash call
- write back through a default for scalars and strings or a caller supplied
  callback, only on a fulfilled outcome, with every slot type checked before
  anything is written
- add ash_pledge_bind: a host implementation bound over the runtime's overlay,
  resolved into a fixed dispatch table at sign, beating a compiled body and
  satisfying an abstract pledge, with an unbound pledge still refusing to sign
- grow the value layer: lists, tuples on the list arm, and a recursive deep copy
  across every representation
- give the skeleton an abstract shout pledge the host binds, prove the write
  back timing on both delivery paths, and pin the value layer with an address and
  leak sanitized unit gate
- extend the ABI reference with the binding and by reference protocols

## [v0.0.5] the real emitter

- retire the canned module: build now runs the whole front end and lowers the
  typed tree, and a construct codegen cannot carry yet is a named build error
  instead of wrong C
- emit per pledge thunks over the uniform frame, descriptor tables with vow
  defaults, and the registrar, all byte stable across rebuilds
- mangle every pledge as __ash_ash_{contract}_{symbol}_{sighash}_v{n} with an
  fnv1a 64 shape hash over the canonical signature strings, cross checked against
  an independent implementation
- store vows on the instance: sign takes named overrides over the declared
  defaults, checked by name and type, with strings deep copied so an instance
  never aliases host memory
- give fulfillment its real shape: fulfill returns a future, wait delivers the
  outcome exactly once, and the synchronous form rides the same path, so hosts
  survive the threading milestone unchanged
- carry the hidden signature on every instance, the shape hash and the signing
  time, and refuse a wrong expected hash at sign
- concatenate String with plus, pinned in the grammar and the checker
- write the ABI reference: value representations, the thunk frame, descriptors,
  ownership, mangling, and the hash algorithm

## [v0.0.4] the checker

- write the type checker in dusk over a hash consed type arena whose canonical
  strings are deterministic, the same strings the signature hashes will eat
- enforce the contract rules: a pledge returns Result, Option, or Unit, a vow
  initializer is a constant of the vow type and never assignable, and an
  incorporated signature matches its clause exactly
- infer expressions with expected type pushdown and no implicit conversion
  anywhere, widths included, over the full operator table
- type sign with named vow overrides, instance pledge calls, first class pledge
  values, and break, and check match exhaustiveness over sums, Option, Result,
  and Bool with the propagation operator held to its enclosing return type
- enumerate the requirements policies and reject a fulfill and break that can
  hold together, naming a witness state, and a policy no state satisfies
- name everything deferred with a not yet supported diagnostic instead of
  accepting it silently
- pin the checker with six golden programs and eighteen reject twins, and pin
  the block as value rule in the grammar

## [v0.0.3] the parser

- write the parser in dusk over parse order arenas: fixed slot nodes for every
  declaration, statement, expression, type, and pattern, a shared kids arena for
  the variable length lists, and interned names
- carry the whole surface: records, sums, provisional clauses, contracts with
  attributes, vows, pledges with optional bodies, clauses, subcontracts, and the
  requirements block with its own operator grammar
- terminate statements on the newline flag at group depth zero, with braces
  resetting the depth so a block inside an argument list keeps statement
  semantics
- ban the bare record literal in an if, while, for, or match head and require
  its brace on the name's line, both pinned in the grammar
- hold the 500 deep nesting ceiling without the quadratic unwind, one diagnostic
  and milliseconds on a ten thousand paren bomb
- add the parse command to ashc with a byte stable tree dump, and pin the front
  end with seven golden fixtures and six reject twins

## [v0.0.2] the token stream

- write the lexer in dusk: every literal form, the full escape set with unicode
  scalar validation, line and block comments, and span tracking with a newline
  flag on each token instead of a newline token
- add the lex command to ashc, one token per line to stdout in a byte stable
  dump format, diagnostics to stderr with the caret line
- pin the lexer with golden fixtures over every construct and reject twins for
  the escape and termination faults, run by tests/run.sh
- pin three literal rules in the grammar: no raw newline in a string, integer
  literals cap at the Int maximum for now, and a unicode escape never names a
  surrogate or a value past 0x10FFFF
- write the how to use section of the README around make smoke and the sanitizer
  gate
- rebuild ashc when any compiler source changes, not just the two files the M0
  rule listed

## [v0.0.1] the walking skeleton

- pin the surface in docs/grammar.md: the statement and expression core, record
  and either declarations, method call fulfillment, the requirements grammar
  with the latch on first Ok rule, and the fixed numeric representations
- write the README: what a contract is, the lifecycle, and how the interop
  crosses the C ABI
- define the ABI in ash_abi.h as the single source of layout truth: the
  AshValue fat value, the uniform pledge thunk frame, and the descriptor tables
  a compiled module registers
- build the runtime: module load over dlopen, contract sign with shape hash and
  unbound pledge checks, synchronous fulfillment through the thunk frame,
  outcome latching, and a per instance block list that break reclaims in one walk
- write ashc in dusk as the first spike: read the source, emit the canned
  Greeter module C, and link it with cc into a loadable .ash.so
- drive the whole pipeline from a C host that checks the greeting, the state
  transitions, and the error paths, gated by make smoke and an address and leak
  sanitizer build
