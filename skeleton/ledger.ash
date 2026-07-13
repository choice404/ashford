// The Layer 3 walking skeleton: a store-backed Ledger signed against a SQLite
// file, one schema reconciled at sign, and loose pledges that read and write
// rows through the store surface. The dsn is a vow, so a host points the ledger
// at a temp database with a sign-time override and nothing more; the Accounts
// schema stands up its table on a fresh file and validates it on an existing
// one, so the skeleton needs no setup step. balance reads a row and answers
// with its balance or NoSuchAccount, and open writes a row, each returning a
// Result whose Err is the ledger's own business, never a store status. A
// backend failure rides ASH_ERR_STORE through the host's wait; a missing
// account is Err(NoSuchAccount), the contract working. tests/runtime is the C
// host that signs, opens rows, reads them back, and breaks.

import ashstd.store

// The ledger's own errors, the business the store never sees. NoSuchAccount is
// a read that found no row; StoreFailed is the surface refusing a value, folded
// here so a pledge answers in one error type; Insufficient is a debit against a
// balance too small, the transfer's own rule that rolls its transaction back.
LedgerError is either NoSuchAccount or StoreFailed or Insufficient

contract Ledger {
    vow dsn: String = "file:ledger.db"

    schema Accounts {
        id: Int
        balance: Float
        owner: String
    }

    // The balance of an account by id: the row's balance when it exists,
    // NoSuchAccount when it does not. The store read is a value, so a hit and a
    // miss are both Ok answers and only a backend failure leaves this Result
    // for the status channel.
    pledge balance(id: Int) -> Result<Float, LedgerError> {
        return match Store.find(Accounts, id) {
            Ok(Some(row)) -> Ok(row.balance)
            Ok(None) -> Err(NoSuchAccount)
            _ -> Err(StoreFailed)
        }
    }

    // Opens an account by inserting its row. The row is the Accounts record, its
    // fields the schema's columns, and a successful insert answers Ok(true).
    pledge open(id: Int, owner: String, amount: Float) -> Result<Bool, LedgerError> {
        let row = Accounts { id: id, balance: amount, owner: owner }
        return match Store.insert(Accounts, row) {
            Ok(_) -> Ok(true)
            _ -> Err(StoreFailed)
        }
    }

    // Sets an account's balance by rewriting its row, the write half of the
    // round trip the gate drives. The whole row is written, its id unchanged, so
    // update is a full-row replace keyed by the primary key.
    pledge set_balance(id: Int, owner: String, amount: Float) -> Result<Bool, LedgerError> {
        let row = Accounts { id: id, balance: amount, owner: owner }
        return match Store.update(Accounts, id, row) {
            Ok(_) -> Ok(true)
            _ -> Err(StoreFailed)
        }
    }

    // The transfer: two writes that must both land or both vanish. The
    // transactional modifier makes the subcontract one all-or-nothing episode,
    // so the runtime opens a transaction on debit, buffers credit's write in the
    // same one, and commits only when both latch Ok. A debit past the balance is
    // Err(Insufficient) and a credit against a missing account is
    // Err(NoSuchAccount), each the ledger's own business rolling the whole
    // episode back so neither write survives.
    subcontract Transfer transactional {
        // Debits an account, refusing an overdraft. The balance check is the
        // contract's own rule, so a shortfall is Err(Insufficient), never a
        // store status; the write that lands buffers in the open transaction and
        // is durable only once the transfer commits.
        pledge debit(id: Int, amount: Float) -> Result<Unit, LedgerError> {
            return match Store.find(Accounts, id) {
                Ok(Some(row)) -> {
                    if row.balance < amount {
                        return Err(Insufficient)
                    }
                    match Store.update(Accounts, id, Accounts { id: id, balance: row.balance - amount, owner: row.owner }) {
                        Ok(u) -> Ok(u)
                        _ -> Err(StoreFailed)
                    }
                }
                Ok(None) -> Err(NoSuchAccount)
                _ -> Err(StoreFailed)
            }
        }

        // Credits an account, the second half of the episode. A missing target
        // is Err(NoSuchAccount), which rolls the whole transfer back so the debit
        // that already ran leaves nothing durable behind.
        pledge credit(id: Int, amount: Float) -> Result<Unit, LedgerError> {
            return match Store.find(Accounts, id) {
                Ok(Some(row)) -> match Store.update(Accounts, id, Accounts { id: id, balance: row.balance + amount, owner: row.owner }) {
                    Ok(u) -> Ok(u)
                    _ -> Err(StoreFailed)
                }
                Ok(None) -> Err(NoSuchAccount)
                _ -> Err(StoreFailed)
            }
        }
    }
}
