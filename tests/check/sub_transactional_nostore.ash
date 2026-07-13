// The transactional modifier on a subcontract of a contract that binds no
// database. A transaction groups durable writes, so it is only meaningful over
// a store-backed contract, one that declares at least one schema; this one
// declares none and the modifier is a type error.

contract Plain {
    subcontract Group transactional {
        pledge one() -> Result<Unit, PlainError>
        pledge two() -> Result<Unit, PlainError>
    }
}

PlainError is either A or B
