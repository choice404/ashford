// The same pledge declared twice inside one named subcontract.

contract Payment {
    subcontract Validation {
        pledge check(card: Card) -> Result<Bool, ValidationError>
        pledge check(card: Card) -> Result<Bool, ValidationError>
    }
}
