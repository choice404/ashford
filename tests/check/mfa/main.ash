// The multi-file accept case: a sibling import and a dotted package import
// merge into one module. The contract uses a record from the sibling, a sum
// from the dotted package, incorporates the sibling's provisional clause,
// and calls its implementing clause from a pledge body.

import util
import geo.shapes

contract Front {
    incorporate Shared

    vow origin: String = "front"

    pledge locate(x: Int) -> Result<Int, Int> {
        let p = Pair { a: x, b: 2 }
        let s = Circle(p.a)
        let n = match s {
            Circle(r) -> r
            Square(side) -> side
        }
        return Ok(describe(n))
    }

    clause describe(n: Int) -> Int {
        return n + 1
    }
}
