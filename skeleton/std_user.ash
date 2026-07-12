// The standard library consumer: one root file whose imports pull four
// ashstd modules through the loader into a single compiled module, so the
// build registers MathOps, ListOps, IntGate, and this contract side by side
// and a host signs any of them by name. A module lends another its types,
// the record and sum shapes, the error sums for the E slot, the provisional
// clauses a contract incorporates, and now its contracts themselves: a
// pledge body signs another contract, fulfills its pledges, and breaks it,
// which is what compute below does against MathOps and IntGate. StdUser
// incorporates Comparable from ashstd.traits, implements its compare
// clause, and sorts through it with index assignment; check_positive fails
// through CommonError from ashstd.errors. tests/runtime/test_std.c drives
// all of it from C.

import ashstd.math
import ashstd.collections
import ashstd.traits
import ashstd.errors

// A small callee whose error type is Int, so compute can show '?' across a
// contract boundary where the error types agree: the gate doubles what fits
// and reports what does not as its own value.
contract IntGate {
    pledge double_pos(n: Int) -> Result<Int, Int> {
        if n > 1000 {
            return Err(n)
        }
        return Ok(n * 2)
    }
}

contract StdUser {
    incorporate Comparable
    incorporate Loggable

    clause compare(a: Int, b: Int) -> Int {
        if a < b {
            return -1
        }
        if a > b {
            return 1
        }
        return 0
    }

    clause log_line(msg: String) -> String {
        return "[stduser] " + msg
    }

    // Three elements sorted by the incorporated comparator: a fixed bubble
    // pass, each swap through list index assignment on a mut binding.
    pledge sort3(a: Int, b: Int, c: Int) -> Result<List<Int>, Int> {
        let mut xs = [a, b, c]
        if compare(xs[0], xs[1]) > 0 {
            let t = xs[0]
            xs[0] = xs[1]
            xs[1] = t
        }
        if compare(xs[1], xs[2]) > 0 {
            let t = xs[1]
            xs[1] = xs[2]
            xs[2] = t
        }
        if compare(xs[0], xs[1]) > 0 {
            let t = xs[0]
            xs[0] = xs[1]
            xs[1] = t
        }
        return Ok(xs)
    }

    pledge check_positive(n: Int) -> Result<Int, CommonError> {
        if n < 0 {
            return Err(Invalid)
        }
        return Ok(n)
    }

    pledge describe(n: Int) -> Result<String, CommonError> {
        if compare(n, 0) < 0 {
            return Ok(log_line("negative"))
        }
        return Ok(log_line("non-negative"))
    }

    // The cross-contract walk. MathOps signs with a vow override, abs runs
    // through the instance, and the instance breaks before the result is
    // read, which works because the call deep copied the result home. The
    // error sum translates by hand since AshMathError is not Int; the gate's
    // error type is Int, so its call propagates with '?', and a propagated
    // Err leaves the gate instance to the runtime shutdown on purpose.
    pledge compute(x: Int) -> Result<Int, Int> {
        let ops = MathOps.sign(epsilon: 0.5)
        let r = ops.abs(x)
        ops.break()
        match r {
            Ok(v) -> {
                let gate = IntGate.sign()
                let d = gate.double_pos(v)?
                gate.break()
                return Ok(d)
            }
            Err(_) -> {
                return Err(-1)
            }
        }
    }
}
