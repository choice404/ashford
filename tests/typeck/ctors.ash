// Constructors: a record literal field by field, sum variants positional and
// labeled, and Some, Ok, Err, and None taking their payload types from the
// annotation, the argument position, and the return type.

record Address {
    street: String
    number: Int
}

Shape is either Dot or Box(width: Int, height: Int)

contract Builder {
    clause build(street: String) -> Address {
        return Address { street: street, number: 12 }
    }

    clause shapes() -> Shape {
        let d: Shape = Dot
        let positional = Box(3, 4)
        let labeled = Box(width: 3, height: 4)
        return labeled
    }

    clause opt(flag: Bool) -> Option<Address> {
        if flag {
            return Some(Address { street: "Main", number: 1 })
        }
        let empty: Option<Address> = None
        return empty
    }

    clause res(flag: Bool) -> Result<Address, String> {
        let good: Result<Address, String> = Ok(Address { street: "High", number: 2 })
        if flag {
            return good
        }
        return Err("no address")
    }

    clause nested() -> Option<List<Int>> {
        let xs: List<Int> = []
        return Some(xs)
    }
}
