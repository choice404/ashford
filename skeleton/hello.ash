// The walking skeleton. This contract is the program every milestone keeps
// green: ashc compiles it, a C host signs it, fulfills greet, checks the
// result, and breaks it. The M0 compiler reads this file only to prove the
// pipeline; real parsing arrives with M1.

GreetError is either Empty

contract Greeter {
    vow prefix: String = "hello, "

    pledge greet(name: String) -> Result<String, GreetError> {
        return Ok(prefix + name)
    }

    // Abstract: no body here, the host binds an implementation before sign.
    pledge shout(name: String) -> Result<String, GreetError>
}
