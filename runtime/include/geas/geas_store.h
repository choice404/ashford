/* geas_store.h: the store vtable of Layer 3, a library first and a schema
 * second. A driver behind this surface opens one connection to one database,
 * runs SQL with positionally bound parameters, and hands result rows back as
 * ordinary GeasValues, and it does all of that without ever naming a contract.
 * docs/database.md pins the model: the backend sits behind open, exec, query,
 * begin, commit, rollback, and close, and the runtime speaks to a database
 * through those and never through a driver's own types. This header is that
 * vtable's C shape, the reference backend is SQLite, and S0 is the driver and
 * its goldens with no contract, no schema block, and no transaction policy
 * wired to it yet.
 *
 * The type map is docs/database.md's, and it is the whole of what a store may
 * hold. Int and UInt cross as a 64 bit integer, a UInt by its bit pattern so
 * the high bit survives a backend that only knows signed; Float is REAL; Bool,
 * Byte, and Char are integers of their declared width; String is raw bytes,
 * bound and read as a blob so a byte is a byte the way the ABI already swears,
 * though the column it lands in is declared TEXT. A non scalar parameter or a
 * non scalar result column is GEAS_ERR_TYPE, because a row is flat and a store
 * is not the place to hide a nested value. A backend that cannot do what it
 * was told, an open that will not open, a statement that will not prepare, a
 * commit a constraint refused, is GEAS_ERR_STORE, the store layer's one new
 * status and deliberately narrow: it is the store failing the runtime, never
 * the contract failing its business. */

#ifndef GEAS_STORE_H
#define GEAS_STORE_H

#include "geas_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rows and their string bytes allocate through this hook, so the driver never
 * names a GeasContract. S0 tests pass a plain arena; S1 wraps geas_bytes on the
 * signing instance, the same seam the wire codec kept over its owner. A NULL
 * return means out of memory and stops the read with GEAS_ERR_OOM. */
typedef struct GeasStoreAlloc {
    uint8_t* (*bytes)(void* ctx, uint64_t n);
    void* ctx;
} GeasStoreAlloc;

/* One connection to one database, opaque to every caller. The reference
 * driver holds a SQLite handle behind it; a foreign caller includes this
 * header and never sees the backend's own types. */
typedef struct GeasStore GeasStore;

/* Opens a connection to dsn, a "file:path" URI or a bare path the backend
 * understands, and returns it through out. A dsn that will not parse or a
 * database that will not open is GEAS_ERR_STORE and out is left NULL. The
 * connection lives until geas_store_close. */
GeasStatus geas_store_open(const char* dsn, GeasStore** out);

/* Closes the connection and frees the handle. NULL is a no op. Any open
 * transaction is the backend's to roll back at close; a caller that wants a
 * commit issues it first. */
void geas_store_close(GeasStore* s);

/* Runs one parameterless statement, the DDL and control path: CREATE TABLE,
 * BEGIN, and their kind. SQL the backend rejects, for a syntax error or a
 * constraint it refused, is GEAS_ERR_STORE. */
GeasStatus geas_store_exec(GeasStore* s, const char* sql);

/* Runs one statement with nparams positional parameters bound in order, never
 * concatenated into the text, so a string holding '; drop table is a string
 * and nothing else. affected, when non NULL, receives the row count the
 * statement changed, the backend's own tally. A parameter count that
 * disagrees with the statement's placeholders, or SQL the backend rejects, is
 * GEAS_ERR_STORE; a non scalar parameter value is GEAS_ERR_TYPE. */
GeasStatus geas_store_exec_params(GeasStore* s, const char* sql,
                                const GeasValue* params, size_t nparams,
                                uint64_t* affected /*nullable*/);

/* Runs a query and materializes its result set. Each result row becomes an
 * GEAS_TY_RECORD whose ncols fields carry the columns in declaration order,
 * each field typed by col_types[i], and every row rides one GEAS_TY_LIST of
 * records placed in *rows_out. col_names, when non NULL, names the columns for
 * a caller that wants them; the positional record repr the ABI pins carries no
 * names of its own, so S0 takes the array as the S1 seam and does not fold it
 * into the value. Every field, its string bytes, and the row backbone are
 * allocated through alloc, so the whole result set is the caller's to own and
 * free the way the alloc's owner frees everything.
 *
 * The parameters bind exactly as geas_store_exec_params binds them. A col_types
 * entry that is not a scalar, or a declared ncols that disagrees with the
 * statement's column count, is GEAS_ERR_TYPE; a backend failure mid query is
 * GEAS_ERR_STORE; an allocation the alloc refuses is GEAS_ERR_OOM. On any error
 * *rows_out is left zeroed and whatever the alloc already handed out stays with
 * the alloc's owner, the one walk reclaim rule the wire codec kept. */
GeasStatus geas_store_query(GeasStore* s, const char* sql,
                          const GeasValue* params, size_t nparams,
                          const uint32_t* col_types,
                          const GeasString* col_names /*nullable*/, size_t ncols,
                          const GeasStoreAlloc* alloc, GeasValue* rows_out);

/* Reconciles a schema with the live database on this connection: an absent
 * table is created to match the schema exactly, one CREATE TABLE from the
 * declared shape with the first column as the primary key, so a fresh database
 * is a working one; a present table is validated column for column, name and
 * declared storage type in declaration order, and a divergence is GEAS_ERR_TYPE.
 * An empty schema or one whose column type is not a scalar is GEAS_ERR_TYPE; a
 * backend failure is GEAS_ERR_STORE. No ALTER is ever issued: reconcile is
 * create or validate, never migrate, which is v1's whole schema defense. */
GeasStatus geas_store_reconcile(GeasStore* s, const GeasSchemaDesc* schema);

/* The transaction primitives. S0 wires their implementations and tests their
 * round trip; S1 and S2 drive them from a transactional subcontract's fate. A
 * begin over an already open transaction, or a commit or rollback with none
 * open, is the backend's to answer, and its refusal is GEAS_ERR_STORE. A commit
 * a constraint blocks is GEAS_ERR_STORE too, the store failing the runtime. */
GeasStatus geas_store_begin(GeasStore* s);
GeasStatus geas_store_commit(GeasStore* s);
GeasStatus geas_store_rollback(GeasStore* s);

#ifdef __cplusplus
}
#endif

#endif /* GEAS_STORE_H */
