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
//   Store.query(S, predicate)      -> Result<List<Row>, StoreError>
//   Store.query(S, predicate, asc(column))  -> Result<List<Row>, StoreError>
//   Store.query(S, predicate, desc(column)) -> Result<List<Row>, StoreError>
//   Store.query(S, predicate, desc(column), limit(n)) -> Result<List<Row>, StoreError>
//   Store.count(S, predicate)      -> Result<Int, StoreError>   (rows not materialized)
//   Store.sum(S, column, predicate) -> Result<Int|Float, StoreError> (rows not materialized)
//   Store.insert(S, row)           -> Result<Unit, StoreError>
//   Store.update(S, key, row)      -> Result<Unit, StoreError>
//   Store.delete(S, key)           -> Result<Unit, StoreError>
//
// Store.query is the bound predicate form, and it reads two shapes over one
// schema. The three argument shape is the equality shorthand, a column and a
// value, and answers every row whose column equals the value. The two argument
// shape is a predicate: a comparison, or comparisons joined with && and ||,
// where each comparison tests a column with ==, !=, <, <=, >, or >= against a
// value. && binds tighter than ||, the precedence the grammar already fixes, so
// a && b || c reads the a && b group or c, and parentheses regroup it the usual
// way; the predicate is a free boolean tree of comparisons the compiler
// normalizes before it reaches the store. Every comparison names its column on
// the left as a bare name resolved against the schema at compile time, never a
// value, and query answers every row the whole predicate accepts. A third argument orders those rows: asc(column) sorts
// them by the named column ascending and desc(column) descending. asc and desc
// are reserved suffixes in the third argument position, not values and not
// functions, and the column they wrap is a bare name resolved against the schema
// at compile time the same way a comparison's column is. A fourth argument
// bounds those ordered rows: limit(n) keeps only the leading n, the count an Int
// the runtime reads. limit rides only behind an order, since a bound with no
// order ahead of it would cut an undefined row, so a limit named without one is
// refused at compile time; the count is a value, a literal or a bound parameter
// alike, and a negative count is a runtime refusal, not a compile time one.
// Store.count reads a two argument predicate and answers how many rows pass it
// as an Int, the rows never materialized into the process: the store counts them
// behind the boundary and hands back only the number. count and sum join their
// comparisons with && alone, never ||, so their predicate is one AND-group and a
// || in it is refused at compile time.
// Store.sum names a schema, a bare Int or Float column to total, and that same
// && only predicate, and answers the total of that column over the rows the
// predicate accepts, an Int column giving Result<Int, StoreError> and a Float column
// Result<Float, StoreError>; the rows are never materialized, the store totals
// them behind the boundary, and an empty set sums to the column's own zero.
// Every operation returns a Result so a store failure
// is a value in the surface's own error type. The
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
