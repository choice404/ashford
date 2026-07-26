// Insert-or-update mutates the map, so the binding must be let mut.

contract Broken {
    clause put() -> Int {
        let m = Map<String, Int>()
        m["a"] = 1
        return 0
    }
}
