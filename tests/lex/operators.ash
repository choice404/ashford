// Every operator and punctuation token the grammar names, in plausible
// surroundings: the comparison and logic ladder, arithmetic, propagation,
// indexing, member access, and the keyword-after-dot carve out.

contract Ops {
    vow limit: Int = 16

    pledge check(xs: List<Int>, m: Map<String, Int>) -> Result<Int, Error> {
        let mut total = 0
        for x in xs {
            if x % 2 == 0 && x != limit || !(x <= 1) {
                total = total + x * 2 - x / 3
            }
            if x >= limit {
                break
            }
            if x < 0 {
                continue
            }
        }
        let first = xs[0]
        let pair = (first, total)
        let empty = []
        let looked = m.get("total")?
        match looked {
            Some(v) -> Ok(v)
            None -> Ok(total)
            _ -> Err(Error { code: 1 })
        }
    }

    pledge teardown(payment: Contract) -> Unit {
        payment.break()
    }
}
