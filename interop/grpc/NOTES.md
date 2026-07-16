# The gRPC bridge, step 1: what the prototype found

Appendix III names instance state over stateless gRPC the load bearing unknown
and asks for one contract in one language before anything else moves. This is
that prototype. `skeleton/payment.ash` is served over gRPC by `bridge_server.py`
and driven by `bridge_client.py`, which holds no runtime handle and knows its
instance only by a uint64 the server issued. The client asserts the same answers
`interop/python/demo_payment.py` gets in process.

It is a prototype. It answers questions; it is not a surface to build on.

## Does the session model work

Yes, and it worked without a fight.

The instance id substitutes for the contract handle with no loss on this
contract. Every check the in process walk makes, the gRPC walk makes and passes:
the vow override lands, the pending order holds, half a subcontract moves
nothing, `Validation` lands and the instance goes partial, charge answers its
declared Bool through a Python body two boundaries away, the instance fulfills.
On the Err path the contract's own break line fires by itself and the partial
surface still reads the payload. The bridge did not have to teach gRPC anything
about the lifecycle. It carries an integer and the runtime does the rest.

The table is 40 lines: a dict under a lock, a monotonic counter, a frozen entry
per instance. Ids are server issued, never client supplied, so a peer cannot
name an instance it was not handed. That much is genuinely easy.

The one real hazard inside the server is the reaper racing a call in flight. It
needs a per instance lock and a recheck of the touch timestamp under that lock,
because a sweep that selected a candidate and a call that arrived a microsecond
later must not both win. That is solved here and it is not hard, but it is the
kind of thing that is invisible until it corrupts an instance under load, and
every bridge in every language will have to solve it again.

## Orphan reaping, and the honest cost

TTL is the only rule available, and it is worse than it looks.

A unary gRPC handler learns nothing about a client that walked away. There is no
disconnect signal, no callback, no close. The handler returns and the server's
knowledge of that peer ends. So the only question the server can ask is how long
the instance sat untouched, and the only answer it can act on is a guess: an
instance idle past the TTL is presumed abandoned, broken, and dropped.

Four things follow, in the order they hurt:

1. **A timer breaks contracts.** This is the sharp one. In Ashford, break is a
   meaningful act. `payment.ash` writes a break policy in its own source. The
   reaper's break is none of that. It is resource management wearing the
   lifecycle's clothes, and from inside the contract it is indistinguishable
   from a party deciding to break. The bridge invents a way for an obligation to
   end that the contract never agreed to and cannot see coming. Nothing in the
   in process model has this shape, because there the handle's owner was the
   process, and a process that goes away takes its obligations with it honestly.

2. **A slow client and a dead client are the same client.** The TTL cannot tell
   them apart because there is nothing to tell them apart with. Set it short and
   a client behind a slow network loses a signed instance mid walk. Set it long
   and orphans accumulate for as long as the TTL says.

3. **One TTL cannot serve two lifetimes.** A payment walk finishes in
   milliseconds and wants a tight TTL. A contract waiting on a human approval is
   legitimately idle for an hour and needs an hour. Both are the same service.
   Any single number is wrong for one of them, and the number is a server side
   guess about a client side intent nobody transmitted.

4. **The break is not the end of the row.** `Break` here leaves the table entry
   standing, because in process a broken handle still answers `ASH_ERR_STATE` to
   a fulfillment and still reads its partial surface, so the owner learns it
   broke. Dropping the row would make a broken instance answer `NOT_FOUND`,
   indistinguishable from an id that never existed, and the bridge would lose a
   distinction the C ABI keeps. So the row becomes a tombstone, and the tombstone
   needs its own TTL, and now the TTL is guessing about two different things.

**The rule TTL is standing in for already exists in gRPC, one layer up.** A
server streaming or bidirectional rpc has a termination callback. A client that
dies terminates the stream and the server is told, immediately and for certain.
If an instance's session were a stream, held open for its lifetime, the
connection would be the session again, exactly the property the C ABI got free
and this prototype gave up by choosing unary. That is the shape worth prototyping
next, and this run is the argument for it: unary plus TTL works, and every cost
above is the cost of the unary choice, not of gRPC.

## Load balancer affinity

One server here, so this is reasoned, not measured.

An instance id names memory inside one process. Under two replicas, a request
carrying id 7 must reach the replica holding id 7 or it gets `NOT_FOUND` from a
peer that is working perfectly. gRPC's default is per call load balancing across
subchannels, which is precisely wrong for this. Three ways out, and they are not
equal:

- **Route on the id.** Encode the replica in the id's high bits, or put a
  lookaside table in front. Works, and it makes every deploy, drain, and scale
  event an instance killer: the replica leaves and every session on it dies.
  This is sticky sessions, with all of their history.
- **Pin the channel.** The client keeps one subchannel for an instance's
  lifetime. Pushes the problem onto the client and onto every generated client
  in every language, which is the opposite of what step 2 promises.
- **Put the instance in the store.** Layer 3 stays under Appendix III and is
  orthogonal to transport, and this is where that pays. An instance whose vows,
  latches, and partial surface can be written down and read back makes every
  replica able to serve every id, and affinity stops being a routing problem.
  The instance is memory resident today, so this is the piece a multi replica
  bridge is built on, and it is a runtime question, not a gRPC question.

The third is the right one and it is the expensive one. Note what it implies: the
same serialization that solves affinity also solves the orphan, because an
instance that can be written down can be handed to a reaper that is a scheduled
job over the store rather than a thread racing a table.

## The typed .proto mapping

`payment_bridge.proto` is hand written to the shape ashc should emit. Every
pledge is its own rpc. This is the choice that makes step 2 work: a Go or Java
consumer gets the argument and result types from protoc alone, with no generic
Value envelope and no dispatch on a pledge name string.

**Clean:**

- **Result to oneof.** Exact. proto3 `oneof` carries presence, so `Ok(false)` is
  representable and does not collide with an unset field, which is the trap a
  bare `bool` plus `int64` would have walked into. Appendix III's type mapping
  read is confirmed on this contract.
- **Vow to optional.** Exact. proto3 `optional` gives presence, and "unset takes
  the declared default" is what `ash_contract_sign` already does with an omitted
  override. The wire form and the ABI form agree without a rule.
- **The partial surface to repeated.** Appendix III's map ordering problem does
  not arise here, because nothing about the partial surface wants to be a `map`.
  `repeated string` preserves the insertion order Ashford pins as normative, for
  free. The problem is real but it is not this contract's problem.
- **The lifecycle to an enum.** `ASH_UNSIGNED` through `ASH_BROKEN` in the
  runtime's own numbering, and `UNSIGNED = 0` is already the zero value protobuf
  wants. Nothing to decide.
- **Business versus transport.** The split Ashford already drew holds perfectly.
  A pledge's `Err` is a value on an OK rpc; an Ashford status is a gRPC code.
  `Charge(-2.0)` answering `err=41` with `StatusCode.OK` is the single most
  reassuring line in the client, because it means the bridge did not smuggle
  business outcomes into the transport's error channel, which is the mistake
  every hand rolled RPC layer makes.

**Friction:**

- **NOT_FOUND is overloaded.** `ASH_ERR_NAME` maps to `NOT_FOUND`, and so does
  an unknown instance id. An unknown pledge and an unknown session arrive at the
  client as the same code and are separable only by reading the message string.
  Typed rpcs make `ASH_ERR_NAME` nearly unreachable, since protoc will not let a
  client name a pledge that does not exist, which mitigates it here and will not
  in a contract with dynamic surface.
- **ASH_ERR_VERSION to ABORTED is a stretch.** Nothing in gRPC means "your shape
  hash disagrees with mine". `ABORTED` is the closest and it is not close.
  Underneath it is Appendix III's schema evolution question showing up as a
  concrete field: `expected_hash` in `SignRequest` imports Ashford's refusal into
  a protocol whose whole tolerance story is why it won. The field works. Whether
  Ashford wants it on a wire that believes the opposite is unanswered, and this
  prototype does not settle it.
- **One result message per shape.** All four payment pledges return
  `Result<Bool, Int>`, so `BoolIntResult` serves all four. A real contract needs
  one message per distinct result type. Mechanical for ashc to emit and dedupe by
  shape, but it means the emitted `.proto` has messages the contract's author
  never wrote and will read in error messages.
- **The Future flattens.** In process, `fulfill` returns a Future and `wait`
  collects it; the bridge only exposes the fused `fulfill_sync`. This is closer
  to fine than it looks, because a gRPC client's own async stub is the Future,
  and the concurrency lands on the client where it already had a shape. It does
  mean the async surface is the transport's, not the contract's.
- **By reference has no analog.** Already refused across the wire, so the Greeter
  shout path is not in this proto and nothing here changes that reading.

**Is the shape clear enough for ashc to emit in step 2?** Yes. Every message in
this file is a mechanical function of the contract's declarations: vows to
optional fields on `SignRequest`, each pledge to an rpc with a request message of
its parameters plus the instance id, each result type to a oneof message, the
partial surface and the lifecycle fixed for every contract. Writing it by hand
turned up no decision that needed taste, only the four frictions above, and none
of them is about generation. `Debug` is the one message ashc must never emit: it
exposes the instance table and it exists here only so the client can watch the
reaper work.

## The verdict: a weekend or a quarter

Both, and the split is the finding.

**The bridge is a weekend.** This one was. The session model works, the type
mapping is mechanical, the business versus transport split holds without
argument, and the walk matched the in process demo check for check. Five
consecutive green runs, no flake. A single server serving one contract to typed
generated clients is a solved problem and step 2, emitting this `.proto` from
ashc and driving it from Go, is small and should be done next. The language
coverage objection dies cheaply.

**The session is a quarter.** Every cost in this document is a lifetime cost or a
routing cost, and neither is about gRPC or about types. Correct lifetimes mean
moving off unary onto a stream so a dead client is a fact rather than a guess.
Multi replica means an instance that can be written down and read back, which is
runtime work in Layer 3, not bridge work. Neither is optional for anything real,
and neither is what a weekend buys.

So Appendix III's sequence holds, with a correction to step 1's own framing: the
load bearing unknown was not whether an instance can live behind gRPC. It can,
and easily. The load bearing unknown is who ends it, and unary gRPC's answer is a
timer, which is not an answer a contract language should accept. Prototype the
streaming session next, and let the store answer affinity. Retire the custom wire
after that, not before.
