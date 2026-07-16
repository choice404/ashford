"""bridge_client.py: the demo_payment.py walk, driven over gRPC instead of
the C ABI, asserting the same answers.

This is the measurement. demo_payment.py signs an instance in process and
reads its partial surface through ctypes; this client never touches libashrt,
holds no handle, and knows the instance only by a uint64 the server issued.
If the two agree pledge for pledge and state for state, the session model
carries the contract across a process boundary.

Four things it demands beyond the happy walk:

  - A pledge's Err is a value. Charge(-2.0) answers err=41 on an OK rpc, and
    the instance goes BROKEN by the contract's own break line, not by anything
    this client did.
  - Fulfilling a broken instance is FAILED_PRECONDITION, the ASH_ERR_STATE the
    in process host gets.
  - An instance id nobody was issued is NOT_FOUND.
  - An instance nobody breaks is reaped. Sign it, touch it once, walk away,
    and the server's live count returns to zero on its own. This is the one
    check with no in process twin: the C ABI never needed it, because the
    handle's owner was the process.

Exit status 0 means every check below held.
"""

import argparse
import sys
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


def wait_for_empty(stub, ttl):
    """Polls the debug count until the reaper has cleared the table. The
    bound is generous on purpose: a TTL check that asserts on a deadline
    measures the machine's load, not the server's behavior. Debug does not
    touch an instance, so polling cannot itself keep one alive."""
    deadline = time.monotonic() + ttl * 8 + 5.0
    while time.monotonic() < deadline:
        if stub.Debug(pb.DebugRequest()).live_instances == 0:
            return True
        time.sleep(0.05)
    live = stub.Debug(pb.DebugRequest()).live_instances
    print(f"[bridge_client] table never emptied, live={live}", file=sys.stderr)
    return False


def walk(stub, ttl):
    # ---- the partial path, signed with a vow override ----
    # demo_payment.py: rt.sign("PaymentService", vows={"currency": "EUR"})

    r = stub.Sign(pb.SignRequest(currency="EUR"))
    check(r.instance_id != 0, "sign issued an instance id")
    check(r.currency == "EUR", "vow override landed across the wire")
    check(r.shape_hash != 0 and r.signed_at > 0, "c1 carries a signature")
    c1 = r.instance_id
    print(f"[bridge_client] signed instance {c1}, currency {r.currency}")

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
    # crosses two boundaries, the C ABI into Python and Python out over gRPC.
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
    stub.Break(pb.BreakRequest(instance_id=c1))

    # ---- the Err path and the automatic break ----
    # The contract's Err is a value: an OK rpc carrying err=41.

    r = stub.Sign(pb.SignRequest())
    check(r.currency == "USD", "c2 signs on the declared default")
    c2 = r.instance_id

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
    # entry outlives the break on purpose, so a broken instance still says
    # broken instead of saying it never existed.
    stub.Break(pb.BreakRequest(instance_id=c2))
    expect_code(lambda: stub.ValidateCard(
        pb.ValidateCardRequest(instance_id=c2, card="4111 1111")),
        grpc.StatusCode.FAILED_PRECONDITION,
        "fulfillment after an explicit break")

    # ---- an id nobody was issued ----

    expect_code(lambda: stub.GetPartial(pb.PartialRequest(instance_id=999999)),
                grpc.StatusCode.NOT_FOUND, "unknown instance id")

    # ---- the orphan: signed, touched once, abandoned ----
    # No Break, no disconnect the server can see. Idle TTL is the only rule
    # that can collect this, and this is the check with no in process twin.
    # The broken instances above are tombstones on the same TTL, so the table
    # settles to empty on its own before the orphan is signed.

    check(wait_for_empty(stub, ttl), "the table settles to empty on its own")

    r = stub.Sign(pb.SignRequest(currency="EUR"))
    orphan = r.instance_id
    stub.ValidateCard(pb.ValidateCardRequest(instance_id=orphan,
                                             card="4111 1111"))
    check(stub.Debug(pb.DebugRequest()).live_instances == 1,
          "the orphan is live before the TTL")
    print(f"[bridge_client] abandoning instance {orphan}, waiting out the "
          f"{ttl}s ttl")

    check(wait_for_empty(stub, ttl), "the reaper collected the orphan")

    # The reaper broke it, so the id is gone from the table entirely.
    expect_code(lambda: stub.GetPartial(pb.PartialRequest(instance_id=orphan)),
                grpc.StatusCode.NOT_FOUND, "the reaped orphan's id")
    print(f"[bridge_client] instance {orphan} reaped, live_instances=0")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=50251)
    ap.add_argument("--ttl", type=float, default=2.0,
                    help="the server's idle ttl, so the orphan check knows "
                         "how long to wait")
    args = ap.parse_args()

    with grpc.insecure_channel(f"127.0.0.1:{args.port}") as ch:
        try:
            grpc.channel_ready_future(ch).result(timeout=10)
        except grpc.FutureTimeoutError:
            print("[bridge_client] FAIL: server never came up",
                  file=sys.stderr)
            return 1
        walk(pb_grpc.PaymentServiceStub(ch), args.ttl)

    if _failures:
        return 1
    print("[bridge_client] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
