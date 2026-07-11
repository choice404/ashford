// The README taste of the language, verbatim: a provisional clause and the
// PaymentService contract with an incorporate, a vow, two named subcontracts,
// a loose pledge, a requirements block, and a clause body.

provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

contract PaymentService {
    incorporate Loggable

    vow currency: String = "USD"

    subcontract Validation {
        pledge validate_card(card: Card) -> Result<Bool, ValidationError>
        pledge validate_amount(amount: Float) -> Result<Bool, ValidationError>
    }

    subcontract Processing {
        pledge charge(card: Card, amount: Float) -> Result<Receipt, PaymentError>
    }

    pledge notify_user(receipt: Receipt) -> Result<Unit, NotifyError>

    requirements {
        fulfill: Validation && Processing && notify_user
        partial: Validation || Processing
        break: !Validation && !Processing && !notify_user
    }

    clause log(message: String) -> Result<Unit, LogError> {
        // log implementation
    }
}
