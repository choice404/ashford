// The standard library consumer: one root file whose imports pull four
// ashstd modules through the loader into a single compiled module, so the
// build registers MathOps, ListOps, and this contract side by side and a
// host signs any of them by name. What a module
// lends another in this version is its types: the record and sum shapes, the
// error sums for the E slot, and the provisional clauses a contract
// incorporates and implements. Calling into another contract from a pledge
// body, sign and the instance operations, is still a named build error in
// the code generator, so cross contract use lives with the host, which signs
// each ashstd contract through the C API. StdUser incorporates Comparable
// from ashstd.traits, implements its compare clause, and sorts through it
// with index assignment; check_positive fails through CommonError from
// ashstd.errors. tests/runtime/test_std.c drives all of it from C.

import ashstd.math
import ashstd.collections
import ashstd.traits
import ashstd.errors

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
}
