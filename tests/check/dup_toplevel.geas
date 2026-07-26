// Top level names live in one namespace across kinds: a sum variant repeated
// in another sum, and a contract reusing a record's name.

record Card {
    number: String
}

LogLevel is either Debug or Info
Trace is either Debug or Fine

contract Card {
    pledge charge(card: Card) -> Result<Unit, PayError>
}
