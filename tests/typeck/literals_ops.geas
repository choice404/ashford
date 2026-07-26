// Literals and operators under the expected type rule: an integer literal
// becomes UInt or Byte where the position asks and the value fits, floats
// stay Float, and every operator holds its row of the table. Mutation goes
// through let mut, assignment reaches fields and elements of mut bindings,
// and mut() copies a vow into assignable space.

record Point {
    x: Int
    y: Int
}

contract Arithmetic {
    vow origin: Int = 0
    vow scale: Float = 2.5
    vow greeting: String = "hi"
    vow enabled: Bool = true
    vow marker: Char = 'x'
    vow offset: Int = -7

    clause work() -> Int {
        let a: Int = 40 + 2
        let u: UInt = 40 + 2
        let b: Byte = 255
        let f = 1.5 * 2.0 - 0.25 / 4.0
        let m = 17 % 5
        let neg = -a
        let cmp = a < 100
        let eqs = a == 42
        let logic = cmp && !eqs || true
        let mut total = origin
        total = total + a
        let mut p = Point { x: 1, y: 2 }
        p.x = total
        let mut xs = [1, 2, 3]
        let first = xs[0]
        let pair = (a, f)
        let copy = mut(origin)
        let mut i = 0
        while i < 10 {
            i = i + 1
            if i == 5 {
                continue
            }
        }
        for v in xs {
            total = total + v
        }
        return total
    }
}
