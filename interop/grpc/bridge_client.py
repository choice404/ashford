"""bridge_client.py: the demo_payment.py walk, driven over gRPC instead of
the C ABI, asserting the same answers, with the session now held as a stream.

This is the measurement. demo_payment.py signs an instance in process and
reads its partial surface through ctypes; this client never touches libashrt,
holds no handle, and knows the instance only by a uint64 the server issued on
a stream it keeps open. If the two agree pledge for pledge and state for
state, the session model carries the contract across a process boundary.

Every check step 1 made still has to hold, because step 1b changed who ends an
instance and nothing else: the vow override lands, the pending order holds,
half a subcontract moves nothing, Validation lands and the instance goes
partial, charge answers its declared Bool through a Python body two boundaries
away, the instance fulfills, Charge(-2.0) answers err=41 on an OK rpc and the
contract's own break line fires, a fulfillment against a broken instance is
FAILED_PRECONDITION, an id nobody was issued is NOT_FOUND.

Four checks are new, and they are what step 1b was built to take:

  - A client that dies takes its instance with it, at once. Step 1 could only
    guess with a 2s idle timer. This kills a real child process and measures
    how long the server takes to know, which should be a fraction of that.
  - A client that goes quiet for several times that old timer, and holds its
    stream, keeps its instance. This is the contract waiting on a human
    approval, the case no single TTL could serve.
  - An explicit Break still reads broken through the stream, then the row
    leaves with the stream. The tombstone is owned, not timed.
  - The server takes no ttl flag, because there is no timer in it. The idle
    check above is the behavioral half of that claim.

Exit status 0 means every check below held.
"""

import argparse
from concurrent import futures
import string
import subprocess
import sys
import threading
import time
from pathlib import Path

import grpc

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "target" / "grpc-gen"))

import payment_bridge_pb2 as pb  # noqa: E402
import payment_bridge_pb2_grpc as pb_grpc  # noqa: E402

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[bridge_client] FAIL: {what}", file=sys.stderr)
        _failures = 1


def expect_code(fn, code, what):
    """Demands a gRPC error with a given code. A pledge's Err never lands
    here: only a fulfillment that did not run does."""
    try:
        fn()
        check(False, f"{what} did not fail")
    except grpc.RpcError as e:
        check(e.code() == code, f"{what} is {code.name}, got {e.code().name}")


class Session:
    """The client half of the session. Opening it signs; the first event off
    the stream is the signature; holding the object holds the instance. This
    is the ergonomic question step 2 inherits: a Go or Java consumer gets
    typed pledge calls from protoc for free, but this object, the thing that
    must stay alive and must be closed, is not something protoc writes."""

    def __init__(self, stub, request, stream_call=None):
        self._stream = (stub.Session if stream_call is None else stream_call)(request)
        event = next(self._stream)
        assert event.WhichOneof("event") == "signed"
        self.signed = event.signed
        self.instance_id = event.signed.instance_id

    def close(self):
        self._stream.cancel()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def live(stub):
    return stub.Debug(pb.DebugRequest()).live_instances


def wait_for_live(stub, want, timeout):
    """Polls the debug count until it reaches want, and answers how long that
    took. Debug never touches an instance, so polling cannot itself change
    what is being measured."""
    t0 = time.monotonic()
    deadline = t0 + timeout
    while time.monotonic() < deadline:
        if live(stub) == want:
            return time.monotonic() - t0
        time.sleep(0.005)
    return None


def park_walk(stub):
    """Parks a partly progressed session and leaves its token as the one
    machine readable line, for the restart half of the round trip."""
    check(live(stub) == 0, "the park table starts empty")
    with Session(stub, pb.SignRequest(currency="EUR")) as session:
        signed = session.signed
        token = signed.park_token
        check(signed.instance_id != 0, "the park session issued an instance id")
        check(signed.shape_hash != 0,
              "the park session carries a nonzero shape hash")
        check(len(token) == 32 and all(ch in string.hexdigits for ch in token),
              "the park session carries a hex park token")
        out = stub.ValidateCard(pb.ValidateCardRequest(
            instance_id=session.instance_id, card="4111 1111"))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the park session latched validate_card")
        print(token, flush=True)

    check(wait_for_live(stub, 0, 5.0) is not None,
          "closing the park stream dropped its row")
    expect_code(lambda: stub.GetPartial(
        pb.PartialRequest(instance_id=signed.instance_id)),
        grpc.StatusCode.NOT_FOUND, "the parked instance id")


def resume_walk(stub, token):
    """Reopens one parked session and proves its latch crossed the store and
    a server restart, while the bad and repeated token cases stay one shot."""
    check(live(stub) == 0, "the resumed server table starts empty")
    expect_code(lambda: Session(
        stub, pb.ResumeRequest(park_token="not-a-park-token"), stub.Resume),
        grpc.StatusCode.NOT_FOUND, "a garbage park token")
    expect_code(lambda: Session(
        stub, pb.ResumeRequest(park_token=token, expected_hash=12345),
        stub.Resume), grpc.StatusCode.ABORTED, "a wrong resume shape hash")

    with Session(stub, pb.ResumeRequest(park_token=token), stub.Resume) as session:
        signed = session.signed
        iid = session.instance_id
        check(iid != 0, "the resumed stream issued a fresh instance id")
        check(signed.park_token == token,
              "the resumed stream kept its park token")

        expect_code(lambda: Session(
            stub, pb.ResumeRequest(park_token=token), stub.Resume),
            grpc.StatusCode.NOT_FOUND, "a consumed park token")

        out = stub.ValidateAmount(pb.ValidateAmountRequest(instance_id=iid,
                                                           amount=25.0))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the resumed validation fulfilled")
        partial = stub.GetPartial(pb.PartialRequest(instance_id=iid))
        check(partial.state == pb.PARTIAL and
              list(partial.fulfilled) == ["Validation"],
              "the validate_card latch crossed park and resume")

        out = stub.Charge(pb.ChargeRequest(instance_id=iid, card="4111 1111",
                                           amount=25.0))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the resumed charge fulfilled")
        out = stub.NotifyUser(pb.NotifyUserRequest(instance_id=iid, ok=True))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the resumed notify_user fulfilled")
        partial = stub.GetPartial(pb.PartialRequest(instance_id=iid))
        check(partial.state == pb.FULFILLED,
              "the resumed instance fulfilled")

    check(wait_for_live(stub, 0, 5.0) is not None,
          "closing the resumed stream dropped its row")


def race_resume(token, port, peer_port):
    """Races two servers against the same parked token. One stream should
    stand the contract up, and the other should see the sqlite DELETE already
    spent by its peer."""
    ch = grpc.insecure_channel(f"127.0.0.1:{port}")
    peer_ch = grpc.insecure_channel(f"127.0.0.1:{peer_port}")
    channels = (ch, peer_ch)
    sessions = []
    try:
        for channel in channels:
            grpc.channel_ready_future(channel).result(timeout=10)

        stub = pb_grpc.PaymentServiceStub(ch)
        peer_stub = pb_grpc.PaymentServiceStub(peer_ch)
        barrier = threading.Barrier(2)

        def attempt(one_stub):
            barrier.wait()
            try:
                return Session(one_stub,
                               pb.ResumeRequest(park_token=token),
                               one_stub.Resume)
            except grpc.RpcError as e:
                return e

        with futures.ThreadPoolExecutor(max_workers=2) as pool:
            future = pool.submit(attempt, stub)
            peer_future = pool.submit(attempt, peer_stub)
            results = ((stub, future.result()),
                       (peer_stub, peer_future.result()))

        winners = [(one_stub, result) for one_stub, result in results
                   if isinstance(result, Session)]
        losers = [result for _, result in results
                  if isinstance(result, grpc.RpcError)]
        sessions = [result for _, result in results
                    if isinstance(result, Session)]
        check(len(winners) == 1 and len(losers) == 1 and
              losers[0].code() == grpc.StatusCode.NOT_FOUND,
              "exactly one replica claimed the park token")
        if len(winners) != 1:
            return

        winner_stub, session = winners[0]
        iid = session.instance_id
        out = winner_stub.ValidateAmount(
            pb.ValidateAmountRequest(instance_id=iid, amount=25.0))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the raced validation fulfilled")
        partial = winner_stub.GetPartial(pb.PartialRequest(instance_id=iid))
        check(partial.state == pb.PARTIAL and
              list(partial.fulfilled) == ["Validation"],
              "the raced validate_card latch crossed park and resume")

        out = winner_stub.Charge(pb.ChargeRequest(instance_id=iid,
                                                  card="4111 1111",
                                                  amount=25.0))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the raced charge fulfilled")
        out = winner_stub.NotifyUser(
            pb.NotifyUserRequest(instance_id=iid, ok=True))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the raced notify_user fulfilled")
        partial = winner_stub.GetPartial(pb.PartialRequest(instance_id=iid))
        check(partial.state == pb.FULFILLED,
              "the raced instance fulfilled")

        session.close()
        sessions.remove(session)
        check(wait_for_live(winner_stub, 0, 5.0) is not None,
              "closing the raced stream dropped its row")
    except grpc.FutureTimeoutError:
        print("[bridge_client] FAIL: server never came up", file=sys.stderr)
        global _failures
        _failures = 1
    finally:
        for session in sessions:
            session.close()
        for channel in channels:
            channel.close()


def walk(stub, legacy_ttl, port):
    check(live(stub) == 0, "the table starts empty")

    # ---- the partial path, signed with a vow override ----
    # demo_payment.py: rt.sign("PaymentService", vows={"currency": "EUR"})

    with Session(stub, pb.SignRequest(currency="EUR")) as s1:
        c1 = s1.instance_id
        check(c1 != 0, "the session stream issued an instance id")
        check(s1.signed.currency == "EUR",
              "vow override landed across the wire")
        check(s1.signed.shape_hash != 0 and s1.signed.signed_at > 0,
              "c1 carries a signature")
        print(f"[bridge_client] session {c1} open, currency "
              f"{s1.signed.currency}")

        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(list(p.pending) == ["Validation", "Processing", "notify_user"],
              "c1 pending order is subs then loose pledges")

        out = stub.ValidateCard(pb.ValidateCardRequest(instance_id=c1,
                                                       card="4111 1111"))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "validate_card Ok")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(p.state == pb.SIGNED, "half a subcontract moves nothing")

        out = stub.ValidateAmount(pb.ValidateAmountRequest(instance_id=c1,
                                                           amount=25.0))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "validate_amount Ok")

        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(p.state == pb.PARTIAL, "Validation lands, c1 partial")
        check(list(p.fulfilled) == ["Validation"] and
              list(p.pending) == ["Processing", "notify_user"] and
              list(p.broken) == [] and list(p.errors) == [],
              "partial names after Validation")
        print(f"[bridge_client] state PARTIAL, fulfilled {list(p.fulfilled)}, "
              f"pending {list(p.pending)}")

        # charge is the Python body behind the abstract pledge: the answer
        # crosses two boundaries, the C ABI into Python and Python out over
        # gRPC.
        out = stub.Charge(pb.ChargeRequest(instance_id=c1, card="4111 1111",
                                           amount=25.0))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "charge Ok carries the declared Bool")

        out = stub.NotifyUser(pb.NotifyUserRequest(instance_id=c1, ok=True))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "notify_user Ok")

        p = stub.GetPartial(pb.PartialRequest(instance_id=c1))
        check(p.state == pb.FULFILLED, "c1 fulfilled")
        check(len(p.fulfilled) == 3 and list(p.pending) == [] and
              list(p.errors) == [], "every item fulfilled")
        print("[bridge_client] state FULFILLED")

    # A fulfilled instance leaves with its stream the same as any other. The
    # walk is over; the client said so by closing, and it did not have to say
    # anything else.
    check(wait_for_live(stub, 0, 5.0) is not None,
          "closing c1's stream dropped its row")

    # ---- the Err path and the automatic break ----
    # The contract's Err is a value: an OK rpc carrying err=41.

    with Session(stub, pb.SignRequest()) as s2:
        c2 = s2.instance_id
        check(s2.signed.currency == "USD", "c2 signs on the declared default")

        out = stub.Charge(pb.ChargeRequest(instance_id=c2, card="4111 1111",
                                           amount=-2.0))
        check(out.WhichOneof("result") == "err" and out.err == 41,
              "charge Err returns to the caller as a value, not an rpc error")
        print(f"[bridge_client] charge answered err={out.err} on an OK rpc")

        p = stub.GetPartial(pb.PartialRequest(instance_id=c2))
        check(p.state == pb.BROKEN, "the break line fired by itself")
        check(list(p.broken) == ["Processing"] and
              list(p.pending) == ["Validation", "notify_user"],
              "c2 broken lists Processing")
        check([(e.pledge, e.err) for e in p.errors] == [("charge", 41)],
              "the automatic break kept the Err payload readable")

        # Fulfillment against a broken instance is ASH_ERR_STATE, which is the
        # one Ashford status this walk turns into a gRPC error.
        expect_code(lambda: stub.ValidateCard(
            pb.ValidateCardRequest(instance_id=c2, card="4111 1111")),
            grpc.StatusCode.FAILED_PRECONDITION,
            "fulfillment after automatic break")

        # An explicit break, then a fulfillment, is the same state error. The
        # row outlives the break on purpose, so a broken instance still says
        # broken instead of saying it never existed. What holds it up now is
        # the stream, not a second guess at a timer.
        stub.Break(pb.BreakRequest(instance_id=c2))
        expect_code(lambda: stub.ValidateCard(
            pb.ValidateCardRequest(instance_id=c2, card="4111 1111")),
            grpc.StatusCode.FAILED_PRECONDITION,
            "fulfillment after an explicit break")

        # The row is still there to be read, which is the point: the stream is
        # up, so the instance's broken state is still the client's to see.
        # What it reads is exactly what an in process host reads after an
        # explicit break, down to the Err payloads being gone. The runtime
        # zeroes them because they point into the heap the break frees, and
        # keeps the latches, so the partial surface still names which pledges
        # landed and which broke. The payload the check above read survived
        # the contract's own break line, not this one. The bridge inherits the
        # distinction without knowing about it.
        p = stub.GetPartial(pb.PartialRequest(instance_id=c2))
        check(p.state == pb.BROKEN,
              "an explicitly broken instance still reads broken on its stream")
        check(list(p.broken) == ["Processing"] and
              list(p.pending) == ["Validation", "notify_user"],
              "the explicit break kept the partial names readable")
        check([(e.pledge, e.err) for e in p.errors] == [],
              "the explicit break zeroed the Err payloads, as it does in process")
        check(live(stub) == 1, "the broken row is still live behind its stream")

    # Now the stream goes, and so does the row. The distinction the C ABI
    # keeps, broken versus never existed, held for exactly as long as somebody
    # was there to read it.
    check(wait_for_live(stub, 0, 5.0) is not None,
          "closing a broken instance's stream dropped its row")
    expect_code(lambda: stub.GetPartial(pb.PartialRequest(instance_id=c2)),
                grpc.StatusCode.NOT_FOUND, "c2's id after its stream closed")
    print("[bridge_client] the broken row left with its stream")

    # ---- an id nobody was issued ----

    expect_code(lambda: stub.GetPartial(pb.PartialRequest(instance_id=999999)),
                grpc.StatusCode.NOT_FOUND, "unknown instance id")

    # ---- a client that dies ----
    # Step 1's orphan, and the reason for this prototype. A child process
    # opens a session and is killed without a Break, without a close, without
    # saying anything. Step 1 could only wait out a 2s idle timer and presume.
    # Here the dead stream is a fact and the server has it in milliseconds.

    child = subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()),
         "--hold-session", "--port", str(port)],
        stdout=subprocess.PIPE, text=True)
    try:
        line = child.stdout.readline().strip()
        check(line.isdigit(), f"the child opened a session, said {line!r}")
        doomed = int(line)
        check(live(stub) == 1, "the child's instance is live")

        child.kill()
        t0 = time.monotonic()
        child.wait()
        settled = wait_for_live(stub, 0, 5.0)
        elapsed = None if settled is None else time.monotonic() - t0
    finally:
        child.kill()
        child.wait()

    check(elapsed is not None, "a dead client's instance was collected")
    if elapsed is not None:
        print(f"[bridge_client] client died, instance {doomed} broken and "
              f"dropped in {elapsed * 1000:.0f}ms "
              f"(step 1 waited out a {legacy_ttl}s timer)")
        check(elapsed < 1.0,
              f"the death was noticed in well under the old {legacy_ttl}s "
              f"timer, took {elapsed:.3f}s")
    expect_code(lambda: stub.GetPartial(pb.PartialRequest(instance_id=doomed)),
                grpc.StatusCode.NOT_FOUND, "the dead client's id")

    # ---- a client that is merely quiet ----
    # The case no single TTL could serve: a contract legitimately idle far
    # past any number a payment walk would want, because it is waiting on
    # something slow. It holds its stream and says nothing, and step 1's
    # server would have broken it several times over.

    idle = legacy_ttl * 3
    with Session(stub, pb.SignRequest(currency="EUR")) as s3:
        c3 = s3.instance_id
        stub.ValidateCard(pb.ValidateCardRequest(instance_id=c3,
                                                 card="4111 1111"))
        print(f"[bridge_client] session {c3} going quiet for {idle}s, "
              f"{idle / legacy_ttl:.0f}x the old {legacy_ttl}s timer")
        time.sleep(idle)

        check(live(stub) == 1, "the quiet instance survived the old timer")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c3))
        check(p.state == pb.SIGNED and list(p.fulfilled) == [],
              "the quiet instance kept its state")

        # And it is not merely present, it still works: the latch validate_card
        # set before the silence is still set, and the walk resumes.
        out = stub.ValidateAmount(pb.ValidateAmountRequest(instance_id=c3,
                                                           amount=25.0))
        check(out.WhichOneof("result") == "ok" and out.ok is True,
              "the quiet instance resumed its walk")
        p = stub.GetPartial(pb.PartialRequest(instance_id=c3))
        check(p.state == pb.PARTIAL,
              "Validation lands after the silence, on latches set before it")
        print(f"[bridge_client] session {c3} resumed after {idle}s idle, "
              f"state PARTIAL")

    check(wait_for_live(stub, 0, 5.0) is not None,
          "the quiet instance left when its stream did")


def hold_session(port, currency):
    """The doomed child. Opens a session, says which instance it got, and then
    holds the stream until something kills it. It never breaks, never closes,
    and never gets a chance to: that is the whole point."""
    with grpc.insecure_channel(f"127.0.0.1:{port}") as ch:
        stub = pb_grpc.PaymentServiceStub(ch)
        stream = stub.Session(pb.SignRequest(currency=currency))
        event = next(stream)
        print(event.signed.instance_id, flush=True)
        for _ in stream:
            pass
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=50251)
    ap.add_argument("--legacy-ttl", type=float, default=2.0,
                    help="the idle timer step 1's server ran, kept only as "
                         "the yardstick the new checks are measured against")
    ap.add_argument("--peer-port", type=int, default=50258,
                    help="the second server port for --race-resume")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--park-walk", action="store_true",
                      help="park one partly progressed session and print its token")
    mode.add_argument("--resume-walk", metavar="TOKEN",
                      help="resume TOKEN and finish its parked walk")
    mode.add_argument("--race-resume", metavar="TOKEN",
                      help="race two servers to resume TOKEN")
    mode.add_argument("--hold-session", action="store_true",
                      help="internal: open a session and hold it until killed")
    args = ap.parse_args()

    if args.hold_session:
        return hold_session(args.port, "EUR")
    if args.race_resume:
        race_resume(args.race_resume, args.port, args.peer_port)
        if _failures:
            return 1
        print("[bridge_client] ok")
        return 0

    with grpc.insecure_channel(f"127.0.0.1:{args.port}") as ch:
        try:
            grpc.channel_ready_future(ch).result(timeout=10)
        except grpc.FutureTimeoutError:
            print("[bridge_client] FAIL: server never came up",
                  file=sys.stderr)
            return 1
        stub = pb_grpc.PaymentServiceStub(ch)
        if args.park_walk:
            park_walk(stub)
        elif args.resume_walk:
            resume_walk(stub, args.resume_walk)
        else:
            walk(stub, args.legacy_ttl, args.port)

    if _failures:
        return 1
    if not args.park_walk:
        print("[bridge_client] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
