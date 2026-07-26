/* test_thread.c: the runtime's threading gate. No compiled module here, the
 * descriptors are handwritten so the test drives libgeasrt the way any host
 * embedding it directly would, and the same binary runs under ASan for the
 * memory story and under TSan for the ordering story. It hammers the pool
 * from four host threads across eight instances, serializes a hundred
 * concurrent fulfillments onto one instance, waits a pile of futures in the
 * wrong order on purpose, drives by-reference arguments through the copy-in
 * and write-back protocol under contention, mixes a compiled-style pledge
 * with a host bound one on the same contract, and races break against
 * in-flight fulfillments demanding every wait land on Ok or GEAS_ERR_STATE
 * and nothing else.
 *
 * The cross-contract section hammers the reentrant path: a pledge body that
 * signs another contract, fulfills it synchronously, and breaks it, all from
 * a pool worker. The pool is two workers and four host threads drive it, so
 * the nested fulfillments deadlock the pool unless they run inline on the
 * worker, which is exactly the rule under test. */

#include <geas/geas.h>

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
#define CROSS_OUTERS     4
#define CROSS_ITERS      12 /* signs one Inner per call; instance slots cap the total */

/* ---- the Calc contract: one compiled-style pledge, one host bound ---- */

/* add(a, b) -> Ok(a + b), the stand-in for a compiled body. */
static GeasStatus add_fn(void* ctx, const GeasValue* args, size_t nargs,
                        GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (nargs != 2) return GEAS_ERR_TYPE;
    if (args[0].ty != GEAS_TY_INT || args[1].ty != GEAS_TY_INT)
        return GEAS_ERR_TYPE;
    GeasValue* box = geas_box(c);
    if (!box) return GEAS_ERR_OOM;
    box->ty = GEAS_TY_INT;
    box->as.i = args[0].as.i + args[1].as.i;
    out->ty = GEAS_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return GEAS_OK;
}

/* scale(factor, value&) multiplies the by-reference slot in place and
 * returns Ok(Unit). The abstract half of the contract; the host binds it. */
static GeasStatus scale_fn(void* ctx, const GeasValue* args, size_t nargs,
                          GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (nargs != 2) return GEAS_ERR_TYPE;
    if (args[0].ty != GEAS_TY_INT || args[1].ty != GEAS_TY_INT)
        return GEAS_ERR_TYPE;
    GeasValue* slot = (GeasValue*)&args[1];
    slot->as.i *= args[0].as.i;
    GeasValue* box = geas_box(c);
    if (!box) return GEAS_ERR_OOM;
    box->ty = GEAS_TY_UNIT;
    out->ty = GEAS_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return GEAS_OK;
}

static const GeasPledgeDesc k_calc_pledges[] = {
    { "add",   "__geas_test_add",   2, add_fn, -1 },
    { "scale", "__geas_test_scale", 2, NULL,   -1 }, /* abstract, host binds */
};

/* No requirements data: the runtime applies the structural default policy
 * over the descriptor shape, all loose pledges here. */
static const GeasContractDesc k_calc = {
    .name = "Calc", .shape_hash = 0x5ULL, .version = 1,
    .npledges = 2, .pledges = k_calc_pledges,
};

static GeasValue int_val(int64_t i) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_INT;
    v.as.i = i;
    return v;
}

/* ---- the cross-contract pair: Outer's pledge body drives Inner ---- */

static const GeasPledgeDesc k_inner_pledges[] = {
    { "add", "__geas_test_inner_add", 2, add_fn, -1 },
};

static const GeasContractDesc k_inner = {
    .name = "Inner", .shape_hash = 0x6ULL, .version = 1,
    .npledges = 1, .pledges = k_inner_pledges,
};

/* compose(a, b): the shape a compiled cross-contract thunk emits. It reaches
 * the runtime through its own ctx, signs Inner, fulfills add synchronously,
 * which must run inline because this body is already on a pool worker, deep
 * copies the callee owned result home, and breaks the callee before using
 * the copy. */
static GeasStatus compose_fn(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (nargs != 2) return GEAS_ERR_TYPE;
    GeasRuntime* rt = geas_instance_runtime(c);
    if (!rt) return GEAS_ERR_STATE;
    GeasContract* inner = NULL;
    GeasStatus st = geas_contract_sign(rt, "Inner", NULL, 0, 0, &inner);
    if (st != GEAS_OK) return st;
    GeasValue res;
    memset(&res, 0, sizeof(res));
    st = geas_pledge_fulfill_sync(inner, "add", args, 2, NULL, 0, &res);
    if (st != GEAS_OK) {
        geas_contract_break(inner);
        return st;
    }
    GeasValue mine;
    memset(&mine, 0, sizeof(mine));
    st = geas_value_deep_copy(c, &res, &mine);
    if (st != GEAS_OK) {
        geas_contract_break(inner);
        return st;
    }
    st = geas_contract_break(inner);
    if (st != GEAS_OK) return st;
    *out = mine;
    return GEAS_OK;
}

static const GeasPledgeDesc k_outer_pledges[] = {
    { "compose", "__geas_test_outer_compose", 2, compose_fn, -1 },
};

static const GeasContractDesc k_outer = {
    .name = "Outer", .shape_hash = 0x7ULL, .version = 1,
    .npledges = 1, .pledges = k_outer_pledges,
};

static int check_ok_int(const GeasValue* out, int64_t want) {
    if (out->ty != GEAS_TY_RESULT || out->tag != 0) return 0;
    const GeasValue* inner = (const GeasValue*)out->as.box;
    return inner && inner->ty == GEAS_TY_INT && inner->as.i == want;
}

/* ---- fan out: 4 threads over 8 instances, add and scale mixed ---- */

typedef struct FanJob {
    GeasContract** instances;
    int           ninstances;
    int           tid;
    int           iters;
} FanJob;

static void* fan_worker(void* arg) {
    FanJob* job = (FanJob*)arg;
    for (int i = 0; i < job->iters; i++) {
        GeasContract* c = job->instances[(job->tid + i) % job->ninstances];
        GeasValue args[2] = { int_val(job->tid), int_val(i) };
        GeasValue out;
        if (geas_pledge_fulfill_sync(c, "add", args, 2, NULL, 0, &out) !=
                GEAS_OK ||
            !check_ok_int(&out, job->tid + i)) {
            CHECK(0, "fan-out add fulfillment");
            continue;
        }
        /* Every fourth iteration drives the bound pledge through a ref, so
         * the copy-in and write-back protocol runs under contention too. */
        if (i % 4 == 0) {
            int64_t cell = i + 1;
            GeasRef ref;
            memset(&ref, 0, sizeof(ref));
            ref.host_ptr = &cell;
            ref.ty = GEAS_TY_INT;
            GeasValue factor = int_val(3);
            if (geas_pledge_fulfill_sync(c, "scale", &factor, 1, &ref, 1,
                                        &out) != GEAS_OK ||
                cell != 3 * (i + 1)) {
                CHECK(0, "fan-out scale write back");
            }
        }
    }
    return NULL;
}

static void test_fan_out(GeasContract** instances) {
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
    GeasContract* c;
    int          tid;
} SameJob;

static void* same_worker(void* arg) {
    SameJob* job = (SameJob*)arg;
    for (int i = 0; i < SAME_INST_ITERS; i++) {
        GeasValue args[2] = { int_val(job->tid * 1000), int_val(i) };
        GeasValue out;
        if (geas_pledge_fulfill_sync(job->c, "add", args, 2, NULL, 0, &out) !=
                GEAS_OK ||
            !check_ok_int(&out, job->tid * 1000 + i)) {
            CHECK(0, "same-instance add fulfillment");
        }
    }
    return NULL;
}

static void test_same_instance(GeasContract* c) {
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

static void test_out_of_order_waits(GeasContract* c) {
    GeasFuture* futures[OOO_FUTURES];
    for (int i = 0; i < OOO_FUTURES; i++) {
        GeasValue args[2] = { int_val(i), int_val(i) };
        futures[i] = geas_pledge_fulfill(c, "add", args, 2, NULL, 0);
        CHECK(futures[i] != NULL, "out-of-order fulfill");
    }
    /* Waited back to front: delivery order and completion order share
     * nothing, and every outcome still lands with the right future. */
    for (int i = OOO_FUTURES - 1; i >= 0; i--) {
        if (!futures[i]) continue;
        GeasValue out;
        CHECK(geas_future_wait(futures[i], &out) == GEAS_OK,
              "out-of-order wait");
        CHECK(check_ok_int(&out, 2 * i), "out-of-order value");
        CHECK(geas_future_wait(futures[i], &out) == GEAS_ERR_STATE,
              "double wait reports GEAS_ERR_STATE");
    }
}

/* ---- cross-contract calls from inside pledge bodies, under contention ---- */

typedef struct CrossJob {
    GeasContract** outers;
    int           nouters;
    int           tid;
    int           iters;
} CrossJob;

/* Each iteration drives compose synchronously from a host thread, so a pool
 * worker is inside compose_fn signing, fulfilling, and breaking Inner while
 * its sibling workers do the same against other Outer instances. */
static void* cross_worker(void* arg) {
    CrossJob* job = (CrossJob*)arg;
    for (int i = 0; i < job->iters; i++) {
        GeasContract* c = job->outers[(job->tid + i) % job->nouters];
        GeasValue args[2] = { int_val(job->tid * 100), int_val(i) };
        GeasValue out;
        if (geas_pledge_fulfill_sync(c, "compose", args, 2, NULL, 0, &out) !=
                GEAS_OK ||
            !check_ok_int(&out, job->tid * 100 + i)) {
            CHECK(0, "cross-contract compose fulfillment");
        }
    }
    return NULL;
}

static void test_cross_contract(GeasRuntime* rt, GeasContract** outers) {
    CrossJob jobs[NUM_THREADS];
    pthread_t th[NUM_THREADS];
    CHECK(geas_instance_runtime(outers[0]) == rt,
          "geas_instance_runtime hands back the signing runtime");
    CHECK(geas_instance_runtime(NULL) == NULL,
          "geas_instance_runtime on NULL is NULL");
    for (int t = 0; t < NUM_THREADS; t++) {
        jobs[t].outers = outers;
        jobs[t].nouters = CROSS_OUTERS;
        jobs[t].tid = t;
        jobs[t].iters = CROSS_ITERS;
        CHECK(pthread_create(&th[t], NULL, cross_worker, &jobs[t]) == 0,
              "spawn cross-contract worker");
    }
    for (int t = 0; t < NUM_THREADS; t++) pthread_join(th[t], NULL);
}

/* ---- break racing in-flight fulfillments ---- */

typedef struct RaceJob {
    GeasContract* c;
} RaceJob;

/* Fires sync fulfillments while the main thread breaks the instance. Every
 * status must be GEAS_OK or GEAS_ERR_STATE; on GEAS_OK the payload is not read
 * because the break may have freed the instance heap already. */
static void* race_worker(void* arg) {
    RaceJob* job = (RaceJob*)arg;
    for (int i = 0; i < RACE_FULFILLS; i++) {
        GeasValue args[2] = { int_val(i), int_val(1) };
        GeasValue out;
        GeasStatus st =
            geas_pledge_fulfill_sync(job->c, "add", args, 2, NULL, 0, &out);
        CHECK(st == GEAS_OK || st == GEAS_ERR_STATE,
              "break race status outside {Ok, ERR_STATE}");
    }
    return NULL;
}

static void test_break_race(GeasRuntime* rt) {
    for (int round = 0; round < RACE_ROUNDS; round++) {
        GeasContract* c = NULL;
        CHECK(geas_contract_sign(rt, "Calc", NULL, 0, 0, &c) == GEAS_OK,
              "sign a race instance");
        if (!c) return;
        RaceJob jobs[2] = { { c }, { c } };
        pthread_t th[2];
        CHECK(pthread_create(&th[0], NULL, race_worker, &jobs[0]) == 0,
              "spawn race worker");
        CHECK(pthread_create(&th[1], NULL, race_worker, &jobs[1]) == 0,
              "spawn race worker");
        /* An async fulfillment left in flight when the break lands. */
        GeasValue args[2] = { int_val(round), int_val(round) };
        GeasFuture* f = geas_pledge_fulfill(c, "add", args, 2, NULL, 0);
        CHECK(geas_contract_break(c) == GEAS_OK, "break mid-race");
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        if (f) {
            GeasValue out;
            GeasStatus st = geas_future_wait(f, &out);
            CHECK(st == GEAS_OK || st == GEAS_ERR_STATE,
                  "in-flight future after break outside {Ok, ERR_STATE}");
        }
        CHECK(geas_contract_state(c) == GEAS_BROKEN, "state after mid-race break");
    }
}

int main(void) {
    /* A small pool on purpose: fewer workers than host threads means the
     * queue actually queues and the drain path actually drains. */
    GeasRuntimeConfig cfg = { 2, 0 };
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(&cfg, &rt) == GEAS_OK, "runtime init with config");
    if (!rt) return 1;

    /* An oversized pool request is refused, not obeyed. */
    GeasRuntimeConfig huge = { 100000 };
    GeasRuntime* rt2 = NULL;
    CHECK(geas_runtime_init(&huge, &rt2) == GEAS_ERR_TYPE,
          "oversized max_threads is refused");

    CHECK(geas_register_contract(rt, &k_calc) == GEAS_OK, "register Calc");
    CHECK(geas_pledge_bind(rt, "Calc.scale", scale_fn) == GEAS_OK,
          "bind Calc.scale");
    CHECK(geas_register_contract(rt, &k_inner) == GEAS_OK, "register Inner");
    CHECK(geas_register_contract(rt, &k_outer) == GEAS_OK, "register Outer");

    GeasContract* instances[NUM_INSTANCES] = {0};
    for (int i = 0; i < NUM_INSTANCES; i++) {
        CHECK(geas_contract_sign(rt, "Calc", NULL, 0, 0, &instances[i]) ==
                  GEAS_OK,
              "sign a fan-out instance");
        if (!instances[i]) return 1;
    }
    GeasContract* outers[CROSS_OUTERS] = {0};
    for (int i = 0; i < CROSS_OUTERS; i++) {
        CHECK(geas_contract_sign(rt, "Outer", NULL, 0, 0, &outers[i]) ==
                  GEAS_OK,
              "sign an outer instance");
        if (!outers[i]) return 1;
    }

    test_fan_out(instances);
    test_same_instance(instances[0]);
    test_out_of_order_waits(instances[1]);
    test_cross_contract(rt, outers);
    test_break_race(rt);

    for (int i = 0; i < NUM_INSTANCES; i++) {
        CHECK(geas_contract_break(instances[i]) == GEAS_OK,
              "break a fan-out instance");
    }
    for (int i = 0; i < CROSS_OUTERS; i++) {
        CHECK(geas_contract_break(outers[i]) == GEAS_OK,
              "break an outer instance");
    }
    geas_runtime_shutdown(rt);

    if (g_failures) {
        fprintf(stderr, "[test_thread] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_thread] ok\n");
    return 0;
}
