// The same provisional clause incorporated twice by one contract.

provisional clause Loggable {
    clause log(message: String) -> Result<Unit, LogError>
}

contract Service {
    incorporate Loggable
    incorporate Loggable

    clause log(message: String) -> Result<Unit, LogError> {
        // log implementation
    }
}
