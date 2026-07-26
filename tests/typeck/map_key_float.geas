// The keyable set: a Map key is Int, UInt, Bool, Byte, Char, or String, and
// Float is not among them.

contract Broken {
    clause build() -> Int {
        let m = Map<Float, Int>()
        return 0
    }
}
