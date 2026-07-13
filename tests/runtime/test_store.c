/* test_store.c: the store driver gate. No contract anywhere; the driver is a
 * library and this test holds it to docs/database.md the way test_wire holds
 * the codec to the network doc. It runs the SQLite backend against a real file
 * database, mkstemp'd under target and unlinked at the end, faithful to the
 * "file:" dsn rather than :memory:, and it pins three claims. Golden: a fixed
 * sequence of a CREATE, three parameterized inserts, and a full table query
 * dumps its rows byte for byte to the checked in golden in tests/store.
 * Round trip: every scalar type writes, reads back, and compares equal through
 * ash_value_eq, the UInt high bit and a String with an embedded NUL included so
 * the bit pattern and the bytes-are-bytes promise are both proven. Refusal: a
 * dsn that will not open, broken SQL, a wrong parameter count, and a non scalar
 * parameter each land a status, never a crash, and the arena reclaims in one
 * free so LSan watches the driver to zero leaks.
 *
 * Regenerating the golden after a deliberate format change:
 *
 *   ./target/ashc-out/test_store --emit tests/store
 *
 * and review the byte diff like any other golden. */

#include <ash/ash.h>
#include <ash/ash_store.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "[test_store] FAIL: %s (%s:%d)\n", what,       \
                    __FILE__, __LINE__);                                   \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/* ---- the bump arena behind the AshStoreAlloc ---- */

/* One block, handed out with 16 byte alignment so an AshValue array behind it
 * is aligned, and freed whole at the end. Every row, string, and backbone the
 * driver builds lands here, so the driver owns no result heap and one free
 * reclaims the lot, the S0 stand in for ash_bytes on an instance. */
typedef struct Arena {
    uint8_t* base;
    size_t   cap;
    size_t   used;
} Arena;

static uint8_t* arena_bytes(void* ctx, uint64_t n) {
    Arena* a = (Arena*)ctx;
    size_t off = (a->used + 15u) & ~(size_t)15u;
    if (n > a->cap || off > a->cap - (size_t)n) return NULL;
    a->used = off + (size_t)n;
    return a->base + off;
}

/* ---- scalar value shorthand ---- */

static AshValue zeroed(uint32_t ty) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ty;
    return v;
}

static AshValue int_val(int64_t i) {
    AshValue v = zeroed(ASH_TY_INT);
    v.as.i = i;
    return v;
}

static AshValue float_val(double f) {
    AshValue v = zeroed(ASH_TY_FLOAT);
    v.as.f = f;
    return v;
}

/* A borrowed string value; the driver copies the bytes through TRANSIENT so a
 * stack pointer is safe to bind. */
static AshValue str_bytes(const uint8_t* p, uint64_t n) {
    AshValue v = zeroed(ASH_TY_STRING);
    v.as.s.ptr = (uint8_t*)p;
    v.as.s.len = n;
    return v;
}

static AshValue str_cstr(const char* s) {
    return str_bytes((const uint8_t*)s, (uint64_t)strlen(s));
}

/* ---- a byte stable dump of a rows list ---- */

/* The golden is text so a diff reads, and byte stable so a diff is a change.
 * Floats dump as the raw 16 hex digits of their bits, not a formatted decimal,
 * so no locale or precision drift can move the bytes; strings dump as the hex
 * of their bytes, so an embedded NUL or a non UTF-8 byte shows and nothing is
 * lost. col_names label each field, the one place the query's name array
 * surfaces, since the record value itself is positional. */
typedef struct Buf {
    char*  b;
    size_t cap;
    size_t n;
} Buf;

static void buf_putc(Buf* w, char c) {
    if (w->n + 1 <= w->cap) w->b[w->n] = c;
    w->n++;
}

static void buf_puts(Buf* w, const char* s) {
    for (; *s; s++) buf_putc(w, *s);
}

static void buf_fmt(Buf* w, const char* fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buf_puts(w, tmp);
}

static void dump_field(Buf* w, const AshValue* f) {
    switch (f->ty) {
    case ASH_TY_INT:
        buf_fmt(w, "i64:%lld", (long long)f->as.i);
        break;
    case ASH_TY_UINT:
        buf_fmt(w, "u64:%llu", (unsigned long long)f->as.u);
        break;
    case ASH_TY_FLOAT: {
        uint64_t bits;
        memcpy(&bits, &f->as.f, 8);
        buf_fmt(w, "f64:%016llx", (unsigned long long)bits);
        break;
    }
    case ASH_TY_BOOL:
        buf_fmt(w, "bool:%u", (unsigned)f->as.b);
        break;
    case ASH_TY_BYTE:
        buf_fmt(w, "byte:%u", (unsigned)f->as.b);
        break;
    case ASH_TY_CHAR:
        buf_fmt(w, "char:%u", (unsigned)f->as.ch);
        break;
    case ASH_TY_STRING:
        buf_puts(w, "str:");
        for (uint64_t i = 0; i < f->as.s.len; i++)
            buf_fmt(w, "%02x", (unsigned)f->as.s.ptr[i]);
        break;
    default:
        buf_puts(w, "?");
        break;
    }
}

static void dump_rows(Buf* w, const AshValue* rows, const AshString* names,
                      size_t ncols) {
    uint64_t nrows = rows->as.list.len;
    const AshValue* rec = (const AshValue*)rows->as.list.data;
    buf_fmt(w, "rows %llu\n", (unsigned long long)nrows);
    for (uint64_t r = 0; r < nrows; r++) {
        const AshValue* fields = (const AshValue*)rec[r].as.list.data;
        buf_fmt(w, "row %llu\n", (unsigned long long)r);
        for (size_t c = 0; c < ncols; c++) {
            buf_puts(w, "  ");
            if (names) {
                for (uint64_t k = 0; k < names[c].len; k++)
                    buf_putc(w, (char)names[c].ptr[k]);
            }
            buf_putc(w, '=');
            dump_field(w, &fields[c]);
            buf_putc(w, '\n');
        }
    }
}

/* ---- file helpers ---- */

static uint8_t* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t* buf = (uint8_t*)malloc(n ? (size_t)n : 1);
    if (buf && n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

static int write_file(const char* path, const uint8_t* buf, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t wrote = len ? fwrite(buf, 1, len, f) : 0;
    fclose(f);
    return wrote == len;
}

/* ---- the fixture: the golden table ---- */

/* "世界" as raw UTF-8 bytes, an owner value that is multibyte on purpose so the
 * TEXT column carries bytes and not a codepoint. */
static const char k_owner_wide[] = "\xE4\xB8\x96\xE7\x95\x8C";

static const uint32_t k_col_types[3] = { ASH_TY_INT, ASH_TY_FLOAT,
                                         ASH_TY_STRING };

static AshString col_name(const char* s) {
    AshString a;
    a.ptr = (uint8_t*)s;
    a.len = (uint64_t)strlen(s);
    return a;
}

/* Creates the table, inserts three rows through bound parameters, queries the
 * whole table in a fixed order, and dumps the rows into w. Both the verify and
 * the emit path run this, so the golden and the check see one code path. */
static int build_fixture(AshStore* s, Arena* arena, Buf* w) {
    if (ash_store_exec(s, "CREATE TABLE t(id INTEGER, amt REAL, owner TEXT)") !=
        ASH_OK) {
        fprintf(stderr, "[test_store] fixture: create failed\n");
        return 1;
    }

    struct {
        int64_t     id;
        double      amt;
        const char* owner;
    } inserts[3] = {
        { 1, 2.5, "alpha" },
        { 2, -3.5, "bob" },
        { 3, 0.0, k_owner_wide },
    };
    for (int i = 0; i < 3; i++) {
        AshValue p[3];
        p[0] = int_val(inserts[i].id);
        p[1] = float_val(inserts[i].amt);
        p[2] = str_cstr(inserts[i].owner);
        uint64_t affected = 0;
        if (ash_store_exec_params(
                s, "INSERT INTO t(id, amt, owner) VALUES(?, ?, ?)", p, 3,
                &affected) != ASH_OK ||
            affected != 1) {
            fprintf(stderr, "[test_store] fixture: insert %d failed\n", i);
            return 1;
        }
    }

    AshStoreAlloc alloc = { arena_bytes, arena };
    AshString names[3] = { col_name("id"), col_name("amt"), col_name("owner") };
    AshValue rows;
    if (ash_store_query(s, "SELECT id, amt, owner FROM t ORDER BY id", NULL, 0,
                        k_col_types, names, 3, &alloc, &rows) != ASH_OK) {
        fprintf(stderr, "[test_store] fixture: query failed\n");
        return 1;
    }
    dump_rows(w, &rows, names, 3);
    return 0;
}

/* ---- the scalar round trip ---- */

/* Writes one value into a single untyped column, reads it back, and asserts
 * ash_value_eq. The column is declared with no type so it holds each value's
 * own storage class exactly, the cleanest way to prove the bind and the read
 * are inverses for every scalar the store maps. */
static void round_trip(AshStore* s, Arena* arena, uint32_t col_ty,
                       const AshValue* v, const char* what) {
    if (ash_store_exec(s, "DELETE FROM rt") != ASH_OK) {
        CHECK(0, "round trip: clear failed");
        return;
    }
    if (ash_store_exec_params(s, "INSERT INTO rt(v) VALUES(?)", v, 1, NULL) !=
        ASH_OK) {
        CHECK(0, what);
        return;
    }
    AshStoreAlloc alloc = { arena_bytes, arena };
    AshValue rows;
    if (ash_store_query(s, "SELECT v FROM rt", NULL, 0, &col_ty, NULL, 1,
                        &alloc, &rows) != ASH_OK) {
        CHECK(0, what);
        return;
    }
    CHECK(rows.as.list.len == 1, what);
    if (rows.as.list.len != 1) return;
    const AshValue* rec = (const AshValue*)rows.as.list.data;
    const AshValue* field = (const AshValue*)rec[0].as.list.data;
    CHECK(ash_value_eq(v, &field[0]), what);
}

static void test_round_trips(AshStore* s, Arena* arena) {
    if (ash_store_exec(s, "CREATE TABLE rt(v)") != ASH_OK) {
        CHECK(0, "round trip: create rt failed");
        return;
    }

    AshValue vi = int_val(-1234567890123LL);
    round_trip(s, arena, ASH_TY_INT, &vi, "Int round trips");

    AshValue vu = zeroed(ASH_TY_UINT);
    vu.as.u = 0xDEADBEEFCAFEBABEULL; /* the high bit is set on purpose */
    round_trip(s, arena, ASH_TY_UINT, &vu, "UInt round trips its bit pattern");

    AshValue vf = float_val(-2.5);
    round_trip(s, arena, ASH_TY_FLOAT, &vf, "Float round trips");

    AshValue vb = zeroed(ASH_TY_BOOL);
    vb.as.b = 1;
    round_trip(s, arena, ASH_TY_BOOL, &vb, "Bool round trips");

    AshValue vy = zeroed(ASH_TY_BYTE);
    vy.as.b = 0xA5;
    round_trip(s, arena, ASH_TY_BYTE, &vy, "Byte round trips");

    AshValue vc = zeroed(ASH_TY_CHAR);
    vc.as.ch = 0x1F600; /* a scalar value past the BMP */
    round_trip(s, arena, ASH_TY_CHAR, &vc, "Char round trips");

    /* Raw bytes with an embedded NUL and a high byte: no UTF-8 check, nothing
     * truncated at the NUL, the bytes-are-bytes promise made concrete. */
    const uint8_t raw[4] = { 'a', 0x00, 'b', 0xFF };
    AshValue vs = str_bytes(raw, 4);
    round_trip(s, arena, ASH_TY_STRING, &vs, "String round trips raw bytes");
}

/* ---- transactions ---- */

/* A commit makes a buffered write visible; a rollback makes it vanish. The
 * count query needs no columns beyond the one it selects, so it rides the same
 * query path the fixture does. */
static uint64_t count_tx(AshStore* s, Arena* arena) {
    AshStoreAlloc alloc = { arena_bytes, arena };
    uint32_t ty = ASH_TY_INT;
    AshValue rows;
    if (ash_store_query(s, "SELECT id FROM tx ORDER BY id", NULL, 0, &ty, NULL,
                        1, &alloc, &rows) != ASH_OK)
        return (uint64_t)-1;
    return rows.as.list.len;
}

static void test_transactions(AshStore* s, Arena* arena) {
    if (ash_store_exec(s, "CREATE TABLE tx(id INTEGER)") != ASH_OK) {
        CHECK(0, "tx: create failed");
        return;
    }
    AshValue one = int_val(1);
    AshValue two = int_val(2);

    CHECK(ash_store_begin(s) == ASH_OK, "begin opens a transaction");
    CHECK(ash_store_exec_params(s, "INSERT INTO tx(id) VALUES(?)", &one, 1,
                                NULL) == ASH_OK,
          "insert inside the transaction");
    CHECK(ash_store_commit(s) == ASH_OK, "commit lands the write");
    CHECK(count_tx(s, arena) == 1, "a committed write is visible");

    CHECK(ash_store_begin(s) == ASH_OK, "begin a second transaction");
    CHECK(ash_store_exec_params(s, "INSERT INTO tx(id) VALUES(?)", &two, 1,
                                NULL) == ASH_OK,
          "insert to be rolled back");
    CHECK(ash_store_rollback(s) == ASH_OK, "rollback discards the write");
    CHECK(count_tx(s, arena) == 1, "a rolled back write vanished");
}

/* ---- the negative corpus ---- */

static void test_negative(AshStore* s, Arena* arena) {
    /* A dsn under a directory that does not exist cannot be created, so the
     * open fails cleanly rather than half opening. */
    AshStore* bad = (AshStore*)0x1;
    CHECK(ash_store_open("file:/nonexistent-ashford-dir-9f3c/db.sqlite",
                         &bad) == ASH_ERR_STORE,
          "a dsn that will not open is ASH_ERR_STORE");
    CHECK(bad == NULL, "a failed open leaves the handle NULL");

    CHECK(ash_store_exec(s, "THIS IS NOT SQL") == ASH_ERR_STORE,
          "broken SQL is ASH_ERR_STORE");

    /* One placeholder, two parameters offered: the counts disagree. */
    AshValue two_params[2] = { int_val(1), int_val(2) };
    CHECK(ash_store_exec_params(s, "INSERT INTO tx(id) VALUES(?)", two_params,
                                2, NULL) == ASH_ERR_STORE,
          "a wrong parameter count is refused");

    /* A composite parameter has no place in a flat row. */
    AshStoreAlloc alloc = { arena_bytes, arena };
    AshValue list_param;
    memset(&list_param, 0, sizeof(list_param));
    list_param.ty = ASH_TY_LIST;
    list_param.as.list.elem_ty = ASH_TY_INT;
    CHECK(ash_store_exec_params(s, "INSERT INTO tx(id) VALUES(?)", &list_param,
                                1, NULL) == ASH_ERR_TYPE,
          "a non scalar parameter is ASH_ERR_TYPE");

    /* A non scalar result column is refused before a row is read. */
    uint32_t bad_col = ASH_TY_LIST;
    AshValue rows;
    CHECK(ash_store_query(s, "SELECT id FROM tx", NULL, 0, &bad_col, NULL, 1,
                          &alloc, &rows) == ASH_ERR_TYPE,
          "a non scalar result column is ASH_ERR_TYPE");
}

/* ---- main ---- */

int main(int argc, char** argv) {
    const char* dir = "tests/store";
    int emit = 0;
    if (argc >= 2 && strcmp(argv[1], "--emit") == 0) {
        emit = 1;
        if (argc >= 3) dir = argv[2];
    } else if (argc >= 2) {
        dir = argv[1];
    }

    /* A real file under target, faithful to the "file:" dsn, mkstemp'd so two
     * runs never collide and unlinked at the end. */
    char db_path[] = "target/ashstore_XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) {
        fprintf(stderr, "[test_store] mkstemp under target failed\n");
        return 1;
    }
    close(fd); /* SQLite opens the path itself; the empty file is a valid db */

    char dsn[64];
    snprintf(dsn, sizeof(dsn), "file:%s", db_path);

    Arena arena;
    arena.cap = 1u << 20;
    arena.used = 0;
    arena.base = (uint8_t*)malloc(arena.cap);
    if (!arena.base) {
        unlink(db_path);
        fprintf(stderr, "[test_store] arena alloc failed\n");
        return 1;
    }

    AshStore* s = NULL;
    int rc = 0;
    if (ash_store_open(dsn, &s) != ASH_OK || !s) {
        CHECK(0, "open the store");
        free(arena.base);
        unlink(db_path);
        return 1;
    }

    Buf w;
    w.cap = 4096;
    w.b = (char*)malloc(w.cap);
    w.n = 0;

    if (emit) {
        rc = build_fixture(s, &arena, &w);
        if (!rc && w.n > w.cap) {
            fprintf(stderr, "[test_store] emit: dump overran the buffer\n");
            rc = 1;
        }
        if (!rc) {
            char path[512];
            snprintf(path, sizeof(path), "%s/select_all.txt", dir);
            if (!write_file(path, (const uint8_t*)w.b, w.n)) {
                fprintf(stderr, "[test_store] emit: writing %s failed\n", path);
                rc = 1;
            } else {
                fprintf(stderr, "[test_store] wrote %s (%zu bytes)\n", path,
                        w.n);
            }
        }
    } else {
        int fx = build_fixture(s, &arena, &w);
        CHECK(fx == 0, "the fixture builds and queries");
        CHECK(w.n <= w.cap, "the dump fits the buffer");
        if (fx == 0 && w.n <= w.cap) {
            char path[512];
            snprintf(path, sizeof(path), "%s/select_all.txt", dir);
            size_t glen = 0;
            uint8_t* golden = read_file(path, &glen);
            CHECK(golden != NULL, path);
            if (golden) {
                CHECK(glen == w.n &&
                          memcmp(w.b, golden, w.n) == 0,
                      "the row dump matches the golden byte for byte");
                free(golden);
            }
        }
        test_round_trips(s, &arena);
        test_transactions(s, &arena);
        test_negative(s, &arena);
    }

    free(w.b);
    ash_store_close(s);
    free(arena.base);
    unlink(db_path);

    if (rc) return rc;
    if (g_failures) {
        fprintf(stderr, "[test_store] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_store] %s\n", emit ? "golden emitted" : "ok");
    return 0;
}
