// The quiet corners: imports recorded without module resolution, data
// declarations beside contracts, an empty contract, and a contract with no
// requirements block, which falls back to the default policy untouched.

import ashstd.math
import ashstd.strings

record Card {
    number: String
    expiry: String
}

LogLevel is either Debug or Info or Warn
MathError is either DivByZero or Overflow(detail: String)

contract Empty {
}

contract Quiet {
    vow retries: Int = 3

    pledge ping() -> Result<Unit, MathError>
}
