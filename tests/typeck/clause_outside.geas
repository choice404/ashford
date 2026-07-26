// A clause is callable by bare name only inside its own contract; it is not
// reachable through a signed instance.

contract Service {
    pledge ping() -> Unit

    clause helper(n: Int) -> Int {
        return n
    }
}

contract Main {
    pledge run(args: List<String>) -> Result<Int, String> {
        let s = Service.sign()
        let v = s.helper(1)
        return Ok(0)
    }
}
