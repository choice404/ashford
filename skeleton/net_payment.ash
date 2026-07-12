// The N2 transparency skeleton: the payment service the network gate drives
// both ways, once by loading this module locally and once by connecting to an
// ashd that loads it, with the same host code and the same assertions on each
// side. It is payment.ash with one change, charge carries a body instead of
// staying abstract, because the daemon has no host to bind an implementation
// and every pledge it serves must dispatch on its own. The vow, the two named
// subcontracts, the loose notify_user, and the requirements policy are the
// same, so the partial surface the gate asserts over is the real thing the
// runtime evaluator computes.

contract PaymentService {
    vow currency: String = "USD"

    subcontract Validation {
        pledge validate_card(card: String) -> Result<Bool, Int> {
            return Ok(true)
        }
        pledge validate_amount(amount: Float) -> Result<Bool, Int> {
            if amount > 0.0 {
                return Ok(true)
            }
            return Err(1)
        }
    }

    subcontract Processing {
        pledge charge(card: String, amount: Float) -> Result<Bool, Int> {
            if amount > 0.0 {
                return Ok(true)
            }
            return Err(2)
        }
    }

    pledge notify_user(ok: Bool) -> Result<Bool, Int> {
        return Ok(true)
    }

    requirements {
        fulfill: Validation && Processing && notify_user
        partial: Validation || Processing
        break: !Validation && !Processing && !notify_user
    }
}
