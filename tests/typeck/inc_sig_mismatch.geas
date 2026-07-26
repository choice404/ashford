// An incorporated provisional clause pins the implementing clause to its
// exact signature; a different parameter type is a different clause.

provisional clause Loggable {
    clause log(message: String) -> Result<Unit, String>
}

contract Broken {
    incorporate Loggable

    clause log(message: Int) -> Result<Unit, String> {
        return Err("full")
    }
}
