// The M7 partial fulfillment skeleton: the README PaymentService reshaped to
// what the M3 code generator lowers today. Parameters are scalars and
// strings, the error slot is Int, and no records appear. Two named
// subcontracts group the validation and processing pledges, notify_user
// stays loose, and the requirements block is the README policy verbatim, so
// the compiled descriptor carries a real source policy for the runtime
// evaluator and the partial result gate to chew on. charge is abstract on
// purpose: the C host binds it and steers it between Ok and Err to walk the
// partial, broken, fulfilled, and automatic break paths.

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
        pledge charge(card: String, amount: Float) -> Result<Bool, Int>
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
