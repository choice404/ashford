// ashstd.collections: the list contract, concrete over List<Int> since user
// declarations take no type parameters yet. The surface is what a list the
// language can walk but never grow supports: every pledge is a single for-in
// pass with an accumulator, nothing pushes, and nothing asks for a length
// the language cannot read. The reductions that need a first element to seed
// return Option and answer None on the empty list; index_of counts slots as
// it walks and answers None on a miss; product refuses to wrap by checking
// each multiply against the Int bounds before it runs. A generic ListOps,
// and any operation that builds a new list, waits on the surface growing
// generics and a push.

import ashstd.errors

contract ListOps {
    clause list_int_max() -> Int {
        return 9223372036854775807
    }

    clause list_int_min() -> Int {
        return -9223372036854775807 - 1
    }

    clause list_mul_overflows(a: Int, b: Int) -> Bool {
        if a == 0 || b == 0 {
            return false
        }
        if a > 0 {
            if b > 0 {
                return a > list_int_max() / b
            }
            return b < list_int_min() / a
        }
        if b > 0 {
            return a < list_int_min() / b
        }
        return b < list_int_max() / a
    }

    pledge sum(xs: List<Int>) -> Result<Int, CommonError> {
        let mut total = 0
        for x in xs {
            total = total + x
        }
        return Ok(total)
    }

    pledge product(xs: List<Int>) -> Result<Int, CommonError> {
        let mut acc = 1
        for x in xs {
            if list_mul_overflows(acc, x) {
                return Err(Invalid)
            }
            acc = acc * x
        }
        return Ok(acc)
    }

    pledge min_of(xs: List<Int>) -> Option<Int> {
        let mut found = false
        let mut best = 0
        for x in xs {
            if !found {
                best = x
                found = true
            } else if x < best {
                best = x
            }
        }
        if !found {
            return None
        }
        return Some(best)
    }

    pledge max_of(xs: List<Int>) -> Option<Int> {
        let mut found = false
        let mut best = 0
        for x in xs {
            if !found {
                best = x
                found = true
            } else if x > best {
                best = x
            }
        }
        if !found {
            return None
        }
        return Some(best)
    }

    pledge contains(xs: List<Int>, want: Int) -> Result<Bool, CommonError> {
        for x in xs {
            if x == want {
                return Ok(true)
            }
        }
        return Ok(false)
    }

    pledge count_of(xs: List<Int>, want: Int) -> Result<Int, CommonError> {
        let mut n = 0
        for x in xs {
            if x == want {
                n = n + 1
            }
        }
        return Ok(n)
    }

    pledge index_of(xs: List<Int>, want: Int) -> Option<Int> {
        let mut i = 0
        for x in xs {
            if x == want {
                return Some(i)
            }
            i = i + 1
        }
        return None
    }

    pledge all_eq(xs: List<Int>, want: Int) -> Result<Bool, CommonError> {
        for x in xs {
            if x != want {
                return Ok(false)
            }
        }
        return Ok(true)
    }
}
