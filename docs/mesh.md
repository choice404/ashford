# The Ashford Mesh

This is the design for Layer 4, the mesh: many processes, each in whatever
language it is written, wired into one bridge where every node provides the
contracts it implements and consumes the contracts others implement. It is the
layer the whole language was built toward. Layer 1 made a contract the shared
artifact two languages meet at, Layer 2 carried that meeting across a socket, and
this layer makes the meeting symmetric, so a process is a provider and a consumer
at once and a project's languages talk to each other through contracts the way a
single program's modules talk through function calls.

This document is normative. It builds on [docs/network.md](network.md) and
changes none of its wire: the frame header, the value encoding, the message
kinds, and the handshake are exactly Layer 2's, because a mesh is Layer 2's
client and server run by every node at once rather than a new protocol. It adds
one thing to the ABI, a library entry point for serving, and no new status and no
new frame.

## The model

A node is an ordinary `AshRuntime` that does two things at once. It **serves** the
contracts it holds, so a peer can sign and fulfill them, and it **connects** to
the peers whose contracts it wants, so it can sign and fulfill theirs. Serving and
connecting were both already built: connecting is `ash_runtime_connect` from Layer
2, and serving is the accept loop the `ashd` daemon already runs. The mesh makes
serving a call any host can make, so the roles stop being two programs, a daemon
and a client, and become two halves of one node.

The `ashd` daemon does not go away; it becomes the thin case, a node that only
serves and never connects, a `main` over the serve call. Every richer topology is
a host that calls both.

## The serve call

Serving is exposed as a library entry point beside connect:

```c
AshStatus ash_runtime_serve(AshRuntime* rt, const char* addr,
                            const char* token, AshServer** out);
void      ash_server_stop(AshServer* server);
```

`addr` is the `host:port` the node listens on. `token` is the shared secret a peer
must present, NULL to accept peers without one. The call binds the address, starts
the accept loop on a background thread, and returns an `AshServer` handle at once,
so the host keeps running: it can serve, connect out, and do its own work on the
calling thread while the loop runs behind it. `ash_server_stop` shuts the listener
down, drains the connection threads, and returns; a node may run more than one
server, one per address it wants to be reachable at.

Serving exposes the runtime's frozen iname table, the same table `ash_iname_dump`
renders. A node is loaded and bound, then served, and only then connected to its
peers, so the table every peer fetches at handshake is exactly the node's own
local registrations, the surface every later sign against it resolves against.
`ash_runtime_serve` before `ash_runtime_freeze` freezes the runtime itself, so the
offered surface is fixed the moment a node is reachable; and because a node serves
its locals and never re-serves a remote it consumed, serving after a consume edge
has merged is refused with `ASH_ERR_STATE`, which keeps a node serving before it
connects and its offered surface exactly its own.

## What a node serves

A node serves the contracts it registered locally, the modules it loaded and the
pledges it bound, and only those. A contract a node consumed from a peer is a
remote origin in that node's table, and a node does not re-serve its remotes: the
mesh does not relay, proxy, or route a call from one node through a second to a
third. A sign resolves at the node that owns the contract, one hop, always. This
keeps the mesh a set of direct edges with no routing table, no loops, and no
question of which path a call took, and it is why a node's served table is exactly
its local registrations, the same dump `ashd` serves today.

A project wires its mesh by giving each node the addresses of the peers whose
contracts it needs and letting it connect to each. The origin marked table merge
Layer 2 already performs does the rest: a node that connects to three peers holds
one table with three remote origins beside its locals, and a lookup, a sign, and a
fulfill read that one table without knowing or caring which peer, or the node
itself, owns the name.

## Two directions, two edges

A connection is one directional: the connecting side signs and fulfills, the
serving side runs the pledge and answers, and the serving side never initiates a
frame. A mesh does not need that to change, because both nodes serve. When node A
wants to call node B it connects to B's address, and when B wants to call A it
connects to A's address. The two directions are two edges, each a plain Layer 2
connection, each one directional, and together they are a full duplex
conversation built out of parts that were already proven.

A mutual pair brings its edges up after it serves, not before. Each node's
connect wants its peer already serving, and serving fixes the offered surface
behind the freeze, so if the freeze also closed the consume side neither node
could open its edge and the pair would deadlock. It does not: the freeze fixes
what a node offers, and a consume edge extends only the remote surface a node
never re-serves, so a serving node connects out past its own freeze. Each node
loads and binds, serves its own contracts, and then opens the edges to the peers
it consumes, its offered surface fixed at the freeze and its consumed surface
grown by the edges it opens after. A node that only
consumes opens the one edge it needs; a node that only provides, the `ashd` case,
opens none and waits to be connected to.

This is why a client implemented pledge, a callback down one connection, is not
part of the mesh: it is not needed. A node that wants to be called stands up a
serve endpoint and is called on it, which is the same mechanism every other
provider uses, rather than a second, inverted path down a connection that was
built to carry one direction.

## The foreign language provider

Because serving is a library call, the provider side is no longer a fixed C
daemon. Any host that links `libashrt` stands up a node: it initializes a runtime,
loads a module or binds its own pledge implementations, in C, in Python, in
anything that reaches the ABI, freezes, and serves. A peer in another language
connects and fulfills, and the results it reads were computed by the provider's
language, live, in the provider's process. A Python service that binds a Python
function over a pledge and serves it, and a C program that connects and fulfills
it, are two running programs in two languages exchanging results through one
contract. That is the bridge the language is for, and the serve call is the last
piece it needed.

## Discovery

A node finds its peers by address. A project configures each node with the
addresses of the peers it depends on, and the node connects to each at startup, the
same explicit wiring a program uses when it names the libraries it links. The iname
table a peer serves is complete and self describing, so once connected a node knows
every contract that peer offers with no further lookup. A registry that maps a
contract name to the node that serves it, so a node can find a provider it was not
configured with, is a natural next step on top of this and is left out of the first
mesh, named below.

## Authentication

Each serve endpoint carries its own token, presented by every peer that connects to
it, compared in constant time before any table crosses, exactly the Layer 2
handshake. A node that serves several endpoints may guard each with its own token,
and a node that connects to several peers presents each peer's token on that edge.
This is perimeter authentication per edge, the same posture Layer 2 takes and with
the same limits: a deployment that needs transport security or per contract
authorization runs the mesh on a trusted network or behind a terminating proxy, and
those are later work the handshake already has the room for.

## Why it composes

The mesh adds a serve call and nothing else because every hard part was already
built. The wire is Layer 2's, unchanged, so two nodes speak the protocol two Layer
2 programs already spoke. Discovery is the iname table, and a node merges a peer's
table through the same origin marked merge a client already performed, so consuming
from many peers is consuming from many daemons, which Layer 2 already allowed.
Serving is the accept loop `ashd` already ran, lifted from a `main` into the
library so any host can run it. Fulfillment is the future whose wait blocks until
the outcome exists, and a wait that blocks on a peer's answer is the wait Layer 2
already delivered. The mesh is not a new system; it is the symmetry the existing
parts were shaped to allow.

## What v1 leaves out

Named so nothing is discovered missing by surprise. Each has a place to land later
without moving the wire or the serve call.

- **Relay and routing.** A node serves its own contracts and does not forward a
  call to a third node. Every sign is one hop to the owner. A routed mesh, where a
  node reaches a contract through an intermediary, is a later layer.
- **A discovery registry.** Nodes are wired by address. A service that resolves a
  contract name to the node serving it, so a node finds a provider it was not
  configured with, is the natural next step and is not in the first mesh.
- **Callbacks on one connection.** A connection carries one direction; a node that
  wants to be called serves an endpoint rather than being called back down a
  connection built to carry the other way.
- **Load balancing and failover.** A contract lives on the node that serves it;
  nothing replicates it across nodes or retries a call on another.
- **Live reconnect.** A dropped edge is final for the runtime that held it, the
  Layer 2 rule unchanged: a reconnect is a fresh runtime.
- **Transport security and per contract authorization.** The per edge token is a
  perimeter, nothing more.
