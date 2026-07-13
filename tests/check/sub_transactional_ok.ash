// A transactional subcontract over a store-backed contract, the shape the
// modifier is for: a schema binds a table, so the group of writes has a
// database to be durable against and the modifier is accepted.

contract Bank {
    vow dsn: String = "file:bank.db"

    schema Accounts {
        id: Int
        balance: Float
    }

    subcontract Move transactional {
        pledge debit(id: Int) -> Result<Unit, BankError>
        pledge credit(id: Int) -> Result<Unit, BankError>
    }
}

BankError is either A or B
