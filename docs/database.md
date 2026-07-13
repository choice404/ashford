# The Ashford Store Runtime

This is the design for Layer 3, the store runtime: contracts backed by a database, discovered and signed the way every contract is, and fulfilled against durable storage. A contract declares the shape it needs as a schema, signs against a live database, and its pledges read and write rows through the same five host calls that drove a local contract and a remote one. The database sits behind the contract, not in front of the host. A pledge that reads a row is fulfilled with the same code as a pledge that computes one, and a group of writes that must all land or all vanish is a subcontract, the unit the language already had for all-or-nothing. That is the product claim of this layer, and the argument for it is at the end.

This document is normative. It builds on [docs/abi.md](abi.md) and [docs/grammar.md](grammar.md). Unlike the network layer, which added no syntax, the store layer adds two pieces of surface: a `schema` block inside a contract and a `transactional` modifier on a subcontract. Everything else is a runtime and standard library concern. It appends one status code, `ASH_ERR_STORE`, and changes nothing else in the ABI.

## The model

A store backed contract holds a connection to a database for the life of its signature. The connection opens at `sign`, the schema is reconciled with the live database in the same call, and the connection closes at `break`. Between those two moments the contract's pledges operate on rows, and every row that crosses into a pledge is an ordinary `AshValue` owned by the instance, dead at its break, the one ownership rule this project has never bent.

The backend sits behind a small vtable, `AshStore`, so the runtime speaks to a database through open, exec, query, begin, commit, rollback, and close, and never through a driver's own types. The reference backend is SQLite, linked into the runtime from the amalgamation as a single translation unit, no server and no external dependency, which is what lets the store tests be hermetic and byte deterministic on every build. Postgres is designed for behind the same vtable and lands later; nothing in the frame or the language surface knows which backend answered.

The database a contract binds is named by a vow:

```text
vow dsn: String = "file:payments.db"
```

`dsn` is the data source name the backend understands, a file path or URI for SQLite. It is a vow like any other: immutable from the sign, overridable at the sign, and locked once the contract activates. A contract with a schema but no `dsn` vow fails to sign with `ASH_ERR_UNBOUND`, the same rule an unsupplied vow always hit.

## The schema

A schema is the table shape a contract vows to operate against. It is declared inside the contract beside the vows:

```text
contract PaymentService {
    vow dsn: String = "file:payments.db"

    schema Accounts {
        id: Int
        balance: Float
        owner: String
    }

    subcontract Transfer transactional {
        pledge debit(id: Int, amount: Float) -> Result<Unit, PaymentError>
        pledge credit(id: Int, amount: Float) -> Result<Unit, PaymentError>
    }
}
```

A `schema` names a table and its columns, each column an Ashford type. The type checker accepts only the column types a store can hold: `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, and `String`. A composite column type is a type error, because a row is flat and a store is not the place to hide a nested value; a contract that wants structure composes it in the pledge from several columns or several schemas. A contract may declare more than one schema, one per table it touches.

The schema is vow shaped: immutable, locked at sign, and load bearing at sign. When the contract signs, the runtime reconciles the declared schema with the live database on the bound connection. An absent table is created to match the schema exactly, one `CREATE TABLE` from the locked shape, so a fresh database is a working one and the walking skeleton needs no setup step. A present table is validated column for column against the schema, name and type, and a table whose shape disagrees fails the sign with `ASH_ERR_TYPE`, the same status a wrong typed value always carried. This is the schema's whole defense, and it is the storage version of the shape hash the iname table already checks: the shape you signed against is the shape you operate against, or the sign never lands. Altering an existing table to match a changed schema is a migration, and migrations are out of v1, listed below.

## The store surface

A pledge body reads and writes through the standard library store module, `lib/ashstd/store`. The module is ordinary Ashford over a handful of runtime primitives, and its operations name a schema declared in the same contract:

```text
pledge debit(id: Int, amount: Float) -> Result<Unit, PaymentError> {
    let acct = Store.find(Accounts, id)?
    match acct {
        None => Err(PaymentError.NoSuchAccount(id)),
        Some(row) => {
            if row.balance < amount {
                Err(PaymentError.Insufficient(id))
            } else {
                Store.update(Accounts, id, Accounts { balance: row.balance - amount })?
                Ok(Unit)
            }
        }
    }
}
```

The surface is small and every operation returns a `Result` so a store failure is a value, never a throw:

| operation | shape | returns |
|---|---|---|
| `Store.find(S, key)` | look up one row by primary key | `Result<Option<Row>, StoreError>` |
| `Store.query(S, where)` | rows matching a bound predicate | `Result<List<Row>, StoreError>` |
| `Store.insert(S, row)` | add one row | `Result<Unit, StoreError>` |
| `Store.update(S, key, row)` | replace a row's columns by key | `Result<Unit, StoreError>` |
| `Store.delete(S, key)` | remove a row by key | `Result<Unit, StoreError>` |

A row is a `Record` whose fields are the schema's columns in declaration order, so `find` returns `Some(row)` or `None`, `query` returns a list, and the write forms return `Unit`. The connection every operation uses is the current instance's, resolved from the pledge's own context, so a store operation never names a connection and never reaches another instance's. Every value bound into an operation crosses as a parameter, positionally bound in the prepared statement, never concatenated into text, so a string column holding `'; drop table` is a string and nothing else. Rows read out are decoded onto the instance the way every result is, so a row lives on the instance heap and dies at the instance's break, one lifetime rule from the language surface down to the database driver.

## Transactions

A `transactional` subcontract runs its pledges as one all-or-nothing episode. The requirements block already writes fulfillment as boolean logic over subcontracts, so a subcontract was already the language's unit of grouped success; the modifier adds durability to the grouping. The transaction opens lazily on the first fulfillment of a pledge in the subcontract, every write inside it is buffered by the backend, and the outcome is decided by the subcontract's fate:

- **Commit** when the subcontract completes, the moment its last pledge latches `Ok`. The buffered writes become durable in one commit, and the subcontract is Fulfilled.
- **Rollback** the instant any pledge in the subcontract returns `Err`. Every buffered write vanishes, the subcontract is Broken, and the contract state follows its policy.
- **Rollback** on `break` before the commit. A contract torn down mid transaction leaves the database exactly as it found it.

Because a transaction is one episode and not a latch, a pledge in a transactional subcontract is fulfilled at most once. A second call to a pledge whose transaction has already committed or rolled back is `ASH_ERR_STATE`, delivered through the wait like every fulfillment error. This is a deliberate departure from the loose pledge latch, where a later call runs and returns but never moves the state; a committed transaction is closed, there is nothing left to join, and pretending otherwise would be the dishonest kind of convenient.

Loose pledges, the ones outside any transactional subcontract, run in autocommit: each store operation is its own implicit transaction, the backend's default, so a plain read or a plain write needs no ceremony and gets none. Only the `transactional` modifier groups, and it groups exactly the pledges under it.

## The lifecycle over storage

The five states are unchanged; storage gives three of them a durable meaning.

```text
Unsigned -> Signed -> Fulfilled
                   -> PartiallyFulfilled
                   -> Broken
```

`sign` opens the connection to the bound `dsn`, reconciles every declared schema with the live database, and locks the vows. A connection that will not open, a `dsn` that will not parse, or a schema that will not reconcile fails the sign, the first two with `ASH_ERR_STORE` and the last with `ASH_ERR_TYPE`, and nothing is left half open. Fulfilling a pledge runs it against the connection, inside a transaction when the pledge belongs to a transactional subcontract and in autocommit otherwise. `break` rolls back any open transaction, closes the connection, and reclaims the instance's heap, in that order, so no uncommitted write survives a break and no connection outlives its instance. A partially fulfilled contract reports exactly which pledges landed and which broke, and a transactional subcontract in that report is all committed or all rolled back, never half.

## Failure

Storage adds one status, the store layer's single genuinely new failure, numbered after the network's:

| status | meaning |
|---|---|
| `ASH_ERR_STORE` | the backend could not complete a store operation |

`ASH_ERR_STORE` is the backend failing to do what it was told: a connection lost mid fulfillment, a disk with no room, a commit a constraint refused, a `dsn` that would not open. It rides back through the future the host already waits on, the same channel every fulfillment error uses, so a host's store error handling has one place to live. It is deliberately narrow. A contract's own rules, an overdrawn account, an unknown id, a duplicate the contract means to reject, are the contract's errors, returned as values in the contract's own error type, `PaymentError` and its variants, not folded into `ASH_ERR_STORE`. The status is for the store failing the runtime, not for the contract failing its business. A `CHECK` constraint the schema did not ask for is the backend's refusal and is `ASH_ERR_STORE`; a balance the pledge checks and rejects is `Err(PaymentError.Insufficient)` and is the contract working.

## Concurrency

One connection lives on one instance, opened at sign, and the instance's own lock already serializes every fulfillment against it, so a connection is never touched by two threads at once and the runtime adds no new lock for the store. Two instances against one database are two connections, and the backend arbitrates between them: SQLite's file locking, or its write ahead log when the `dsn` asks for it, decides who writes when, and a writer that must wait surfaces as `ASH_ERR_STORE` rather than a silent stall past the backend's busy timeout. Isolation between connections is the backend's to define and this layer does not weaken or strengthen it; a contract that needs a stronger isolation than the default asks the backend for it in the `dsn`. The worker pool runs store pledges the way it runs any pledge, and because the connection sits under the instance lock the pool needs no store awareness.

## Why host code does not change

The claim is the same claim the last two layers made, now against a database: a host drives a store backed contract with the five calls it always used, and the storage is invisible to it. It holds because every seam was already cut. Discovery is a table of mangled names, and a store backed contract registers in it like any other, its schema part of the shape hash so a host that signs against yesterday's shape fails the sign cleanly. Signing was already by name with vow overrides, and the `dsn` is a vow, so pointing a contract at a different database is a sign time override and nothing more. Fulfillment was already a future whose wait blocks until the outcome exists, and a wait that blocks on a disk write satisfies the same contract as a wait that blocked on a pool worker or a network frame. Values were already copied at every boundary and owned by an instance, so a row read from a table behaves exactly like a value computed in memory. Subcontracts were already the unit of grouped, all-or-nothing success, so a transaction needed a modifier and not a new idea. And every failure was already a status through the wait, so the one new fact a database adds, that the store itself can fail, arrives as one new status through the channel the host was already reading. The host does not change because the language spent three layers refusing the shortcuts that would have made this paragraph false.

## What v1 leaves out

Named so nothing is discovered missing by surprise. Each has a place to land later without moving the schema surface or the status numbers.

- **NoSQL and key value stores.** v1 is relational: a schema is a table and a row is flat. A document or key value backend behind the same `AshStore` vtable, with its own schema shape, is a later layer.
- **Postgres and other servers.** The vtable is built for a second backend and SQLite is the only one wired in v1. A server backend adds a driver, not a language change.
- **Migrations.** A schema reconciles with a live table by create or validate; altering an existing table to a changed schema is a migration, and v1 fails the sign on divergence rather than pretend to migrate.
- **A query language surface.** `Store.query` takes a bound predicate, not arbitrary SQL, and joins across schemas are composed in the pledge from several reads. Raw SQL in the language is not a v1 feature.
- **Cross instance and distributed transactions.** A transaction lives on one instance's connection. Two instances do not share one transaction, and nothing coordinates a commit across two databases.
- **Connection pooling.** One instance holds one connection for its life. A shared pool under many short lived instances is future work and a runtime concern only, invisible to the surface.
- **A prepared statement cache.** Statements are prepared per operation in v1; caching them per connection is a backend optimization with no surface effect.
