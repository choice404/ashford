/* test_stress.c: the N3 resilience gate under load. It drives ashd hard from
 * many connections at once, then tears the world down the two violent ways a
 * network can and demands the one new status lands everywhere it should with no
 * crash, no hang, and no leak.
 *
 * Three phases, in order, because the last one kills the daemon:
 *
 *   1. load     N client connections, each its own runtime, each running T
 *               worker threads that sign their own instance and fulfill it K
 *               times, so the daemon serves N*T instances and N*T*K
 *               fulfillments concurrently across its connection and waiter
 *               threads. Every outcome must be the module's Ok(true). This is
 *               the surface TSan watches: the reader, the waiters, the pool,
 *               and the per instance lock all under real contention.
 *
 *   2. churn    a steady worker runs a long sign/fulfill/break loop while a
 *               storm of short lived connections springs up and is torn down
 *               out from under the daemon. The daemon must keep serving the
 *               survivor and a fresh connection after, proving one connection's
 *               death frees its instances without touching another's and
 *               without leaking the fd or the thread that served it.
 *
 *   3. killstorm  N connections sign an instance and launch a batch of
 *               fulfillments, then the daemon takes a SIGKILL mid flight. Every
 *               in flight wait must deliver ASH_ERR_NET or a clean Ok, at least
 *               one must see ASH_ERR_NET, a later fulfill must be a local state
 *               error, and every runtime must still shut down cleanly. This is
 *               the hard kill the graceful path never exercises.
 *
 * A hard alarm bounds the whole run, so a teardown that ever deadlocks fails
 * the gate loudly instead of hanging a CI forever, the one thing the design
 * refuses to allow. The address, token, module, and daemon pid arrive as
 * argv, the same shape the N2 gate's client takes. */

#include <ash/ash.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTRACT   "PaymentService"
#define N_CONN     6    /* concurrent client connections, each its own runtime */
#define N_THREAD   4    /* worker threads per connection */
#define N_FULFILL  40   /* fulfillments per worker */
#define N_INFLIGHT 96   /* async fulfillments launched per connection before kill */
#define N_CHURN    40   /* short lived connections in the churn storm */
#define ALARM_SECS 90   /* the whole run's hard ceiling */

static int fail(const char* where, const char* what) {
    fprintf(stderr, "[test-net-stress] FAIL (%s): %s\n", where, what);
    return 1;
}

static AshValue vstr(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof v);
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

/* Demands an Ok(true), the shape every pledge in this module returns. */
static int check_ok_true(const AshValue* out) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* in = (const AshValue*)out->as.box;
    return in && in->ty == ASH_TY_BOOL && in->as.b == 1;
}

/* A hang anywhere below trips this: the gate fails rather than parking a CI. */
static void on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "[test-net-stress] FAIL: timed out, a teardown hung\n";
    ssize_t n = write(2, msg, sizeof msg - 1);
    (void)n;
    _exit(3);
}

/* ---- phase 1: concurrent load ---- */

typedef struct WorkArg {
    AshRuntime* rt;
    int         fails;
} WorkArg;

/* One worker: its own instance, K synchronous fulfillments, then a break, so
 * the instance's whole lifecycle runs concurrently with every sibling's on the
 * same connection and every other connection's on the daemon. */
static void* load_worker(void* p) {
    WorkArg* a = (WorkArg*)p;
    AshContract* c = NULL;
    if (ash_contract_sign(a->rt, CONTRACT, NULL, 0, 0, &c) != ASH_OK) {
        a->fails++;
        return NULL;
    }
    AshValue card = vstr("4111111111111111");
    for (int i = 0; i < N_FULFILL; i++) {
        AshValue out;
        if (ash_pledge_fulfill_sync(c, "validate_card", &card, 1, NULL, 0,
                                    &out) != ASH_OK ||
            !check_ok_true(&out)) {
            a->fails++;
            break;
        }
    }
    if (ash_contract_break(c) != ASH_OK) a->fails++;
    return NULL;
}

typedef struct ConnArg {
    const char* addr;
    const char* token;
    int         fails;
} ConnArg;

/* One connection running T load workers, then closing cleanly. */
static void* load_conn(void* p) {
    ConnArg* a = (ConnArg*)p;
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) {
        a->fails++;
        return NULL;
    }
    if (ash_runtime_connect(rt, a->addr, a->token) != ASH_OK) {
        ash_runtime_shutdown(rt);
        a->fails++;
        return NULL;
    }
    WorkArg wargs[N_THREAD];
    pthread_t ths[N_THREAD];
    int spawned = 0;
    for (int i = 0; i < N_THREAD; i++) {
        wargs[i].rt = rt;
        wargs[i].fails = 0;
        if (pthread_create(&ths[i], NULL, load_worker, &wargs[i]) != 0) {
            a->fails++;
            break;
        }
        spawned++;
    }
    for (int i = 0; i < spawned; i++) {
        pthread_join(ths[i], NULL);
        a->fails += wargs[i].fails;
    }
    ash_runtime_shutdown(rt);
    return NULL;
}

static int run_load(const char* addr, const char* token) {
    ConnArg cargs[N_CONN];
    pthread_t ths[N_CONN];
    int spawned = 0;
    for (int i = 0; i < N_CONN; i++) {
        cargs[i].addr = addr;
        cargs[i].token = token;
        cargs[i].fails = 0;
        if (pthread_create(&ths[i], NULL, load_conn, &cargs[i]) != 0) {
            return fail("load", "spawn a connection thread");
        }
        spawned++;
    }
    int fails = 0;
    for (int i = 0; i < spawned; i++) {
        pthread_join(ths[i], NULL);
        fails += cargs[i].fails;
    }
    if (fails) return fail("load", "a concurrent fulfillment did not land Ok(true)");
    fprintf(stderr, "[test-net-stress] load: %d conns x %d threads x %d fulfills ok\n",
            N_CONN, N_THREAD, N_FULFILL);
    return 0;
}

/* ---- phase 2: connection churn the daemon must survive ---- */

/* A short lived connection: connect, sign, one fulfill, then shut down, the
 * disconnect a running daemon must absorb without dropping its other clients. */
static void* churn_conn(void* p) {
    ConnArg* a = (ConnArg*)p;
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) {
        a->fails++;
        return NULL;
    }
    if (ash_runtime_connect(rt, a->addr, a->token) != ASH_OK) {
        ash_runtime_shutdown(rt);
        a->fails++;
        return NULL;
    }
    AshContract* c = NULL;
    if (ash_contract_sign(rt, CONTRACT, NULL, 0, 0, &c) == ASH_OK) {
        AshValue card = vstr("4111111111111111");
        AshValue out;
        (void)ash_pledge_fulfill_sync(c, "validate_card", &card, 1, NULL, 0, &out);
    }
    /* Shutdown severs the connection under the daemon while its siblings and the
     * steady worker keep running; the daemon breaks this connection's instance
     * and reaps its thread on its own. */
    ash_runtime_shutdown(rt);
    return NULL;
}

static int run_churn(const char* addr, const char* token) {
    /* A steady worker whose long loop must survive the storm intact. */
    AshRuntime* steady = NULL;
    if (ash_runtime_init(NULL, &steady) != ASH_OK) return fail("churn", "init steady");
    if (ash_runtime_connect(steady, addr, token) != ASH_OK) {
        ash_runtime_shutdown(steady);
        return fail("churn", "connect steady");
    }

    ConnArg cargs[N_CHURN];
    pthread_t ths[N_CHURN];
    int spawned = 0;
    for (int i = 0; i < N_CHURN; i++) {
        cargs[i].addr = addr;
        cargs[i].token = token;
        cargs[i].fails = 0;
        if (pthread_create(&ths[i], NULL, churn_conn, &cargs[i]) != 0) break;
        spawned++;
    }

    /* Drive the steady worker's whole lifecycle while the storm rages. */
    WorkArg sw = { steady, 0 };
    load_worker(&sw);

    int fails = sw.fails;
    for (int i = 0; i < spawned; i++) {
        pthread_join(ths[i], NULL);
        fails += cargs[i].fails;
    }
    ash_runtime_shutdown(steady);
    if (fails) return fail("churn", "the steady worker or a churn client failed");

    /* The daemon survived the storm: a fresh connection still gets served. */
    AshRuntime* after = NULL;
    if (ash_runtime_init(NULL, &after) != ASH_OK) return fail("churn", "init after");
    if (ash_runtime_connect(after, addr, token) != ASH_OK) {
        ash_runtime_shutdown(after);
        return fail("churn", "daemon did not survive the churn");
    }
    AshContract* c = NULL;
    if (ash_contract_sign(after, CONTRACT, NULL, 0, 0, &c) != ASH_OK) {
        ash_runtime_shutdown(after);
        return fail("churn", "sign after the churn");
    }
    AshValue card = vstr("4111111111111111");
    AshValue out;
    if (ash_pledge_fulfill_sync(c, "validate_card", &card, 1, NULL, 0, &out) !=
            ASH_OK ||
        !check_ok_true(&out)) {
        ash_runtime_shutdown(after);
        return fail("churn", "fulfill after the churn");
    }
    ash_runtime_shutdown(after);
    fprintf(stderr, "[test-net-stress] churn: daemon survived %d disconnects\n",
            N_CHURN);
    return 0;
}

/* ---- phase 3: the SIGKILL storm ---- */

typedef struct KillArg {
    const char*  addr;
    const char*  token;
    AshRuntime*  rt;
    AshContract* c;
    AshFuture*   fs[N_INFLIGHT];
    int          nfs;
    int          nnet;
    int          nok;
    int          fails;
} KillArg;

static int run_killstorm(const char* addr, const char* token, pid_t pid) {
    KillArg ka[N_CONN];
    memset(ka, 0, sizeof ka);

    /* Stand up N connections, each with an instance and a batch of in flight
     * fulfillments, before the daemon is touched. */
    for (int i = 0; i < N_CONN; i++) {
        ka[i].addr = addr;
        ka[i].token = token;
        if (ash_runtime_init(NULL, &ka[i].rt) != ASH_OK)
            return fail("killstorm", "init");
        if (ash_runtime_connect(ka[i].rt, addr, token) != ASH_OK) {
            ash_runtime_shutdown(ka[i].rt);
            ka[i].rt = NULL;
            return fail("killstorm", "connect");
        }
        if (ash_contract_sign(ka[i].rt, CONTRACT, NULL, 0, 0, &ka[i].c) != ASH_OK)
            return fail("killstorm", "sign");
        AshValue card = vstr("4111111111111111");
        for (int j = 0; j < N_INFLIGHT; j++) {
            AshFuture* f =
                ash_pledge_fulfill(ka[i].c, "validate_card", &card, 1, NULL, 0);
            if (!f) return fail("killstorm", "launch a fulfillment");
            ka[i].fs[ka[i].nfs++] = f;
        }
    }

    /* Sever the daemon out from under every in flight batch at once. */
    kill(pid, SIGKILL);

    int total_net = 0;
    for (int i = 0; i < N_CONN; i++) {
        for (int j = 0; j < ka[i].nfs; j++) {
            AshValue out;
            AshStatus st = ash_future_wait(ka[i].fs[j], &out);
            if (st == ASH_ERR_NET) {
                ka[i].nnet++;
                total_net++;
            } else if (st == ASH_OK && check_ok_true(&out)) {
                ka[i].nok++;
            } else {
                return fail("killstorm",
                            "an in flight wait was neither Ok nor ASH_ERR_NET");
            }
        }
        /* The proxy has latched Broken: a later fulfill is a clean local error
         * and the state read is Broken without touching the dead wire. */
        AshValue card = vstr("4111111111111111");
        AshValue out;
        AshStatus later = ash_pledge_fulfill_sync(ka[i].c, "validate_card", &card,
                                                  1, NULL, 0, &out);
        if (later != ASH_ERR_STATE && later != ASH_ERR_NET)
            return fail("killstorm", "a fulfill after the death was not clean");
        if (ash_contract_state(ka[i].c) != ASH_BROKEN)
            return fail("killstorm", "state after the death should be Broken");
    }
    if (total_net == 0)
        return fail("killstorm", "no in flight wait saw ASH_ERR_NET");

    /* A fresh connect to the now dead address is a network failure, not a hang. */
    if (ash_runtime_connect(ka[0].rt, addr, token) != ASH_ERR_NET)
        return fail("killstorm", "connect to the dead daemon was not ASH_ERR_NET");

    /* Every runtime shuts down cleanly with its connection already dead. */
    for (int i = 0; i < N_CONN; i++) {
        if (ka[i].rt) ash_runtime_shutdown(ka[i].rt);
    }
    fprintf(stderr,
            "[test-net-stress] killstorm: %d in flight across %d conns, "
            "%d saw ASH_ERR_NET\n",
            N_CONN * N_INFLIGHT, N_CONN, total_net);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s host:port token module.so daemon_pid\n",
                argv[0]);
        return 2;
    }
    const char* addr = argv[1];
    const char* token = argv[2];
    pid_t pid = (pid_t)strtol(argv[4], NULL, 10);

    signal(SIGALRM, on_alarm);
    alarm(ALARM_SECS);

    if (run_load(addr, token) != 0) return 1;
    if (run_churn(addr, token) != 0) return 1;
    /* Kills the daemon; must run last. */
    if (run_killstorm(addr, token, pid) != 0) return 1;

    fprintf(stderr, "[test-net-stress] ok: load, churn, and kill storm held\n");
    return 0;
}
