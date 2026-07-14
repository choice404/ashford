/* test_store_stress.c: the S3 concurrency gate, the all or nothing claim held
 * under many instances racing for one file. It signs one Ledger instance per
 * transfer, each its own connection to the shared database, and runs a storm of
 * transfers from a pool of worker threads: pick two accounts, debit one and
 * credit the other as one transactional episode, and tear the instance down.
 * The point is not that every transfer lands. Two connections writing one file
 * arbitrate through SQLite's own locking, and the loser of a race surfaces
 * ASH_ERR_STORE rather than a half written episode, exactly the failure the
 * milestone pins; a transfer can also refuse itself, an overdraft that is the
 * ledger's own Err. The point is that every transfer is whole either way: it
 * commits both writes or it rolls both back, so the money in the pool is a
 * conserved quantity no race and no refusal can bend. The gate seeds a known
 * total, runs the storm, and reads every account back on one thread: the sum is
 * the seed to the last unit, which can only hold if no episode ever committed a
 * debit without its credit or a credit without its debit.
 *
 * Every fulfillment is checked to deliver one of exactly three ways, an ASH_OK
 * commit, an ASH_OK business Err, or an ASH_ERR_STORE the backend raised; any
 * other status is a bug and fails the gate. The connection sits under the
 * instance lock, single threaded by construction, so the runtime adds no store
 * lock and this storm is what TSan watches for a race in the pool, the waiters,
 * and the per instance serialization. Built once under ASan and LSan so every
 * instance the storm signs is proven reclaimed at its break, and once under
 * ThreadSanitizer so the concurrency is proven race free. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

/* The pool, the workers, and the transfers per worker. One instance is signed
 * per transfer, and the runtime tracks every instance it ever signs in a fixed
 * table freed at shutdown, ASH_MAX_INSTANCES entries, so a broken instance holds
 * its slot for the life of the runtime and a run of signs is bounded by that
 * cap, not by how many instances are live at once. The storm is sized to stay
 * inside the cap with margin: NTHREADS * NITERS transfers plus a seed and two
 * reads. A worker that does reach the cap stops clean rather than failing, so
 * the conservation check still stands over the transfers that did run. */
#define NACCTS 6
#define NTHREADS 6
#define NITERS 38
#define SEED_EACH 1000.0

static int g_fail = 0;
static const char* g_dsn;
static AshRuntime* g_rt;

static void fail(const char* what) {
    fprintf(stderr, "[test_store_stress] FAIL: %s\n", what);
    g_fail = 1;
}

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

static AshContract* sign_ledger(AshStatus* st_out) {
    AshVowBinding ovr = { "dsn", str_val(g_dsn) };
    AshContract* c = NULL;
    AshStatus st = ash_contract_sign(g_rt, "Ledger", &ovr, 1, 0, &c);
    if (st_out) *st_out = st;
    return c;
}

/* An Ok result, either arm read by tag: tag 0 is a commit worthy Ok, tag 1 is
 * the ledger's own Err. Called only on an ASH_OK delivery. */
static int result_is_ok(const AshValue* r) {
    return r->ty == ASH_TY_RESULT && r->tag == 0;
}

/* Classifies one pledge delivery. An ASH_OK is a value, an Ok to go on or a
 * business Err to abort; an ASH_ERR_STORE is the backend's refusal, the
 * contended loser; anything else is unexpected and fails the gate. Returns 1
 * when the delivery was an ASH_OK Ok, so the caller proceeds to the credit. */
static int step(AshContract* c, const char* name, const AshValue* args,
                size_t nargs, int* did_store_err) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    AshStatus st = ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out);
    if (st == ASH_OK) {
        return result_is_ok(&out);
    }
    if (st == ASH_ERR_STORE) {
        *did_store_err = 1;
        return 0;
    }
    fail("a transfer pledge delivered neither ASH_OK nor ASH_ERR_STORE");
    return 0;
}

struct worker {
    pthread_t thread;
    unsigned seed;
    long committed;
    long business_abort;
    long store_abort;
};

/* One worker's storm of transfers. Each is a fresh instance, its own connection:
 * pick two distinct accounts and a small amount, debit the first and credit the
 * second as one episode, and break. The episode is whole no matter which way it
 * ends, so the tallies are for visibility, not correctness; conservation is the
 * check and it is read on the main thread after every worker joins. */
static void* run_worker(void* arg) {
    struct worker* w = (struct worker*)arg;
    for (int i = 0; i < NITERS; i++) {
        int src = (int)(rand_r(&w->seed) % NACCTS);
        int dst = (int)(rand_r(&w->seed) % NACCTS);
        if (dst == src) dst = (dst + 1) % NACCTS;
        double amount = (double)(rand_r(&w->seed) % 100 + 1);

        AshStatus ss = ASH_OK;
        AshContract* c = sign_ledger(&ss);
        if (ss == ASH_ERR_OOM) {
            /* the runtime's instance table is full for this run's lifetime, the
             * documented cap and not a store failure; stop clean so the
             * conservation check stands over the transfers that did run. */
            if (c) ash_contract_break(c);
            break;
        }
        if (!c || ash_contract_state(c) != ASH_SIGNED) {
            fail("a worker could not sign an instance");
            if (c) ash_contract_break(c);
            continue;
        }
        int store_err = 0;
        AshValue d[2] = { int_val(src + 1), float_val(amount) };
        AshValue cr[2] = { int_val(dst + 1), float_val(amount) };
        if (step(c, "debit", d, 2, &store_err)) {
            if (step(c, "credit", cr, 2, &store_err)) {
                w->committed++;
            } else if (store_err) {
                w->store_abort++;
            } else {
                w->business_abort++;
            }
        } else if (store_err) {
            w->store_abort++;
        } else {
            w->business_abort++;
        }
        ash_contract_break(c);
    }
    return NULL;
}

/* The whole pool's balance on the main thread, one fresh read instance, so the
 * committed file is the witness. */
static double pool_total(void) {
    AshContract* c = sign_ledger(NULL);
    if (!c || ash_contract_state(c) != ASH_SIGNED) {
        fail("could not sign a read instance for the total");
        if (c) ash_contract_break(c);
        return -1.0;
    }
    double total = 0.0;
    for (int i = 0; i < NACCTS; i++) {
        AshValue key[1] = { int_val(i + 1) };
        AshValue out;
        memset(&out, 0, sizeof(out));
        AshStatus st = ash_pledge_fulfill_sync(c, "balance", key, 1, NULL, 0,
                                               &out);
        if (st != ASH_OK || out.ty != ASH_TY_RESULT || out.tag != 0 ||
            !out.as.box) {
            fail("a balance read for the total did not deliver a value");
            break;
        }
        const AshValue* p = (const AshValue*)out.as.box;
        if (p->ty != ASH_TY_FLOAT) {
            fail("a balance for the total was not a float");
            break;
        }
        total += p->as.f;
    }
    ash_contract_break(c);
    return total;
}

int main(void) {
    if (ash_runtime_init(NULL, &g_rt) != ASH_OK || !g_rt) {
        fprintf(stderr, "[test_store_stress] FAIL: runtime init\n");
        return 1;
    }
    if (ash_module_load(g_rt, "target/ashc-out/libledger.ash.so") != ASH_OK) {
        fprintf(stderr, "[test_store_stress] FAIL: load ledger module\n");
        return 1;
    }

    char db_path[] = "target/ashstress_XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) {
        fprintf(stderr, "[test_store_stress] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);
    char dsn[96];
    snprintf(dsn, sizeof(dsn), "file:%s", db_path);
    g_dsn = dsn;

    /* ---- seed the pool with a known total ---- */

    AshContract* seed = sign_ledger(NULL);
    if (seed && ash_contract_state(seed) == ASH_SIGNED) {
        for (int i = 0; i < NACCTS; i++) {
            AshValue a[3] = { int_val(i + 1), str_val("acct"),
                              float_val(SEED_EACH) };
            AshValue out;
            memset(&out, 0, sizeof(out));
            if (ash_pledge_fulfill_sync(seed, "open", a, 3, NULL, 0, &out) !=
                ASH_OK) {
                fail("seed insert did not commit");
            }
        }
        ash_contract_break(seed);
    } else {
        fail("could not sign the seed instance");
        if (seed) ash_contract_break(seed);
    }

    double before = pool_total();
    const double want = NACCTS * SEED_EACH;
    if (before != want) fail("the seeded total was not the sum of the opens");

    /* ---- the storm: every worker signs, transfers, and breaks in a loop ---- */

    struct worker workers[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) {
        memset(&workers[t], 0, sizeof(workers[t]));
        workers[t].seed = (unsigned)(0x9e3779b9u * (unsigned)(t + 1));
        if (pthread_create(&workers[t].thread, NULL, run_worker,
                           &workers[t]) != 0) {
            fail("could not spawn a worker");
            workers[t].thread = 0;
        }
    }
    long committed = 0, business = 0, store = 0;
    for (int t = 0; t < NTHREADS; t++) {
        if (workers[t].thread) pthread_join(workers[t].thread, NULL);
        committed += workers[t].committed;
        business += workers[t].business_abort;
        store += workers[t].store_abort;
    }

    /* ---- conservation: the pool total is the seed to the last unit ---- */

    double after = pool_total();
    if (after != want) fail("the pool total drifted, a transfer was not whole");

    fprintf(stderr,
            "[test_store_stress] transfers: %ld committed, %ld business abort, "
            "%ld store abort; total %.0f -> %.0f\n",
            committed, business, store, before, after);

    ash_runtime_shutdown(g_rt);
    unlink(db_path);

    if (g_fail) {
        fprintf(stderr, "[test_store_stress] failures\n");
        return 1;
    }
    fprintf(stderr, "[test_store_stress] ok\n");
    return 0;
}
