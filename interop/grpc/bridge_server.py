"""bridge_server.py: one signed PaymentService instance per session, served
over gRPC, where the session is a stream. Step 1 asked whether an instance can
live behind gRPC and found that it can, easily, and that the hard question is
who ends it. Step 1 answered that with an idle timer. This one answers it with
the connection, which is the answer the C ABI always had.

The runtime side is unchanged and ordinary. ashford.py loads libashrt and
libpayment.ash.so through ctypes, binds a Python charge over the abstract
pledge exactly the way demo_payment.py does, and freezes. What sits above it is
a table keyed by a server issued instance id, a lock per instance, and one
rule.

The rule: an instance lives exactly as long as its Session stream.

  Session signs, yields the signature as the stream's first event, and then
  blocks. The handler holds the instance for as long as the peer holds the
  stream. gRPC calls the termination callback the moment the stream ends, for
  any reason, and that callback breaks the instance and drops the row. There
  is no window in which an instance exists without a stream: the id is issued
  on the stream and cannot outlive it.

Three lines this server holds, and they are the findings:

  1. The instance id is the session's name; the stream is the session. Every
     pledge rpc carries the id, the table resolves it, an unknown one is
     NOT_FOUND. The id names an instance only for as long as the stream that
     issued it is up.
  2. A pledge's Err is a value. The contract answering Err(41) is a
     BoolIntResult with the err arm set and an OK rpc status. Only an Ashford
     status, a fulfillment that did not run, becomes a gRPC error.
  3. There is no timer in this file. Nothing here presumes anything about a
     client from how long it has been quiet, because it no longer has to
     guess: a client that left is a fact gRPC hands over.

Run it directly: python bridge_server.py --port 50251
"""

import argparse
import sqlite3
import sys
import threading
import uuid
from concurrent import futures
from dataclasses import dataclass
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

# A session holds a worker thread for the instance's whole life, so the pool
# has to be wide enough for every live session plus the pledge calls driving
# them. This is the price of the streaming session on a sync Python server and
# it is a real number, not a detail.
MAX_WORKERS = 32

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
class Instance:
    """One signed instance. Frozen and never replaced: entries are written
    once at sign and read until the stream drops them, because the liveness
    bookkeeping that used to rewrite them existed only to feed a timer.

    lock guards the contract handle against a pledge call in flight racing the
    stream's termination. done releases the Session handler, and shutdown sets
    it so a handler cannot outlive the server it belongs to."""
    instance_id: int
    contract: object
    park_token: str
    lock: threading.Lock
    done: threading.Event


class InstanceTable:
    """The session store gRPC does not provide, minus the half that was
    guessing. Ids are server issued and monotonic, never client supplied, so a
    peer cannot name an instance it was not handed. remove is atomic and
    returns the entry only to its first caller, which is what makes ending a
    session exactly once free: whoever pops the row owns the break."""

    def __init__(self):
        self._lock = threading.Lock()
        self._next_id = 1
        self._live = {}

    def insert(self, contract, park_token=""):
        with self._lock:
            iid = self._next_id
            self._next_id += 1
            inst = Instance(instance_id=iid, contract=contract,
                            park_token=park_token,
                            lock=threading.Lock(), done=threading.Event())
            self._live[iid] = inst
            return inst

    def get(self, instance_id):
        with self._lock:
            return self._live.get(instance_id)

    def remove(self, instance_id):
        with self._lock:
            return self._live.pop(instance_id, None)

    def count(self):
        with self._lock:
            return len(self._live)

    def drain(self):
        with self._lock:
            out = list(self._live.values())
            self._live.clear()
            return out


def _break_quietly(inst):
    """Breaks an instance and swallows the state error a broken one answers.
    An instance the contract already broke by itself, or one that fulfilled,
    is not an error to reclaim."""
    try:
        inst.contract.break_()
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
    """One servicer over one frozen runtime. Every pledge handler is the same
    three steps: resolve the id, run one runtime call under the instance lock,
    and map what came back. Session is the one handler that is not shaped like
    that, because it is the one that owns a lifetime."""

    def __init__(self, rt, table, park_dsn=None):
        self._rt = rt
        self._table = table
        self._park_dsn = park_dsn
        self._resume_lock = threading.Lock()

    def _instance(self, instance_id, context):
        s = self._table.get(instance_id)
        if s is None:
            context.abort(grpc.StatusCode.NOT_FOUND,
                          f"unknown instance {instance_id}")
        return s

    def _fulfill(self, context, instance_id, pledge, *args):
        s = self._instance(instance_id, context)
        with s.lock:
            try:
                return _result_pb(s.contract.fulfill_sync(pledge, *args))
            except AshError as e:
                _abort(context, e)

    def end_session(self, instance_id, reason):
        """Ends a session, once. The table pop is the arbiter: the termination
        callback, the handler's own exit, and shutdown all call this, and only
        the caller that pops the row breaks the instance.

        The order matters. The row goes first, so no new pledge call can
        resolve the id while the break is in flight. Then the instance lock,
        which waits out any call admitted before the row went, so a body that
        was already running finishes against a live instance instead of one
        being torn down under it. With a park store, the instance writes its
        resumable record before the break."""
        s = self._table.remove(instance_id)
        if s is None:
            return
        s.done.set()
        with s.lock:
            if self._park_dsn:
                try:
                    s.contract.park(self._park_dsn, s.park_token)
                except AshError as e:
                    if e.status == ASH_ERR_STATE:
                        print(f"[bridge_server] session {instance_id} ended "
                              f"({reason}), explicit break left no heap to "
                              "park", flush=True)
                    else:
                        print(f"[bridge_server] session {instance_id} ended "
                              f"({reason}), park failed: {e}", flush=True)
            _break_quietly(s)
        action = "parked, broken and dropped" if self._park_dsn else \
            "instance broken and dropped"
        print(f"[bridge_server] session {instance_id} ended ({reason}), "
              f"{action}", flush=True)

    @staticmethod
    def _signed(inst):
        c = inst.contract
        return pb.SessionEvent(signed=pb.Signed(
            instance_id=inst.instance_id, currency=c.vow("currency"),
            shape_hash=c.hash(), signed_at=c.signed_at(),
            park_token=inst.park_token))

    # ---- the session, which is the signing rpc ----

    def Session(self, request, context):
        """Sign, hand back the signature, hold the instance for as long as the
        peer holds the stream.

        This is the whole finding of step 1b in one handler. The id is issued
        on the stream, so there is no window where an instance exists that no
        stream owns, and no orphan for a timer to guess at. add_callback fires
        on termination for every way a stream can end: an explicit cancel, a
        channel close, a client process that died, a network that dropped. It
        returns False if the rpc is already over, which is the one case the
        callback cannot cover, so that case ends the session inline. The
        finally is the third path and it is free, because ending is idempotent
        by the table."""
        vows = None
        if request.HasField("currency"):
            vows = {"currency": request.currency}
        try:
            c = self._rt.sign("PaymentService", vows=vows,
                              expected_hash=request.expected_hash)
        except AshError as e:
            _abort(context, e)

        token = uuid.uuid4().hex if self._park_dsn else ""
        inst = self._table.insert(c, token)
        iid = inst.instance_id
        print(f"[bridge_server] session {iid} signed, currency "
              f"{c.vow('currency')}", flush=True)

        if not context.add_callback(lambda: self.end_session(iid, "stream terminated")):
            self.end_session(iid, "stream was already over")
            return

        try:
            yield self._signed(inst)
            # Nothing else to send. The handler exists from here on only to
            # keep the stream up, and the stream exists only to say the peer
            # is still there. That sentence is what the idle timer was
            # standing in for.
            inst.done.wait()
        finally:
            self.end_session(iid, "handler exit")

    def Resume(self, request, context):
        """Stand one parked instance back up, claim its store row with the
        DELETE, and hold its new session stream. The DELETE is the one shot
        claim, including across replicas sharing the same park store. The
        same token is kept so the next stream termination parks the same
        session name again."""
        if not self._park_dsn:
            context.abort(grpc.StatusCode.FAILED_PRECONDITION,
                          "server runs without a park store")

        with self._resume_lock:
            try:
                c = self._rt.resume(self._park_dsn, request.park_token,
                                    request.expected_hash)
                with sqlite3.connect(self._park_dsn) as db:
                    cur = db.execute("DELETE FROM ash_park WHERE pkey = ?",
                                     (request.park_token.encode(),))
                if cur.rowcount == 0:
                    try:
                        c.break_()
                    except AshError as e:
                        if e.status != ASH_ERR_STATE:
                            raise
                    context.abort(
                        grpc.StatusCode.NOT_FOUND,
                        "park token was claimed by another replica")
                inst = self._table.insert(c, request.park_token)
            except AshError as e:
                _abort(context, e)

        iid = inst.instance_id
        print(f"[bridge_server] session {iid} resumed, currency "
              f"{c.vow('currency')}", flush=True)

        if not context.add_callback(lambda: self.end_session(iid, "stream terminated")):
            self.end_session(iid, "stream was already over")
            return

        try:
            yield self._signed(inst)
            inst.done.wait()
        finally:
            self.end_session(iid, "handler exit")

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
        s = self._instance(request.instance_id, context)
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
        """Breaks the instance but leaves the entry standing, which is the
        same choice step 1 made and it survives the move to streams intact. In
        process, a broken handle still answers ASH_ERR_STATE to a fulfillment
        and still reads its partial surface, so the owner learns it broke.
        Dropping the row here would make a broken instance answer NOT_FOUND
        instead, indistinguishable from an id that never existed, and the
        bridge would lose a distinction the C ABI keeps.

        What changed is what collects the row afterward. It was a tombstone on
        a second guess at a TTL. It is now a row the stream still owns: it
        reads broken for as long as the peer stays to read it, and it goes
        when the peer goes. The tombstone lifetime question does not have a
        better answer here, it stopped being a question."""
        s = self._instance(request.instance_id, context)
        with s.lock:
            try:
                _break_quietly(s)
            except AshError as e:
                _abort(context, e)
        return pb.BreakReply()

    # ---- prototype only ----

    def Debug(self, request, context):
        """Not a contract surface. It exists so the client can watch an
        instance leave with its stream, which is the measurement this
        prototype was built to take."""
        return pb.DebugReply(live_instances=self._table.count())


def serve(port, park_dsn=None):
    rt = Runtime(OUT / "libashrt.so")
    rt.load(OUT / "libpayment.ash.so")
    rt.bind("PaymentService.charge", charge)
    rt.freeze()

    table = InstanceTable()
    servicer = PaymentServicer(rt, table, park_dsn)

    pool = futures.ThreadPoolExecutor(max_workers=MAX_WORKERS)
    server = grpc.server(pool)
    pb_grpc.add_PaymentServiceServicer_to_server(servicer, server)
    bound = server.add_insecure_port(f"127.0.0.1:{port}")
    server.start()
    print(f"[bridge_server] serving PaymentService on 127.0.0.1:{bound}, "
          f"sessions are streams", flush=True)
    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        pass
    finally:
        # Shutdown breaks every instance still standing. A signed contract
        # that outlives its server is an obligation nobody can keep. Draining
        # the table sets each session's done, so the handlers wake and exit on
        # their own rather than depending on the transport to tell them, and
        # the pool joins before the runtime goes away underneath a body.
        server.stop(0).wait()
        for s in table.drain():
            s.done.set()
            with s.lock:
                _break_quietly(s)
        pool.shutdown(wait=True)
        rt.shutdown()
        print("[bridge_server] stopped", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=50251)
    ap.add_argument("--park-dsn", metavar="PATH",
                    help="the sqlite park store a closed session writes to")
    args = ap.parse_args()
    serve(args.port, args.park_dsn)
    return 0


if __name__ == "__main__":
    sys.exit(main())
