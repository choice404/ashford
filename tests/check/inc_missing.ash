// An incorporate whose provisional clause carries a signature the contract
// never implements.

provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

contract Silent {
    incorporate Loggable

    pledge run() -> Result<Unit, RunError>
}
