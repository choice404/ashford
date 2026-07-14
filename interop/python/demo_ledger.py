"""demo_ledger.py: the ledger walk from Python, no C written, no binding
generated. It is the store twin of demo_payment.py and demo_remote.py, and it
carries the product claim into durable storage: the same host surface that
signed a local contract and a remote one signs a contract backed by a SQLite
file, and the database never shows through ctypes. ashford.py speaks the
documented ABI and the store is invisible to it, which is the whole point.

The contract is skeleton/ledger.ash, its Accounts schema reconciled at sign and
its loose store pledges open, balance, and set_balance beside the transactional
Transfer subcontract of debit and credit. This demo drives the ground the C
hosts cover in tests/runtime/test_store_ledger.c and test_store_txn.c: sign
against a temp file with a dsn override, open accounts and read their balances,
rewrite a row and read it back, bind an injection string as a value the table
survives, commit a good transfer and roll a bad one back, and prove an overdraft
is the ledger's own Err and never a store status. Every outcome it asserts is the
outcome the C host asserts.

Exit status 0 means every check below held. The temp file is unlinked at the end.
"""

import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import ashford
from ashford import ASH_ERR_STATE, ASH_SIGNED, AshError, Err, Ok, Runtime

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "target" / "ashc-out"
LIBRT = OUT / "libashrt.so"
LEDGER = OUT / "libledger.ash.so"

# LedgerError as skeleton/ledger.ash declares it, in declaration order:
# NoSuchAccount, StoreFailed, Insufficient. A payload-free sum variant decodes
# to (tag, []), the variant's declaration index and its empty field list, so the
# ledger's own errors read back through the same shape any declared sum does.
NO_SUCH_ACCOUNT = (0, [])
STORE_FAILED = (1, [])
INSUFFICIENT = (2, [])

_failures = 0


def check(cond, what):
    global _failures
    if not cond:
        print(f"[demo_ledger] FAIL: {what}", file=sys.stderr)
        _failures = 1


def read_balance(rt, dsn, acct_id):
    """Reads one account's balance through a fresh, loose autocommit instance,
    so the read sees the committed file and never a live episode's buffer, the
    discipline the C host holds when it reopens the file to witness a write."""
    c = rt.sign("Ledger", vows={"dsn": dsn})
    try:
        out = c.fulfill_sync("balance", acct_id)
        check(isinstance(out, Ok), f"balance of {acct_id} is Ok")
        return out.value if isinstance(out, Ok) else None
    finally:
        c.break_()


def seed_and_read(rt, dsn):
    """The loose store pledges: open writes a row, balance reads one back, and
    set_balance rewrites one, each a Result whose Err is the ledger's own
    business. It seeds accounts 1 and 2 at 100 for the transfer that follows."""
    c = rt.sign("Ledger", vows={"dsn": dsn})
    check(c.state() == ASH_SIGNED, "seed instance signed")
    check(c.vow("dsn") == dsn, "dsn override landed")

    check(c.fulfill_sync("open", 1, "alice", 100.0) == Ok(True), "open account 1")
    check(c.fulfill_sync("open", 2, "bob", 100.0) == Ok(True), "open account 2")
    check(c.fulfill_sync("balance", 1) == Ok(100.0), "balance of 1 reads 100")

    # A missing account is the contract's own error, not a store status.
    check(c.fulfill_sync("balance", 99) == Err(NO_SUCH_ACCOUNT),
          "balance of a missing account is Err(NoSuchAccount)")

    # Update a row and read the new value back, then reset it for the transfer.
    check(c.fulfill_sync("set_balance", 1, "alice", 250.0) == Ok(True),
          "set_balance of 1 to 250")
    check(c.fulfill_sync("balance", 1) == Ok(250.0), "balance of 1 now reads 250")
    check(c.fulfill_sync("set_balance", 1, "alice", 100.0) == Ok(True),
          "reset 1 to 100")

    # Injection resistance: an owner holding a DROP TABLE is bound as a value,
    # positionally, never SQL, so the row lands and the table survives.
    check(c.fulfill_sync("open", 3, "'); DROP TABLE Accounts; --", 5.0) == Ok(True),
          "open account 3 with an injection owner")
    check(c.fulfill_sync("balance", 3) == Ok(5.0), "balance of 3 reads 5")
    check(c.fulfill_sync("balance", 1) == Ok(100.0),
          "the table survived the injection string")

    c.break_()


def good_transfer(rt, dsn):
    """A transfer that commits: debit and credit both latch Ok, the subcontract
    completes, the transaction commits, and both balances move. A second call to
    either resolved transactional pledge is ASH_ERR_STATE, the once-only law a
    committed transaction earns."""
    c = rt.sign("Ledger", vows={"dsn": dsn})
    check(c.fulfill_sync("debit", 1, 30.0) == Ok(None), "debit 30 from 1 is Ok")
    check(c.fulfill_sync("credit", 2, 30.0) == Ok(None), "credit 30 to 2 is Ok")

    try:
        c.fulfill_sync("debit", 1, 30.0)
        check(False, "re-calling a committed transactional pledge did not fail")
    except AshError as e:
        check(e.status == ASH_ERR_STATE,
              "re-calling a committed transactional pledge is ASH_ERR_STATE")
    try:
        c.fulfill_sync("credit", 2, 30.0)
        check(False, "re-calling the other committed pledge did not fail")
    except AshError as e:
        check(e.status == ASH_ERR_STATE,
              "re-calling the other committed pledge is ASH_ERR_STATE")
    c.break_()

    # The file reflects both writes: 1 fell to 70, 2 rose to 130.
    check(read_balance(rt, dsn, 1) == 70.0, "good transfer left 1 at 70")
    check(read_balance(rt, dsn, 2) == 130.0, "good transfer left 2 at 130")


def failed_transfer(rt, dsn):
    """A transfer that rolls back: the debit lands in the open transaction, the
    credit against a missing account returns Err(NoSuchAccount), the whole
    episode rolls back, and the debit that ran leaves nothing durable behind."""
    c = rt.sign("Ledger", vows={"dsn": dsn})
    check(c.fulfill_sync("debit", 1, 50.0) == Ok(None), "debit 50 from 1 is Ok")
    check(c.fulfill_sync("credit", 99, 50.0) == Err(NO_SUCH_ACCOUNT),
          "credit to a missing account is Err(NoSuchAccount)")
    c.break_()

    # The debit did not survive: 1 is still 70, byte for byte the pre-transfer value.
    check(read_balance(rt, dsn, 1) == 70.0,
          "failed transfer rolled the debit back, 1 still 70")


def overdraft_is_business(rt, dsn):
    """The line between the contract and the store: an overdraft is the ledger's
    own rule, Err(Insufficient) returned as a value with an ASH_OK delivery,
    never a store status. The transaction rolls back and the balance is unmoved."""
    c = rt.sign("Ledger", vows={"dsn": dsn})
    out = c.fulfill_sync("debit", 1, 1000.0)
    check(out == Err(INSUFFICIENT),
          "an overdraft is Err(Insufficient), the contract working")
    c.break_()
    check(read_balance(rt, dsn, 1) == 70.0, "the overdraft moved no balance, 1 still 70")


def break_mid_transaction(rt, dsn):
    """A break before the commit: the debit buffers in the open transaction, the
    contract is torn down before its credit, and the transaction rolls back so no
    half written episode survives the teardown."""
    c = rt.sign("Ledger", vows={"dsn": dsn})
    check(c.fulfill_sync("debit", 1, 25.0) == Ok(None), "debit 25 from 1 is Ok")
    c.break_()
    check(read_balance(rt, dsn, 1) == 70.0,
          "break before commit left no debit durable, 1 still 70")


def main():
    fd, path = tempfile.mkstemp(prefix="ashledger_", dir=str(ROOT / "target"))
    os.close(fd)
    dsn = "file:" + path
    try:
        with Runtime(LIBRT) as rt:
            rt.load(LEDGER)

            seed_and_read(rt, dsn)
            good_transfer(rt, dsn)
            failed_transfer(rt, dsn)
            overdraft_is_business(rt, dsn)
            break_mid_transaction(rt, dsn)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass

    if _failures:
        return 1
    print("[demo_ledger] ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
