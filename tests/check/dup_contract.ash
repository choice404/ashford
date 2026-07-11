// Two contracts with the same name at the top level.

contract Payment {
    pledge charge(amount: Float) -> Result<Unit, PayError>
}

contract Payment {
    pledge refund(amount: Float) -> Result<Unit, PayError>
}
