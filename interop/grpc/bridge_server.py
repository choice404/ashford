"""bridge_server.py: one signed PaymentService instance per session, served
over gRPC. The prototype Appendix III asks for first, because the session is
the load bearing unknown: the C ABI got a session free from the connection
and gRPC gives one back for nothing.

The runtime side is ordinary. ashford.py loads libashrt and libpayment.ash.so
through ctypes, binds a Python charge over the abstract pledge exactly the way
demo_payment.py does, and freezes. What is new sits above that: a table
keyed by a server issued instance id, a lock over it, and a reaper thread
that breaks instances nobody touched inside the TTL.

Three lines this server holds, and they are the findings:

  1. The instance id is the session. Every rpc carries it, the table resolves
     it, an unknown one is NOT_FOUND. There is no other handle.
  2. A pledge's Err is a value. The contract answering Err(41) is a BoolIntResult
     with the err arm set and an OK rpc status. Only an Ashford status, a
     fulfillment that did not run, becomes a gRPC error.
  3. Nothing tells this server a client left. Unary gRPC has no disconnect
     signal a handler can hang cleanup off, so idle TTL is the only reaping
     rule available, and it is a guess about the client's intent.

Run it directly: python bridge_server.py --port 50251 --ttl 2.0
"""

import argparse
import sys
import threading
import time
from concurrent import futures
from dataclasses import dataclass, replace
from pathlib import Path

import grpc

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "interop" / "python"))
sys.path.insert(0, str(ROOT / "target" / "grpc-gen"))

import ashford  # noqa: E402
from ashford import (ASH_ERR_NAME, ASH_ERR_STATE, ASH_ERR_TYPE,  # noqa: E402
                     ASH_ERR_UNBOUND, ASH_ERR_VERSION, AshError, Err, Ok,
                     Runtime)

import payment_bridge_pb2 as pb  # noqa: E402
import payment_bridge_pb2_grpc as pb_grpc  # noqa: E402

OUT = ROOT / "target" / "ashc-out"

# The status map, and the whole point of the split. An Ashford status is a
# transport or lifecycle failure: the fulfillment did not run. A contract's
# own Err is a value that rides an OK rpc. This table says nothing about
# business outcomes, only about calls that never reached a body.
STATUS_TO_GRPC = {
    ASH_ERR_STATE: grpc.StatusCode.FAILED_PRECONDITION,
    ASH_ERR_NAME: grpc.StatusCode.NOT_FOUND,
    ASH_ERR_TYPE: grpc.StatusCode.INVALID_ARGUMENT,
    ASH_ERR_VERSION: grpc.StatusCode.ABORTED,
    ASH_ERR_UNBOUND: grpc.StatusCode.FAILED_PRECONDITION,
}

STATE_TO_PB = {
    ashford.ASH_UNSIGNED: pb.UNSIGNED,
    ashford.ASH_SIGNED: pb.SIGNED,
    ashford.ASH_FULFILLED: pb.FULFILLED,
    ashford.ASH_PARTIAL: pb.PARTIAL,
    ashford.ASH_BROKEN: pb.BROKEN,
}


def charge(inst, args):
    """The Python body behind the abstract PaymentService.charge, the same one
    demo_payment.py binds: a positive amount charges and answers Ok(True),
    anything else is Err(41). It reads the currency vow off the signed
    instance, which is the part a stateless service could not do without the
    session this server keeps."""
    card, amount = args
    if amount > 0:
        print(f"[bridge_server] charging {inst.vow('currency')} "
              f"{amount:.2f} to {card}", flush=True)
        return Ok(True)
    return Err(41)


@dataclass(frozen=True)
class Session:
    """One signed instance and its liveness bookkeeping. Frozen: a touch
    replaces the entry rather than mutating it, so a reader holding an entry
    holds a consistent snapshot. The lock is per instance and guards the
    contract handle itself against a reap racing a fulfillment."""
    instance_id: int
    contract: object
    created: float
    last_touched: float
    lock: threading.Lock


class InstanceTable:
    """The session store gRPC does not provide. Ids are server issued and
    monotonic, never client supplied, so a peer cannot name an instance it
    was not handed."""

    def __init__(self):
        self._lock = threading.Lock()
        self._next_id = 1
        self._live = {}

    def insert(self, contract):
        now = time.monotonic()
        with self._lock:
            iid = self._next_id
            self._next_id += 1
            self._live[iid] = Session(instance_id=iid, contract=contract,
                                      created=now, last_touched=now,
                                      lock=threading.Lock())
            return iid

    def touch(self, instance_id):
        """Resolves an id and marks it live in one step. The touch lands
        before the caller takes the per instance lock, so a reaper already
        holding that lock rechecks and stands down instead of breaking an
        instance with a call in flight."""
        with self._lock:
            s = self._live.get(instance_id)
            if s is None:
                return None
            fresh = replace(s, last_touched=time.monotonic())
            self._live[instance_id] = fresh
            return fresh

    def peek(self, instance_id):
        """Reads an entry without marking it live. The reaper needs this:
        touching an instance it is deciding to reap would keep it alive
        forever."""
        with self._lock:
            return self._live.get(instance_id)

    def remove(self, instance_id):
        with self._lock:
            return self._live.pop(instance_id, None)

    def count(self):
        with self._lock:
            return len(self._live)

    def idle_candidates(self, ttl):
        cutoff = time.monotonic() - ttl
        with self._lock:
            return [s for s in self._live.values() if s.last_touched <= cutoff]

    def drain(self):
        with self._lock:
            out = list(self._live.values())
            self._live.clear()
            return out


class Reaper(threading.Thread):
    """The orphan collector, and the honest cost of this design. A unary gRPC
    handler learns nothing about a client that walked away, so the only
    available rule is idle time: an instance untouched for ttl seconds is
    presumed abandoned, broken, and dropped. This is a guess. A slow client
    and a dead client look identical from here."""

    def __init__(self, table, ttl, interval):
        super().__init__(daemon=True, name="ash-reaper")
        self._table = table
        self._ttl = ttl
        self._interval = interval
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        while not self._stop.wait(self._interval):
            for s in self._table.idle_candidates(self._ttl):
                with s.lock:
                    # Recheck under the instance lock: a call that touched
                    # the entry after the sweep read it is still in flight,
                    # and its instance is not an orphan.
                    current = self._table.peek(s.instance_id)
                    if current is None:
                        continue
                    if current.last_touched > s.last_touched:
                        continue
                    self._table.remove(s.instance_id)
                    _break_quietly(s)
                    print(f"[bridge_server] reaped orphan instance "
                          f"{s.instance_id} after {self._ttl}s idle",
                          flush=True)


def _break_quietly(session):
    """Breaks an instance and swallows the state error a broken one answers.
    An instance the contract already broke by itself is not an error to
    reclaim."""
    try:
        session.contract.break_()
    except AshError as e:
        if e.status != ASH_ERR_STATE:
            raise


def _abort(context, err):
    """Maps an Ashford status onto a gRPC code. Anything unmapped is
    INTERNAL: the bridge does not invent a meaning it was not given."""
    code = STATUS_TO_GRPC.get(err.status, grpc.StatusCode.INTERNAL)
    context.abort(code, f"ashford status {err.status}: {err}")


def _result_pb(value):
    """Result<Bool, Int> to the oneof. Ok and Err are both ordinary answers
    here and both ride an OK rpc status."""
    if isinstance(value, Ok):
        return pb.BoolIntResult(ok=bool(value.value))
    if isinstance(value, Err):
        return pb.BoolIntResult(err=int(value.value))
    raise TypeError(f"charge answered {value!r}, not a Result")


class PaymentServicer(pb_grpc.PaymentServiceServicer):
    """One servicer over one frozen runtime. Every handler is the same three
    steps: resolve the id, run one runtime call under the instance lock, and
    map what came back."""

    def __init__(self, rt, table):
        self._rt = rt
        self._table = table

    def _session(self, instance_id, context):
        s = self._table.touch(instance_id)
        if s is None:
            context.abort(grpc.StatusCode.NOT_FOUND,
                          f"unknown instance {instance_id}")
        return s

    def _fulfill(self, context, instance_id, pledge, *args):
        s = self._session(instance_id, context)
        with s.lock:
            try:
                return _result_pb(s.contract.fulfill_sync(pledge, *args))
            except AshError as e:
                _abort(context, e)

    # ---- signing ----

    def Sign(self, request, context):
        vows = None
        if request.HasField("currency"):
            vows = {"currency": request.currency}
        try:
            c = self._rt.sign("PaymentService", vows=vows,
                              expected_hash=request.expected_hash)
        except AshError as e:
            _abort(context, e)
        iid = self._table.insert(c)
        print(f"[bridge_server] signed instance {iid}, currency "
              f"{c.vow('currency')}", flush=True)
        return pb.SignReply(instance_id=iid, currency=c.vow("currency"),
                            shape_hash=c.hash(), signed_at=c.signed_at())

    # ---- the pledges, one typed rpc each ----

    def ValidateCard(self, request, context):
        return self._fulfill(context, request.instance_id, "validate_card",
                             request.card)

    def ValidateAmount(self, request, context):
        return self._fulfill(context, request.instance_id, "validate_amount",
                             request.amount)

    def Charge(self, request, context):
        return self._fulfill(context, request.instance_id, "charge",
                             request.card, request.amount)

    def NotifyUser(self, request, context):
        return self._fulfill(context, request.instance_id, "notify_user",
                             request.ok)

    # ---- the partial surface and the break ----

    def GetPartial(self, request, context):
        s = self._session(request.instance_id, context)
        with s.lock:
            try:
                p = s.contract.partial()
                state = s.contract.state()
            except AshError as e:
                _abort(context, e)
        errors = [pb.PledgeError(pledge=name, err=int(val))
                  for name, val in p.errors if val is not None]
        return pb.PartialReply(state=STATE_TO_PB.get(state, pb.UNSIGNED),
                               fulfilled=p.fulfilled, pending=p.pending,
                               broken=p.broken, errors=errors)

    def Break(self, request, context):
        """Breaks the instance but leaves the entry standing. This is
        deliberate and it is a finding: in process, a broken handle still
        answers ASH_ERR_STATE to a fulfillment and still reads its partial
        surface, so the owner learns it broke. Dropping the row here would
        make a broken instance answer NOT_FOUND instead, indistinguishable
        from an id that never existed, and the bridge would lose a
        distinction the C ABI keeps. The row is a tombstone the reaper
        collects on the same TTL as any other idle instance."""
        s = self._session(request.instance_id, context)
        with s.lock:
            try:
                _break_quietly(s)
            except AshError as e:
                _abort(context, e)
        return pb.BreakReply()

    # ---- prototype only ----

    def Debug(self, request, context):
        """Not a contract surface. It exists so the client can watch the
        reaper collect an instance nobody broke, which is the measurement
        this prototype was built to take."""
        return pb.DebugReply(live_instances=self._table.count())


def serve(port, ttl, reap_interval):
    rt = Runtime(OUT / "libashrt.so")
    rt.load(OUT / "libpayment.ash.so")
    rt.bind("PaymentService.charge", charge)
    rt.freeze()

    table = InstanceTable()
    reaper = Reaper(table, ttl, reap_interval)
    reaper.start()

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    pb_grpc.add_PaymentServiceServicer_to_server(
        PaymentServicer(rt, table), server)
    bound = server.add_insecure_port(f"127.0.0.1:{port}")
    server.start()
    print(f"[bridge_server] serving PaymentService on 127.0.0.1:{bound}, "
          f"ttl {ttl}s", flush=True)
    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        pass
    finally:
        # Shutdown breaks every instance still standing. A signed contract
        # that outlives its server is an obligation nobody can keep.
        reaper.stop()
        server.stop(0).wait()
        for s in table.drain():
            _break_quietly(s)
        rt.shutdown()
        print("[bridge_server] stopped", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=50251)
    ap.add_argument("--ttl", type=float, default=2.0,
                    help="seconds an instance may sit untouched before the "
                         "reaper breaks it")
    ap.add_argument("--reap-interval", type=float, default=0.25)
    args = ap.parse_args()
    serve(args.port, args.ttl, args.reap_interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
