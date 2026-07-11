// The README payment example grown real bodies: the provisional clause is
// implemented, the loose pledge and one subcontract pledge carry Ashford
// bodies, and a Main contract signs the service and drives it end to end.

record Card {
    number: String
    expiry: String
}

record Receipt {
    id: Int
    total: Float
}

ValidationError is either BadNumber or Expired(when: String)
PaymentError is either Declined(reason: String) or NetworkDown
NotifyError is either NoAddress
LogError is either Full

provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

contract PaymentService {
    incorporate Loggable

    vow currency: String = "USD"

    subcontract Validation {
        pledge validate_card(card: Card) -> Result<Bool, ValidationError>
        pledge validate_amount(amount: Float) -> Result<Bool, ValidationError> {
            if amount > 0.0 {
                return Ok(true)
            }
            return Ok(false)
        }
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
        let tagged = describe(message)
        let n = 1
        return Err(Full)
    }

    clause describe(message: String) -> String {
        return message
    }
}

contract Main {
    pledge run(args: List<String>) -> Result<Int, PaymentError> {
        let payment = PaymentService.sign(currency: "EUR")
        let card = Card { number: "4111 1111", expiry: "12/30" }
        let verdict = payment.validate_card(card)
        let receipt = payment.charge(card, 25.0)?
        let checker = payment.validate_amount
        let again = checker(receipt.total)
        let code = match verdict {
            Ok(good) -> {
                let base = receipt.id
                base + 1
            }
            Err(e) -> 0
        }
        payment.break()
        return Ok(code)
    }
}
