// The statement forms: let and let mut with and without a type annotation,
// while with break and continue, for over a sequence, assignment to a
// binding, a field, and an element, if with an else if chain, and both
// return forms.

FlowError is either Empty

contract Flow {
    vow limit: Int = 10

    pledge run(args: List<String>) -> Result<Int, FlowError> {
        let mut total: Int = 0
        let mut i = 0
        while i < limit {
            if i == 7 {
                i = i + 1
                continue
            }
            if total > 100 {
                break
            }
            total = total + i
            i = i + 1
        }
        for arg in args {
            total = total + 1
        }
        let mut copy = mut(limit)
        copy = copy + 1
        grid[0] = total
        holder.point.x = 5
        if total == 0 {
            return Err(FlowError.Empty)
        } else if total == 1 {
            log_once()
        } else {
            total = total - 1
        }
        return Ok(total)
    }

    clause log_once() -> Unit {
        return
    }
}
