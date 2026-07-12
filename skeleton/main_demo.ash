// The standalone demo. ashc build --bin compiles this file into a real
// executable: the emitted wrapper signs Main, hands argv to run as a
// List<String>, and maps the Result onto the process exit code. Counting
// the arguments keeps the Ok path observable from a shell, `main_demo a b c`
// exits 3, and the word fail flips the program onto the Err path, which the
// wrapper renders to stderr before exiting 1. Sidekick rides along to prove
// a --bin module still emits every contract in the file, not Main alone.

DemoError is either BadMood(reason: String) or Empty

contract Sidekick {
    pledge double(x: Int) -> Result<Int, DemoError> {
        return Ok(x * 2)
    }
}

contract Main {
    vow greeting: String = "counted"

    pledge run(args: List<String>) -> Result<Int, DemoError> {
        let mut n = 0
        for a in args {
            if a == "fail" {
                return Err(BadMood("asked to fail"))
            }
            n = n + 1
        }
        return Ok(n)
    }
}
