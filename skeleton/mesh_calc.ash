// The third mesh contract, named apart from Greeter and PaymentService so a node
// that connects to both peers merges three origins with no name collision. Its
// double pledge is a plain body with no vow and no store, so a node serves it with
// a bare load and freeze and a peer signs and fulfills it with nothing bound. The
// doubling is the distinctive answer the stress gate checks: a result that is
// twice the argument proves the sign routed to this contract's owner, since a
// misrouted sign would land a String or a Bool from another node, not an Int.

contract Calculator {
    // The instant pledge the fulfillment storm drives: twice its argument, one
    // frame across the wire, computed on the owner and read back by the peer.
    pledge double(n: Int) -> Result<Int, Int> {
        return Ok(n * 2)
    }

    // Abstract, so the serving node binds a body. The kill gate binds one that
    // dawdles, which keeps a fulfillment of slow outstanding when its provider is
    // dropped and so holds an in flight wait across the kill; the storm never
    // touches it, but a sign resolves every pledge, so the owner binds it before
    // it serves.
    pledge slow(n: Int) -> Result<Int, Int>
}
