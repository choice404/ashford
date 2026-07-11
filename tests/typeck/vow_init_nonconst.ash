// A vow initializer must be a constant literal, not a computed expression.

contract Broken {
    vow retries: Int = 1 + 2
}
