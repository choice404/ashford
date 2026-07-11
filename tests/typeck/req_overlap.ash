// The one static rejection of a requirements block: a fulfill line and a
// break line that an assignment can satisfy at the same time.

contract Broken {
    pledge save() -> Unit
    pledge audit() -> Unit

    requirements {
        fulfill: save
        break: !audit
    }
}
