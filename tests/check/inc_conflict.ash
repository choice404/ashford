// Two incorporated provisional clauses contributing the same clause name,
// rejected at the second incorporate site.

provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

provisional clause Traceable {
    clause log(message: String) -> Result<Unit, LogError>
}

contract Service {
    incorporate Loggable
    incorporate Traceable

    clause log(message: String) -> Result<Unit, LogError> {
        // shared implementation
    }
}
