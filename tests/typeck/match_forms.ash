// Every match shape: a sum covered variant by variant, payloads bound, an
// Option, a Result, a Bool over its two literals, an open Int closed by '_',
// a binding arm as the catch all, and block bodies whose final expression is
// the arm's value.

Level is either Low or High or Custom(value: Int, label: String)

contract Matcher {
    clause classify(l: Level) -> Int {
        return match l {
            Low -> 0
            High -> 1
            Custom(v, name) -> v
        }
    }

    clause options(o: Option<Int>) -> Int {
        return match o {
            Some(v) -> v + 1
            None -> 0
        }
    }

    clause results(r: Result<String, Int>) -> String {
        return match r {
            Ok(s) -> s
            Err(code) -> "failed"
        }
    }

    clause bools(b: Bool) -> Int {
        return match b {
            true -> 1
            false -> 0
        }
    }

    clause open(n: Int) -> String {
        return match n {
            0 -> "zero"
            1 -> "one"
            _ -> "many"
        }
    }

    clause bindings(n: Int) -> Int {
        return match n {
            0 -> 0
            other -> other * 2
        }
    }

    clause blocks(o: Option<Int>) -> Int {
        return match o {
            Some(v) -> {
                let doubled = v * 2
                doubled
            }
            None -> {
                0
            }
        }
    }
}
