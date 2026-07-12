// A contract type in a pledge parameter: an instance has no tagged ABI
// representation, so the boundary cannot carry or check it.

contract Vault {
    pledge ping() -> Result<Int, Int>
}

contract Gate {
    pledge accept(v: Vault) -> Result<Int, Int> {
        return Ok(1)
    }
}
