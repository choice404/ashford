// The instance surface skeleton: the lifecycle read and written from inside
// the language. Driver walks a Counter instance through the whole story,
// sign with an override, status() at each station, the vow read back through
// the instance, park into a store row, break, and resume, and answers the
// stations as one string so a host asserts the walk in one comparison. The
// two refusal pledges pin the fault convention: a park against a dsn that
// will not open faults the enclosing pledge with ASH_ERR_STORE, and a resume
// of a key nobody parked faults it with ASH_ERR_NAME, the same statuses the
// C surface answers.

contract Counter {
    vow label: String = "counter"

    pledge bump(n: Int) -> Result<Int, Int> {
        if n < 0 {
            return Err(1)
        }
        return Ok(n + 1)
    }
}

// Two loose pledges under an explicit policy, so the partial surface has a
// real middle to report: one lands and the contract sits Partial with one
// name in each list.
contract Duo {
    pledge first(x: Int) -> Result<Bool, Int> {
        return Ok(x > 0)
    }

    pledge second(x: Int) -> Result<Bool, Int> {
        return Ok(true)
    }

    requirements {
        fulfill: first && second
        partial: first || second
    }
}

contract Driver {
    vow park_dsn: String = "file:target/lifecycle.db"

    pledge walk() -> Result<String, Int> {
        let c = Counter.sign(label: "alpha")
        let s0 = c.status()
        let before = c.label
        let r = c.bump(1)
        match r {
            Ok(v) -> {
                if v != 2 {
                    return Err(10)
                }
            }
            Err(e) -> { return Err(e) }
        }
        let s1 = c.status()
        c.park(park_dsn, "walk-1")
        c.break()

        let d = Counter.resume(park_dsn, "walk-1")
        let s2 = d.status()
        let after = d.label
        let r2 = d.bump(4)
        match r2 {
            Ok(v2) -> {
                if v2 != 5 {
                    return Err(11)
                }
            }
            Err(e2) -> { return Err(e2) }
        }
        d.break()
        return Ok(s0 + "," + s1 + "," + s2 + "," + before + "," + after)
    }

    // The partial surface as a value: the record before any fulfillment and
    // the record after one, their states and their name lists read as
    // ordinary fields, answered as one string for the host to compare.
    pledge panel() -> Result<String, Int> {
        let d = Duo.sign()
        let p0 = d.partial()
        let r = d.first(3)
        match r {
            Ok(v) -> {
                if !v {
                    return Err(20)
                }
            }
            Err(e) -> { return Err(e) }
        }
        let p1 = d.partial()
        let f = p1.fulfilled
        let pend = p1.pending
        d.break()
        return Ok(p0.state + "," + p1.state + "," + f[0] + "," + pend[0])
    }

    pledge bad_park() -> Result<Bool, Int> {
        let c = Counter.sign()
        c.park("file:/ashford-no-such-dir/x.db", "doomed")
        c.break()
        return Ok(true)
    }

    pledge bad_resume() -> Result<Bool, Int> {
        let d = Counter.resume(park_dsn, "ghost")
        d.break()
        return Ok(true)
    }
}
