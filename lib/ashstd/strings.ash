// ashstd.strings: the string contract the current surface can honestly
// offer. The language gives String exactly two operations, concatenation and
// equality, and no way to read a length, a byte, or a case table, so the
// contract is built from concatenation alone: repeat builds n copies of a
// string, join glues two strings around a separator, and wrap parenthesizes
// a string between a prefix and a suffix. The classics that need more than
// concatenation stay out rather than ship broken: pad needs the string's
// width, upper and lower need case mapping, trim and split need byte access.
// Each lands when the surface grows the primitive it stands on. AshStrSpan
// is the original placeholder record and keeps its spot.

import ashstd.errors

record AshStrSpan {
    text: String
    width: Int
}

contract StringOps {
    pledge repeat(s: String, n: Int) -> Result<String, CommonError> {
        if n < 0 {
            return Err(Invalid)
        }
        let mut out = ""
        let mut i = 0
        while i < n {
            out = out + s
            i = i + 1
        }
        return Ok(out)
    }

    pledge join(a: String, b: String, sep: String) -> Result<String, CommonError> {
        return Ok(a + sep + b)
    }

    pledge wrap(s: String, prefix: String, suffix: String) -> Result<String, CommonError> {
        return Ok(prefix + s + suffix)
    }
}
