"""demo_payment.py: the payment walk from Python, no C written, no binding
generated. ashford.py speaks the documented ABI through ctypes and this demo
drives the same ground tests/runtime/test_partial.c covers in C: load the
runtime and two compiled modules, bind Python callables over the abstract
pledges, sign with a vow override, and walk the partial, fulfilled, and
automatic break paths, reading the partial surface at every turn. The by
reference protocol runs against the Greeter contract, whose shout takes its
argument by ref and hands the shouted bytes back through the default write
back.

Exit status 0 means every check below held.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import ashford
from ashford import (ASH_ERR_STATE, ASH_FULFILLED, ASH_PARTIAL, ASH_SIGNED,
                     ASH_BROKEN, AshError, Err, Ok, Runtime, StringRef)

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "ashc-out"

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[demo_payment] FAIL: {what}", file=sys.stderr)
        _failures = 1


def charge(inst, args):
    """The Python implementation of the abstract PaymentService.charge,
    honest to its declared Result<Bool, Int>: the currency vow is read off
    the signed instance like any thunk would, a positive amount charges
    and answers Ok(True), anything else is Err(41)."""
    card, amount = args
    if amount > 0:
        print(f"[demo_payment] charging {inst.vow('currency')} "
              f"{amount:.2f} to {card}")
        return Ok(True)
    return Err(41)


def shout(inst, args, nargs, out):
    """The Python implementation of Greeter.shout, in the raw thunk shape
    because it updates its by-reference slot: the shouted string is built in
    instance owned memory, written into the slot, and returned as Ok. The
    delivery writes the final slot value back to host storage."""
    import ctypes as C
    if nargs != 1 or args[0].ty != ashford.ASH_TY_STRING:
        return ashford.ASH_ERR_TYPE
    shouted = ashford.decode(args[0]).upper().encode("utf-8")
    sv = inst._rt._lib.ash_string_copy(inst._ptr, shouted, len(shouted))
    C.memmove(C.byref(args[0]), C.byref(sv), C.sizeof(ashford.AshValue))
    inst._rt._encode_owned(inst._ptr, out[0], Ok(shouted.decode("utf-8")))
    return ashford.ASH_OK


def main():
    with Runtime(OUT / "libashrt.so") as rt:
        rt.load(OUT / "libpayment.ash.so")
        rt.load(OUT / "libhello.ash.so")
        rt.bind("PaymentService.charge", charge)
        rt.bind("Greeter.shout", shout, raw=True)
        rt.freeze()

        # The freeze latched the registration surface: a late bind is a
        # clean state error, not a quiet success.
        try:
            rt.bind("PaymentService.charge", charge)
            check(False, "bind after freeze did not fail")
        except AshError as e:
            check(e.status == ASH_ERR_STATE, "bind after freeze is ERR_STATE")

        # ---- discovery: the iname table as Python sees it ----

        dump = rt.iname_dump()
        print(dump, end="")
        check("PaymentService" in dump and "Greeter" in dump,
              "iname dump names both contracts")
        check(rt.iname_count() == dump.count("\n"),
              "iname count matches the dump")
        charge_rows = [ln for ln in dump.splitlines()
                       if "PaymentService_charge" in ln]
        check(len(charge_rows) == 1, "charge has one iname row")
        entry = rt.iname_lookup(charge_rows[0].split()[0])
        check(entry["kind"] == "pledge" and entry["contract"] ==
              "PaymentService" and entry["nargs"] == 2,
              "iname lookup resolves charge")

        # ---- the partial path, signed with a vow override ----

        c1 = rt.sign("PaymentService", vows={"currency": "EUR"})
        check(c1.state() == ASH_SIGNED, "c1 signed")
        check(c1.vow("currency") == "EUR", "vow override landed")
        check(c1.hash() != 0 and c1.signed_at() > 0, "c1 carries a signature")

        p = c1.partial()
        check(p.pending == ["Validation", "Processing", "notify_user"],
              "c1 pending order is subs then loose pledges")

        # Validation, one pledge synchronously and one through a future.
        check(c1.fulfill_sync("validate_card", "4111 1111") == Ok(True),
              "validate_card Ok")
        check(c1.state() == ASH_SIGNED, "half a subcontract moves nothing")
        fut = c1.fulfill("validate_amount", 25.0)
        check(fut.wait() == Ok(True), "validate_amount Ok through a future")
        try:
            fut.wait()
            check(False, "second wait did not fail")
        except AshError as e:
            check(e.status == ASH_ERR_STATE, "second wait is ERR_STATE")

        check(c1.state() == ASH_PARTIAL, "Validation lands, c1 partial")
        p = c1.partial()
        check(p.fulfilled == ["Validation"] and
              p.pending == ["Processing", "notify_user"] and
              p.broken == [] and p.errors == [],
              "partial names after Validation")
        print(f"[demo_payment] state {c1.state_name()}, "
              f"fulfilled {p.fulfilled}, pending {p.pending}")

        # charge succeeds: the Python body answers its declared Bool.
        out = c1.fulfill_sync("charge", "4111 1111", 25.0)
        check(isinstance(out, Ok) and out.value is True,
              "charge Ok carries the declared Bool")
        print(f"[demo_payment] charge answered {out}")

        check(c1.fulfill_sync("notify_user", True) == Ok(True),
              "notify_user Ok")
        check(c1.state() == ASH_FULFILLED, "c1 fulfilled")
        p = c1.partial()
        check(len(p.fulfilled) == 3 and p.pending == [] and p.errors == [],
              "every item fulfilled")
        print(f"[demo_payment] state {c1.state_name()}")

        # ---- the Err path and the automatic break ----

        c2 = rt.sign("PaymentService")
        check(c2.vow("currency") == "USD", "c2 signs on the declared default")
        out = c2.fulfill_sync("charge", "4111 1111", -2.0)
        check(out == Err(41), "charge Err returns to the caller")
        check(c2.state() == ASH_BROKEN, "the break line fired by itself")

        p = c2.partial()
        check(p.broken == ["Processing"] and
              p.pending == ["Validation", "notify_user"],
              "c2 broken lists Processing")
        check(p.errors == [("charge", 41)],
              "the automatic break kept the Err payload readable")
        print(f"[demo_payment] state {c2.state_name()}, errors {p.errors}")

        # Fulfillment against a broken instance is a state error.
        try:
            c2.fulfill_sync("validate_card", "4111 1111")
            check(False, "fulfillment after break did not fail")
        except AshError as e:
            check(e.status == ASH_ERR_STATE,
                  "fulfillment after automatic break is ERR_STATE")

        # An explicit break reclaims the heap the automatic one kept; the
        # payload reads as a zeroed Unit afterwards.
        c2.break_()
        check(c2.partial().errors == [("charge", None)],
              "explicit break reclaimed the payload")

        # ---- by reference: Greeter.shout writes back into host storage ----

        g = rt.sign("Greeter")
        ref = StringRef("whisper")
        out = g.fulfill_sync("shout", refs=[ref])
        check(out == Ok("WHISPER"), "shout Ok")
        check(ref.value == "WHISPER",
              "the default write back landed in host storage")
        print(f"[demo_payment] shout wrote back {ref.value!r}")
        g.break_()

    if _failures:
        return 1
    print("[demo_payment] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
