# The Ashford Bridge

Ashford connects languages in two shapes, and this document is the normative reference for the second. Inside one process, contracts glue languages over the C ABI: every language loads `libashrt`, signs, fulfills, and breaks in shared memory, and nothing is serialized. Across processes, contracts speak gRPC: the compiler emits the wire surface, stock tooling builds the clients, and the contract's lifecycle rides a protocol every language already carries. One contract, two boundaries, one lifecycle.

## The emitted surface

`ashc emit-proto file.ash` runs the whole front end and writes three artifacts into `target/ashc-out`, each byte stable for a given source and pinned as a golden:

- `<stem>.proto`, the wire surface. Every public contract is its own service, every pledge its own typed rpc, so a consumer in any gRPC language gets the contract's argument and result types from protoc alone, with no generic value envelope and no dispatch on a name string.
- `<stem>_session.go`, the Go session wrapper, living in the same package as protoc's output.
- `<stem>_session.ts`, the TypeScript session wrapper, structural interfaces over `@grpc/proto-loader`, no protoc step and no native code, and node runs it directly under type stripping.

The wrappers exist because protoc gives a consumer every typed call and cannot give it the one thing the surface turns on: the stream whose lifetime is the instance's. Each wrapper is a page with one idea in it, and another language's wrapper is a morning, not a milestone.

## The session

Signing is a server streaming rpc, and that is the surface's one load bearing decision. `Session` takes what sign takes, the vow overrides as optional fields and an `expected_hash` where 0 skips the check, and answers with a stream whose first event is the signature: the instance id every pledge rpc is keyed by, the effective vows, the shape hash, the signing time, and the park token when the server parks. The stream then stays open for as long as the instance lives. Holding it is how a client says still mine; closing it is how a client says done; dying is a fact the server has in milliseconds. No timer ever decides a contract's ending.

The pledge rpcs are unary and typed, keyed by the instance id. `GetPartial` reads the partial surface, the name lists repeated in the runtime's insertion order, which is normative. `Break` ends the contract explicitly; the row outlives the break behind its stream, so a broken instance still says broken instead of saying it never existed, and leaves when the stream does.

## The shape hash

The wrappers pin the contract's shape hash as a constant, the same value the compiled module registers, computed by the same calls the module emitter makes. A client that signs under the pinned hash has proven the wire surface and the loaded module describe one contract before the first pledge runs. A hash that disagrees is refused at sign, before an instance exists.

## Business and transport

A contract's own `Err` is a value. It crosses as one arm of the result oneof on an OK rpc, exactly as it returns in process, and never becomes a transport error. An Ashford status is the transport and lifecycle layer, a fulfillment that did not run, and crosses as a gRPC code:

| Ashford status    | gRPC code           |
| ----------------- | ------------------- |
| `ASH_ERR_STATE`   | FAILED_PRECONDITION |
| `ASH_ERR_NAME`    | NOT_FOUND           |
| `ASH_ERR_TYPE`    | INVALID_ARGUMENT    |
| `ASH_ERR_VERSION` | ABORTED             |
| `ASH_ERR_UNBOUND` | FAILED_PRECONDITION |

The split is the language's own line, drawn once and held across the wire.

## The type set

Scalars cross as proto scalars, Int and UInt as their 64 bit forms, Float as double, Bool and String as themselves. A declared record is a message with its fields in declaration order. A declared sum whose arms are all bare is an enum numbered in declared arm order, the ABI's own tag space; a sum with a payload arm is a message whose oneof carries one message per arm in the same order. Unit is the empty message. Option and List ride oneof arms as wrapper messages and sit in fields as optional and repeated. A pledge may answer a bare Option and it crosses as the wrapper directly. Map and tuple are named build errors that write nothing, because proto's map carries no deterministic order and Ashford pins map iteration as insertion ordered; a service with a silent hole is worse than a refusal with a name.

Message names carry no contract prefix while a file declares one public contract; a file with several prefixes every contract scoped message with its contract's name. The rpc spellings `Session`, `Resume`, `GetPartial`, and `Break` belong to the surface, and a pledge under one of those names is refused by name; a pledge spelled `sign` keeps its rpc and moves its request message alone to `SignPledgeRequest`.

## Park and resume

A server started with a park store issues every session a park token beside its signature. When a stream ends without an explicit `Break`, the server parks the instance under that token before the break that drops the row: the vows, the latches, the error payloads, and the transactional fates go into one store row through the runtime's own park call. `Resume` takes the token, stands the instance back up under a fresh id on a new stream, and consumes the row, one shot: a spent or unknown token is NOT_FOUND, a lying hash is ABORTED before the row is touched, and a resumed session that later drops parks again under the same token. An explicit `Break` stays the one true ending, and the token dies with the instance.

The park store is what makes the session survive everything the stream cannot. A network partition that kills the stream loses nothing a resume cannot recover; a server that dies is replaced by any server holding the park store, which is the whole answer to replica affinity. Who may resume a token, how often, and for how long is server policy, deliberately outside the runtime: the runtime writes and reads park rows, and the bridge decides what a token is worth.

## The server

The reference bridge server is a Python host over `libashrt`: it loads the compiled module through the C ABI, binds host implementations over abstract pledges, and serves the emitted surface with grpc. Nothing about it is privileged. Any language that reaches the ABI can hold the runtime, and any language with a gRPC server can serve the surface, because the bridge is ordinary host code on one side and ordinary protobuf on the other. The gates stand the server up against clients in Python, Go, and Node, kill it between a park and a resume, and demand the contract finish on latches set before the death.

## What the bridge does not do

There is no relay and no registry: a client dials a server that holds the contract, and a server serves the contracts its runtime registered. There is no cross server transaction and no shared session bus; one instance lives behind one stream or one park row at a time, never both, never neither. Discovery, load balancing, and transport security are gRPC's own ground, and the bridge stands on it rather than beside it.
