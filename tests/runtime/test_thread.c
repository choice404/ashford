/* test_thread.c: the runtime's threading gate. No compiled module here, the
 * descriptors are handwritten so the test drives libashrt the way any host
 * embedding it directly would, and the same binary runs under ASan for the
 * memory story and under TSan for the ordering story. It hammers the pool
 * from four host threads across eight instances, serializes a hundred
 * concurrent fulfillments onto one instance, waits a pile of futures in the
 * wrong order on purpose, drives by-reference arguments through the copy-in
 * and write-back protocol under contention, mixes a compiled-style pledge
 * with a host bound one on the same contract, and races break against
 * in-flight fulfillments demanding every wait land on Ok or ASH_ERR_STATE
 * and nothing else. */

#include <ash/ash.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static pthread_mutex_t g_fail_mu = PTHREAD_MUTEX_INITIALIZER;

static void fail_at(const char* what, const char* file, int line) {
    pthread_mutex_lock(&g_fail_mu);
    fprintf(stderr, "[test_thread] FAIL: %s (%s:%d)\n", what, file, line);
    g_failures++;
    pthread_mutex_unlock(&g_fail_mu);
}

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) fail_at(what, __FILE__, __LINE__);                    \
    } while (0)

#define NUM_INSTANCES    8
#define NUM_THREADS      4
#define ITERS_PER_THREAD 100
#define SAME_INST_ITERS  25
#define OOO_FUTURES      100
#define RACE_ROUNDS      8
#define RACE_FULFILLS    16

/* ---- the Calc contract: one compiled-style pledge, one host bound ---- */

/* add(a, b) -> Ok(a + b), the stand-in for a compiled body. */
static AshStatus add_fn(void* ctx, const AshValue* args, size_t nargs,
                        AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 2) return ASH_ERR_TYPE;
    if (args[0].ty != ASH_TY_INT || args[1].ty != ASH_TY_INT)
        return ASH_ERR_TYPE;
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    box->ty = ASH_TY_INT;
    box->as.i = args[0].as.i + args[1].as.i;
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

/* scale(factor, value&) multiplies the by-reference slot in place and
 * returns Ok(Unit). The abstract half of the contract; the host binds it. */
static AshStatus scale_fn(void* ctx, const AshValue* args, size_t nargs,
                          AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 2) return ASH_ERR_TYPE;
    if (args[0].ty != ASH_TY_INT || args[1].ty != ASH_TY_INT)
        return ASH_ERR_TYPE;
    AshValue* slot = (AshValue*)&args[1];
    slot->as.i *= args[0].as.i;
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    box->ty = ASH_TY_UNIT;
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

static const AshPledgeDesc k_calc_pledges[] = {
    { "add",   "__ash_test_add",   2, add_fn, -1 },
    { "scale", "__ash_test_scale", 2, NULL,   -1 }, /* abstract, host binds */
};

/* No requirements data: the runtime applies the structural default policy
 * over the descriptor shape, all loose pledges here. */
static const AshContractDesc k_calc = {
    .name = "Calc", .shape_hash = 0x5ULL, .version = 1,
    .npledges = 2, .pledges = k_calc_pledges,
};

static AshValue int_val(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

static int check_ok_int(const AshValue* out, int64_t want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* inner = (const AshValue*)out->as.box;
    return inner && inner->ty == ASH_TY_INT && inner->as.i == want;
}

/* ---- fan out: 4 threads over 8 instances, add and scale mixed ---- */

typedef struct FanJob {
    AshContract** instances;
    int           ninstances;
    int           tid;
    int           iters;
} FanJob;

static void* fan_worker(void* arg) {
    FanJob* job = (FanJob*)arg;
    for (int i = 0; i < job->iters; i++) {
        AshContract* c = job->instances[(job->tid + i) % job->ninstances];
        AshValue args[2] = { int_val(job->tid), int_val(i) };
        AshValue out;
        if (ash_pledge_fulfill_sync(c, "add", args, 2, NULL, 0, &out) !=
                ASH_OK ||
            !check_ok_int(&out, job->tid + i)) {
            CHECK(0, "fan-out add fulfillment");
            continue;
        }
        /* Every fourth iteration drives the bound pledge through a ref, so
         * the copy-in and write-back protocol runs under contention too. */
        if (i % 4 == 0) {
            int64_t cell = i + 1;
            AshRef ref;
            memset(&ref, 0, sizeof(ref));
            ref.host_ptr = &cell;
            ref.ty = ASH_TY_INT;
            AshValue factor = int_val(3);
            if (ash_pledge_fulfill_sync(c, "scale", &factor, 1, &ref, 1,
                                        &out) != ASH_OK ||
                cell != 3 * (i + 1)) {
                CHECK(0, "fan-out scale write back");
            }
        }
    }
    return NULL;
}

static void test_fan_out(AshContract** instances) {
    FanJob jobs[NUM_THREADS];
    pthread_t th[NUM_THREADS];
    for (int t = 0; t < NUM_THREADS; t++) {
        jobs[t].instances = instances;
        jobs[t].ninstances = NUM_INSTANCES;
        jobs[t].tid = t;
        jobs[t].iters = ITERS_PER_THREAD;
        CHECK(pthread_create(&th[t], NULL, fan_worker, &jobs[t]) == 0,
              "spawn fan-out worker");
    }
    for (int t = 0; t < NUM_THREADS; t++) pthread_join(th[t], NULL);
}

/* ---- one instance, 100 concurrent fulfillments ---- */

typedef struct SameJob {
    AshContract* c;
    int          tid;
} SameJob;

static void* same_worker(void* arg) {
    SameJob* job = (SameJob*)arg;
    for (int i = 0; i < SAME_INST_ITERS; i++) {
        AshValue args[2] = { int_val(job->tid * 1000), int_val(i) };
        AshValue out;
        if (ash_pledge_fulfill_sync(job->c, "add", args, 2, NULL, 0, &out) !=
                ASH_OK ||
            !check_ok_int(&out, job->tid * 1000 + i)) {
            CHECK(0, "same-instance add fulfillment");
        }
    }
    return NULL;
}

static void test_same_instance(AshContract* c) {
    SameJob jobs[NUM_THREADS];
    pthread_t th[NUM_THREADS];
    for (int t = 0; t < NUM_THREADS; t++) {
        jobs[t].c = c;
        jobs[t].tid = t;
        CHECK(pthread_create(&th[t], NULL, same_worker, &jobs[t]) == 0,
              "spawn same-instance worker");
    }
    for (int t = 0; t < NUM_THREADS; t++) pthread_join(th[t], NULL);
}

/* ---- futures waited out of order ---- */

static void test_out_of_order_waits(AshContract* c) {
    AshFuture* futures[OOO_FUTURES];
    for (int i = 0; i < OOO_FUTURES; i++) {
        AshValue args[2] = { int_val(i), int_val(i) };
        futures[i] = ash_pledge_fulfill(c, "add", args, 2, NULL, 0);
        CHECK(futures[i] != NULL, "out-of-order fulfill");
    }
    /* Waited back to front: delivery order and completion order share
     * nothing, and every outcome still lands with the right future. */
    for (int i = OOO_FUTURES - 1; i >= 0; i--) {
        if (!futures[i]) continue;
        AshValue out;
        CHECK(ash_future_wait(futures[i], &out) == ASH_OK,
              "out-of-order wait");
        CHECK(check_ok_int(&out, 2 * i), "out-of-order value");
        CHECK(ash_future_wait(futures[i], &out) == ASH_ERR_STATE,
              "double wait reports ASH_ERR_STATE");
    }
}

/* ---- break racing in-flight fulfillments ---- */

typedef struct RaceJob {
    AshContract* c;
} RaceJob;

/* Fires sync fulfillments while the main thread breaks the instance. Every
 * status must be ASH_OK or ASH_ERR_STATE; on ASH_OK the payload is not read
 * because the break may have freed the instance heap already. */
static void* race_worker(void* arg) {
    RaceJob* job = (RaceJob*)arg;
    for (int i = 0; i < RACE_FULFILLS; i++) {
        AshValue args[2] = { int_val(i), int_val(1) };
        AshValue out;
        AshStatus st =
            ash_pledge_fulfill_sync(job->c, "add", args, 2, NULL, 0, &out);
        CHECK(st == ASH_OK || st == ASH_ERR_STATE,
              "break race status outside {Ok, ERR_STATE}");
    }
    return NULL;
}

static void test_break_race(AshRuntime* rt) {
    for (int round = 0; round < RACE_ROUNDS; round++) {
        AshContract* c = NULL;
        CHECK(ash_contract_sign(rt, "Calc", NULL, 0, 0, &c) == ASH_OK,
              "sign a race instance");
        if (!c) return;
        RaceJob jobs[2] = { { c }, { c } };
        pthread_t th[2];
        CHECK(pthread_create(&th[0], NULL, race_worker, &jobs[0]) == 0,
              "spawn race worker");
        CHECK(pthread_create(&th[1], NULL, race_worker, &jobs[1]) == 0,
              "spawn race worker");
        /* An async fulfillment left in flight when the break lands. */
        AshValue args[2] = { int_val(round), int_val(round) };
        AshFuture* f = ash_pledge_fulfill(c, "add", args, 2, NULL, 0);
        CHECK(ash_contract_break(c) == ASH_OK, "break mid-race");
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        if (f) {
            AshValue out;
            AshStatus st = ash_future_wait(f, &out);
            CHECK(st == ASH_OK || st == ASH_ERR_STATE,
                  "in-flight future after break outside {Ok, ERR_STATE}");
        }
        CHECK(ash_contract_state(c) == ASH_BROKEN, "state after mid-race break");
    }
}

int main(void) {
    /* A small pool on purpose: fewer workers than host threads means the
     * queue actually queues and the drain path actually drains. */
    AshRuntimeConfig cfg = { 2 };
    AshRuntime* rt = NULL;
    CHECK(ash_runtime_init(&cfg, &rt) == ASH_OK, "runtime init with config");
    if (!rt) return 1;

    /* An oversized pool request is refused, not obeyed. */
    AshRuntimeConfig huge = { 100000 };
    AshRuntime* rt2 = NULL;
    CHECK(ash_runtime_init(&huge, &rt2) == ASH_ERR_TYPE,
          "oversized max_threads is refused");

    CHECK(ash_register_contract(rt, &k_calc) == ASH_OK, "register Calc");
    CHECK(ash_pledge_bind(rt, "Calc.scale", scale_fn) == ASH_OK,
          "bind Calc.scale");

    AshContract* instances[NUM_INSTANCES] = {0};
    for (int i = 0; i < NUM_INSTANCES; i++) {
        CHECK(ash_contract_sign(rt, "Calc", NULL, 0, 0, &instances[i]) ==
                  ASH_OK,
              "sign a fan-out instance");
        if (!instances[i]) return 1;
    }

    test_fan_out(instances);
    test_same_instance(instances[0]);
    test_out_of_order_waits(instances[1]);
    test_break_race(rt);

    for (int i = 0; i < NUM_INSTANCES; i++) {
        CHECK(ash_contract_break(instances[i]) == ASH_OK,
              "break a fan-out instance");
    }
    ash_runtime_shutdown(rt);

    if (g_failures) {
        fprintf(stderr, "[test_thread] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_thread] ok\n");
    return 0;
}
