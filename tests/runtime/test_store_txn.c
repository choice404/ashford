/* test_store_txn.c: the S2 store gate, transactional subcontracts run end to
 * end. It loads the compiled Ledger module, signs it against a real SQLite file
 * mkstemp'd under target and unlinked at the end, and drives the Transfer
 * subcontract the milestone ships: debit and credit as one all-or-nothing
 * episode. It pins the claims the roadmap fixes for S2. A good transfer commits
 * both writes and the file reflects both moved balances. A failed transfer, a
 * credit against a missing account, rolls the whole episode back so the debit
 * that already ran leaves nothing durable and the balance is exactly what it was
 * before. A second call to a pledge whose episode already resolved is
 * ASH_ERR_STATE, the once-only law a committed transaction earns. A break after
 * a debit and before its credit rolls the open transaction back, so no half
 * written episode survives the teardown. Every assertion that a write did or did
 * not persist reopens the file in a fresh instance and reads it, so the file,
 * not a live cache, is the witness. Runs under ASan and LSan so every instance
 * allocation is proven reclaimed at break and shutdown. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond, what)                                          \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "[test_store_txn] FAIL: %s\n", what);  \
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

/* Fulfills one pledge synchronously, checks the delivery status, and hands back
 * the result value. */
static AshValue run(AshContract* c, const char* name, const AshValue* args,
                    size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out) == ASH_OK,
          name);
    return out;
}

/* The delivery status of one synchronous fulfillment, the value discarded; the
 * once-only path returns ASH_ERR_STATE here. */
static AshStatus run_status(AshContract* c, const char* name,
                            const AshValue* args, size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    return ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out);
}

/* An Ok(Unit) result, the shape a transfer pledge answers on success. */
static int ok_unit(const AshValue* r) {
    return r->ty == ASH_TY_RESULT && r->tag == 0;
}

/* An Ok(Float) result, its value read out for comparison. */
static int ok_float(const AshValue* r, double* out) {
    if (r->ty != ASH_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const AshValue* p = (const AshValue*)r->as.box;
    if (p->ty != ASH_TY_FLOAT) return 0;
    *out = p->as.f;
    return 1;
}

/* Whether a Result is an Err, the contract's own error surfacing as a value. */
static int is_err(const AshValue* r) {
    return r->ty == ASH_TY_RESULT && r->tag == 1;
}

/* Signs a fresh Ledger instance against the temp file, the schema validated into
 * the existing table each time. */
static AshContract* sign_ledger(AshRuntime* rt, AshVowBinding* dsn_ovr) {
    AshContract* c = NULL;
    CHECK(ash_contract_sign(rt, "Ledger", dsn_ovr, 1, 0, &c) == ASH_OK,
          "sign Ledger");
    CHECK(c && ash_contract_state(c) == ASH_SIGNED, "Ledger signed");
    return c;
}

/* Reads one account's balance through the loose balance pledge, a fresh instance
 * so the read sees the committed file and never a live episode's buffer. */
static double read_balance(AshRuntime* rt, AshVowBinding* dsn_ovr, int64_t id) {
    AshContract* c = sign_ledger(rt, dsn_ovr);
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
        fprintf(stderr, "[test_store_txn] FAIL: runtime init\n");
        return 1;
    }
    CHECK(ash_module_load(rt, "target/ashc-out/libledger.ash.so") == ASH_OK,
          "load ledger module");

    char db_path[] = "target/ashtxn_XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) {
        fprintf(stderr, "[test_store_txn] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);
    char dsn[80];
    snprintf(dsn, sizeof(dsn), "file:%s", db_path);
    AshVowBinding dsn_ovr = { "dsn", str_val(dsn) };

    /* ---- seed two accounts through the loose, autocommit pledges ---- */

    AshContract* seed = sign_ledger(rt, &dsn_ovr);
    if (seed) {
        AshValue a1[3] = { int_val(1), str_val("alice"), float_val(100.0) };
        AshValue a2[3] = { int_val(2), str_val("bob"), float_val(100.0) };
        AshValue r = run(seed, "open", a1, 3);
        CHECK(r.ty == ASH_TY_RESULT && r.tag == 0, "open account 1");
        r = run(seed, "open", a2, 3);
        CHECK(r.ty == ASH_TY_RESULT && r.tag == 0, "open account 2");
        ash_contract_break(seed);
    }

    /* ---- a good transfer: both writes commit, both balances move ---- */

    AshContract* c = sign_ledger(rt, &dsn_ovr);
    if (c) {
        AshValue d[2] = { int_val(1), float_val(30.0) };
        AshValue cr[2] = { int_val(2), float_val(30.0) };
        AshValue r = run(c, "debit", d, 2);
        CHECK(ok_unit(&r), "debit 30 from 1 is Ok");
        r = run(c, "credit", cr, 2);
        CHECK(ok_unit(&r), "credit 30 to 2 is Ok");

        /* the episode has committed; a second call to a resolved pledge is
         * ASH_ERR_STATE, the once-only law. */
        CHECK(run_status(c, "debit", d, 2) == ASH_ERR_STATE,
              "re-calling a committed transactional pledge is ASH_ERR_STATE");
        CHECK(run_status(c, "credit", cr, 2) == ASH_ERR_STATE,
              "re-calling the other committed pledge is ASH_ERR_STATE");
        ash_contract_break(c);
    }

    /* the file reflects both writes: 1 fell to 70, 2 rose to 130. */
    CHECK(read_balance(rt, &dsn_ovr, 1) == 70.0, "good transfer left 1 at 70");
    CHECK(read_balance(rt, &dsn_ovr, 2) == 130.0, "good transfer left 2 at 130");

    /* ---- a failed transfer: credit a missing account, the whole episode
     * rolls back so the debit that ran leaves nothing durable ---- */

    AshContract* cb = sign_ledger(rt, &dsn_ovr);
    if (cb) {
        AshValue d[2] = { int_val(1), float_val(50.0) };
        AshValue cr[2] = { int_val(99), float_val(50.0) };
        AshValue r = run(cb, "debit", d, 2);
        CHECK(ok_unit(&r), "debit 50 from 1 is Ok");
        r = run(cb, "credit", cr, 2);
        CHECK(is_err(&r), "credit to a missing account is Err");
        ash_contract_break(cb);
    }
    /* the debit did not survive: 1 is still 70, byte for byte the pre-transfer
     * value. */
    CHECK(read_balance(rt, &dsn_ovr, 1) == 70.0,
          "failed transfer rolled the debit back, 1 still 70");

    /* ---- a break mid episode: debit runs, then the contract is torn down
     * before the credit, and the open transaction rolls back ---- */

    AshContract* ck = sign_ledger(rt, &dsn_ovr);
    if (ck) {
        AshValue d[2] = { int_val(1), float_val(25.0) };
        AshValue r = run(ck, "debit", d, 2);
        CHECK(ok_unit(&r), "debit 25 from 1 is Ok");
        CHECK(ash_contract_break(ck) == ASH_OK, "break mid transaction");
    }
    /* no debit survived the break: 1 is still 70. */
    CHECK(read_balance(rt, &dsn_ovr, 1) == 70.0,
          "break before commit left no debit durable, 1 still 70");

    ash_runtime_shutdown(rt);
    unlink(db_path);

    if (g_fail) {
        fprintf(stderr, "[test_store_txn] failures\n");
        return 1;
    }
    fprintf(stderr, "[test_store_txn] ok\n");
    return 0;
}
