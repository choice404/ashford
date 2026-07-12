// A record field of contract type: records are plain data copyable across
// the ABI, so an instance cannot hide inside one.

contract Vault {
    pledge ping() -> Result<Int, Int>
}

record Holder {
    v: Vault
}
