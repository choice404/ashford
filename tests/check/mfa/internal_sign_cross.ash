// An internal contract signed from another package.

import priv.secret

contract CallsSecret {
    pledge go() -> Result<Int, Int> {
        let s = Secret.sign()
        return Ok(1)
    }
}
