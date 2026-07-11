// Match as an expression and as a statement, with every pattern form:
// literals, the wildcard, bindings, Option and Result constructors, variant
// payloads, and a nested pattern inside Err. Arm bodies come in both the
// expression and the block form.

MathError is either DivByZero or Overflow(detail: String)

contract Matcher {
    clause classify(n: Int) -> String {
        let word = match n {
            0 -> "zero"
            1 -> { "one" }
            _ -> "many"
        }
        return word
    }

    clause unwrap(o: Option<Int>, r: Result<Int, MathError>) -> Int {
        let x = match o {
            Some(v) -> v
            None -> 0
        }
        let y = match r {
            Ok(v) -> v + 1
            Err(DivByZero) -> -1
            Err(Overflow(detail)) -> -2
            other -> 0
        }
        match x {
            0 -> {
                return y
            }
            _ -> {
                return x + y
            }
        }
    }
}
