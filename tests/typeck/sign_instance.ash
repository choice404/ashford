// The contract operations: sign with named vow overrides, a defaulted vow
// left to its default, pledge fulfillment on the instance including a pledge
// inside a named subcontract, the first class pledge value bound and called
// later, break, and a clause called by bare name inside its own contract.

contract Store {
    vow region: String
    vow retries: Int = 3

    subcontract Reads {
        pledge get(key: String) -> Option<String>
    }

    pledge put(key: String, value: String) -> Result<Unit, String>

    clause key_of(n: Int) -> String {
        return "key"
    }

    clause exercise(n: Int) -> Option<String> {
        let k = key_of(n)
        return Some(k)
    }
}

contract Main {
    pledge run(args: List<String>) -> Result<Int, String> {
        let store = Store.sign(region: "eu-west", retries: 5)
        let missing = store.get("absent")
        let saved = store.put("name", "ashford")
        let getter = store.get
        let later = getter("cached")
        let annotated: pledge(String) -> Option<String> = store.get
        store.break()
        return Ok(0)
    }
}
