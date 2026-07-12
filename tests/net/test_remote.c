/* test_remote.c: the N2 transparency gate's client. It runs one sequence of
 * host calls against PaymentService twice, once by loading the compiled module
 * into a local runtime and once by connecting to an ashd that serves the same
 * module, and demands the identical outcome from each. The sequence is the
 * product claim in miniature: sign with a vow override, read the vow back,
 * fulfill the validation pledges, assert the partial surface, fulfill the rest
 * to fulfilled, and break. The very same run_sequence code drives the local
 * instance and the remote proxy, so the host truly does not know which side of
 * the wire the contract lives on; the origin alone decides.
 *
 * A third phase proves the network's one new failure. It signs a fresh remote
 * instance, launches a batch of fulfillments, kills the daemon out from under
 * them, and demands that every in flight wait delivers ASH_ERR_NET, that a
 * later fulfill is a clean state error, and that a fresh connect to the dead
 * address is ASH_ERR_NET. It exits zero only when every check on both sides and
 * across the disconnect held. */

#include <ash/ash.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONTRACT "PaymentService"
#define NET_BATCH 128

static int fail(const char* where, const char* what) {
    fprintf(stderr, "[test-net2] FAIL (%s): %s\n", where, what);
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

static AshValue vfloat(double d) {
    AshValue v;
    memset(&v, 0, sizeof v);
    v.ty = ASH_TY_FLOAT;
    v.as.f = d;
    return v;
}

static AshValue vbool(int b) {
    AshValue v;
    memset(&v, 0, sizeof v);
    v.ty = ASH_TY_BOOL;
    v.as.b = b ? 1 : 0;
    return v;
}

/* Demands an Ok(true), the shape every pledge in this module returns. */
static int check_ok_true(const AshValue* out) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* in = (const AshValue*)out->as.box;
    return in && in->ty == ASH_TY_BOOL && in->as.b == 1;
}

/* The one sequence, run against a local instance and a remote proxy alike. The
 * label names the side only for a diagnostic; the calls are byte for byte the
 * same, which is the whole point. */
static int run_sequence(AshRuntime* rt, const char* label) {
    AshVowBinding cur;
    cur.name = "currency";
    cur.value = vstr("EUR");

    AshContract* c = NULL;
    if (ash_contract_sign(rt, CONTRACT, &cur, 1, 0, &c) != ASH_OK) {
        return fail(label, "sign PaymentService with a currency override");
    }
    if (ash_contract_state(c) != ASH_SIGNED) {
        return fail(label, "state after sign");
    }
    if (ash_contract_hash(c) == 0 || ash_contract_signed_at(c) <= 0) {
        return fail(label, "signature read back empty");
    }
    /* The override crossed and reads back as a local vow. */
    const AshValue* cv = ash_vow_ref(c, "currency");
    if (!cv || cv->ty != ASH_TY_STRING || cv->as.s.len != 3 ||
        memcmp(cv->as.s.ptr, "EUR", 3) != 0) {
        return fail(label, "currency vow did not read back as the override");
    }

    AshValue out;
    AshValue card = vstr("4111111111111111");
    AshValue amount = vfloat(42.0);

    if (ash_pledge_fulfill_sync(c, "validate_card", &card, 1, NULL, 0, &out) !=
            ASH_OK ||
        !check_ok_true(&out)) {
        return fail(label, "fulfill validate_card");
    }
    if (ash_pledge_fulfill_sync(c, "validate_amount", &amount, 1, NULL, 0,
                                &out) != ASH_OK ||
        !check_ok_true(&out)) {
        return fail(label, "fulfill validate_amount");
    }

    /* Validation is complete, Processing and notify_user are not: the policy's
     * partial line fires, and the partial surface names the item that landed. */
    if (ash_contract_state(c) != ASH_PARTIAL) {
        return fail(label, "state after the validation pledges");
    }
    if (ash_partial_count(c, ASH_ITEM_FULFILLED) != 1) {
        return fail(label, "one item should read fulfilled");
    }
    const char* done = ash_partial_name(c, ASH_ITEM_FULFILLED, 0);
    if (!done || strcmp(done, "Validation") != 0) {
        return fail(label, "the fulfilled item should be Validation");
    }
    if (ash_partial_count(c, ASH_ITEM_PENDING) != 2) {
        return fail(label, "Processing and notify_user should read pending");
    }
    if (ash_partial_nerrors(c) != 0) {
        return fail(label, "no pledge should have broken");
    }

    AshValue charge_args[2];
    charge_args[0] = vstr("4111111111111111");
    charge_args[1] = vfloat(42.0);
    if (ash_pledge_fulfill_sync(c, "charge", charge_args, 2, NULL, 0, &out) !=
            ASH_OK ||
        !check_ok_true(&out)) {
        return fail(label, "fulfill charge");
    }
    AshValue ok = vbool(1);
    if (ash_pledge_fulfill_sync(c, "notify_user", &ok, 1, NULL, 0, &out) !=
            ASH_OK ||
        !check_ok_true(&out)) {
        return fail(label, "fulfill notify_user");
    }
    if (ash_contract_state(c) != ASH_FULFILLED) {
        return fail(label, "state after every pledge landed");
    }

    if (ash_contract_break(c) != ASH_OK) {
        return fail(label, "break");
    }
    if (ash_contract_state(c) != ASH_BROKEN) {
        return fail(label, "state after break");
    }
    if (ash_pledge_fulfill_sync(c, "validate_card", &card, 1, NULL, 0, &out) !=
        ASH_ERR_STATE) {
        return fail(label, "fulfill after break should be ASH_ERR_STATE");
    }
    return 0;
}

/* The partial-read versus break race. One thread hammers the partial surface of
 * a remote proxy, which decodes an Err payload and copies item names onto the
 * proxy heap; another breaks the proxy, which reclaims that heap. Before the
 * lock discipline was restored these reads allocated and wrote outside the
 * instance lock, so a break freeing the heap under a write was a use after
 * free. The reader only checks each call's status, never dereferencing a
 * returned pointer, so the one thing under test is the safety of the c owned
 * write, not the ownership of what it returns. */
typedef struct RaceArg {
    AshContract* c;
    int          fails;
} RaceArg;

static void* race_reader(void* p) {
    RaceArg* a = (RaceArg*)p;
    for (int i = 0; i < 600; i++) {
        const char* nm = NULL;
        const AshValue* ev = NULL;
        AshStatus st = ash_partial_error(a->c, 0, &nm, &ev);
        /* Only Ok or a clean name miss; never a torn status. */
        if (st != ASH_OK && st != ASH_ERR_NAME && st != ASH_ERR_OOM) a->fails++;
        (void)ash_partial_name(a->c, ASH_ITEM_BROKEN, 0);
        (void)ash_partial_count(a->c, ASH_ITEM_FULFILLED);
        (void)ash_contract_state(a->c);
    }
    return NULL;
}

static void* race_breaker(void* p) {
    RaceArg* a = (RaceArg*)p;
    ash_contract_break(a->c);
    return NULL;
}

static int run_race(const char* addr, const char* token) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return fail("race", "init");
    if (ash_runtime_connect(rt, addr, token) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("race", "connect");
    }
    AshContract* c = NULL;
    if (ash_contract_sign(rt, CONTRACT, NULL, 0, 0, &c) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("race", "sign");
    }
    /* Break validate_amount so the partial surface carries a real Err payload
     * for the reader to decode onto the proxy every iteration. */
    AshValue out;
    AshValue bad = vfloat(-1.0);
    if (ash_pledge_fulfill_sync(c, "validate_amount", &bad, 1, NULL, 0, &out) !=
        ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("race", "fulfill the failing amount");
    }
    if (ash_partial_nerrors(c) != 1) {
        ash_runtime_shutdown(rt);
        return fail("race", "expected one broken pledge before the race");
    }

    RaceArg a = { c, 0 };
    pthread_t reader, breaker;
    if (pthread_create(&reader, NULL, race_reader, &a) != 0 ||
        pthread_create(&breaker, NULL, race_breaker, &a) != 0) {
        ash_runtime_shutdown(rt);
        return fail("race", "spawn the racers");
    }
    pthread_join(reader, NULL);
    pthread_join(breaker, NULL);
    if (a.fails) {
        ash_runtime_shutdown(rt);
        return fail("race", "a partial read returned a torn status");
    }
    if (ash_contract_state(c) != ASH_BROKEN) {
        ash_runtime_shutdown(rt);
        return fail("race", "state after the break");
    }
    ash_runtime_shutdown(rt);
    return 0;
}

/* The disconnect phase: sign, launch a batch of fulfillments, kill the daemon
 * mid flight, and demand ASH_ERR_NET on the in flight waits. */
static int run_disconnect(const char* addr, const char* token, pid_t pid) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return fail("disconnect", "init");
    if (ash_runtime_connect(rt, addr, token) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("disconnect", "connect");
    }
    AshContract* c = NULL;
    if (ash_contract_sign(rt, CONTRACT, NULL, 0, 0, &c) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("disconnect", "sign");
    }

    AshValue card = vstr("4111111111111111");
    AshFuture* fs[NET_BATCH];
    for (int i = 0; i < NET_BATCH; i++) {
        fs[i] = ash_pledge_fulfill(c, "validate_card", &card, 1, NULL, 0);
        if (!fs[i]) {
            ash_runtime_shutdown(rt);
            return fail("disconnect", "launch a fulfillment");
        }
    }

    /* Sever the daemon while those fulfillments are in flight. */
    kill(pid, SIGKILL);

    int nnet = 0, nok = 0;
    for (int i = 0; i < NET_BATCH; i++) {
        AshValue out;
        AshStatus st = ash_future_wait(fs[i], &out);
        if (st == ASH_ERR_NET) {
            nnet++;
        } else if (st == ASH_OK && check_ok_true(&out)) {
            nok++;
        } else {
            ash_runtime_shutdown(rt);
            return fail("disconnect", "an in flight wait was neither Ok nor NET");
        }
    }
    /* At least one fulfillment was still in flight when the peer vanished, and
     * its wait delivered the network's one new status. */
    if (nnet == 0) {
        ash_runtime_shutdown(rt);
        return fail("disconnect", "no in flight wait saw ASH_ERR_NET");
    }
    fprintf(stderr, "[test-net2] disconnect: %d Ok, %d ASH_ERR_NET\n", nok, nnet);

    /* The proxy has latched Broken, so a later fulfill is a local state error
     * and a state read is Broken without touching the wire. */
    AshValue out;
    AshStatus later = ash_pledge_fulfill_sync(c, "validate_card", &card, 1, NULL,
                                              0, &out);
    if (later != ASH_ERR_STATE && later != ASH_ERR_NET) {
        ash_runtime_shutdown(rt);
        return fail("disconnect", "a fulfill after the death was not clean");
    }
    if (ash_contract_state(c) != ASH_BROKEN) {
        ash_runtime_shutdown(rt);
        return fail("disconnect", "state after the death should be Broken");
    }

    /* A fresh connect to the now dead address is a network failure. */
    if (ash_runtime_connect(rt, addr, token) != ASH_ERR_NET) {
        ash_runtime_shutdown(rt);
        return fail("disconnect", "connect to the dead daemon was not NET");
    }
    ash_runtime_shutdown(rt);
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
    const char* module = argv[3];
    pid_t pid = (pid_t)strtol(argv[4], NULL, 10);

    /* ---- local: the module loaded into this process ---- */
    AshRuntime* rl = NULL;
    if (ash_runtime_init(NULL, &rl) != ASH_OK) return fail("local", "init");
    if (ash_module_load(rl, module) != ASH_OK) {
        ash_runtime_shutdown(rl);
        return fail("local", "module load");
    }
    if (run_sequence(rl, "local") != 0) {
        ash_runtime_shutdown(rl);
        return 1;
    }
    ash_runtime_shutdown(rl);

    /* ---- remote: the same module served by ashd over the wire ---- */
    AshRuntime* rr = NULL;
    if (ash_runtime_init(NULL, &rr) != ASH_OK) return fail("remote", "init");
    if (ash_runtime_connect(rr, addr, token) != ASH_OK) {
        ash_runtime_shutdown(rr);
        return fail("remote", "connect");
    }
    if (run_sequence(rr, "remote") != 0) {
        ash_runtime_shutdown(rr);
        return 1;
    }
    ash_runtime_shutdown(rr);

    /* ---- the partial-read versus break race, daemon still alive ---- */
    if (run_race(addr, token) != 0) return 1;

    /* ---- the disconnect, which also kills the daemon; run it last ---- */
    if (run_disconnect(addr, token, pid) != 0) return 1;

    fprintf(stderr, "[test-net2] client ok: local and remote agreed\n");
    return 0;
}
