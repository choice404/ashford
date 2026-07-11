// '?' on a Result propagates the Err, so the enclosing body's error type
// must be the same one.

contract Broken {
    clause fetch() -> Result<Int, String> {
        return Ok(1)
    }

    clause use_it() -> Result<Int, Int> {
        let v = fetch()?
        return Ok(v)
    }
}
