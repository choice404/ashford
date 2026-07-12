// An internal contract named as a type annotation from another package.

import priv.secret

contract AnnUser {
    clause hold(s: Secret) -> Int {
        return 1
    }

    pledge go() -> Result<Int, Int> {
        return Ok(1)
    }
}
