// A vow of contract type: vows cross the boundary at sign time.

contract Vault {
    pledge ping() -> Result<Int, Int>
}

contract Gate {
    vow keeper: Vault
}
