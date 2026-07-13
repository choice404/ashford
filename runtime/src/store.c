/* store.c: the SQLite driver behind the AshStore vtable, docs/database.md's
 * reference backend made executable. This is the one translation unit that
 * calls sqlite3_* directly; everything above it speaks open, exec, query,
 * begin, commit, rollback, and close and never touches a driver type. It is
 * self contained: it leans on SQLite and the AshValue layout in ash_abi.h and
 * nothing in runtime.c, so a gate can link it against the sqlite amalgamation
 * and the value helpers alone.
 *
 * The type map is the store's whole vocabulary and it is narrow on purpose. A
 * row is flat, so a parameter or a column that is not a scalar is ASH_ERR_TYPE
 * before a byte moves. Everything a query builds, the fields, the string
 * bytes, and the list backbone the rows ride, is allocated through the caller's
 * AshStoreAlloc, the same discipline the wire decoder kept over its owner: the
 * driver owns no result heap of its own, so there is nothing for it to leak and
 * one break reclaims the lot. A backend that fails at what it was told is
 * ASH_ERR_STORE, the store failing the runtime, never the contract failing its
 * business, which stays a value in the contract's own error type where the
 * language put it. */

#include <ash/ash_store.h>

#include "sqlite3.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct AshStore {
    sqlite3* db;
};

/* ---- the scalar type map ---- */

/* The tags a column or a parameter may carry. A composite type never reaches a
 * flat row, so it is refused here and reported ASH_ERR_TYPE by every caller. */
static int tag_scalar(uint32_t ty) {
    switch (ty) {
    case ASH_TY_INT:
    case ASH_TY_UINT:
    case ASH_TY_FLOAT:
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:
    case ASH_TY_CHAR:
    case ASH_TY_STRING:
        return 1;
    default:
        return 0;
    }
}

/* Binds one value into slot idx, one based, the way SQLite counts. Int and
 * UInt both cross as a 64 bit integer, the UInt reinterpreted so its high bit
 * rides through a backend that only stores signed; reading it back casts the
 * same bits home. Float is REAL. Bool, Byte, and Char are integers of their
 * width. A String binds as a blob, raw bytes with no UTF-8 check, so a byte is
 * a byte even into a column declared TEXT; SQLITE_TRANSIENT tells SQLite to
 * take its own copy, so the caller keeps ownership of the bytes. */
static AshStatus bind_one(sqlite3_stmt* stmt, int idx, const AshValue* v) {
    switch (v->ty) {
    case ASH_TY_INT:
        return sqlite3_bind_int64(stmt, idx, v->as.i) == SQLITE_OK
                   ? ASH_OK : ASH_ERR_STORE;
    case ASH_TY_UINT:
        /* The bit pattern, not the magnitude: a value past INT64_MAX becomes a
         * negative integer in the cell and reads back to the same u64. */
        return sqlite3_bind_int64(stmt, idx, (sqlite3_int64)v->as.u) == SQLITE_OK
                   ? ASH_OK : ASH_ERR_STORE;
    case ASH_TY_FLOAT:
        return sqlite3_bind_double(stmt, idx, v->as.f) == SQLITE_OK
                   ? ASH_OK : ASH_ERR_STORE;
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:
        return sqlite3_bind_int64(stmt, idx, (sqlite3_int64)v->as.b) == SQLITE_OK
                   ? ASH_OK : ASH_ERR_STORE;
    case ASH_TY_CHAR:
        return sqlite3_bind_int64(stmt, idx, (sqlite3_int64)v->as.ch) == SQLITE_OK
                   ? ASH_OK : ASH_ERR_STORE;
    case ASH_TY_STRING: {
        const void* p = v->as.s.len ? (const void*)v->as.s.ptr : (const void*)"";
        int rc = sqlite3_bind_blob(stmt, idx, p, (int)v->as.s.len,
                                   SQLITE_TRANSIENT);
        return rc == SQLITE_OK ? ASH_OK : ASH_ERR_STORE;
    }
    default:
        return ASH_ERR_TYPE;
    }
}

/* Binds the whole parameter frame. A count that disagrees with the statement's
 * placeholders is the caller building the wrong call, ASH_ERR_STORE; a non
 * scalar value is ASH_ERR_TYPE, and the first of either stops the bind. */
static AshStatus bind_params(sqlite3_stmt* stmt, const AshValue* params,
                             size_t nparams) {
    if (sqlite3_bind_parameter_count(stmt) != (int)nparams) return ASH_ERR_STORE;
    for (size_t i = 0; i < nparams; i++) {
        AshStatus st = bind_one(stmt, (int)i + 1, &params[i]);
        if (st != ASH_OK) return st;
    }
    return ASH_OK;
}

/* ---- reading a row ---- */

/* Builds one record from the current result row: ncols fields in declaration
 * order, each typed by col_types[i] and read through the column accessor that
 * matches. A String field copies its bytes through alloc so the record owns
 * them; a scalar field copies by value. The fields array itself is alloc owned
 * too, so the whole record hangs off the caller's owner. A col_types entry
 * that is not a scalar is ASH_ERR_TYPE; an allocation the alloc refuses is
 * ASH_ERR_OOM. */
static AshStatus read_row(sqlite3_stmt* stmt, const uint32_t* col_types,
                          size_t ncols, const AshStoreAlloc* alloc,
                          AshValue* rec_out) {
    AshValue* fields = NULL;
    if (ncols) {
        fields = (AshValue*)alloc->bytes(alloc->ctx, ncols * sizeof(AshValue));
        if (!fields) return ASH_ERR_OOM;
        memset(fields, 0, ncols * sizeof(AshValue));
    }
    for (size_t i = 0; i < ncols; i++) {
        uint32_t ty = col_types[i];
        if (!tag_scalar(ty)) return ASH_ERR_TYPE;
        AshValue* f = &fields[i];
        f->ty = ty;
        f->tag = 0;
        switch (ty) {
        case ASH_TY_INT:
            f->as.i = (int64_t)sqlite3_column_int64(stmt, (int)i);
            break;
        case ASH_TY_UINT:
            f->as.u = (uint64_t)sqlite3_column_int64(stmt, (int)i);
            break;
        case ASH_TY_FLOAT:
            f->as.f = sqlite3_column_double(stmt, (int)i);
            break;
        case ASH_TY_BOOL:
            f->as.b = sqlite3_column_int64(stmt, (int)i) ? 1 : 0;
            break;
        case ASH_TY_BYTE:
            f->as.b = (uint8_t)sqlite3_column_int64(stmt, (int)i);
            break;
        case ASH_TY_CHAR:
            f->as.ch = (uint32_t)sqlite3_column_int64(stmt, (int)i);
            break;
        case ASH_TY_STRING: {
            const void* blob = sqlite3_column_blob(stmt, (int)i);
            int nb = sqlite3_column_bytes(stmt, (int)i);
            uint8_t* dst = NULL;
            if (nb > 0) {
                dst = alloc->bytes(alloc->ctx, (uint64_t)nb);
                if (!dst) return ASH_ERR_OOM;
                memcpy(dst, blob, (size_t)nb);
            }
            f->as.s.ptr = dst;
            f->as.s.len = (uint64_t)(nb > 0 ? nb : 0);
            break;
        }
        default:
            return ASH_ERR_TYPE; /* unreachable past tag_scalar */
        }
    }
    memset(rec_out, 0, sizeof(*rec_out));
    rec_out->ty = ASH_TY_RECORD;
    rec_out->as.list.data = fields;
    rec_out->as.list.len = ncols;
    rec_out->as.list.cap = ncols;
    return ASH_OK;
}

/* ---- connection lifecycle ---- */

AshStatus ash_store_open(const char* dsn, AshStore** out) {
    if (!out) return ASH_ERR_TYPE;
    *out = NULL;
    if (!dsn) return ASH_ERR_STORE;

    AshStore* s = (AshStore*)sqlite3_malloc((int)sizeof(AshStore));
    if (!s) return ASH_ERR_OOM;
    s->db = NULL;

    /* URI parsing lets "file:path" mean what docs/database.md says it does;
     * READWRITE|CREATE opens an absent file so a fresh database is a working
     * one and the walking skeleton needs no setup step. */
    int rc = sqlite3_open_v2(dsn, &s->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_URI,
                             NULL);
    if (rc != SQLITE_OK || !s->db) {
        sqlite3_close(s->db);
        sqlite3_free(s);
        return ASH_ERR_STORE;
    }
    /* A writer that must wait on another connection surfaces as a store error
     * rather than a silent stall past this window, the busy behavior
     * docs/database.md pins. */
    sqlite3_busy_timeout(s->db, 5000);
    *out = s;
    return ASH_OK;
}

void ash_store_close(AshStore* s) {
    if (!s) return;
    sqlite3_close(s->db);
    sqlite3_free(s);
}

/* ---- schema reconcile ---- */

/* The SQLite column type a scalar tag declares in a CREATE TABLE. Every
 * integer width and both signednesses land in INTEGER, Float in REAL, and
 * String in TEXT though the driver binds and reads it as a blob so a byte is a
 * byte the way the ABI swears. A non scalar has no column type, the composite a
 * flat row refuses; the caller reports ASH_ERR_TYPE on the NULL. */
static const char* store_sql_type(uint32_t ty) {
    switch (ty) {
    case ASH_TY_INT:
    case ASH_TY_UINT:
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:
    case ASH_TY_CHAR:
        return "INTEGER";
    case ASH_TY_FLOAT:
        return "REAL";
    case ASH_TY_STRING:
        return "TEXT";
    default:
        return NULL;
    }
}

/* Whether s spells a plain SQL identifier, the ascii letters, digits, and
 * underscore with a non digit lead. A schema and its columns are ashford
 * identifiers the compiler chose, never host input, so this holds by
 * construction; the guard is here so a reconcile that interpolates a name into
 * DDL, which parameters cannot carry, can never smuggle a fragment past it. */
static int store_ident_ok(const char* s) {
    if (!s || !*s) return 0;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        int digit = (c >= '0' && c <= '9');
        if (p == s && !alpha) return 0;
        if (!alpha && !digit) return 0;
    }
    return 1;
}

/* Case insensitive ascii equality, for matching a declared column type against
 * what PRAGMA table_info reports the live table carries. */
static int store_ieq(const char* a, const char* b) {
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

/* Creates the table from a schema, one CREATE TABLE built from the locked
 * shape: every column in declaration order at its mapped storage type, the
 * first column PRIMARY KEY, the convention the store surface keys against. The
 * SQL is assembled into a heap buffer since a schema's width is not bounded at
 * compile time here. */
static AshStatus store_create_table(AshStore* s, const AshSchemaDesc* schema) {
    /* Two passes: size the buffer, then fill it, so no fixed cap can clip a
     * wide schema. */
    size_t need = 64 + strlen(schema->table);
    for (uint32_t i = 0; i < schema->ncols; i++) {
        const char* ct = store_sql_type(schema->cols[i].ty);
        if (!ct) return ASH_ERR_TYPE;
        if (!store_ident_ok(schema->cols[i].name)) return ASH_ERR_STORE;
        need += strlen(schema->cols[i].name) + strlen(ct) + 24;
    }
    char* sql = (char*)sqlite3_malloc((int)need);
    if (!sql) return ASH_ERR_OOM;
    size_t n = 0;
    n += (size_t)snprintf(sql + n, need - n, "CREATE TABLE %s(", schema->table);
    for (uint32_t i = 0; i < schema->ncols; i++) {
        const char* ct = store_sql_type(schema->cols[i].ty);
        n += (size_t)snprintf(sql + n, need - n, "%s%s %s%s",
                              i ? ", " : "", schema->cols[i].name, ct,
                              i == 0 ? " PRIMARY KEY" : "");
    }
    snprintf(sql + n, need - n, ")");
    AshStatus st = ash_store_exec(s, sql);
    sqlite3_free(sql);
    return st;
}

/* Validates a live table against a schema, column for column: the same count,
 * each column's name and declared storage type matching in declaration order.
 * A divergence is ASH_ERR_TYPE, the storage version of the shape hash, the same
 * status a wrong typed value always carried. table_info is read directly here
 * because it is the one introspection SQLite exposes and a PRAGMA takes no
 * bound parameter; the table name was identifier checked before it was
 * interpolated. */
static AshStatus store_validate_table(AshStore* s, const AshSchemaDesc* schema) {
    char pragma[128 + 64];
    snprintf(pragma, sizeof(pragma), "PRAGMA table_info(%s)", schema->table);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(s->db, pragma, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return ASH_ERR_STORE;
    }
    uint32_t i = 0;
    int rc;
    AshStatus st = ASH_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        /* table_info columns: cid, name, type, notnull, dflt_value, pk. */
        const unsigned char* nm = sqlite3_column_text(stmt, 1);
        const unsigned char* tp = sqlite3_column_text(stmt, 2);
        if (i >= schema->ncols) { st = ASH_ERR_TYPE; break; }
        const char* want_ty = store_sql_type(schema->cols[i].ty);
        if (!nm || !tp || !want_ty ||
            strcmp((const char*)nm, schema->cols[i].name) != 0 ||
            !store_ieq((const char*)tp, want_ty)) {
            st = ASH_ERR_TYPE;
            break;
        }
        i++;
    }
    sqlite3_finalize(stmt);
    if (st != ASH_OK) return st;
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) return ASH_ERR_STORE;
    if (i != schema->ncols) return ASH_ERR_TYPE;
    return ASH_OK;
}

AshStatus ash_store_reconcile(AshStore* s, const AshSchemaDesc* schema) {
    if (!s || !s->db || !schema || !schema->table) return ASH_ERR_STORE;
    if (schema->ncols == 0) return ASH_ERR_TYPE;
    if (!store_ident_ok(schema->table)) return ASH_ERR_STORE;
    /* A first probe tells absent from present: table_info over an unknown table
     * yields no rows, so a zero count is the create path and any count is the
     * validate path. */
    char pragma[128 + 64];
    snprintf(pragma, sizeof(pragma), "PRAGMA table_info(%s)", schema->table);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(s->db, pragma, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return ASH_ERR_STORE;
    }
    int present = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    if (present) return store_validate_table(s, schema);
    return store_create_table(s, schema);
}

/* ---- exec ---- */

AshStatus ash_store_exec(AshStore* s, const char* sql) {
    if (!s || !s->db || !sql) return ASH_ERR_STORE;
    char* err = NULL;
    int rc = sqlite3_exec(s->db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK ? ASH_OK : ASH_ERR_STORE;
}

AshStatus ash_store_exec_params(AshStore* s, const char* sql,
                                const AshValue* params, size_t nparams,
                                uint64_t* affected) {
    if (!s || !s->db || !sql) return ASH_ERR_STORE;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        sqlite3_finalize(stmt);
        return ASH_ERR_STORE;
    }
    AshStatus st = bind_params(stmt, params, nparams);
    if (st != ASH_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        /* A statement run through exec_params yields no rows a caller reads;
         * stepping past any it produces keeps the driver honest about the
         * change count without materializing them. */
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return ASH_ERR_STORE;
    if (affected) *affected = (uint64_t)sqlite3_changes64(s->db);
    return ASH_OK;
}

/* ---- query ---- */

AshStatus ash_store_query(AshStore* s, const char* sql,
                          const AshValue* params, size_t nparams,
                          const uint32_t* col_types,
                          const AshString* col_names, size_t ncols,
                          const AshStoreAlloc* alloc, AshValue* rows_out) {
    /* Names ride the API for S1, which maps a schema's columns; the positional
     * record repr the ABI pins carries no names of its own, so S0 accepts the
     * array and does not fold it into the value. */
    (void)col_names;

    if (!rows_out) return ASH_ERR_TYPE;
    memset(rows_out, 0, sizeof(*rows_out));
    if (!s || !s->db || !sql || !alloc || !alloc->bytes) return ASH_ERR_STORE;
    if (ncols && !col_types) return ASH_ERR_TYPE;
    for (size_t i = 0; i < ncols; i++) {
        if (!tag_scalar(col_types[i])) return ASH_ERR_TYPE;
    }

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        sqlite3_finalize(stmt);
        return ASH_ERR_STORE;
    }
    if ((size_t)sqlite3_column_count(stmt) != ncols) {
        sqlite3_finalize(stmt);
        return ASH_ERR_TYPE;
    }
    AshStatus st = bind_params(stmt, params, nparams);
    if (st != ASH_OK) {
        sqlite3_finalize(stmt);
        return st;
    }

    /* Two passes over one statement. The first counts the rows so the backbone
     * is sized once from the alloc, no driver side growth and no realloc to
     * leak; the reset keeps the bindings, and the second pass reads each row
     * into its slot. The query is ordered by the caller when a stable order
     * matters, so two walks of one unchanged result agree. */
    uint64_t n = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) n++;
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return ASH_ERR_STORE;
    }
    if (sqlite3_reset(stmt) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return ASH_ERR_STORE;
    }

    AshValue* backbone = NULL;
    if (n) {
        backbone = (AshValue*)alloc->bytes(alloc->ctx, n * sizeof(AshValue));
        if (!backbone) {
            sqlite3_finalize(stmt);
            return ASH_ERR_OOM;
        }
        memset(backbone, 0, (size_t)n * sizeof(AshValue));
    }
    uint64_t j = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && j < n) {
        st = read_row(stmt, col_types, ncols, alloc, &backbone[j]);
        if (st != ASH_OK) {
            sqlite3_finalize(stmt);
            return st;
        }
        j++;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return ASH_ERR_STORE;
    }
    sqlite3_finalize(stmt);

    rows_out->ty = ASH_TY_LIST;
    rows_out->as.list.data = backbone;
    rows_out->as.list.len = n;
    rows_out->as.list.cap = n;
    rows_out->as.list.elem_ty = ASH_TY_RECORD;
    return ASH_OK;
}

/* ---- transactions ---- */

/* The primitives are plain statements today; S1 opens one lazily on the first
 * fulfillment of a transactional subcontract and lets the subcontract's fate
 * pick commit or rollback. A backend that refuses any of them, a commit a
 * constraint blocks, a begin over an open transaction, is ASH_ERR_STORE. */
AshStatus ash_store_begin(AshStore* s) {
    return ash_store_exec(s, "BEGIN");
}

AshStatus ash_store_commit(AshStore* s) {
    return ash_store_exec(s, "COMMIT");
}

AshStatus ash_store_rollback(AshStore* s) {
    return ash_store_exec(s, "ROLLBACK");
}
