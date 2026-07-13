// A store-backed contract type checks: a dsn vow, an Accounts schema of seven-
// scalar columns, and loose pledges reading and writing rows through the Store
// surface. The row is the schema's record, so a field read and a record literal
// are ordinary, and every Store.* call lands its Result.
LedgerErr is either NoAccount or StoreBroke

contract Ledger {
    vow dsn: String = "file:ledger.db"

    schema Accounts {
        id: Int
        balance: Float
        owner: String
    }

    pledge balance(id: Int) -> Result<Float, LedgerErr> {
        return match Store.find(Accounts, id) {
            Ok(Some(row)) -> Ok(row.balance)
            Ok(None) -> Err(NoAccount)
            _ -> Err(StoreBroke)
        }
    }

    pledge open(id: Int, owner: String, amount: Float) -> Result<Bool, LedgerErr> {
        let row = Accounts { id: id, balance: amount, owner: owner }
        return match Store.insert(Accounts, row) {
            Ok(_) -> Ok(true)
            _ -> Err(StoreBroke)
        }
    }
}
