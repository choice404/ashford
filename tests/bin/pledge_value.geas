// The first class pledge value read off an instance type checks, the member
// form the grammar admits, but it has no lowering yet: the code generator
// must refuse it as a named build error rather than emit C it would be
// guessing at.

contract Helper {
    pledge ping() -> Result<Int, Int> {
        return Ok(1)
    }
}

contract Main {
    pledge run(args: List<String>) -> Result<Int, Int> {
        let h = Helper.sign()
        let p = h.ping
        h.break()
        return Ok(0)
    }
}
