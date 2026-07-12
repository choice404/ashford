// An index read on a map takes the map's key type exactly.

contract Broken {
    clause read() -> Int {
        let mut m = Map<String, Int>()
        m["a"] = 1
        let x = m[3]
        return 0
    }
}
