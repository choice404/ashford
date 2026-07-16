/* test_mesh_pair.c: the B1 symmetric node gate. It proves one runtime serves and
 * connects at once, and that two such runtimes form a full duplex mesh out of two
 * one directional edges. Node A loads Greeter, binds its abstract shout, and
 * serves it; node B loads the concrete payment module and serves it. Each node
 * then opens the edge to the other past its own freeze, which the serving node's
 * consume side allows, so A holds a proxy to B's PaymentService and B a proxy to
 * A's Greeter. Two host threads drive the two directions at once and in a loop: A
 * fulfills charge across its edge to B while B fulfills greet across its edge to
 * A, each thread signing, fulfilling, checking, and breaking its own instance
 * every pass. The directions are independent, so a stall on one is not a stall on
 * the other, and the gate demands both deliver the right Ok every pass.
 *
 * This is the coexistence B1 exists to pin: node A's serve loop runs greet on a
 * connection thread and its pool while node A's own reader thread completes a
 * charge it fulfilled as a client, both on the one runtime at the one time. The
 * ASan build watches every accept loop, connection thread, waiter, and reader for
 * a leak across the two serves, the two connects, and the two stops; the TSan
 * build watches the same threads for a race between the serve side and the
 * connect side of a single runtime, the one risk two edges on one node open. It
 * exits zero only when every pass of both directions held. */

#include <ash/ash.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Two loopback addresses distinct from every other net gate's port so a parallel
 * make does not collide, and a token per endpoint, each edge presenting the one
 * its peer serves. */
#define ADDR_A  "127.0.0.1:8478"
#define ADDR_B  "127.0.0.1:8479"
#define TOKEN_A "mesh-pair-token-a"
#define TOKEN_B "mesh-pair-token-b"

/* Enough passes per direction to give the sanitizers a real concurrent window,
 * kept under half the instance cap so the signed instances of both directions,
 * client proxies and served locals both, never exhaust one runtime's table
 * before a shutdown reclaims them. */
#define ITERS 40

static int fail(const char* what) {
    fprintf(stderr, "[test-mesh-pair] FAIL: %s\n", what);
    return 1;
}

/* Greeter.shout, the abstract pledge node A must bind before it can serve Greeter
 * to a peer. It uppercases its name argument the way the B0 gate's does; the gate
 * never fulfills it, but a bound body is what makes the sign a peer asks for
 * legal, since a sign resolves every pledge to a body or a binding. */
static AshStatus host_shout(void* ctx, const AshValue* args, size_t nargs,
                            AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 1 || args[0].ty != ASH_TY_STRING) return ASH_ERR_TYPE;
    AshValue up = ash_string_copy(c, args[0].as.s.ptr, args[0].as.s.len);
    if (args[0].as.s.len && !up.as.s.ptr) return ASH_ERR_OOM;
    for (uint64_t i = 0; i < up.as.s.len; i++) {
        uint8_t ch = up.as.s.ptr[i];
        if (ch >= 'a' && ch <= 'z') up.as.s.ptr[i] = (uint8_t)(ch - 32);
    }
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    *box = up;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

static AshValue str_arg(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static int check_ok_string(const AshValue* out, const char* want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* inner = (const AshValue*)out->as.box;
    if (!inner || inner->ty != ASH_TY_STRING) return 0;
    if (inner->as.s.len != strlen(want)) return 0;
    return memcmp(inner->as.s.ptr, want, inner->as.s.len) == 0;
}

static int check_ok_bool(const AshValue* out, int want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* inner = (const AshValue*)out->as.box;
    if (!inner || inner->ty != ASH_TY_BOOL) return 0;
    return (inner->as.b != 0) == (want != 0);
}

/* One pass of the Greeter direction on the runtime that consumes it: sign the
 * remote contract by plain name so the origin routes the sign to its owner, drive
 * greet across the wire, demand the same greeting the local host gate pins, and
 * break the proxy. The expected hash is zero, so the sign takes whatever the owner
 * serves rather than pinning a compiled constant the gate would carry by hand. */
static int drive_greet(AshRuntime* rt) {
    AshContract* c = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, 0, &c) != ASH_OK) return -1;
    AshValue name = str_arg("world");
    AshValue out;
    memset(&out, 0, sizeof out);
    int rc = 0;
    if (ash_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &out) != ASH_OK) {
        rc = -1;
    } else if (!check_ok_string(&out, "hello, world")) {
        rc = -1;
    }
    if (ash_contract_break(c) != ASH_OK && rc == 0) rc = -1;
    return rc;
}

/* One pass of the PaymentService direction on the runtime that consumes it: sign
 * the remote contract, fulfill charge with a card and a positive amount so the
 * owner's concrete body lands Ok(true), check it, and break the proxy. Two args, a
 * string and a float, cross the wire, the same values the N2 gate drives, so the
 * direction exercises a real argument frame and not a bare call. */
static int drive_charge(AshRuntime* rt) {
    AshContract* c = NULL;
    if (ash_contract_sign(rt, "PaymentService", NULL, 0, 0, &c) != ASH_OK) return -1;
    AshValue args[2];
    args[0] = str_arg("4111111111111111");
    memset(&args[1], 0, sizeof args[1]);
    args[1].ty = ASH_TY_FLOAT;
    args[1].as.f = 42.0;
    AshValue out;
    memset(&out, 0, sizeof out);
    int rc = 0;
    if (ash_pledge_fulfill_sync(c, "charge", args, 2, NULL, 0, &out) != ASH_OK) {
        rc = -1;
    } else if (!check_ok_bool(&out, 1)) {
        rc = -1;
    }
    if (ash_contract_break(c) != ASH_OK && rc == 0) rc = -1;
    return rc;
}

/* One direction's driver: run its pass fn against its runtime ITERS times and
 * count the failures, so a single bad pass fails the gate without unwinding the
 * loop the other thread is still running. */
typedef struct DriveArg {
    AshRuntime* rt;
    int (*pass)(AshRuntime*);
    int fails;
} DriveArg;

static void* drive_main(void* p) {
    DriveArg* d = (DriveArg*)p;
    for (int i = 0; i < ITERS; i++) {
        if (d->pass(d->rt) != 0) d->fails++;
    }
    return NULL;
}

int main(int argc, char** argv) {
    const char* mod_greet =
        argc > 1 ? argv[1] : "target/ashc-out/libhello.ash.so";
    const char* mod_pay =
        argc > 2 ? argv[2] : "target/ashc-out/libnet_payment.ash.so";

    /* A serving host owns the SIGPIPE policy the library leaves to it, so a write
     * to a peer that left is a failed write, not a killed process. */
    signal(SIGPIPE, SIG_IGN);

    /* ---- node A: serves Greeter, will consume PaymentService ---- */

    AshRuntime* rtA = NULL;
    if (ash_runtime_init(NULL, &rtA) != ASH_OK) return fail("A init");
    if (ash_module_load(rtA, mod_greet) != ASH_OK) {
        ash_runtime_shutdown(rtA);
        return fail("load Greeter into A");
    }
    if (ash_pledge_bind(rtA, "Greeter.shout", host_shout) != ASH_OK) {
        ash_runtime_shutdown(rtA);
        return fail("bind Greeter.shout on A");
    }
    AshServer* srvA = NULL;
    if (ash_runtime_serve(rtA, ADDR_A, TOKEN_A, &srvA) != ASH_OK) {
        ash_runtime_shutdown(rtA);
        return fail("serve A");
    }

    /* ---- node B: serves PaymentService, will consume Greeter ---- */

    AshRuntime* rtB = NULL;
    if (ash_runtime_init(NULL, &rtB) != ASH_OK) {
        ash_server_stop(srvA);
        ash_runtime_shutdown(rtA);
        return fail("B init");
    }
    if (ash_module_load(rtB, mod_pay) != ASH_OK) {
        ash_runtime_shutdown(rtB);
        ash_server_stop(srvA);
        ash_runtime_shutdown(rtA);
        return fail("load PaymentService into B");
    }
    AshServer* srvB = NULL;
    if (ash_runtime_serve(rtB, ADDR_B, TOKEN_B, &srvB) != ASH_OK) {
        ash_runtime_shutdown(rtB);
        ash_server_stop(srvA);
        ash_runtime_shutdown(rtA);
        return fail("serve B");
    }

    /* ---- the two edges, opened after both nodes serve ---- */

    /* Both runtimes are frozen by their serve, and both connect anyway because a
     * serving node keeps its consume side open: A opens the edge to B's
     * PaymentService, B the edge to A's Greeter. Bringing the servers up first and
     * the edges up second is what lets a mutual pair bootstrap at all, since each
     * connect wants its peer already serving. Each side wires its one peer through
     * ash_runtime_connect_all, the config array convenience, a single element list
     * here that still exercises the loop and its first failure return. */
    int rc = 0;
    const char* peers_a[1] = { ADDR_B };
    const char* toks_a[1]  = { TOKEN_B };
    const char* peers_b[1] = { ADDR_A };
    const char* toks_b[1]  = { TOKEN_A };
    if (ash_runtime_connect_all(rtA, peers_a, toks_a, 1) != ASH_OK) {
        rc = fail("A connect to B");
    } else if (ash_runtime_connect_all(rtB, peers_b, toks_b, 1) != ASH_OK) {
        rc = fail("B connect to A");
    }

    /* ---- both directions at once ---- */

    if (rc == 0) {
        DriveArg da = { rtA, drive_charge, 0 }; /* A consumes B's PaymentService */
        DriveArg db = { rtB, drive_greet, 0 };  /* B consumes A's Greeter */
        pthread_t ta, tb;
        int sa = pthread_create(&ta, NULL, drive_main, &da);
        int sb = pthread_create(&tb, NULL, drive_main, &db);
        if (sa != 0 || sb != 0) {
            if (sa == 0) pthread_join(ta, NULL);
            if (sb == 0) pthread_join(tb, NULL);
            rc = fail("spawn the direction drivers");
        } else {
            pthread_join(ta, NULL);
            pthread_join(tb, NULL);
            if (da.fails) rc = fail("charge direction A to B");
            if (db.fails && rc == 0) rc = fail("greet direction B to A");
        }
    }

    /* Teardown: stop both servers so their accept loops and connection threads
     * join and every served instance breaks, then shut both runtimes down so the
     * pools join and the client edges' readers join, one runtime's stop waking the
     * other's reader on the closed socket. */
    ash_server_stop(srvA);
    ash_server_stop(srvB);
    ash_runtime_shutdown(rtA);
    ash_runtime_shutdown(rtB);

    if (rc == 0) fprintf(stderr, "[test-mesh-pair] both directions ok\n");
    return rc;
}
