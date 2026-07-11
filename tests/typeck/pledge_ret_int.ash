// A pledge return type must be Result, Option, or Unit; Int is none of them.

contract Broken {
    pledge count() -> Int
}
