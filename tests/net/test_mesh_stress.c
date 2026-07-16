/* test_mesh_stress.c: the B3 N node mesh gate. Three nodes in one process, three
 * runtimes and three servers on three ports, each a provider and a consumer at
 * once: node A serves Greeter, node B serves PaymentService, node C serves
 * Calculator, and each node connects to the other two, so the six directed edges
 * carry a full three node mesh out of six one directional Layer 2 connections.
 * Every node's table then holds its own contract beside two remote origins, and a
 * sign by plain name resolves one hop to the owner with no relay and no routing.
 *
 * The three contracts are named apart on purpose, and their answers have three
 * different shapes, a String, a Bool, and an Int, so a misrouted sign cannot pass
 * unnoticed: it would land another node's contract and read back the wrong shape.
 * Correctness across the storm is therefore the routing proof, each result checked
 * against the one its true owner computes.
 *
 * Three phases, in order:
 *
 *   1. storm    every edge under a batch of worker threads, each signing its own
 *               proxy and driving a run of fulfillments, so all six edges run at
 *               once and every node's serve loop, its two connection threads for
 *               the peers it serves, and its two client readers for the peers it
 *               consumes all contend on the one runtime. This is the surface TSan
 *               watches: the serve side and the connect side of three nodes at
 *               once. Every outcome must be its owner's exact answer.
 *
 *   2. kill     a node dropped mid mesh. Node C's peers launch a batch of slow
 *               fulfillments to it and C's server is stopped out from under them;
 *               every in flight wait on the two edges into C must deliver
 *               ASH_ERR_NET or a clean Ok and at least one must see ASH_ERR_NET,
 *               the dropped proxy must latch Broken, and the edges that never
 *               touched C, A to B and B to A, must keep delivering. The Layer 2
 *               disconnect semantics hold per edge.
 *
 *   3. memflat  a serving node absorbs a connect and drop loop, a peer signing,
 *               fulfilling, breaking, and disconnecting over and over, and keeps
 *               serving a fresh peer after. The whole gate builds under ASan and
 *               LSan, so a per connection or per fulfillment leak across the churn
 *               fails the run, the instance reclaim proof from Layer 2 carried
 *               onto an embedded server.
 *
 * A hard alarm bounds the whole run, so a teardown that ever deadlocks fails the
 * gate loudly rather than parking a CI. The modules arrive as argv, the Greeter,
 * the PaymentService, and the Calculator library in that order. */

#include <ash/ash.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Loopback addresses distinct from every other net gate's port, one triple per
 * phase so a phase never rebinds a port a prior phase is still tearing down, and
 * a token per node, each edge presenting the one its peer serves. */
#define ADDR_SA "127.0.0.1:8481"
#define ADDR_SB "127.0.0.1:8482"
#define ADDR_SC "127.0.0.1:8483"
#define ADDR_KA "127.0.0.1:8484"
#define ADDR_KB "127.0.0.1:8485"
#define ADDR_KC "127.0.0.1:8486"
#define ADDR_M  "127.0.0.1:8487"
#define TOKEN_A "mesh-stress-token-a"
#define TOKEN_B "mesh-stress-token-b"
#define TOKEN_C "mesh-stress-token-c"

/* Worker threads per directed edge and fulfillments per worker. Four edges touch
 * every node, two it serves and two it consumes, so a node absorbs 4*EDGE_WORKERS
 * signs across the storm, far under the instance cap a break does not reclaim, and
 * the fulfillment count is a real storm the sanitizers chew on. */
#define EDGE_WORKERS 6
#define EDGE_FULFILLS 100

/* The kill phase: in flight slow fulfillments launched per edge into the victim
 * before its server is stopped. Slow is a bound body that dawdles, so the batch is
 * still outstanding when the drop lands. */
#define KILL_INFLIGHT 24

/* The memflat phase: connect and drop cycles a serving node must absorb, bounded
 * under the instance cap since a broken instance holds its slot until shutdown. */
#define CHURN_CYCLES 64

#define ALARM_SECS 120

static int fail(const char* where, const char* what) {
    fprintf(stderr, "[test-mesh-stress] FAIL (%s): %s\n", where, what);
    return 1;
}

/* A hang anywhere below trips this: the gate fails rather than parking a CI. */
static void on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "[test-mesh-stress] FAIL: timed out, a teardown hung\n";
    ssize_t n = write(2, msg, sizeof msg - 1);
    (void)n;
    _exit(3);
}

/* ---- value helpers ---- */

static AshValue str_arg(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof v);
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static AshValue int_arg(int64_t n) {
    AshValue v;
    memset(&v, 0, sizeof v);
    v.ty = ASH_TY_INT;
    v.as.i = n;
    return v;
}

static int check_ok_string(const AshValue* out, const char* want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* in = (const AshValue*)out->as.box;
    if (!in || in->ty != ASH_TY_STRING) return 0;
    if (in->as.s.len != strlen(want)) return 0;
    return memcmp(in->as.s.ptr, want, in->as.s.len) == 0;
}

static int check_ok_true(const AshValue* out) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* in = (const AshValue*)out->as.box;
    return in && in->ty == ASH_TY_BOOL && in->as.b == 1;
}

static int check_ok_int(const AshValue* out, int64_t want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* in = (const AshValue*)out->as.box;
    return in && in->ty == ASH_TY_INT && in->as.i == want;
}

/* ---- provider bodies ---- */

/* Greeter.shout, the abstract pledge node A binds before it can serve Greeter. It
 * is never fulfilled by this gate; a bound body is only what makes a peer's sign
 * of Greeter legal, since a sign resolves every pledge to a body or a binding. */
static AshStatus host_shout(void* ctx, const AshValue* args, size_t nargs,
                            AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 1 || args[0].ty != ASH_TY_STRING) return ASH_ERR_TYPE;
    AshValue up = ash_string_copy(c, args[0].as.s.ptr, args[0].as.s.len);
    if (args[0].as.s.len && !up.as.s.ptr) return ASH_ERR_OOM;
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    *box = up;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

/* Calculator.slow, the abstract pledge node C binds. It doubles its argument like
 * double does but dawdles first, so a fulfillment of slow is still outstanding on
 * the wire when C's server is dropped, which is how the kill phase holds an in
 * flight wait across the drop. */
static AshStatus host_slow(void* ctx, const AshValue* args, size_t nargs,
                           AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 1 || args[0].ty != ASH_TY_INT) return ASH_ERR_TYPE;
    usleep(2000);
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    *box = int_arg(args[0].as.i * 2);
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

/* ---- per edge drivers ---- */

/* Each driver signs its peer's contract by plain name, so the origin routes the
 * sign one hop to the owner, drives a run of fulfillments against it checking each
 * result is the owner's exact answer, and breaks the proxy. A wrong owner would
 * refuse the pledge or return the wrong shape, so a green run is the routing proof.
 * The expected hash is zero, so a sign takes whatever the owner serves. */
static int drive_greet(AshRuntime* rt, int iters) {
    AshContract* c = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, 0, &c) != ASH_OK) return -1;
    int rc = 0;
    for (int i = 0; i < iters; i++) {
        AshValue name = str_arg("world");
        AshValue out;
        memset(&out, 0, sizeof out);
        if (ash_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &out) != ASH_OK ||
            !check_ok_string(&out, "hello, world")) {
            rc = -1;
            break;
        }
    }
    if (ash_contract_break(c) != ASH_OK && rc == 0) rc = -1;
    return rc;
}

static int drive_payment(AshRuntime* rt, int iters) {
    AshContract* c = NULL;
    if (ash_contract_sign(rt, "PaymentService", NULL, 0, 0, &c) != ASH_OK) return -1;
    int rc = 0;
    for (int i = 0; i < iters; i++) {
        AshValue card = str_arg("4111111111111111");
        AshValue out;
        memset(&out, 0, sizeof out);
        if (ash_pledge_fulfill_sync(c, "validate_card", &card, 1, NULL, 0, &out) !=
                ASH_OK ||
            !check_ok_true(&out)) {
            rc = -1;
            break;
        }
    }
    if (ash_contract_break(c) != ASH_OK && rc == 0) rc = -1;
    return rc;
}

static int drive_calc(AshRuntime* rt, int iters) {
    AshContract* c = NULL;
    if (ash_contract_sign(rt, "Calculator", NULL, 0, 0, &c) != ASH_OK) return -1;
    int rc = 0;
    for (int i = 0; i < iters; i++) {
        AshValue n = int_arg(21);
        AshValue out;
        memset(&out, 0, sizeof out);
        if (ash_pledge_fulfill_sync(c, "double", &n, 1, NULL, 0, &out) != ASH_OK ||
            !check_ok_int(&out, 42)) {
            rc = -1;
            break;
        }
    }
    if (ash_contract_break(c) != ASH_OK && rc == 0) rc = -1;
    return rc;
}

/* ---- the mesh ---- */

typedef struct Mesh {
    AshRuntime* rtA;
    AshRuntime* rtB;
    AshRuntime* rtC;
    AshServer*  srvA;
    AshServer*  srvB;
    AshServer*  srvC;
} Mesh;

/* Stands three nodes up and wires them into a mesh. Every node serves before any
 * node connects: the three serves run in order on this thread, and serve binds and
 * listens before it returns, so once the third serve returns all three listeners
 * are up and the connects that follow cannot race a listen. That ordering is the
 * barrier the design demands, and it is also B1's law, a node serves before it
 * connects, since a serving node keeps its consume side open past its own freeze
 * while a bare frozen client's connect is refused. Returns 0, or -1 with whatever
 * was built handed back through m for teardown. */
static int mesh_up(Mesh* m, const char* mod_greet, const char* mod_pay,
                   const char* mod_calc, const char* addr_a, const char* addr_b,
                   const char* addr_c) {
    memset(m, 0, sizeof *m);

    if (ash_runtime_init(NULL, &m->rtA) != ASH_OK) return -1;
    if (ash_module_load(m->rtA, mod_greet) != ASH_OK) return -1;
    if (ash_pledge_bind(m->rtA, "Greeter.shout", host_shout) != ASH_OK) return -1;
    if (ash_runtime_serve(m->rtA, addr_a, TOKEN_A, &m->srvA) != ASH_OK) return -1;

    if (ash_runtime_init(NULL, &m->rtB) != ASH_OK) return -1;
    if (ash_module_load(m->rtB, mod_pay) != ASH_OK) return -1;
    if (ash_runtime_serve(m->rtB, addr_b, TOKEN_B, &m->srvB) != ASH_OK) return -1;

    if (ash_runtime_init(NULL, &m->rtC) != ASH_OK) return -1;
    if (ash_module_load(m->rtC, mod_calc) != ASH_OK) return -1;
    if (ash_pledge_bind(m->rtC, "Calculator.slow", host_slow) != ASH_OK) return -1;
    if (ash_runtime_serve(m->rtC, addr_c, TOKEN_C, &m->srvC) != ASH_OK) return -1;

    /* All three are serving; open the six edges. Each node connects to the two
     * peers whose contracts it consumes, past its own freeze, which its serving
     * consume side allows. */
    if (ash_runtime_connect(m->rtA, addr_b, TOKEN_B) != ASH_OK) return -1; /* A->B */
    if (ash_runtime_connect(m->rtA, addr_c, TOKEN_C) != ASH_OK) return -1; /* A->C */
    if (ash_runtime_connect(m->rtB, addr_a, TOKEN_A) != ASH_OK) return -1; /* B->A */
    if (ash_runtime_connect(m->rtB, addr_c, TOKEN_C) != ASH_OK) return -1; /* B->C */
    if (ash_runtime_connect(m->rtC, addr_a, TOKEN_A) != ASH_OK) return -1; /* C->A */
    if (ash_runtime_connect(m->rtC, addr_b, TOKEN_B) != ASH_OK) return -1; /* C->B */
    return 0;
}

/* Reaps the mesh in reverse: stop every live server so its accept loop and
 * connection threads join and its served instances break, then shut every runtime
 * down so its pool joins and its client readers join, one node's stop waking
 * another's reader on the closed socket. Safe on a partial mesh_up, since every
 * handle is NULL until it is built. */
static void mesh_down(Mesh* m) {
    if (m->srvA) ash_server_stop(m->srvA);
    if (m->srvB) ash_server_stop(m->srvB);
    if (m->srvC) ash_server_stop(m->srvC);
    if (m->rtA) ash_runtime_shutdown(m->rtA);
    if (m->rtB) ash_runtime_shutdown(m->rtB);
    if (m->rtC) ash_runtime_shutdown(m->rtC);
}

/* ---- phase 1: the storm ---- */

typedef struct EdgeArg {
    AshRuntime* rt;
    int (*drive)(AshRuntime*, int);
    int iters;
    int fails;
} EdgeArg;

static void* edge_main(void* p) {
    EdgeArg* e = (EdgeArg*)p;
    if (e->drive(e->rt, e->iters) != 0) e->fails++;
    return NULL;
}

static int phase_storm(const char* mod_greet, const char* mod_pay,
                       const char* mod_calc) {
    Mesh m;
    if (mesh_up(&m, mod_greet, mod_pay, mod_calc, ADDR_SA, ADDR_SB, ADDR_SC) != 0) {
        mesh_down(&m);
        return fail("storm", "bring the mesh up");
    }

    /* The six edges: each consumer node against each of its two peers. A consumes
     * B's PaymentService and C's Calculator; B consumes A's Greeter and C's
     * Calculator; C consumes A's Greeter and B's PaymentService. */
    struct { AshRuntime* rt; int (*drive)(AshRuntime*, int); } edges[6] = {
        { m.rtA, drive_payment }, /* A -> B */
        { m.rtA, drive_calc },    /* A -> C */
        { m.rtB, drive_greet },   /* B -> A */
        { m.rtB, drive_calc },    /* B -> C */
        { m.rtC, drive_greet },   /* C -> A */
        { m.rtC, drive_payment }, /* C -> B */
    };

    EdgeArg args[6 * EDGE_WORKERS];
    pthread_t ths[6 * EDGE_WORKERS];
    int spawned = 0;
    int rc = 0;
    for (int e = 0; e < 6; e++) {
        for (int w = 0; w < EDGE_WORKERS; w++) {
            EdgeArg* a = &args[spawned];
            a->rt = edges[e].rt;
            a->drive = edges[e].drive;
            a->iters = EDGE_FULFILLS;
            a->fails = 0;
            if (pthread_create(&ths[spawned], NULL, edge_main, a) != 0) {
                rc = fail("storm", "spawn an edge worker");
                break;
            }
            spawned++;
        }
        if (rc != 0) break;
    }
    int fails = 0;
    for (int i = 0; i < spawned; i++) {
        pthread_join(ths[i], NULL);
        fails += args[i].fails;
    }
    if (fails && rc == 0) {
        rc = fail("storm", "an edge did not deliver its owner's answer");
    }

    mesh_down(&m);
    if (rc == 0) {
        fprintf(stderr,
                "[test-mesh-stress] storm: 6 edges x %d workers x %d fulfills ok\n",
                EDGE_WORKERS, EDGE_FULFILLS);
    }
    return rc;
}

/* ---- phase 2: the kill ---- */

static int phase_kill(const char* mod_greet, const char* mod_pay,
                      const char* mod_calc) {
    Mesh m;
    if (mesh_up(&m, mod_greet, mod_pay, mod_calc, ADDR_KA, ADDR_KB, ADDR_KC) != 0) {
        mesh_down(&m);
        return fail("kill", "bring the mesh up");
    }

    int rc = 0;
    AshContract* ca = NULL; /* A's proxy to C's Calculator */
    AshContract* cb = NULL; /* B's proxy to C's Calculator */
    AshFuture* fa[KILL_INFLIGHT];
    AshFuture* fb[KILL_INFLIGHT];
    memset(fa, 0, sizeof fa);
    memset(fb, 0, sizeof fb);

    if (ash_contract_sign(m.rtA, "Calculator", NULL, 0, 0, &ca) != ASH_OK ||
        ash_contract_sign(m.rtB, "Calculator", NULL, 0, 0, &cb) != ASH_OK) {
        rc = fail("kill", "sign the victim's contract");
    }

    /* Launch a batch of slow fulfillments on both edges into C, still outstanding
     * because slow dawdles, then stop C's server out from under them. */
    if (rc == 0) {
        for (int i = 0; i < KILL_INFLIGHT; i++) {
            AshValue na = int_arg(7);
            AshValue nb = int_arg(9);
            fa[i] = ash_pledge_fulfill(ca, "slow", &na, 1, NULL, 0);
            fb[i] = ash_pledge_fulfill(cb, "slow", &nb, 1, NULL, 0);
            if (!fa[i] || !fb[i]) {
                rc = fail("kill", "launch an in flight fulfillment");
                break;
            }
        }
    }

    if (rc == 0) {
        ash_server_stop(m.srvC);
        m.srvC = NULL; /* dropped; teardown must not stop it twice */

        /* Every in flight wait on the two dead edges is the owner's Ok or the
         * ASH_ERR_NET the drop delivers, never a wrong value and never a hang, and
         * at least one must see the disconnect. */
        int net = 0;
        for (int i = 0; i < KILL_INFLIGHT; i++) {
            AshValue oa, ob;
            AshStatus sa = ash_future_wait(fa[i], &oa);
            AshStatus sb = ash_future_wait(fb[i], &ob);
            if (sa == ASH_ERR_NET) net++;
            else if (sa != ASH_OK || !check_ok_int(&oa, 14))
                rc = fail("kill", "an in flight wait to C was neither Ok nor NET");
            if (sb == ASH_ERR_NET) net++;
            else if (sb != ASH_OK || !check_ok_int(&ob, 18))
                rc = fail("kill", "an in flight wait to C was neither Ok nor NET");
        }
        if (rc == 0 && net == 0) {
            rc = fail("kill", "no in flight wait to C saw ASH_ERR_NET");
        }

        /* The dropped proxy latched Broken: a later fulfill is a clean local error
         * and the state read is Broken without touching the dead wire. */
        if (rc == 0) {
            AshValue n = int_arg(21);
            AshValue out;
            AshStatus later =
                ash_pledge_fulfill_sync(ca, "double", &n, 1, NULL, 0, &out);
            if (later != ASH_ERR_STATE && later != ASH_ERR_NET) {
                rc = fail("kill", "a fulfill after the drop was not a clean error");
            } else if (ash_contract_state(ca) != ASH_BROKEN) {
                rc = fail("kill", "the dropped proxy's state was not Broken");
            }
        }

        /* The edges that never touched C stand: A to B and B to A still deliver
         * their owners' answers, the per edge disconnect semantics holding. */
        if (rc == 0 && drive_payment(m.rtA, 4) != 0) {
            rc = fail("kill", "the A to B edge did not survive C's drop");
        }
        if (rc == 0 && drive_greet(m.rtB, 4) != 0) {
            rc = fail("kill", "the B to A edge did not survive C's drop");
        }
    }

    if (ca) ash_contract_break(ca);
    if (cb) ash_contract_break(cb);
    mesh_down(&m);
    if (rc == 0) {
        fprintf(stderr,
                "[test-mesh-stress] kill: %d in flight across 2 edges into C, "
                "ASH_ERR_NET on the dead edges, A<->B held\n",
                2 * KILL_INFLIGHT);
    }
    return rc;
}

/* ---- phase 3: memflat ---- */

/* One connect and drop cycle against the serving node: a fresh runtime connects,
 * signs, fulfills, breaks, and shuts down, severing the connection under the
 * server. The server breaks the cycle's instance and reaps its thread on its own,
 * so a leak of the instance heap, the fd, or the thread would surface under LSan. */
static int churn_once(const char* addr) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return -1;
    if (ash_runtime_connect(rt, addr, TOKEN_B) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return -1;
    }
    int rc = drive_payment(rt, 1);
    ash_runtime_shutdown(rt);
    return rc;
}

static int phase_memflat(const char* mod_pay) {
    AshRuntime* rtS = NULL;
    AshServer* srvS = NULL;
    if (ash_runtime_init(NULL, &rtS) != ASH_OK) return fail("memflat", "init S");
    if (ash_module_load(rtS, mod_pay) != ASH_OK) {
        ash_runtime_shutdown(rtS);
        return fail("memflat", "load PaymentService into S");
    }
    if (ash_runtime_serve(rtS, ADDR_M, TOKEN_B, &srvS) != ASH_OK) {
        ash_runtime_shutdown(rtS);
        return fail("memflat", "serve S");
    }

    int rc = 0;
    for (int i = 0; i < CHURN_CYCLES; i++) {
        if (churn_once(ADDR_M) != 0) {
            rc = fail("memflat", "a churn cycle did not deliver");
            break;
        }
    }
    /* The server survived the churn: one more fresh peer is still served. */
    if (rc == 0 && churn_once(ADDR_M) != 0) {
        rc = fail("memflat", "the server did not survive the churn");
    }

    ash_server_stop(srvS);
    ash_runtime_shutdown(rtS);
    if (rc == 0) {
        fprintf(stderr,
                "[test-mesh-stress] memflat: server absorbed %d connect/drop "
                "cycles, no leak\n",
                CHURN_CYCLES);
    }
    return rc;
}

int main(int argc, char** argv) {
    const char* mod_greet = argc > 1 ? argv[1] : "target/ashc-out/libhello.ash.so";
    const char* mod_pay =
        argc > 2 ? argv[2] : "target/ashc-out/libnet_payment.ash.so";
    const char* mod_calc =
        argc > 3 ? argv[3] : "target/ashc-out/libmesh_calc.ash.so";

    /* A serving host owns the SIGPIPE policy the library leaves to it, so a write
     * to a peer that left is a failed write, not a killed process. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, on_alarm);
    alarm(ALARM_SECS);

    if (phase_storm(mod_greet, mod_pay, mod_calc) != 0) return 1;
    if (phase_kill(mod_greet, mod_pay, mod_calc) != 0) return 1;
    if (phase_memflat(mod_pay) != 0) return 1;

    fprintf(stderr, "[test-mesh-stress] ok: storm, kill, and memflat held\n");
    return 0;
}
