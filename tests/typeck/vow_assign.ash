// A vow is never assignable, not even inside its own contract's bodies.

contract Broken {
    vow currency: String = "USD"

    clause change() -> Unit {
        currency = "EUR"
    }
}
