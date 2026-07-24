/* test_store_ledger.c: the S1 store gate, the walking skeleton of Layer 3 run
 * end to end. It loads the compiled Ledger module, signs it against a real
 * SQLite file mkstemp'd under target and unlinked at the end, and drives the
 * loose store pledges the milestone ships: open writes a row through
 * Store.insert, balance reads one back through Store.find, set_balance rewrites
 * one through Store.update, and owned_total reads every row of one owner through
 * Store.query and folds their balances, each a Result whose Ok arm the primitive
 * built. It pins the claims the roadmap fixes for S1. A fresh database file is
 * created and works, the schema reconciled into it at sign. A written row reads
 * back its value, and an update round trips. A missing account is
 * Err(NoSuchAccount), the contract's own business, never a store status. A
 * value bound into an operation is a value and never SQL: an owner holding a
 * DROP TABLE statement is stored as bytes and the table survives. A store
 * backed contract with no dsn vow fails the sign with ASH_ERR_UNBOUND, and a
 * schema that disagrees with the live table fails it with ASH_ERR_TYPE. Runs
 * under ASan and LSan so every instance allocation, the rows read onto the
 * instance included, is proven reclaimed at break and shutdown. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond, what)                                             \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "[test_store_ledger] FAIL: %s\n", what);  \
            g_fail = 1;                                               \
        }                                                             \
    } while (0)

static AshValue int_val(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

static AshValue float_val(double f) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_FLOAT;
    v.as.f = f;
    return v;
}

static AshValue str_val(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

/* Fulfills one pledge synchronously and hands back its result value. */
static AshValue run(AshContract* c, const char* name, const AshValue* args,
                    size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out) == ASH_OK,
          name);
    return out;
}

/* An Ok(Bool) result reads back true. */
static int ok_true(const AshValue* r) {
    if (r->ty != ASH_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const AshValue* p = (const AshValue*)r->as.box;
    return p->ty == ASH_TY_BOOL && p->as.b == 1;
}

/* An Ok(Int) result, its value read out for comparison. */
static int ok_int(const AshValue* r, int64_t* out) {
    if (r->ty != ASH_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const AshValue* p = (const AshValue*)r->as.box;
    if (p->ty != ASH_TY_INT) return 0;
    *out = p->as.i;
    return 1;
}

/* An Ok(Float) result, its value read out for comparison. */
static int ok_float(const AshValue* r, double* out) {
    if (r->ty != ASH_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const AshValue* p = (const AshValue*)r->as.box;
    if (p->ty != ASH_TY_FLOAT) return 0;
    *out = p->as.f;
    return 1;
}

/* An Ok(String) result, compared against an expected C string byte for byte. */
static int ok_str(const AshValue* r, const char* want) {
    if (r->ty != ASH_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const AshValue* p = (const AshValue*)r->as.box;
    if (p->ty != ASH_TY_STRING) return 0;
    size_t wl = strlen(want);
    if ((size_t)p->as.s.len != wl) return 0;
    if (wl == 0) return 1;
    return memcmp(p->as.s.ptr, want, wl) == 0;
}

/* Whether a Result is an Err, the contract's own error surfacing as a value. */
static int is_err(const AshValue* r) {
    return r->ty == ASH_TY_RESULT && r->tag == 1;
}

/* ---- handwritten descriptors for the two refusal paths ---- */

/* A store backed contract whose Accounts schema disagrees with the one the
 * Ledger created: owner is declared Int where the live table holds TEXT, so a
 * sign against the same file validates the table column for column and fails
 * ASH_ERR_TYPE. It carries a dsn vow so the sign reaches reconcile. */
static const AshSchemaCol k_div_cols[] = {
    { "id", ASH_TY_INT },
    { "balance", ASH_TY_FLOAT },
    { "owner", ASH_TY_INT },
};
static const AshSchemaDesc k_div_schemas[] = {
    { "Accounts", 3, k_div_cols },
};
static const AshVowDesc k_div_vows[] = {
    { "dsn", ASH_TY_STRING, 0, { 0 } },
};
static const AshContractDesc k_div = {
    .name = "LedgerDiverge", .version = 1,
    .nvows = 1, .vows = k_div_vows,
    .nschemas = 1, .schemas = k_div_schemas,
};

/* A store backed contract with a schema and no dsn vow: the sign has no
 * database to bind and fails ASH_ERR_UNBOUND, the same gap an unsupplied vow
 * always hit. */
static const AshSchemaCol k_nod_cols[] = {
    { "id", ASH_TY_INT },
    { "balance", ASH_TY_FLOAT },
    { "owner", ASH_TY_STRING },
};
static const AshSchemaDesc k_nod_schemas[] = {
    { "Vault", 3, k_nod_cols },
};
static const AshContractDesc k_nod = {
    .name = "LedgerNoDsn", .version = 1,
    .nschemas = 1, .schemas = k_nod_schemas,
};

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK || !rt) {
        fprintf(stderr, "[test_store_ledger] FAIL: runtime init\n");
        return 1;
    }
    CHECK(ash_module_load(rt, "target/ashc-out/libledger.ash.so") == ASH_OK,
          "load ledger module");
    CHECK(ash_register_contract(rt, &k_div) == ASH_OK, "register diverge");
    CHECK(ash_register_contract(rt, &k_nod) == ASH_OK, "register no-dsn");

    /* A fresh file under target, faithful to the "file:" dsn, unlinked at end. */
    char db_path[] = "target/ashledger_XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) {
        fprintf(stderr, "[test_store_ledger] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);
    char dsn[80];
    snprintf(dsn, sizeof(dsn), "file:%s", db_path);
    AshVowBinding dsn_ovr = { "dsn", str_val(dsn) };

    /* ---- sign against a fresh file: the schema is created ---- */

    AshContract* c = NULL;
    CHECK(ash_contract_sign(rt, "Ledger", &dsn_ovr, 1, 0, &c) == ASH_OK,
          "sign Ledger on a fresh file");
    CHECK(c && ash_contract_state(c) == ASH_SIGNED, "Ledger signed");

    if (c) {
        /* open account 1, then read its balance back. */
        AshValue open1[3] = { int_val(1), str_val("alice"), float_val(100.0) };
        AshValue r = run(c, "open", open1, 3);
        CHECK(ok_true(&r), "open account 1");

        AshValue key1[1] = { int_val(1) };
        r = run(c, "balance", key1, 1);
        double bal = 0.0;
        CHECK(ok_float(&r, &bal) && bal == 100.0, "balance of 1 reads 100");

        /* a missing account is the contract's own error, not a store status. */
        AshValue key2[1] = { int_val(2) };
        r = run(c, "balance", key2, 1);
        CHECK(is_err(&r), "balance of a missing account is Err");

        /* update the row and read the new value back. */
        AshValue set1[3] = { int_val(1), str_val("alice"), float_val(250.0) };
        r = run(c, "set_balance", set1, 3);
        CHECK(ok_true(&r), "set_balance of 1");
        r = run(c, "balance", key1, 1);
        CHECK(ok_float(&r, &bal) && bal == 250.0, "balance of 1 now reads 250");

        /* injection resistance: an owner holding a DROP TABLE is a value, bound
         * positionally and never SQL, so the row lands and the table survives. */
        AshValue open3[3] = {
            int_val(3), str_val("'); DROP TABLE Accounts; --"), float_val(5.0)
        };
        r = run(c, "open", open3, 3);
        CHECK(ok_true(&r), "open account 3 with an injection owner");
        AshValue key3[1] = { int_val(3) };
        r = run(c, "balance", key3, 1);
        CHECK(ok_float(&r, &bal) && bal == 5.0, "balance of 3 reads 5");
        r = run(c, "balance", key1, 1);
        CHECK(ok_float(&r, &bal) && bal == 250.0,
              "the table survived the injection string");

        /* Store.query over a non-key column: two accounts share the owner "ada"
         * and a third holds "bob", so owned_total binds the owner column and
         * folds only ada's rows into their sum, never bob's. An owner with no
         * matching row is a clean Ok(0.0), the empty list a fold that never
         * runs. */
        AshValue open_ada1[3] = { int_val(10), str_val("ada"), float_val(40.0) };
        r = run(c, "open", open_ada1, 3);
        CHECK(ok_true(&r), "open ada account 10");
        AshValue open_ada2[3] = { int_val(11), str_val("ada"), float_val(60.0) };
        r = run(c, "open", open_ada2, 3);
        CHECK(ok_true(&r), "open ada account 11");
        AshValue open_bob[3] = { int_val(12), str_val("bob"), float_val(999.0) };
        r = run(c, "open", open_bob, 3);
        CHECK(ok_true(&r), "open bob account 12");

        AshValue who_ada[1] = { str_val("ada") };
        r = run(c, "owned_total", who_ada, 1);
        CHECK(ok_float(&r, &bal) && bal == 100.0,
              "owned_total of ada sums both her balances and excludes bob");

        AshValue who_none[1] = { str_val("nobody") };
        r = run(c, "owned_total", who_none, 1);
        CHECK(ok_float(&r, &bal) && bal == 0.0,
              "owned_total of an owner with no rows is Ok(0.0)");

        /* Store.query in its predicate form. The table now holds 1/alice 250,
         * 3/injection 5, 10/ada 40, 11/ada 60, and 12/bob 999. rich compares
         * the balance column against a floor with '>=' and counts every account
         * that clears it across all owners; owned_above joins two comparisons
         * with &&, the owner column bound to a name and the balance column
         * against a floor, and folds the matching balances into one sum. */
        int64_t cnt = 0;
        AshValue rich100[1] = { float_val(100.0) };
        r = run(c, "rich", rich100, 1);
        CHECK(ok_int(&r, &cnt) && cnt == 2,
              "rich(100.0) counts the two accounts at or above 100");
        AshValue rich40[1] = { float_val(40.0) };
        r = run(c, "rich", rich40, 1);
        CHECK(ok_int(&r, &cnt) && cnt == 4,
              "rich(40.0) counts the four accounts at or above 40");
        AshValue rich10k[1] = { float_val(10000.0) };
        r = run(c, "rich", rich10k, 1);
        CHECK(ok_int(&r, &cnt) && cnt == 0,
              "rich(10000.0) counts none and is Ok(0)");

        /* owned_above proves the left of '==' is the owner column and never the
         * owner parameter that shares its name: only ada's rows are considered,
         * and of those only the balance at or above the floor is folded. */
        AshValue above_ada[2] = { str_val("ada"), float_val(50.0) };
        r = run(c, "owned_above", above_ada, 2);
        CHECK(ok_float(&r, &bal) && bal == 60.0,
              "owned_above of ada at 50 folds only her 60 balance");
        AshValue above_ada_hi[2] = { str_val("ada"), float_val(1000.0) };
        r = run(c, "owned_above", above_ada_hi, 2);
        CHECK(ok_float(&r, &bal) && bal == 0.0,
              "owned_above of ada at 1000 matches no row and is Ok(0.0)");

        /* Store.query in its ordered form: a predicate then asc(column) or
         * desc(column). The table holds 1/alice 250, 3/injection 5, 10/ada 40,
         * 11/ada 60, and 12/bob 999. owners_asc(40.0) keeps every row at or
         * above 40, orders them by ascending balance, 40 ada, 60 ada, 250 alice,
         * 999 bob, and folds their owners into one string; owners_desc(40.0)
         * folds them largest balance first. The balances are all distinct, so
         * the order is total and the concatenation is exact. */
        AshValue floor40[1] = { float_val(40.0) };
        r = run(c, "owners_asc", floor40, 1);
        CHECK(ok_str(&r, "adaadaalicebob"),
              "owners_asc(40.0) folds owners by ascending balance");
        r = run(c, "owners_desc", floor40, 1);
        CHECK(ok_str(&r, "bobaliceadaada"),
              "owners_desc(40.0) folds owners by descending balance");

        /* A floor no account clears orders an empty list into Ok(""). */
        AshValue floor_hi[1] = { float_val(100000.0) };
        r = run(c, "owners_asc", floor_hi, 1);
        CHECK(ok_str(&r, ""),
              "owners_asc(100000.0) matches no row and is an ordered Ok(\"\")");

        /* Store.query in its bounded form: a predicate, an order, then
         * limit(count) capping the ordered page. desc(balance) orders the rows
         * at or above 40, 999 bob, 250 alice, 60 ada, 40 ada, so top_owner keeps
         * the single largest, limit(2) keeps bob then alice, limit(0) keeps
         * nothing, and a limit past the page folds every matching owner. The
         * count in top_owners is a runtime parameter, so the bound is a value the
         * caller supplies and never a literal the compiler read. */
        r = run(c, "top_owner", floor40, 1);
        CHECK(ok_str(&r, "bob"),
              "top_owner(40.0) keeps the single largest balance owner");
        AshValue top2[2] = { float_val(40.0), int_val(2) };
        r = run(c, "top_owners", top2, 2);
        CHECK(ok_str(&r, "bobalice"),
              "top_owners(40.0, 2) folds the two largest balance owners");
        AshValue top0[2] = { float_val(40.0), int_val(0) };
        r = run(c, "top_owners", top0, 2);
        CHECK(ok_str(&r, ""),
              "top_owners(40.0, 0) keeps no row and is Ok(\"\")");
        AshValue top100[2] = { float_val(40.0), int_val(100) };
        r = run(c, "top_owners", top100, 2);
        CHECK(ok_str(&r, "bobaliceadaada"),
              "top_owners(40.0, 100) folds every matching owner in order");

        CHECK(ash_contract_break(c) == ASH_OK, "break Ledger");
    }

    /* ---- the refusal paths ---- */

    /* A schema that disagrees with the live table fails the sign ASH_ERR_TYPE. */
    AshContract* cd = NULL;
    CHECK(ash_contract_sign(rt, "LedgerDiverge", &dsn_ovr, 1, 0, &cd) ==
              ASH_ERR_TYPE,
          "a divergent schema fails the sign with ASH_ERR_TYPE");
    CHECK(cd == NULL, "a failed reconcile leaves no instance");

    /* A store backed contract with no dsn vow fails the sign ASH_ERR_UNBOUND. */
    AshContract* cn = NULL;
    CHECK(ash_contract_sign(rt, "LedgerNoDsn", NULL, 0, 0, &cn) ==
              ASH_ERR_UNBOUND,
          "a missing dsn vow fails the sign with ASH_ERR_UNBOUND");
    CHECK(cn == NULL, "a missing dsn leaves no instance");

    ash_runtime_shutdown(rt);
    unlink(db_path);

    if (g_fail) {
        fprintf(stderr, "[test_store_ledger] failures\n");
        return 1;
    }
    fprintf(stderr, "[test_store_ledger] ok\n");
    return 0;
}
