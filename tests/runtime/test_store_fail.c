/* test_store_fail.c: the S3 failure gate, ASH_ERR_STORE proven to be exactly
 * what docs/database.md says it is and the business boundary proven to hold
 * against it. It loads the compiled Ledger module, signs it against a real
 * SQLite file mkstemp'd under target and unlinked at the end, and drives every
 * way a store can refuse the runtime, checking that each lands ASH_ERR_STORE
 * through the wait and leaves the file consistent. A backend refusal comes two
 * ways here, both hermetic and deterministic. A read only connection, the dsn
 * carrying SQLite's own mode=ro, opens and reconciles fine because a validate
 * is all reads, then refuses every write, the disk with no room made local: a
 * loose insert is ASH_ERR_STORE, and a transactional debit is ASH_ERR_STORE
 * with the whole episode rolled back so the read write file it could not touch
 * is byte for byte what it was. A constraint refusal is a duplicate primary
 * key, the backend blocking an insert the schema's own PRIMARY KEY forbids,
 * ASH_ERR_STORE and distinct from the contract's own Err, and the table stands
 * after it. A contended writer is the loser of a race for one file: one
 * instance holds an open transaction and a second instance's write cannot land,
 * so it surfaces ASH_ERR_STORE rather than stalling forever, and both roll back.
 *
 * The business boundary is the milestone's guard and it sits beside every store
 * failure on purpose: an overdraft is Err(Insufficient), the ledger's own rule
 * as a value with an ASH_OK delivery, and it is never once ASH_ERR_STORE, the
 * line between a store layer and an ORM that swallows domain logic. Every
 * persistence assertion reopens the file in a fresh instance so the file, not a
 * live cache, is the witness. Runs under ASan and LSan so every instance
 * allocation, the rows that land on the instance included, is proven reclaimed
 * at break and shutdown. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond, what)                                          \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "[test_store_fail] FAIL: %s\n", what); \
            g_fail = 1;                                            \
        }                                                          \
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

/* The delivery status of one synchronous fulfillment, the value discarded; a
 * backend refusal returns ASH_ERR_STORE here, the store failing the runtime. */
static AshStatus run_status(AshContract* c, const char* name,
                            const AshValue* args, size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    return ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out);
}

/* Fulfills one pledge synchronously, checks an ASH_OK delivery, and hands back
 * the result value; a business Err rides here as a value, not a status. */
static AshValue run(AshContract* c, const char* name, const AshValue* args,
                    size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out) == ASH_OK,
          name);
    return out;
}

/* Whether a Result is an Err, the contract's own error surfacing as a value. */
static int is_err(const AshValue* r) {
    return r->ty == ASH_TY_RESULT && r->tag == 1;
}

/* An Ok(Float) result, its value read out for comparison. */
static int ok_float(const AshValue* r, double* out) {
    if (r->ty != ASH_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const AshValue* p = (const AshValue*)r->as.box;
    if (p->ty != ASH_TY_FLOAT) return 0;
    *out = p->as.f;
    return 1;
}

/* Signs a fresh Ledger instance against a dsn, the schema reconciled into the
 * live table each time. */
static AshContract* sign_dsn(AshRuntime* rt, const char* dsn) {
    AshVowBinding ovr = { "dsn", str_val(dsn) };
    AshContract* c = NULL;
    AshStatus st = ash_contract_sign(rt, "Ledger", &ovr, 1, 0, &c);
    CHECK(st == ASH_OK && c && ash_contract_state(c) == ASH_SIGNED,
          "sign Ledger");
    return c;
}

/* Reads one account's balance through the loose read path, a fresh read write
 * instance so the witness is the committed file and never a live buffer. */
static double read_balance(AshRuntime* rt, const char* dsn, int64_t id) {
    AshContract* c = sign_dsn(rt, dsn);
    double bal = -1.0;
    if (c) {
        AshValue key[1] = { int_val(id) };
        AshValue r = run(c, "balance", key, 1);
        CHECK(ok_float(&r, &bal), "balance reads a value");
        ash_contract_break(c);
    }
    return bal;
}

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK || !rt) {
        fprintf(stderr, "[test_store_fail] FAIL: runtime init\n");
        return 1;
    }
    CHECK(ash_module_load(rt, "target/ashc-out/libledger.ash.so") == ASH_OK,
          "load ledger module");

    char db_path[] = "target/ashfail_XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) {
        fprintf(stderr, "[test_store_fail] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);
    char rw[96];
    snprintf(rw, sizeof(rw), "file:%s", db_path);
    char ro[128];
    snprintf(ro, sizeof(ro), "file:%s?mode=ro", db_path);

    /* ---- seed two accounts through the loose autocommit pledges ---- */

    AshContract* seed = sign_dsn(rt, rw);
    if (seed) {
        AshValue a1[3] = { int_val(1), str_val("alice"), float_val(100.0) };
        AshValue a2[3] = { int_val(2), str_val("bob"), float_val(100.0) };
        CHECK(run_status(seed, "open", a1, 3) == ASH_OK, "seed account 1");
        CHECK(run_status(seed, "open", a2, 3) == ASH_OK, "seed account 2");
        ash_contract_break(seed);
    }

    /* ---- the business boundary: an overdraft is the ledger's own Err, a
     * value with an ASH_OK delivery, and never a store status. This is the
     * milestone's guard and it comes first, red before the store even fails,
     * so the line between a domain error and a backend refusal is fixed before
     * anything crosses it. ---- */

    AshContract* cb = sign_dsn(rt, rw);
    if (cb) {
        AshValue over[2] = { int_val(1), float_val(500.0) };
        AshValue out;
        memset(&out, 0, sizeof(out));
        AshStatus st = ash_pledge_fulfill_sync(cb, "debit", over, 2, NULL, 0,
                                               &out);
        CHECK(st == ASH_OK,
              "an overdraft delivers ASH_OK, the contract working");
        CHECK(st != ASH_ERR_STORE,
              "an overdraft is never ASH_ERR_STORE, the boundary holds");
        CHECK(is_err(&out),
              "an overdraft is the ledger's own Err, a value not a status");
        ash_contract_break(cb);
    }
    /* the overdraft moved nothing: 1 is still 100, its episode rolled back on
     * the Err the way every failed transfer does. */
    CHECK(read_balance(rt, rw, 1) == 100.0,
          "the refused overdraft left 1 at 100");

    /* ---- a constraint refusal: a duplicate primary key. The backend blocks
     * the second insert the schema's PRIMARY KEY forbids, ASH_ERR_STORE, and it
     * is a store status and not a value, distinct from the ledger's own Err.
     * The table stands after the refusal. ---- */

    AshContract* cc = sign_dsn(rt, rw);
    if (cc) {
        AshValue dup[3] = { int_val(1), str_val("mallory"), float_val(7.0) };
        CHECK(run_status(cc, "open", dup, 3) == ASH_ERR_STORE,
              "a duplicate primary key is ASH_ERR_STORE");
        /* the table survives the refused insert: a fresh row still lands. */
        AshValue fresh[3] = { int_val(3), str_val("carol"), float_val(5.0) };
        CHECK(run_status(cc, "open", fresh, 3) == ASH_OK,
              "the table stands after a refused duplicate");
        ash_contract_break(cc);
    }
    CHECK(read_balance(rt, rw, 1) == 100.0,
          "the refused duplicate did not overwrite 1");
    CHECK(read_balance(rt, rw, 3) == 5.0, "the fresh row after it landed");

    /* ---- a read only connection: the disk with no room made local. mode=ro
     * opens and reconciles because a validate is all reads, then refuses every
     * write. A loose insert is ASH_ERR_STORE. ---- */

    AshContract* cro = sign_dsn(rt, ro);
    if (cro) {
        AshValue w[3] = { int_val(9), str_val("dave"), float_val(1.0) };
        CHECK(run_status(cro, "open", w, 3) == ASH_ERR_STORE,
              "a write to a read only connection is ASH_ERR_STORE");

        /* a transactional debit against the read only file: the write the
         * episode buffers cannot land, ASH_ERR_STORE, and the episode rolls
         * back so nothing half written survives. */
        AshValue d[2] = { int_val(1), float_val(10.0) };
        CHECK(run_status(cro, "debit", d, 2) == ASH_ERR_STORE,
              "a transactional write to a read only connection is ASH_ERR_STORE");
        ash_contract_break(cro);
    }
    /* the read only refusals touched nothing: 1 is still 100 and 9 never
     * existed, read back through the read write connection. */
    CHECK(read_balance(rt, rw, 1) == 100.0,
          "the read only refusals left 1 at 100");
    {
        AshContract* cm = sign_dsn(rt, rw);
        if (cm) {
            AshValue k9[1] = { int_val(9) };
            AshValue r = run(cm, "balance", k9, 1);
            CHECK(is_err(&r), "the refused read only insert left no row 9");
            ash_contract_break(cm);
        }
    }

    /* ---- a contended writer: two instances race for one file. The first holds
     * an open transaction, its debit run and uncommitted; the second's write
     * cannot acquire the file and surfaces ASH_ERR_STORE rather than stalling
     * forever past the backend's busy window. Both roll back, so the file is
     * exactly what it was. This is one connection against another, so it needs
     * no thread: the first episode stays open because its subcontract has not
     * completed, and the second writes into the lock it holds. ---- */

    AshContract* hold = sign_dsn(rt, rw);
    AshContract* race = sign_dsn(rt, rw);
    if (hold && race) {
        AshValue d1[2] = { int_val(1), float_val(10.0) };
        CHECK(run_status(hold, "debit", d1, 2) == ASH_OK,
              "the holder's debit opens the transaction");
        AshValue d2[2] = { int_val(2), float_val(10.0) };
        CHECK(run_status(race, "debit", d2, 2) == ASH_ERR_STORE,
              "the contended writer loses to the open transaction, ASH_ERR_STORE");
    }
    if (race) ash_contract_break(race);
    if (hold) ash_contract_break(hold);
    /* neither write survived: the holder rolled back at break, the loser never
     * landed, and both accounts are back at 100. */
    CHECK(read_balance(rt, rw, 1) == 100.0, "the holder's debit rolled back");
    CHECK(read_balance(rt, rw, 2) == 100.0, "the contended writer wrote nothing");

    ash_runtime_shutdown(rt);
    unlink(db_path);

    if (g_fail) {
        fprintf(stderr, "[test_store_fail] failures\n");
        return 1;
    }
    fprintf(stderr, "[test_store_fail] ok\n");
    return 0;
}
