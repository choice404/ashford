// String '+' is concatenation: both operands String, the result String, in
// every position an expression stands. A numeric operand beside a String
// still faults under the numeric rule, covered by the mixed_arith fixture.

contract Concat {
    vow prefix: String = "hello, "

    pledge greet(name: String) -> Result<String, String> {
        let joined = prefix + name
        let doubled: String = joined + "!" + "!"
        if doubled == prefix + "world!!" {
            return Ok(doubled)
        }
        return Err("mismatch" + "!")
    }
}
