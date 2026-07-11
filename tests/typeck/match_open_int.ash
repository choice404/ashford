// A match over an open type such as Int needs a final '_' or binding arm.

contract Broken {
    clause pick(n: Int) -> Int {
        return match n {
            0 -> 0
            1 -> 1
        }
    }
}
