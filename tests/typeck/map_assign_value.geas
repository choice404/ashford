// m[k] = v checks v against the map's value type, not Option of it.

contract Broken {
    clause put() -> Int {
        let mut m = Map<String, Int>()
        m["a"] = "one"
        return 0
    }
}
