"""demo_mesh.py: the foreign language provider, made literal. A Python host
binds a Python function over an abstract pledge and serves it as a mesh node,
and a peer in another language connects and fulfills it and reads back a value
this process computed, live. It is the last piece the language was built toward:
demo_remote.py proved a Python host consuming a contract a C daemon serves, and
this proves the other direction, a Python host serving a contract a C peer
consumes, the pledge body running in Python across the wire.

The contract is skeleton/payment.ash, whose charge is abstract on purpose, so
the served body is the Python charge below and nothing compiled. There is no C
in this file and no generated binding: ashford.py speaks the documented ABI
through ctypes, Runtime.serve stands the node up, and the C consumer in
tests/net/test_mesh_python.c drives it. The value the consumer reads, Ok(True)
on a positive amount and Err(41) on anything else, is exactly what this Python
function returns in process, and 41 is a constant no C in the consumer carries,
so a consumer that reads Err(41) read a number only this live Python computation
could have produced.

Usage: demo_mesh.py serve ADDR [TOKEN]

serve stands the provider up on ADDR, a host:port, under TOKEN when one is given
and tokenless otherwise, and blocks serving until a SIGTERM or SIGINT stops it
cleanly. The harness starts it in the background, waits until it listens, runs
the C consumer against it, and signals it to stop. The token is read from argv
and never logged. Exit status 0 means a clean serve and stop.
"""

import signal
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import ashford
from ashford import Err, Ok, Runtime

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "ashc-out"
LIBRT = OUT / "libashrt.so"
PAYMENT = OUT / "libpayment.ash.so"

# The Err payload this provider answers a bad amount with, a constant chosen
# here and nowhere in the consumer, so a consumer that reads it read this
# process's own computation and not a value it could have made itself.
BAD_AMOUNT = 41


def charge(inst, args):
    """The Python implementation of the abstract PaymentService.charge, honest
    to its declared Result<Bool, Int>: the currency vow is read off the signed
    instance the way any thunk reads it, a positive amount charges and answers
    Ok(True), and anything else is Err(41). It runs on a runtime pool worker,
    the trampoline holding the GIL, whether the fulfillment began in this
    process or arrived over a peer's connection; the serve path changes where
    the request comes from, not where the body runs."""
    card, amount = args
    if amount > 0:
        print(f"[demo_mesh] charging {inst.vow('currency')} {amount:.2f} "
              f"to {card}", flush=True)
        return Ok(True)
    return Err(BAD_AMOUNT)


def serve(addr, token):
    """Stand the provider up: load the payment module whose charge is abstract,
    bind the Python charge over it, freeze, and serve on addr. Block until a
    signal asks for a stop, then stop the server and shut the runtime down, the
    same teardown order a C node holds, the server before the runtime it
    served."""
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    with Runtime(LIBRT) as rt:
        rt.load(PAYMENT)
        rt.bind("PaymentService.charge", charge)
        rt.freeze()

        # The in process baseline the consumer's assertions must equal: the same
        # bound charge, run here before a single peer connects, is the value the
        # served path will hand across the wire, so the cross language result is
        # this process's own computation and the equality is closed on both ends.
        local = rt.sign("PaymentService")
        assert local.fulfill_sync("charge", "4111 1111", 25.0) == Ok(True)
        assert local.fulfill_sync("charge", "4111 1111", -2.0) == Err(BAD_AMOUNT)
        local.break_()

        server = rt.serve(addr, token)
        print(f"[demo_mesh] serving PaymentService on {addr}", flush=True)
        try:
            stop.wait()
        finally:
            server.stop()

    print("[demo_mesh] stopped", flush=True)
    return 0


def main():
    if len(sys.argv) < 3 or sys.argv[1] != "serve":
        print("usage: demo_mesh.py serve ADDR [TOKEN]", file=sys.stderr)
        return 2
    addr = sys.argv[2]
    token = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != "-" else None
    return serve(addr, token)


if __name__ == "__main__":
    sys.exit(main())
