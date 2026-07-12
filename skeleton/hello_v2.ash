// The version mismatch twin of the walking skeleton. Greeter2 is the shipped
// second generation: version 2 on the contract and a greet whose signature
// grew a parameter, so its mangled names agree with Greeter's on nothing.
// The iname gate loads this beside hello.ash and proves that yesterday's
// mangled name misses today's module with ASH_ERR_NAME instead of silently
// resolving to the wrong shape.

GreetError2 is either Empty

contract Greeter2 [version: 2] {
    vow prefix: String = "hello, "

    pledge greet(name: String, suffix: String) -> Result<String, GreetError2> {
        return Ok(prefix + name + suffix)
    }
}
