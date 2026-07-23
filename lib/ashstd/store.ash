// ashstd.store: the store surface a store-backed contract reads and writes
// through. Store is not a contract and not a value; it is a namespace the
// compiler knows, so Store.find, Store.query, Store.insert, Store.update, and
// Store.delete name a schema declared in the same contract and lower onto the
// runtime primitives that carry a row across the boundary. A row is the schema's
// record, its fields the columns in declaration order, so a row read out of a
// table is an ordinary value owned by the instance and dead at its break, the
// one ownership rule this project never bends. The first column of a schema is
// its primary key, the key find, update, and delete match against.
//
// The operations and their shapes, the surface docs/database.md pins:
//
//   Store.find(S, key)             -> Result<Option<Row>, StoreError>
//   Store.query(S, column, value)  -> Result<List<Row>, StoreError>
//   Store.insert(S, row)           -> Result<Unit, StoreError>
//   Store.update(S, key, row)      -> Result<Unit, StoreError>
//   Store.delete(S, key)           -> Result<Unit, StoreError>
//
// Store.query is the bound predicate form, and in this release it binds one
// column to one value with equality and answers every row that matches;
// comparison operators and conjunction are later steps. Every operation returns
// a Result so a store failure is a value in the surface's own error type. The
// backend failing the runtime, a connection lost, a disk with no room, a
// constraint the backend refused, rides back as the ASH_ERR_STORE status
// through the wait the host already reads, never folded into a contract's own
// error; the contract's own rules, an unknown id, an overdraft, stay values in
// the contract's error type where the language put them.

// The error a store operation carries in its Result's E slot. The variants name
// the two ways the surface itself can refuse a value before the backend is even
// asked: a row whose bytes will not decode onto the instance, and a shape the
// operation cannot bind. A backend failure is the ASH_ERR_STORE status, not one
// of these, so a host that reads the status and a pledge that reads the value
// never race for the same failure.
StoreError is either StoreDecode or StoreShape
