// The accept twin: a contract type is legal where the value never leaves
// the module, a clause parameter and a let annotation, and the clause can
// fulfill a pledge on the instance it received.

contract Vault {
    pledge ping() -> Result<Int, Int> {
        return Ok(7)
    }
}

contract Gate {
    pledge open() -> Result<Int, Int> {
        let v: Vault = Vault.sign()
        return Ok(probe(v))
    }

    clause probe(v: Vault) -> Int {
        let r = v.ping()
        return 2
    }
}
