// ashstd.traits: the shared provisional clauses. A provisional clause is a
// clause template, so each one here is a set of signatures a contract
// incorporates and must implement exactly; nothing in this file has a body.
// Loggable turns a message into the line the implementor would log, a pure
// decoration since a clause has no IO to reach; Comparable is the ordering
// hook, negative, zero, or positive the way every comparator has ever
// spoken; Serializable names the tag a value would serialize under. They
// stay internal code sharing for now, since a provisional clause cannot yet
// require pledges.

provisional clause Loggable {
    clause log_line(msg: String) -> String
}

provisional clause Comparable {
    clause compare(a: Int, b: Int) -> Int
}

provisional clause Serializable {
    clause serialize_tag() -> String
}
