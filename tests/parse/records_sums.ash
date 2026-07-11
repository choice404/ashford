// Data declarations: records, sum declarations with and without payloads,
// and every builtin composite in field position, including the first class
// pledge type.

record Card {
    number: String
    expiry: String
}

record Point {
    x: Int
    y: Int
}

LogLevel is either Debug or Info or Error
MathError is either DivByZero or Overflow(detail: String)
Shape is either Circle(radius: Float) or Rect(w: Float, h: Float)

record Holder {
    cards: List<Card>
    lookup: Map<String, Int>
    maybe: Option<Card>
    outcome: Result<Card, MathError>
    pair: Tuple<Int, String>
    hook: pledge(Int, String) -> Result<Unit, MathError>
}
