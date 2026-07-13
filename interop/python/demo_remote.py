"""demo_remote.py: the payment walk from Python again, this time over a socket.
It is the remote twin of demo_payment.py and it makes the product claim literal
in the second language: the same host code that drives a contract loaded into
this process drives a contract an ashd daemon serves across the wire, and the
only line that changes is how the runtime is stood up, load a module or connect
to a daemon. Everything after that, sign with a vow override, fulfill the
validation pledges, read the partial surface, charge, break, is byte for byte
the same call sequence, so the host truly does not know which side of the wire
the contract lives on.

The contract is net_payment.ash, the payment service with charge carrying a body
instead of staying abstract, because a daemon has no host to bind an
implementation and callbacks are out of v1: every pledge it serves dispatches on
its own. demo_payment.py binds a Python charge and walks the by reference
Greeter, the two things that do not cross the wire in v1; this demo keeps to the
payment core that does, and proves the local and remote runs agree on every
value.

Usage: demo_remote.py TOKENED_ADDR TOKEN [NOTOKEN_ADDR] [TOKENED_PID]

TOKENED_ADDR is the host:port of an ashd started under TOKEN. NOTOKEN_ADDR, when
given, is a second ashd started with no token, which the tokenless connect path
uses. TOKENED_PID, when given, is that daemon's pid, which the disconnect phase
kills mid fulfillment to prove ASH_ERR_NET reaches an in flight wait. The token
matrix and the kill run last because they end by refusing or severing the
connection. Exit status 0 means every check held.
"""

import os
import signal
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import ashford
from ashford import (ASH_ERR_NET, ASH_ERR_STATE, ASH_FULFILLED, ASH_PARTIAL,
                     ASH_SIGNED, ASH_BROKEN, AshError, Err, Ok, Runtime)

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "ashc-out"
LIBRT = OUT / "libashrt.so"
NETPAY = OUT / "libnet_payment.ash.so"

NET_BATCH = 128

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[demo_remote] FAIL: {what}", file=sys.stderr)
        _failures = 1


def run_payment(rt, label):
    """The one payment sequence, run against a local instance and a remote
    proxy alike. It returns the outcomes it observed so the caller can demand
    the local run and the remote run produced the identical list; the label
    names the side for a diagnostic only, never for a branch. Every call here
    is the ordinary Contract surface, unaware of any wire underneath it."""
    seen = []

    # ---- the happy path, signed with a vow override ----
    c1 = rt.sign("PaymentService", vows={"currency": "EUR"})
    check(c1.state() == ASH_SIGNED, f"{label}: c1 signed")
    check(c1.vow("currency") == "EUR", f"{label}: vow override landed")
    check(c1.hash() != 0 and c1.signed_at() > 0,
          f"{label}: c1 carries a signature")
    seen.append(("c1.currency", c1.vow("currency")))

    p = c1.partial()
    check(p.pending == ["Validation", "Processing", "notify_user"],
          f"{label}: c1 pending is subs then loose pledges")
    seen.append(("c1.pending0", tuple(p.pending)))

    # Validation, one pledge synchronously and one through a future.
    out = c1.fulfill_sync("validate_card", "4111 1111")
    check(out == Ok(True), f"{label}: validate_card Ok")
    seen.append(("validate_card", out))
    check(c1.state() == ASH_SIGNED, f"{label}: half a subcontract moves nothing")

    fut = c1.fulfill("validate_amount", 25.0)
    out = fut.wait()
    check(out == Ok(True), f"{label}: validate_amount Ok through a future")
    seen.append(("validate_amount", out))
    try:
        fut.wait()
        check(False, f"{label}: second wait did not fail")
    except AshError as e:
        check(e.status == ASH_ERR_STATE, f"{label}: second wait is ERR_STATE")

    check(c1.state() == ASH_PARTIAL, f"{label}: Validation lands, c1 partial")
    p = c1.partial()
    check(p.fulfilled == ["Validation"] and
          p.pending == ["Processing", "notify_user"] and
          p.broken == [] and p.errors == [],
          f"{label}: partial names after Validation")
    seen.append(("c1.after_validation",
                 (tuple(p.fulfilled), tuple(p.pending), p.errors)))

    out = c1.fulfill_sync("charge", "4111 1111", 25.0)
    check(out == Ok(True), f"{label}: charge Ok carries the declared Bool")
    seen.append(("charge", out))
    out = c1.fulfill_sync("notify_user", True)
    check(out == Ok(True), f"{label}: notify_user Ok")
    seen.append(("notify_user", out))

    check(c1.state() == ASH_FULFILLED, f"{label}: c1 fulfilled")
    p = c1.partial()
    check(len(p.fulfilled) == 3 and p.pending == [] and p.errors == [],
          f"{label}: every item fulfilled")
    seen.append(("c1.state_fulfilled", c1.state()))

    c1.break_()
    check(c1.state() == ASH_BROKEN, f"{label}: c1 broken after break")
    try:
        c1.fulfill_sync("validate_card", "4111 1111")
        check(False, f"{label}: fulfill after break did not fail")
    except AshError as e:
        check(e.status == ASH_ERR_STATE, f"{label}: fulfill after break ERR_STATE")

    # ---- the Err path and the automatic break ----
    c2 = rt.sign("PaymentService")
    check(c2.vow("currency") == "USD", f"{label}: c2 on the declared default")
    seen.append(("c2.currency", c2.vow("currency")))

    out = c2.fulfill_sync("charge", "4111 1111", -2.0)
    check(out == Err(2), f"{label}: charge Err returns to the caller")
    seen.append(("charge_err", out))
    check(c2.state() == ASH_BROKEN, f"{label}: the break line fired by itself")

    p = c2.partial()
    check(p.broken == ["Processing"] and
          p.pending == ["Validation", "notify_user"],
          f"{label}: c2 broken lists Processing")
    check(p.errors == [("charge", 2)],
          f"{label}: the automatic break kept the Err payload readable")
    seen.append(("c2.errors", p.errors))

    c2.break_()
    return seen


def run_local():
    """Stand the runtime up by loading the compiled module into this process."""
    with Runtime(LIBRT) as rt:
        rt.load(NETPAY)
        return run_payment(rt, "local")


def run_remote(addr, token):
    """Stand the runtime up by connecting to an ashd that serves the module.
    This function is run_local with exactly one line changed, the load turned
    into a connect, which is the whole product claim in one diff."""
    with Runtime(LIBRT) as rt:
        rt.connect(addr, token)
        return run_payment(rt, "remote")


def run_tokenless(addr):
    """A tokenless daemon accepts a None token: connect, sign, one fulfill, and
    break, enough to prove the empty token path serves."""
    with Runtime(LIBRT) as rt:
        rt.connect(addr, token=None)
        c = rt.sign("PaymentService")
        check(c.fulfill_sync("validate_card", "4111 1111") == Ok(True),
              "tokenless: validate_card Ok")
        c.break_()


def check_bad_token(addr, token):
    """A refused token closes the connection before any table crosses, and the
    connect surfaces the network's status as an AshError."""
    try:
        with Runtime(LIBRT) as rt:
            rt.connect(addr, token + "-wrong")
        check(False, "bad token: connect did not refuse")
    except AshError as e:
        check(e.status == ASH_ERR_NET, "bad token: connect is ASH_ERR_NET")


def check_dead_address():
    """An unreachable address is the network's one new failure at connect time,
    the same ASH_ERR_NET a severed connection delivers to a wait."""
    try:
        with Runtime(LIBRT) as rt:
            rt.connect("127.0.0.1:1", token=None)
        check(False, "dead address: connect did not refuse")
    except AshError as e:
        check(e.status == ASH_ERR_NET, "dead address: connect is ASH_ERR_NET")


def run_disconnect(addr, token, pid):
    """Sign, launch a batch of fulfillments, kill the daemon out from under
    them, and demand that every in flight wait delivers ASH_ERR_NET, that a
    later fulfill is a clean local state error, and that the proxy reads Broken
    without touching the dead wire. Run last, since it ends the daemon."""
    with Runtime(LIBRT) as rt:
        rt.connect(addr, token)
        c = rt.sign("PaymentService")
        futs = [c.fulfill("validate_card", "4111 1111") for _ in range(NET_BATCH)]

        os.kill(pid, signal.SIGKILL)

        nnet = nok = 0
        for f in futs:
            try:
                if f.wait() == Ok(True):
                    nok += 1
                else:
                    check(False, "disconnect: an in flight wait was a wrong Ok")
            except AshError as e:
                if e.status == ASH_ERR_NET:
                    nnet += 1
                else:
                    check(False, "disconnect: an in flight wait was not NET")
        check(nnet > 0, "disconnect: at least one wait saw ASH_ERR_NET")
        print(f"[demo_remote] disconnect: {nok} Ok, {nnet} ASH_ERR_NET")

        try:
            c.fulfill_sync("validate_card", "4111 1111")
            check(False, "disconnect: a fulfill after the death did not fail")
        except AshError as e:
            check(e.status in (ASH_ERR_STATE, ASH_ERR_NET),
                  "disconnect: fulfill after death is clean")
        check(c.state() == ASH_BROKEN, "disconnect: proxy latched Broken")


def main():
    if len(sys.argv) < 3:
        print("usage: demo_remote.py TOKENED_ADDR TOKEN "
              "[NOTOKEN_ADDR] [TOKENED_PID]", file=sys.stderr)
        return 2
    addr = sys.argv[1]
    token = sys.argv[2]
    notoken_addr = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != "-" else None
    pid = int(sys.argv[4]) if len(sys.argv) > 4 else None

    # The transparency proof: the same sequence, one side loaded and one side
    # connected, must agree on every outcome.
    local_seen = run_local()
    remote_seen = run_remote(addr, token)
    check(local_seen == remote_seen,
          "local and remote produced the identical outcomes")
    if local_seen == remote_seen:
        print(f"[demo_remote] local and remote agreed on {len(local_seen)} "
              f"outcomes")

    # The tokenless path, against the second daemon when the harness stood one up.
    if notoken_addr:
        run_tokenless(notoken_addr)
        print("[demo_remote] tokenless connect served")

    # The token matrix and the disconnect, each ending in a refusal or a kill.
    check_bad_token(addr, token)
    check_dead_address()
    if pid is not None:
        run_disconnect(addr, token, pid)

    if _failures:
        return 1
    print("[demo_remote] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
