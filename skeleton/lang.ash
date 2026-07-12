// The language gauntlet: one contract whose pledge bodies walk every
// construct the code generator lowers. Loops with break and continue,
// assignment to locals, record fields, and list elements, list, tuple, and
// record literals, sum construction and matching with payload bindings,
// Option and Result matching, the propagation operator, deep equality, and
// the Byte, UInt, and Char literal forms. tests/runtime/test_lang.c drives
// every pledge from C and asserts each path, the out of bounds index
// included, which returns ASH_ERR_TYPE from the thunk by the ABI's rule.

record Point {
    x: Int
    y: Int
}

Grade is either Pass(score: Int) or Fail(reason: String) or Skip

contract LangGauntlet {
    vow greeting: String = "lang"

    pledge sum_list(xs: List<Int>) -> Result<Int, Int> {
        let mut total = 0
        for x in xs {
            total = total + x
        }
        return Ok(total)
    }

    pledge classify(n: Int) -> Result<String, Int> {
        let g: Grade = match n < 0 {
            true -> Fail("neg")
            false -> Pass(n)
        }
        match g {
            Pass(s) -> {
                let mut p = Point { x: s, y: 0 }
                p.y = p.x + 1
                if p.y > 10 {
                    return Ok("big")
                }
                return Ok("small")
            }
            Fail(reason) -> {
                return Err(1)
            }
            Skip -> {
                return Ok("skip")
            }
        }
        return Err(9)
    }

    pledge fizz(n: Int) -> Result<String, Int> {
        if n < 0 {
            return Err(-n)
        }
        let mut i = 0
        let mut out = ""
        while i < n {
            i = i + 1
            if i % 3 == 0 {
                out = out + "f"
                continue
            }
            out = out + "."
        }
        return Ok(out)
    }

    pledge find(xs: List<Int>, want: Int) -> Result<Bool, Int> {
        let mut hit = false
        for x in xs {
            if x == want {
                hit = true
                break
            }
        }
        return Ok(hit)
    }

    pledge pick(i: Int) -> Result<Int, Int> {
        let mut ys = [10, 20, 30]
        ys[1] = 21
        return Ok(ys[i])
    }

    pledge head_doubled(xs: List<Int>) -> Option<Int> {
        let mut found: Option<Int> = None
        for x in xs {
            found = Some(x)
            break
        }
        let h = found?
        return Some(h * 2)
    }

    pledge nest(n: Int) -> Result<Int, Int> {
        let r: Result<Int, Int> = match n {
            0 -> Err(0)
            _ -> Ok(n)
        }
        let v: Int = match r {
            Ok(x) -> match x > 5 {
                true -> x * 10
                false -> x
            }
            Err(e) -> e
        }
        return Ok(v + 1)
    }

    pledge deep(n: Int) -> Result<Int, Int> {
        let w: Result<Option<Int>, Int> = match n {
            0 -> Err(5)
            1 -> Ok(None)
            _ -> Ok(Some(n))
        }
        match w {
            Ok(Some(x)) -> {
                return Ok(x + 1)
            }
            Ok(None) -> {
                return Ok(0)
            }
            _ -> {
                return Err(7)
            }
        }
        return Err(9)
    }

    pledge misc(flag: Bool) -> Result<String, Int> {
        let bt: Byte = 7
        let u: UInt = 9
        let ch: Char = 'x'
        let t = (1, "one")
        let t2 = (1, "one")
        let mut acc = ""
        if t == t2 {
            acc = acc + "t"
        }
        if ch == 'x' {
            acc = acc + "c"
        }
        if bt == 7 {
            acc = acc + "b"
        }
        if u == 9 {
            acc = acc + "u"
        }
        let g = Skip
        if g == Skip {
            acc = acc + "s"
        }
        while false {
            acc = acc + "never"
        }
        let empty: List<Int> = []
        for z in empty {
            acc = acc + "never"
        }
        if !flag {
            return Err(2)
        }
        let msg = mut(greeting)
        return Ok(msg + ":" + acc)
    }

    // Short circuit: a bounds guard whose right operands never touch xs[i]
    // when the left has already decided the answer. i out of range takes the
    // Ok(false) path here, not the ASH_ERR_TYPE an eager xs[i] would raise.
    pledge guard(xs: List<Int>, i: Int) -> Result<Bool, Int> {
        return Ok(i >= 0 && i < 3 && xs[i] == 0)
    }

    // The '||' mirror: xs[i] is left untouched when i is already out of range,
    // so an out of bounds i is Ok(true) rather than a fault.
    pledge orguard(xs: List<Int>, i: Int) -> Result<Bool, Int> {
        return Ok(i >= 3 || xs[i] == 0)
    }

    // Value semantics on a record: the copy is mutated, the original is not.
    pledge rec_copy() -> Result<Int, Int> {
        let mut p = Point { x: 1, y: 2 }
        let mut q = p
        q.x = 99
        return Ok(p.x * 100 + q.x)
    }

    // Value semantics on a list: writing one binding's slot leaves the other
    // list alone.
    pledge list_copy() -> Result<Int, Int> {
        let mut a = [1, 2, 3]
        let mut b = a
        b[0] = 99
        return Ok(a[0] * 100 + b[0])
    }

    // A composite lifted out of a slot is its own copy, so mutating the row
    // read from the outer list never writes back through it.
    pledge nested_copy() -> Result<Int, Int> {
        let mut outer = [[1, 2], [3, 4]]
        let mut row = outer[0]
        row[0] = 99
        let check = outer[0]
        return Ok(check[0] * 100 + row[0])
    }

    // Clauses: contract private helpers the pledges below reach by bare
    // name. suffix_for returns a plain String, the any type rule a clause
    // enjoys and a pledge does not, and scaled calls a sibling clause from
    // inside a clause body.
    clause suffix_for(n: Int) -> String {
        if n > 9 {
            return "big"
        }
        return "small"
    }

    clause bump(n: Int) -> Int {
        return n + 1
    }

    clause scaled(n: Int) -> Int {
        return bump(n) * 2
    }

    clause total_of(xs: List<Int>) -> Int {
        let mut t = 0
        for x in xs {
            t = t + x
        }
        return t
    }

    pledge label(n: Int) -> Result<String, Int> {
        let s = suffix_for(n)
        return Ok(s + ":" + suffix_for(0))
    }

    pledge scale(n: Int) -> Result<Int, Int> {
        return Ok(scaled(n))
    }

    pledge total_twice(xs: List<Int>) -> Result<Int, Int> {
        return Ok(total_of(xs) + total_of(xs))
    }
}
