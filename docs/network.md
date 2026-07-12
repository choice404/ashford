# The Ashford Network Runtime

This is the design for Layer 2, the network runtime: contracts served over a socket, discovered remotely, and fulfilled transparently. A daemon, `ashd`, loads compiled modules and serves their contracts. A client links the same `libashrt` it always linked, calls `ash_runtime_connect`, and every contract the daemon serves appears in the client's iname table beside the local ones. Signing and fulfilling a remote contract is the same code as signing and fulfilling a local one; the host does not change when the contract moves across the network. That is the product claim of this layer, and the argument for it is spelled out at the end.

This document will be normative once the wire ships. It builds on [docs/abi.md](abi.md) and changes none of it except one appended status code; the value representation, the thunk frame, the future semantics, and the iname dump format are all load bearing here, on purpose. Everything below the frame header is little endian, the same choice the in memory ABI already made.

## The model

`ashd` is a small program over the runtime that already exists. It brings up an `AshRuntime`, loads the modules named on its command line, freezes the runtime, and listens on a TCP address. From that moment its iname table is immutable, which is what makes it a discovery surface worth synchronizing: the table a client fetches at handshake is the table every later sign resolves against, byte for byte, the same promise `ash_runtime_freeze` already makes locally.

The client side is `libashrt` itself, one call wider:

```c
AshStatus ash_runtime_connect(AshRuntime* rt, const char* addr,
                              const char* token);
```

`addr` is `host:port`. `token` is the shared secret the daemon expects, NULL when the daemon runs without one. The call dials, performs the handshake, fetches the daemon's iname table, and merges every entry into the local table with an origin mark naming the connection. From then on `ash_contract_sign` on a remote origin name sends a sign across the wire and returns a proxy instance, `ash_pledge_fulfill` on that instance sends the fulfillment and returns an ordinary future, and `ash_future_wait` blocks until the result frame arrives. The host walks the same five calls it walked before.

Connect counts as registration. It adds names to the iname table, so it obeys the freeze law: `ash_runtime_connect` after `ash_runtime_freeze` is `ASH_ERR_STATE`, exactly like `ash_module_load`. Connect first, freeze after, and the frozen table covers local and remote names alike. A remote name that collides with a name already in the table, local or from another connection, fails the connect with `ASH_ERR_NAME` and merges nothing, the same rule double registration hits locally. A client may hold connections to several daemons; each is its own origin, and instances die with the connection that signed them.

The origin mark is client side bookkeeping, not wire data. The iname dump format is unchanged: a client that dumps its table after a connect prints remote entries in the same canonical lines as local ones, and the text stays byte stable for a given set of registrations and connections.

## The wire

One connection carries frames in both directions. Every frame is a fixed header and a payload:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic, the bytes `A` `S` `H` `W` |
| 4 | 4 | kind, u32, one of the message kinds below |
| 8 | 8 | request id, u64 |
| 16 | 4 | payload length, u32, bytes that follow |
| 20 | ... | payload |

All integers on the wire are little endian, header and payload both. The request id is chosen by the client, unique per outstanding request on the connection; every reply echoes the id of the request it answers, which is how results arriving out of order find their futures. The daemon never initiates a frame in v1, so ids flow one way and cannot collide.

A payload longer than 64 MiB is refused: the receiver answers `ERROR` with `ASH_ERR_OOM` and closes the connection, since a peer that ignores the cap cannot be resynchronized. A frame with the wrong magic or an unknown kind is malformed rather than oversized, refused with `ASH_ERR_TYPE`, and the connection closes the same way; `ASH_ERR_OOM` is reserved for the cap alone. Nothing on the wire is trusted; every length is checked against the frame before it is read.

## The value encoding

Values cross the wire in the canonical encoding, a serialization of the `AshValue` representation in [docs/abi.md](abi.md) with the pointers flattened out. Every encoded value opens with the same two tags the struct carries:

```text
u32 ty    the AshTypeTag
u32 tag   the variant for the sum shaped types, 0 otherwise
```

and continues by type:

| type | payload |
|---|---|
| Unit | nothing |
| Int | i64 |
| UInt | u64 |
| Float | 8 bytes, the IEEE 754 double bit pattern |
| Bool, Byte | 1 byte |
| Char | u32, the Unicode scalar value |
| String | u64 byte length, then the UTF-8 bytes inline |
| List | u64 element count, u32 elem_ty, then each element encoded recursively |
| Tuple | u64 arity, then each element |
| Map | u64 slot count, u32 key tag, then the interleaved slots in insertion order |
| Record | u64 field count, then the fields in declaration order |
| Sum | u64 payload field count, then the fields; 0 fields for a bare variant |
| Option | tag 1 carries one encoded value, tag 0 carries nothing |
| Result | one encoded value, the Ok or Err payload |

The encoding drops what the wire does not need. A list's `cap` is an allocation detail and never crosses; the decoder chooses its own capacity. A map's slot count is `len` from the representation, twice the pair count, and the slots cross in insertion order, which [docs/abi.md](abi.md) already pins as the order any serialization sees. A boxed payload crosses inline, so `None` is eight bytes of header and nothing else.

The encoding is canonical: one value has one byte string, and encoding a decoded value reproduces the input exactly. That is what lets the codec be golden tested byte for byte and what keeps two implementations honest against each other.

Two tags never cross. `ASH_TY_INSTANCE` is already forbidden across the ABI and the wire forbids it the same way; `ASH_TY_PLEDGE_REF` is a value bound to a live instance and has no meaning on another machine. An encoder handed either refuses with `ASH_ERR_TYPE`, and a decoder meeting either closes the connection as malformed. A decoder also caps nesting at 64 levels, so a hostile payload cannot recurse the stack away; a frame deeper than that is malformed. Malformed reaches further than shape: a map payload carrying a duplicate key is a value no runtime could have built and is refused, a Bool is exactly 0 or 1, and a Char is a Unicode scalar value, with the encoder holding itself to the same rules so that what one side emits the other always accepts. String bytes are not validated as UTF-8, the same bytes are bytes stance the ABI takes.

Decoded values obey the ownership rule unchanged: the client decodes a result onto the proxy instance that fulfilled it, so everything the host receives is instance owned and dies at that instance's break, exactly like a local result.

## The messages

Thirteen kinds, numbered on the wire; requests flow client to daemon, replies daemon to client. `ERROR` answers any request whose normal reply cannot be produced.

| kind | name | direction | answers |
|---|---|---|---|
| 1 | `HELLO` | client | |
| 2 | `HELLO_OK` | daemon | `HELLO` |
| 3 | `INAME_SYNC` | client | |
| 4 | `INAME_TABLE` | daemon | `INAME_SYNC` |
| 5 | `SIGN` | client | |
| 6 | `SIGNED` | daemon | `SIGN` |
| 7 | `FULFILL` | client | |
| 8 | `RESULT` | daemon | `FULFILL` |
| 9 | `BREAK` | client | |
| 10 | `BROKEN` | daemon | `BREAK` |
| 11 | `PARTIAL_QUERY` | client | |
| 12 | `PARTIAL` | daemon | `PARTIAL_QUERY` |
| 13 | `ERROR` | daemon | any request |

Strings inside payloads are a u32 byte length followed by UTF-8 bytes, no terminator, the fat value flattened.

**`HELLO`**: u32 protocol version, then the token as a string, empty when the client has none. The daemon accepts exactly the protocol versions it speaks; v1 speaks exactly version 1, and the field exists so a later daemon can meet an older client without a new magic. A version the daemon does not speak is `ERROR` with `ASH_ERR_VERSION`. A token the daemon refuses is `ERROR` with `ASH_ERR_NET` and the connection closes; the client learns the handshake failed and the diagnostic string says why, and nothing before a successful `HELLO_OK` is served.

**`HELLO_OK`**: u32 accepted version, u64 table hash, the FNV-1a 64 of the daemon's canonical iname dump text. The hash lets a reconnecting client check it is looking at the same world before it resyncs, and a client that holds yesterday's table against today's hash knows to fetch again.

**`INAME_SYNC`**: empty. The reply, **`INAME_TABLE`**, is the daemon's whole `ash_iname_dump` text as a string, the canonical form [docs/abi.md](abi.md) pins, minus the terminating NUL. The whole table crosses at handshake rather than entry by entry on demand: the table is frozen, it is small, one line per contract and pledge, and a complete local copy means every later lookup, enumeration, and dump on the client is a local read with no round trip and no partial view. A query form can arrive later without a new message kind, since an unknown payload in `INAME_SYNC` is room the format reserves; v1 sends it empty and gets everything.

**`SIGN`**: contract name string, u64 expected hash, u32 override count, then per override a vow name string and an encoded value. The daemon runs `ash_contract_sign` with exactly those arguments, so the whole local rulebook applies unchanged: `ASH_ERR_NAME` for an unknown vow, `ASH_ERR_TYPE` for a wrong typed override, `ASH_ERR_UNBOUND` for an unsupplied vow or an abstract pledge nothing bound daemon side, `ASH_ERR_VERSION` when the expected hash disagrees, each riding back in an `ERROR` frame.

**`SIGNED`**: u64 instance id, i64 signed_at, u64 shape hash, u32 vow count, then per vow a name string and an encoded value, the full effective vow set after defaults and overrides. The client builds the proxy instance from this one frame: `ash_vow_ref`, `ash_contract_hash`, and `ash_contract_signed_at` on a remote proxy are local reads forever after, which is sound because vows are immutable from the moment the sign lands. Instance ids are daemon assigned, unique for the life of the connection, and meaningless outside it.

**`FULFILL`**: u64 instance id, pledge name string, the bare pledge name the local `ash_pledge_fulfill` takes, resolved within the instance's own contract, u32 argument count, then each argument encoded. No refs cross the wire; see the semantics section. **`RESULT`**: u32 status, then the pledge's value encoded when the status is `ASH_OK`. The request id is the future: the client's `ash_pledge_fulfill` sends the frame and returns a local future, and the wait blocks until the `RESULT` carrying that id arrives. There is no separate future id because the request id already is one.

**`BREAK`**: u64 instance id. The daemon runs `ash_contract_break`, and **`BROKEN`** carries its u32 status back. The client side proxy forfeits its unwaited futures to `ASH_ERR_STATE` before the frame is sent, the same order of events a local break walks.

**`PARTIAL_QUERY`**: u64 instance id. **`PARTIAL`**: u32 contract state, u32 item count, then per item a name string and a u32 item state, in descriptor order, named subcontracts first then loose pledges; u32 error count, then per error a pledge name string and an encoded Err payload, in declaration order. One frame carries the whole snapshot, so the answer is at least internally coherent, taken under the daemon instance's lock the way every local partial read is.

**`ERROR`**: u32 status, one of the `AshStatus` numbers, then a diagnostic string. The status is normative and the string is not: a client maps the status onto the same error handling it already had, and the string exists for humans and logs. The status numbers are already part of the ABI's wire, fixed forever, which is why they can ride here without a translation table.

## Semantics across the wire

The design rule of this whole layer: a remote operation has the semantics of the local call it stands in for, and where the network makes that impossible the operation is refused rather than bent.

**Sign** is `ash_contract_sign` run on the daemon with the client's arguments. Every validation and every failure status is the local one. The shape hash rides in the `SIGN` frame, so the defense against fulfilling yesterday's contract through today's module works across the network exactly as it works in process; and since the client resolves names against the synced table, a stale client usually fails earlier and cleaner, at name resolution, before any hash is compared.

**Fulfill** is `ash_pledge_fulfill` at both ends. The future was always the one real shape of fulfillment, an async shell over a sync core, and the network is the case the shell was shaped for: the wait that used to block on a pool worker now blocks on a frame, and the caller cannot tell, because blocking until the outcome exists was already the contract. Argument values are encoded on the caller's thread inside the fulfill call, so host argument lifetimes end when the call returns, unchanged. Errors keep their delivery point: every fulfillment error, wrong state, unknown pledge, argument mismatch, arrives through the wait, never the fulfill, so a host's error handling still has one place to live. `ash_pledge_fulfill_sync` remains the two calls fused.

**Ordering.** Fulfillments against one instance serialize in the order the daemon receives them, because the daemon side instance carries the same lock every instance carries. A client sends its frames in call order on one connection and TCP preserves that order, so per instance serialization in fulfill call order holds across the network for a single client, the same guarantee the local pool gives. Results for distinct instances may arrive in any order; the request id matches each to its future. Two clients fulfilling against contracts on one daemon interleave the way two host threads already interleave locally.

**By-reference arguments do not cross the wire in v1.** A `FULFILL` on a remote proxy with a nonzero ref count is refused with `ASH_ERR_TYPE`, delivered through the wait like every fulfillment error. The write back protocol is defined by two moments on threads the host is blocked in, and its promise is that host memory is touched only then; a network hop in the middle would either break that promise or demand a distributed write back whose failure semantics v1 has no honest answer for. Refusing loudly is the honest answer. A pledge that wants to hand data back over the network returns it in its value, which is what return values are for.

**State and the partial surface.** The contract state lives where the latches live, on the daemon, because fulfillments land there and the policy evaluates there. `ash_contract_state` and every `ash_partial_*` call on a remote proxy issues one `PARTIAL_QUERY` and reads the snapshot it returns, so each call is one round trip and the snapshot inside one frame is coherent. Two separate calls bracket a coherent picture only when no fulfillment lands between them, which is exactly the local caveat with a wider window; a host that wants the coherent picture reads it after its waits complete, the advice [docs/abi.md](abi.md) already gives. Err payloads in a `PARTIAL` frame are decoded onto the proxy, so their lifetime is the proxy's heap, one rule everywhere.

**Break** breaks the daemon instance and reclaims its heap there, and the proxy latches Broken locally. Every later fulfillment on the proxy is `ASH_ERR_STATE` without touching the wire, the client already knowing the answer.

**Disconnect.** When a connection dies, every proxy signed over it latches Broken, and every future still waiting delivers `ASH_ERR_NET`, a new status appended to `AshStatus` after `ASH_ERR_LOAD`:

| status | meaning |
|---|---|
| `ASH_ERR_NET` | the connection carrying this operation failed |

The daemon side mirrors it: a connection's death breaks every instance that connection signed, explicitly, heap reclaimed, so an absent client cannot pin daemon memory. This is the network's one genuinely new failure and it gets exactly one new number; everything else that can go wrong already had a status.

**Timeouts.** The handshake has a timeout, `AshRuntimeConfig.handshake_ms` on the runtime, default ten seconds when the field is zero, because a peer that will not finish a `HELLO` is not a peer. After the handshake there is no protocol level timeout: a fulfillment runs as long as its pledge runs, exactly as a local wait blocks as long as the body runs, and inventing a deadline the language does not have would make the network case semantically different from the local case, which is the one thing this design refuses to do. Dead peers are the transport's job: keepalive is on, and a reset or closed socket is `ASH_ERR_NET` through every waiting future. A host that wants call deadlines wants a language feature, not a wire feature, and that is future work in both places at once.

## Authentication

v1 authenticates with a shared token over plain TCP, and says so plainly. The daemon is started with a token, read from a file so it never sits in an argument list; the client presents it in `HELLO`; the daemon compares in constant time and refuses the connection on a mismatch, before any table, name, or contract crosses the wire. A daemon started without a token accepts empty tokens, for loopback and for tests.

This is perimeter authentication, not identity and not confidentiality. It keeps an accidental client out; it does not keep a network eavesdropper out, and a deployment that needs that runs v1 on loopback, a private network, or under a TLS terminating proxy in front of `ashd`. Real transport security and per contract authorization are later work, listed below, and the `HELLO` exchange is where they will land when they do. Building half of TLS by hand into v1 would be worse than saying this paragraph out loud.

## Why host code does not change

The claim is that a host written against a local module runs against a remote daemon with one added `ash_runtime_connect`, and it holds because every seam was already in place. Discovery was already a table of mangled names, and connect fills the same table through the same canonical dump format, so lookup, enumeration, and resolution do not know the difference. Signing was already by name with a hash check, and the name now routes by origin while the arguments, the failure statuses, and the returned handle keep their shapes. Fulfillment was already a future whose wait blocks until the outcome exists, and the ABI never promised where the body runs, so a wait that spans a network instead of a pool queue satisfies the same contract; the sync form was already the two calls fused. Values were already copied at every boundary, owned by an instance, and dead at its break, so a decoded result behaves exactly like a computed one. And every failure was already a status delivered through the wait, so the one new fact a network adds, that the peer can vanish, arrives as one new status through the channel the host was already reading. The host changes by one line because the ABI spent all of Layer 1 refusing shortcuts that would have made this paragraph false.

## What v1 leaves out

Named so nothing is discovered missing by surprise. Each has a place to land later without moving the frame format or the message numbers.

- **Hot reload.** A daemon serves what it froze at startup; new modules mean a new daemon.
- **Multiple daemons as one mesh.** A client may connect to several daemons, but daemons do not discover, proxy, or route to each other.
- **Load balancing and failover.** One instance lives on one daemon; nothing migrates and nothing retries on another host.
- **Callbacks and client hosted implementations.** The daemon never initiates a frame, so a pledge served by a daemon cannot be implemented by a client. `ash_pledge_bind` on a remote origin pledge is `ASH_ERR_STATE`.
- **By-reference arguments over the network**, refused with `ASH_ERR_TYPE` as described above.
- **TLS, identity, and per contract authorization.** v1's token is a perimeter, nothing more.
- **Fulfillment deadlines.** No protocol timeout on a running pledge, for the semantic reason given above.
- **Live reconnect.** A dropped connection is final for the runtime that held it: its proxies stay Broken and its remote names stay merged, so reconnecting the same runtime to the same daemon collides on those names. A reconnect is a fresh runtime, and reviving a proxy across a new connection is future work.
- **Partial iname queries and compression.** The whole table crosses at handshake, uncompressed; both are payload level changes if a table ever grows enough to want them.
