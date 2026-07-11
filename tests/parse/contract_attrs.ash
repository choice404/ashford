// Attribute lists on a contract header and after a pledge return type, an
// anonymous subcontract, an internal contract, a vow without a default, an
// abstract pledge bound to a foreign symbol, requirements with parentheses
// and negation, and an import heading the file.

import ashstd.math

contract Meter [version: 2] {
    vow scale: Float

    subcontract {
        pledge tick(count: Int) -> Result<Unit, MeterError>
    }

    pledge read() -> Result<Float, MeterError> [abi: "c", symbol: "meter_read"]

    pledge helper(x: Int) -> Option<Int> {
        let v = math.clamp(x, 0, 10)?
        return Some(v)
    }

    requirements {
        fulfill: read && helper
        partial: read || (helper && !read)
    }
}

internal contract Scratch {
    pledge poke() -> Unit
}

MeterError is either Cold or Hot
