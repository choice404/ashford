// Assignment targets a let mut binding; a plain let stays immutable.

contract Broken {
    clause bump() -> Int {
        let n = 1
        n = 2
        return n
    }
}
