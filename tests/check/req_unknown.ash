// A requirements atom naming nothing in the contract.

contract Shop {
    pledge sell(item: String) -> Result<Unit, ShopError>

    requirements {
        fulfill: sell && shipping
    }
}
