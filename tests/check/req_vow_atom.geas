// A requirements atom naming a vow. Only subcontracts and loose pledges have
// a fulfillment state the policy can test.

contract Config {
    vow retries: Int = 3

    pledge ping() -> Result<Unit, PingError>

    requirements {
        fulfill: ping && retries
    }
}
