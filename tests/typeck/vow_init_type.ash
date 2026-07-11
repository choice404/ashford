// A vow initializer must be a constant of the vow's own type.

contract Broken {
    vow currency: String = 5
}
