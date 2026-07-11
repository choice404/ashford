// None with no expected type has no element type to take; the binding needs
// an annotation.

contract Broken {
    clause make() -> Unit {
        let empty = None
    }
}
