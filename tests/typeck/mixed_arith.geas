// There is no implicit numeric conversion anywhere, widening included, so
// Int and Float never meet under one operator.

contract Broken {
    clause add() -> Int {
        let mixed = 1 + 2.0
        return 0
    }
}
