// ashstd.math: the integer and float arithmetic contract. Every operation is
// a pledge on MathOps, so a host signs the contract and fulfills them by
// name, and the error paths ride the AshMathError sum in the Result's E
// slot. The sum's variants would have been DomainError and Overflow, but
// sum variant names share one global namespace with every importer and the
// misc fixture already owns Overflow, so the second variant is Overflowed
// here. abs reports Overflowed on the one integer whose magnitude does not
// fit, pow_int guards every multiply through the mul_overflows clause, and
// the float pair of each pledge carries the _f suffix. The epsilon vow
// feeds approx_eq and can be overridden at sign time. AshMathRatio is the
// original placeholder record and keeps its spot in the surface.

AshMathError is either DomainError or Overflowed

record AshMathRatio {
    num: Int
    den: Int
}

contract MathOps {
    vow epsilon: Float = 0.000001

    // The largest Int, and the smallest built from it, since the minimum's
    // magnitude is one past what a literal can spell.
    clause int_max() -> Int {
        return 9223372036854775807
    }

    clause int_min() -> Int {
        return -9223372036854775807 - 1
    }

    // Whether a * b leaves the Int range, decided by division bounds before
    // the multiply ever happens, so no lowered arithmetic wraps.
    clause mul_overflows(a: Int, b: Int) -> Bool {
        if a == 0 || b == 0 {
            return false
        }
        if a > 0 {
            if b > 0 {
                return a > int_max() / b
            }
            return b < int_min() / a
        }
        if b > 0 {
            return a < int_min() / b
        }
        return b < int_max() / a
    }

    clause abs_float(x: Float) -> Float {
        if x < 0.0 {
            return -x
        }
        return x
    }

    pledge abs(n: Int) -> Result<Int, AshMathError> {
        if n == int_min() {
            return Err(Overflowed)
        }
        if n < 0 {
            return Ok(-n)
        }
        return Ok(n)
    }

    pledge sign(n: Int) -> Result<Int, AshMathError> {
        if n < 0 {
            return Ok(-1)
        }
        if n > 0 {
            return Ok(1)
        }
        return Ok(0)
    }

    pledge min(a: Int, b: Int) -> Result<Int, AshMathError> {
        if a < b {
            return Ok(a)
        }
        return Ok(b)
    }

    pledge max(a: Int, b: Int) -> Result<Int, AshMathError> {
        if a > b {
            return Ok(a)
        }
        return Ok(b)
    }

    pledge clamp(x: Int, lo: Int, hi: Int) -> Result<Int, AshMathError> {
        if lo > hi {
            return Err(DomainError)
        }
        if x < lo {
            return Ok(lo)
        }
        if x > hi {
            return Ok(hi)
        }
        return Ok(x)
    }

    // Integer exponentiation by repeated multiply. A negative exponent is a
    // domain fault, and every step is overflow checked before it runs.
    pledge pow_int(base: Int, exp: Int) -> Result<Int, AshMathError> {
        if exp < 0 {
            return Err(DomainError)
        }
        let mut acc = 1
        let mut i = 0
        while i < exp {
            if mul_overflows(acc, base) {
                return Err(Overflowed)
            }
            acc = acc * base
            i = i + 1
        }
        return Ok(acc)
    }

    pledge abs_f(x: Float) -> Result<Float, AshMathError> {
        return Ok(abs_float(x))
    }

    pledge sign_f(x: Float) -> Result<Float, AshMathError> {
        if x < 0.0 {
            return Ok(-1.0)
        }
        if x > 0.0 {
            return Ok(1.0)
        }
        return Ok(0.0)
    }

    pledge min_f(a: Float, b: Float) -> Result<Float, AshMathError> {
        if a < b {
            return Ok(a)
        }
        return Ok(b)
    }

    pledge max_f(a: Float, b: Float) -> Result<Float, AshMathError> {
        if a > b {
            return Ok(a)
        }
        return Ok(b)
    }

    pledge clamp_f(x: Float, lo: Float, hi: Float) -> Result<Float, AshMathError> {
        if lo > hi {
            return Err(DomainError)
        }
        if x < lo {
            return Ok(lo)
        }
        if x > hi {
            return Ok(hi)
        }
        return Ok(x)
    }

    // Equality within the epsilon vow, the tolerance a signer can override.
    pledge approx_eq(a: Float, b: Float) -> Result<Bool, AshMathError> {
        return Ok(abs_float(a - b) <= epsilon)
    }
}
