// Every vow without a default must be supplied at sign time.

contract Service {
    vow region: String

    pledge ping() -> Unit
}

contract Main {
    pledge run(args: List<String>) -> Result<Int, String> {
        let s = Service.sign()
        return Ok(0)
    }
}
