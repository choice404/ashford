# The gRPC bridge, step 1: what the unary prototype found

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

# The gRPC bridge, step 1b: the session as a stream

Step 1 named the rule the TTL was standing in for and said it already exists in
gRPC, one layer up. This is that rule, taken. `Sign` is now
`rpc Session(SignRequest) returns (stream SessionEvent)`: it signs, yields the
signature as the stream's first event, and then holds. The instance lives
exactly as long as the stream. Everything else is untouched on purpose. The
pledge rpcs are still one typed unary call each carrying the instance id,
because that is what step 2 consumes and nothing about lifetimes should be
allowed to cost it.

The reaper is gone. Not reduced to a backstop, gone, and the reason is the
finding below.

## Does the stream own the lifetime

Yes, and the measurement is not close.

A child process opens a session, is killed with no break and no close, and the
server has the instance broken and dropped in **1ms**, on five consecutive runs
with no spread. Step 1 waited out a 2s timer and then presumed. The same fact,
three orders of magnitude apart, and one of them is a fact.

The other direction holds too. A client that holds its stream and says nothing
for **6s, three times the old TTL**, keeps its instance, keeps its state, and
resumes its walk on latches it set before the silence. Step 1's server would
have broken that instance three times over. This is the human approval case from
cost 3, and it now runs in the same server, with the same code, as the payment
walk that finishes in milliseconds. There is no number to pick because there is
no number.

`context.add_callback` fires on every way a stream ends: an explicit cancel, a
channel close, a dead process, a dropped connection. The handler blocks on an
event, the callback sets it, and the table pop is the arbiter so the break
happens exactly once no matter which of the three paths, callback, handler exit,
or shutdown, gets there first. That is the whole mechanism, and it is smaller
than the reaper it replaced. The table shrank with it: entries used to be
rewritten on every touch to feed the sweep, and now they are written once and
read until the stream drops them. `touch`, `peek`, `idle_candidates`, and the
recheck under the instance lock all went away. The per instance lock stayed,
because the race it guards is real and unchanged: a pledge call in flight when
the stream terminates. The end path pops the row first so no new call can
resolve the id, then takes the lock, which waits out any call admitted before
the pop, and only then breaks.

The hazard step 1 called invisible until it corrupts under load is still there
and still needs the lock. Streams did not fix it. It just got smaller, because
the thing racing the call is now an event rather than a thread that wakes up
four times a second looking for work.

## The four costs, revisited

All four go, and it is worth being precise about how, because they do not go the
same way.

1. **A timer breaks contracts.** Gone. There is no timer in the server. Every
   break it now performs has a cause the contract could name: its own break
   line, an explicit `Break`, or the party holding the obligation ceasing to
   exist. That last one is the in process semantics restated, not invented. Step
   1's complaint was that a process that goes away takes its obligations with it
   honestly and the bridge had no analog. The stream is the analog.
2. **A slow client and a dead client are the same client.** Gone. They are now
   different, and the difference is observed rather than inferred. A slow client
   is a quiet client and a quiet client is a live one.
3. **One TTL cannot serve two lifetimes.** Gone, and this is the one that could
   not have been fixed by picking better. Both lifetimes are the same code now.
4. **The break is not the end of the row.** Dissolved rather than answered. The
   row still outlives the break, and it still must, because a broken instance
   answering `NOT_FOUND` would lose a distinction the C ABI keeps. What changed
   is who collects it. The tombstone is now owned by the stream: it reads broken
   for exactly as long as somebody is there to read it, and it leaves when they
   leave. There is no second TTL guessing at a second thing, because there is no
   first one. The client asserts this directly: break explicitly, read `BROKEN`
   off the still open stream, close the stream, and the id is `NOT_FOUND`.

Note what that list is. Every one of these was a cost of the unary choice, which
is what step 1 predicted, and paying for the stream is what collects all four at
once.

## What the stream costs, honestly

Three things, and the first is the one that matters.

**The stream is a hard liveness coupling, and it trades one failure for
another.** A client that is alive, well, and intends to finish loses its
instance if the connection blips. Under a TTL of an hour, a thirty second
partition was survivable; under a stream it is not. This is real and it is the
strongest argument against this design, so take it at full strength: the TTL was
more forgiving of a bad network, and forgiveness is what a long lived contract
on a real network wants.

It is still the right trade, on three counts and against one.

- **It is an event, not a guess.** The server acts on something that happened.
  When it is wrong, it is wrong because the network lied, which is a failure the
  whole stack already knows how to talk about. When the TTL was wrong, it was
  wrong because a number was wrong, which is a failure nobody can debug.
- **The client learns.** A broken stream breaks at both ends. The client knows
  its instance is gone at the moment it is gone, and can resign. Under the TTL
  the client found out later, by getting `NOT_FOUND` from its next call, which
  is both strictly later and indistinguishable from an id it never had.
- **It is the transport's problem, tuned in the transport's language.**
  Keepalives, deadlines, and retry policy are gRPC's, they are configured per
  deployment, and they are not the application inventing a lifecycle rule. The
  TTL was Ashford making a networking decision in a contract runtime, which is
  the wrong place for it.
- **Against:** none of that gives back the thirty second partition. A stream is
  strictly less tolerant than a long timer.

That last point has an answer and it is the same answer as before. An instance
that can be written down survives losing its stream, because the row is not the
memory. This is now the third distinct problem Layer 3 has answered from this
document: affinity under a load balancer, the orphan as a scheduled job over the
store rather than a thread racing a table, and now partition tolerance for a
long lived session. Three unrelated symptoms, one cause, and the cause is that
an instance is memory resident. That convergence is the most useful thing in
this file.

**A session holds a thread.** The sync Python server dedicates a pool worker to
each instance for its whole life, so the pool size is the concurrency ceiling
and `max_workers` went from 8 to 32 to hold this walk's sessions plus the pledge
calls driving them. This is a property of a sync threaded server, not of gRPC,
and an async server or a Go server does not have it. It is named here anyway
because a naive port of this server into any sync threaded language inherits it
silently and discovers it at exactly the wrong time.

**The break at stream end is still a break the contract did not write.** It has
a defensible reading now, the holder is gone, which the timer never had. It is
still not something `payment.ash` can see coming or write a policy about. The
honest statement is that the bridge no longer invents an ending, it reports one,
and reporting is a different act from inventing. That closes cost 1 as written.
It does not mean a contract language has nothing left to say about what a
counterparty's disappearance means, and it probably does have something to say.
That question is now askable, which it was not while a timer was answering it.

## An explicit break zeroes the payload, and the bridge inherits that

Writing the tombstone check turned this up and it is worth recording. An
explicit `ash_contract_break` zeroes the stored Err payloads, deliberately,
because they point into the heap the break frees, and it keeps the latches so
the partial surface still names which pledges landed and which broke. So step
1's line about the partial surface still reading the payload belongs to the
contract's **own** break line, not to an explicit break. After
`Charge(-2.0)` the errors read `[("charge", 41)]`; after an explicit `Break` on
top of that they read `[]`, and `broken` still reads `["Processing"]`.

The bridge reproduces this exactly without knowing about it, which is the
fidelity result and is reassuring. It is also a note for step 2: a Go client
that reads `errors` after calling `Break` gets an empty list, the `.proto` says
nothing about why, and nothing in the generated surface would lead anyone to
expect it. That is a documentation obligation on the emitted contract, not a bug
in either layer.

## What step 2 inherits

The typed pledge surface is untouched, which was the requirement. A Go consumer
still gets `client.Charge(ctx, &ChargeRequest{...})` from protoc alone, with the
argument and result types the contract declared and no dispatch on a string.
Nothing about the lifetime change reached the part step 2 is about.

What protoc does not write is the session. A Go consumer must call
`client.Session(ctx, req)`, take the first message off `stream.Recv()`, keep
both the stream and its context alive for the instance's life, and cancel when
done. That is idiomatic Go and it is small, a struct holding the stream and a
`defer cancel()`, maybe twenty lines. It is not free. The failure mode of
getting it wrong, letting the stream variable fall out of scope or letting a
request scoped context cancel, is losing the instance, and it will be somebody's
first bug.

So step 1's claim needs one word changed. It said protoc alone kills the
language coverage objection. It is protoc plus a small emitted wrapper, and the
objection is just as dead, because a wrapper that holds a stream and cancels it
is twenty lines in every gRPC language and ashc emits it once per contract from
the same declarations it already reads. The consequence for step 2 is concrete:
**ashc emits a session wrapper, not only a `.proto`**. Worth knowing before the
Go client is written rather than after.

The trade is also worth stating plainly, because it reads as a loss and is not.
The stream adds an object the consumer must hold and removes a guess the server
had to make. Under unary the consumer held nothing, which was the appeal, but it
also meant a consumer had no way to say it was finished except to stop calling
and let a timer notice. Holding a stream is how a client says "still mine", and
closing it is how a client says "done". Those are things a contract's counterparty
should be able to say.

## The verdict: the quarter moved

Step 1 said the bridge is a weekend and the session is a quarter, and split them
because correct lifetimes meant moving off unary onto a stream and multi replica
meant an instance that can be written down.

**The streaming half was a day.** The proto change is one rpc and one message.
The server got smaller. The client's new checks are the only part that took real
thought, and only because measuring a death honestly means killing a real
process. Five consecutive green runs, no flake, 1ms every time.

So the quarter is smaller than step 1 priced it, and what is left of it is not
bridge work at all. Affinity, partition tolerance, and an instance that outlives
its connection are one piece of work, and it is Layer 3. **The session is no
longer a quarter of transport work. It is a quarter of runtime work, and it is
the same quarter Appendix III already keeps Layer 3 for.** The bridge does not
need a quarter of its own.

Appendix III's sequence holds, and step 1's correction to it survives intact
with its answer filled in. The load bearing unknown was who ends an instance.
The answer is the party that holds it, and gRPC will tell you the moment they
stop. Emit the `.proto` and the session wrapper from ashc and drive it from Go
next, let the store answer everything left in this file, and retire the custom
wire after that.

# The gRPC bridge, step 2: the surface out of the compiler

Steps 1 and 1b proved the shape by hand: a hand written `.proto` for one
contract, a hand written session object in one language. Step 2 makes ashc
write both, and proves the emitted surface with the consumer the whole
exercise was aimed at, a Go client that has never heard of Ashford.

## What ashc emits

`ashc emit-proto skeleton/payment.ash` runs the same front end as `build` and
writes two files. `payment.proto` is the step 1b surface exactly: one typed
rpc per pledge, the streaming `Session` whose lifetime is the instance's, the
Result oneof, the partial surface with its repeated name lists, and no Debug
rpc, because the instance table is server internal and nothing shipped should
expose it. `payment_session.go` is the wrapper step 1b said protoc cannot
write: open, first event, handle, Close, about forty lines, in the same Go
package as protoc's own output so the two halves import as one.

The wrapper also pins the shape hash. `shape_string` and `fnv1a64` are the
same calls the module emitter and the header emitter make, so the constant in
the Go source and the hash the compiled module registers cannot disagree. A
Go client that signs with `paymentpb.PaymentServiceShapeHash` and gets a
signature back has proven the emitted surface and the loaded module describe
the same contract, end to end, before the first pledge runs.

Both files are byte stable and pinned as goldens, the same discipline the
module C and the header live under. `make test-proto` diffs them on every
run of the suite.

## The Go walk

`make test-grpc-go` builds a client from nothing but the emitted artifacts,
protoc's output over the emitted `.proto` plus the emitted wrapper, and runs
the whole payment lifecycle against the Python server from step 1b. Every
assertion the Python client makes, the Go client makes, minus the Debug
counts: where Python counts rows, Go reads the row's own fate, present until
its stream ends, NOT_FOUND after. The vow override lands, half a subcontract
moves nothing, the value Err crosses as err=41 on an OK rpc, the automatic
break keeps its payload and the explicit break zeroes it, a killed client's
instance is collected in milliseconds, and a session quiet for three times
the old timer resumes on latches set before the silence.

Two things surfaced that the Python walk could not have said:

- **The wire compatibility claim is now a fact, not a comment.** The hand
  written proto said "the shape ashc emits in step 2" at the top. The Go
  client is generated from the emitted proto and the server from the hand
  one, and every call routes: same package, same service, same field
  numbers. The comment is now enforced by a passing gate.
- **A lying hash is ABORTED.** Shape skew at sign is ASH_ERR_VERSION in the
  runtime, and the bridge's status table maps it to ABORTED, not
  INVALID_ARGUMENT. Fair: it is not a malformed request, it is two builds
  disagreeing about the world. The wrapper's pinned constant is what makes
  the honest path the easy one.

## What the language coverage objection is now

Step 1b closed with "emit the .proto and the session wrapper from ashc and
drive it from Go". Both are done. The objection Appendix III kept open, that
the bridge only spoke languages someone hand wrote a binding for, is dead:
any language protoc speaks gets the typed surface from the emitted proto,
and the session wrapper is a per language template of about forty lines with
exactly one idea in it. Go's is written by the compiler today; another
language's is a morning, not a milestone.

What remains is what 1b already said remains: the instance that outlives its
connection, affinity, and partition tolerance are one piece of work and it
is Layer 3. The surface is out of the compiler; the store is the rest of the
quarter.

## The type set, filled in

Step 2 shipped with the scalar set and the walk that needed nothing more.
The declared types cross now: a record is a message with its fields in
declaration order; a sum whose arms are all bare is an enum numbered in
declared arm order, the ABI's own tag space; a sum with a payload arm is a
message whose oneof carries one message per arm in the same order; Unit is
the empty message; Option and List ride oneof arms as wrapper messages and
sit in fields as optional and repeated. A pledge may answer a bare Option,
the shape the stdlib's min_of and index_of already have, and it crosses as
the wrapper directly.

Two names earned rules. A pledge spelled sign, which the stdlib has, would
take the session surface's SignRequest, so its request message alone steps
aside to SignPledgeRequest; a pledge whose rpc spelling would take Session,
GetPartial, or Break is refused by name. Map and tuple stay named refusals,
the gauntlet's snapshot pledge being the standing proof, a nonzero exit
that writes nothing rather than a service with a hole in it.

Every skeleton and the whole stdlib emit and pass protoc now, the gauntlet
excepted by design. The goldens pin the walk: hello's enum Err, ledger's
Unit, main_demo's payload sum under two services, std_user's four contracts
with the prefix rule, the wrappers, and the reserved spelling.
