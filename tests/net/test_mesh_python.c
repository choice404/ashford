/* test_mesh_python.c: the B2 mesh gate's consumer, the C half of the foreign
 * language provider proof. A Python host stands up a mesh node that binds a
 * Python function over PaymentService.charge and serves it on loopback; this C
 * client connects, signs PaymentService by plain name so the origin routes the
 * sign to its owner, fulfills charge across the wire, and demands the value the
 * Python function computed. charge with a positive amount lands Ok(true); charge
 * with a bad amount lands Err(41), and 41 is a constant this file does not carry
 * anywhere but the assertion below, so a client that reads it read a number only
 * the live Python process could have produced. Two languages, two processes, one
 * contract, and the result crossing between them is computed in Python.
 *
 * A second phase proves the token guards the endpoint: a fresh runtime that dials
 * the same address with the wrong token is refused with ASH_ERR_NET before any
 * table crosses, so a bad secret buys nothing, not even the served surface. The
 * client links libashrt like any foreign host and drives the ordinary connect,
 * sign, fulfill, break surface, unaware that the body answering it is Python.
 *
 * Usage: test_mesh_python ADDR TOKEN. Exit zero only when every check held. */

#include <ash/ash.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The contract the Python node serves and the Err payload its charge answers a
 * bad amount with, the one the provider chose and this file only checks. */
#define PAYMENT_NAME "PaymentService"
#define BAD_AMOUNT   41

static int fail(const char* what) {
    fprintf(stderr, "[test-mesh-python] FAIL: %s\n", what);
    return 1;
}

static AshValue str_arg(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static AshValue float_arg(double f) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_FLOAT;
    v.as.f = f;
    return v;
}

/* Ok(want) over a Bool arm: a Result at tag 0 whose box holds the wanted bool. */
static int check_ok_bool(const AshValue* out, int want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* inner = (const AshValue*)out->as.box;
    if (!inner || inner->ty != ASH_TY_BOOL) return 0;
    return (inner->as.b != 0) == (want != 0);
}

/* Err(want) over an Int arm: a Result at tag 1 whose box holds the wanted int,
 * the Python function's sentinel arriving across the wire as the caller's Err. */
static int check_err_int(const AshValue* out, int64_t want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 1) return 0;
    const AshValue* inner = (const AshValue*)out->as.box;
    if (!inner || inner->ty != ASH_TY_INT) return 0;
    return inner->as.i == want;
}

/* The consume path: sign the Python node's PaymentService and fulfill charge
 * twice, once on a positive amount for the Ok the Python body answers and once
 * on a negative amount for its Err(41). Both bodies run in the provider's Python
 * on its pool; this client only reads the results back. */
static int drive_charge(AshRuntime* rt) {
    AshContract* c = NULL;
    if (ash_contract_sign(rt, PAYMENT_NAME, NULL, 0, 0, &c) != ASH_OK) {
        return fail("sign PaymentService across the wire");
    }

    int rc = 0;
    AshValue good[2] = { str_arg("4111 1111"), float_arg(25.0) };
    AshValue out;
    memset(&out, 0, sizeof out);
    if (ash_pledge_fulfill_sync(c, "charge", good, 2, NULL, 0, &out) != ASH_OK) {
        rc = fail("fulfill charge (good) across the wire");
    } else if (!check_ok_bool(&out, 1)) {
        rc = fail("charge good was not the Python Ok(true)");
    }

    if (rc == 0) {
        AshValue bad[2] = { str_arg("4111 1111"), float_arg(-2.0) };
        memset(&out, 0, sizeof out);
        if (ash_pledge_fulfill_sync(c, "charge", bad, 2, NULL, 0, &out) !=
            ASH_OK) {
            rc = fail("fulfill charge (bad) across the wire");
        } else if (!check_err_int(&out, BAD_AMOUNT)) {
            rc = fail("charge bad was not the Python Err(41)");
        }
    }

    if (ash_contract_break(c) != ASH_OK && rc == 0) {
        rc = fail("break the remote PaymentService instance");
    }
    return rc;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_mesh_python ADDR TOKEN\n");
        return 2;
    }
    const char* addr = argv[1];
    const char* token = argv[2];

    /* A client that outlives a peer owns its own SIGPIPE policy, so a write to a
     * node that left is a failed write and not a killed process. */
    signal(SIGPIPE, SIG_IGN);

    /* ---- the good path: connect with the right token and consume ---- */

    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return fail("client init");
    if (ash_runtime_connect(rt, addr, token) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("connect to the Python provider");
    }
    int rc = drive_charge(rt);
    ash_runtime_shutdown(rt);

    /* ---- the token guard: a wrong token is refused before any table crosses ---- */

    if (rc == 0) {
        AshRuntime* bad_rt = NULL;
        if (ash_runtime_init(NULL, &bad_rt) != ASH_OK) {
            return fail("bad-token client init");
        }
        char wrong[256];
        snprintf(wrong, sizeof wrong, "%s-wrong", token);
        AshStatus st = ash_runtime_connect(bad_rt, addr, wrong);
        if (st != ASH_ERR_NET) {
            rc = fail("wrong token was not refused with ASH_ERR_NET");
        } else if (ash_iname_count(bad_rt) != 0) {
            rc = fail("a refused connect merged a table");
        }
        ash_runtime_shutdown(bad_rt);
    }

    if (rc == 0) fprintf(stderr, "[test-mesh-python] consumer ok\n");
    return rc;
}
