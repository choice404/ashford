// A requirements atom naming a pledge inside an anonymous subcontract. The
// parse accepts the name, since anonymity is invisible to the grammar; the
// resolver rejects it, because a subcontract pledge is not a policy atom.

contract Payment {
    subcontract {
        pledge audit(card: Card) -> Result<Bool, AuditError>
    }

    pledge charge(card: Card) -> Result<Unit, PayError>

    requirements {
        fulfill: charge && audit
    }
}
