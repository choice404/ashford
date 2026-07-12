// The map surface end to end: the constructor with its spelled type
// arguments, insert and update through index assignment, the Option an index
// read yields on hit and miss, and a map riding a record field.

record Cache {
    name: String
    entries: Map<String, Int>
}

contract MapUser {
    pledge lookup(k: String) -> Result<Int, Int> {
        let mut ages = Map<String, Int>()
        ages["ada"] = 36
        ages["ada"] = 37
        let hit: Option<Int> = ages[k]
        match hit {
            Some(v) -> {
                return Ok(v)
            }
            None -> {
                return Err(404)
            }
        }
        return Err(9)
    }

    pledge cached(k: String) -> Result<Int, Int> {
        let mut m = Map<String, Int>()
        m["a"] = 1
        let c = Cache { name: "c", entries: m }
        let inner = c.entries
        match inner[k] {
            Some(v) -> {
                return Ok(v)
            }
            None -> {
                return Err(0)
            }
        }
        return Err(9)
    }
}
