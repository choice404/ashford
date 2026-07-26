// '?' on an Option propagates None, so the enclosing body must return
// Option; this one returns Result.

contract Broken {
    clause fetch() -> Option<Int> {
        return Some(1)
    }

    clause use_it() -> Result<Int, String> {
        let v = fetch()?
        return Ok(v)
    }
}
