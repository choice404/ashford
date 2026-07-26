// A record literal supplies every field exactly.

record Card {
    number: String
    expiry: String
}

contract Broken {
    clause build() -> Card {
        return Card { number: "4111" }
    }
}
