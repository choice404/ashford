// The standard library accept fixture: every ashstd module loads through one
// root, the contracts and sums they declare all resolve and typecheck in one
// merged program, and a local contract incorporates two of the shared
// provisional clauses and touches a sum from each error vocabulary.

import ashstd.math
import ashstd.strings
import ashstd.collections
import ashstd.errors
import ashstd.traits

contract StdCheck {
    incorporate Comparable
    incorporate Serializable

    clause compare(a: Int, b: Int) -> Int {
        if a < b {
            return -1
        }
        if a > b {
            return 1
        }
        return 0
    }

    clause serialize_tag() -> String {
        return "stdcheck"
    }

    pledge smallest(a: Int, b: Int) -> Result<Int, CommonError> {
        if compare(a, b) <= 0 {
            return Ok(a)
        }
        return Ok(b)
    }

    pledge widen(x: Int) -> Result<Int, AshMathError> {
        if x == 0 {
            return Err(DomainError)
        }
        return Ok(x * 2)
    }
}
