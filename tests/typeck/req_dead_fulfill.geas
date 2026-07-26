// A fulfill line no assignment satisfies is dead policy.

contract Broken {
    pledge save() -> Unit

    requirements {
        fulfill: save && !save
        break: !save
    }
}
