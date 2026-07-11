// Two provisional clauses incorporated side by side: every signature has a
// same named clause in the contract, and the contributed names never collide.

provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

internal provisional clause Audited {
    clause audit(event: String) -> Result<Unit, AuditError>
    clause audit_count() -> Int
}

contract Ledger {
    incorporate Loggable
    incorporate Audited

    pledge post(amount: Int) -> Result<Unit, LedgerError>

    clause log(message: String) -> Result<Unit, LogError> {
        // log implementation
    }

    clause audit(event: String) -> Result<Unit, AuditError> {
        // audit implementation
    }

    clause audit_count() -> Int {
        return 0
    }
}
