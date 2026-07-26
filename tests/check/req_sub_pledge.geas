// A requirements atom naming a pledge inside a named subcontract. The policy
// tests the subcontract, never a pledge within it.

contract Payment {
    subcontract Validation {
        pledge validate_card(card: Card) -> Result<Bool, ValidationError>
    }

    requirements {
        fulfill: validate_card
    }
}
