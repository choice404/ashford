// Vows, pledges, and clauses share the contract namespace: a repeated vow is
// an error, and a pledge reusing a vow's name is the same error across kinds.

contract Config {
    vow retries: Int = 3
    vow retries: Int = 5
    vow timeout: Int = 10

    pledge timeout() -> Result<Unit, ConfigError>
}
