// A contract type nested inside a composite at an ABI position is as
// unrepresentable as a bare one.

contract Vault {
    pledge ping() -> Result<Int, Int>
}

contract Gate {
    pledge all_of() -> Result<List<Vault>, Int> {
        return Err(1)
    }
}
