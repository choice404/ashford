// The two lawful homes of '?': an Option unwrap inside an Option returning
// body, and a Result unwrap inside a Result returning body whose error types
// agree, including through a chain of unwraps.

FetchError is either Timeout or Gone

contract Propagation {
    clause head(xs: List<Int>) -> Option<Int> {
        return Some(xs[0])
    }

    clause second(xs: List<Int>) -> Option<Int> {
        let first = head(xs)?
        return Some(first + 1)
    }

    clause fetch(id: Int) -> Result<String, FetchError> {
        return Err(Timeout)
    }

    clause fetch_both(a: Int, b: Int) -> Result<String, FetchError> {
        let left = fetch(a)?
        let right = fetch(b)?
        return Ok(left)
    }
}
