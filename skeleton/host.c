/* host.c: the C half of the walking skeleton. This is a foreign program, it
 * knows nothing about geas, it only links libgeasrt and speaks the ABI header.
 * It loads the compiled module and walks the whole M4 surface: prove the
 * abstract pledge blocks signing until the host binds an implementation,
 * bind one, sign on the declared defaults and demand exactly
 * Ok("hello, world"), sign again with a vow override and demand the override
 * showed up, drive the bound pledge through a by-reference argument and
 * watch the write back land in host memory, by the default protocol and
 * through a callback, fulfill through a future and wait it exactly once,
 * prove a second wait is a state error, read the signature the instance
 * carries and prove a wrong expected hash is refused, and exercise the
 * lifecycle errors on both sides of break. M5 adds the concurrency half:
 * two host threads hammering one instance, two instances running in
 * parallel, and a break racing an in-flight fulfillment whose wait must
 * land on Ok or GEAS_ERR_STATE and nothing else. M6 makes the first sign a
 * real discovery: the host resolves greet's mangled name through the iname
 * table, signs under the contract name and shape hash the entry carries,
 * and at the end freezes the runtime and proves that binding is refused
 * while signing and fulfilling still work, and that the canonical dump is
 * non-empty and names the pledge it resolved. It exits zero only when
 * every check held. valgrind runs this and expects silence. */

#include <geas/geas.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The mangled name of Greeter.greet, hardcoded the way a foreign host ships
 * it today; the generated header that spells it for the host is M9 work. */
#define GREET_MANGLED "__geas_ash_Greeter_greet_17cef80f14421b9b_v1"

static int fail(const char* what) {
    fprintf(stderr, "[host] FAIL: %s\n", what);
    return 1;
}

/* The host implementation bound over Greeter.shout. It runs in the uniform
 * thunk frame like any compiled body: everything it builds goes through the
 * instance in ctx, and the name parameter is its own instance owned slot, so
 * shouting mutates the slot in place and the runtime's write back carries
 * the change out when the caller passed the name by reference. */
static GeasStatus host_shout(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    GeasContract* c = (GeasContract*)ctx;
    if (nargs != 1) return GEAS_ERR_TYPE;
    if (args[0].ty != GEAS_TY_STRING) return GEAS_ERR_TYPE;
    GeasValue up = geas_string_copy(c, args[0].as.s.ptr, args[0].as.s.len);
    if (args[0].as.s.len && !up.as.s.ptr) return GEAS_ERR_OOM;
    for (uint64_t i = 0; i < up.as.s.len; i++) {
        uint8_t ch = up.as.s.ptr[i];
        if (ch >= 'a' && ch <= 'z') up.as.s.ptr[i] = (uint8_t)(ch - 32);
    }
    ((GeasValue*)args)[0] = up;
    GeasValue* box = geas_box(c);
    if (!box) return GEAS_ERR_OOM;
    *box = up;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return GEAS_OK;
}

/* A write back callback: instead of letting the default point the host's
 * GeasString at instance owned bytes, copy them into a buffer the host owns,
 * reached through user, so nothing aliases instance memory after the call. */
#define SHOUT_CAP 32
static void copy_shout_out(void* host_ptr, const GeasValue* v, void* user) {
    (void)host_ptr;
    char* buf = (char*)user;
    size_t n = v->as.s.len < SHOUT_CAP - 1 ? v->as.s.len : SHOUT_CAP - 1;
    memcpy(buf, v->as.s.ptr, n);
    buf[n] = '\0';
}

static GeasValue str_arg(const char* s) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

/* Demands that a value is Ok(<want>) for a string payload. */
static int check_ok_string(const GeasValue* out, const char* want) {
    if (out->ty != GEAS_TY_RESULT || out->tag != 0) return 0;
    const GeasValue* inner = (const GeasValue*)out->as.box;
    if (!inner || inner->ty != GEAS_TY_STRING) return 0;
    if (inner->as.s.len != strlen(want)) return 0;
    return memcmp(inner->as.s.ptr, want, inner->as.s.len) == 0;
}

/* ---- the M5 concurrency workers ---- */

#define GREET_ITERS 32

/* One host thread's share of the hammering: fulfill greet synchronously
 * over and over against one instance and demand the exact greeting every
 * time. Failures are counted, never asserted mid-thread, so every thread
 * runs to completion and the main thread reports once. */
typedef struct GreetJob {
    GeasContract* c;
    const char*  want;
    int          iters;
    int          failures;
} GreetJob;

static void* greet_worker(void* arg) {
    GreetJob* job = (GreetJob*)arg;
    GeasValue name = str_arg("world");
    for (int i = 0; i < job->iters; i++) {
        GeasValue out;
        if (geas_pledge_fulfill_sync(job->c, "greet", &name, 1, NULL, 0,
                                    &out) != GEAS_OK ||
            !check_ok_string(&out, job->want)) {
            job->failures++;
        }
    }
    return NULL;
}

/* The waiter half of the break race. It waits one future and records the
 * status; the main thread breaks the instance concurrently. On GEAS_OK it
 * must not read the payload, the instance heap may already be gone. */
typedef struct WaitJob {
    GeasFuture* f;
    GeasStatus  status;
} WaitJob;

static void* wait_worker(void* arg) {
    WaitJob* job = (WaitJob*)arg;
    GeasValue out;
    job->status = geas_future_wait(job->f, &out);
    return NULL;
}

int main(void) {
    GeasRuntime* rt = NULL;
    if (geas_runtime_init(NULL, &rt) != GEAS_OK) return fail("runtime init");

    if (geas_module_load(rt, "target/geas-out/libhello.geas.so") != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("module load");
    }

    /* ---- the abstract pledge: no sign until the host binds it ---- */

    GeasContract* c0 = NULL;
    if (geas_contract_sign(rt, "Greeter", NULL, 0, 0, &c0) != GEAS_ERR_UNBOUND) {
        geas_runtime_shutdown(rt);
        return fail("sign before bind did not report GEAS_ERR_UNBOUND");
    }
    if (geas_pledge_bind(rt, "Greeter.nope", host_shout) != GEAS_ERR_NAME) {
        geas_runtime_shutdown(rt);
        return fail("binding an unknown pledge did not report GEAS_ERR_NAME");
    }
    if (geas_pledge_bind(rt, "Greeter.shout", host_shout) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("bind Greeter.shout");
    }

    /* ---- discovery: resolve the pledge's mangled name first ---- */

    /* The iname table turns the mangled name the host shipped with into the
     * owning contract and the shape hash to sign under, so the first sign is
     * a checked handshake instead of a bare string. */
    GeasInameEntry ent;
    if (geas_iname_lookup(rt, GREET_MANGLED, &ent) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("iname lookup of the greet mangled name");
    }
    if (ent.kind != GEAS_INAME_PLEDGE || strcmp(ent.contract, "Greeter") != 0 ||
        !ent.symbol || strcmp(ent.symbol, "greet") != 0 || ent.nargs != 1 ||
        ent.version != 1 || ent.shape_hash == 0) {
        geas_runtime_shutdown(rt);
        return fail("iname entry for greet carries the wrong facts");
    }
    GeasInameEntry miss;
    if (geas_iname_lookup(rt, "__geas_ash_Greeter_greet_0000000000000000_v1",
                         &miss) != GEAS_ERR_NAME) {
        geas_runtime_shutdown(rt);
        return fail("a wrong mangled name did not report GEAS_ERR_NAME");
    }

    /* ---- the default path: sign on the declared vow defaults ---- */

    GeasContract* c = NULL;
    if (geas_contract_sign(rt, ent.contract, NULL, 0, ent.shape_hash, &c) !=
        GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("sign under the discovered contract and hash");
    }
    if (geas_contract_state(c) != GEAS_SIGNED) {
        geas_runtime_shutdown(rt);
        return fail("state after sign");
    }
    if (geas_contract_hash(c) == 0) {
        geas_runtime_shutdown(rt);
        return fail("signed instance carries no shape hash");
    }
    if (geas_contract_signed_at(c) <= 0) {
        geas_runtime_shutdown(rt);
        return fail("signed instance carries no timestamp");
    }

    /* The argument is host owned; the runtime never keeps it. */
    GeasValue name = str_arg("world");

    GeasValue out;
    if (geas_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &out) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("fulfill greet");
    }
    if (!check_ok_string(&out, "hello, world")) {
        geas_runtime_shutdown(rt);
        return fail("default greeting mismatch");
    }
    const GeasValue* inner = (const GeasValue*)out.as.box;
    printf("%.*s\n", (int)inner->as.s.len, (const char*)inner->as.s.ptr);

    /* One of two pledges has latched Ok; the contract is not fulfilled yet. */
    if (geas_contract_state(c) != GEAS_SIGNED) {
        geas_runtime_shutdown(rt);
        return fail("state after a partial fulfill");
    }

    /* ---- the bound pledge, called with the name by reference ---- */

    /* The host's own storage for the argument. The runtime copies the value
     * in at fulfill, the bound body shouts its instance owned slot, and the
     * default write back repoints this struct at the shouted bytes before
     * the call returns. */
    GeasString by_ref;
    by_ref.ptr = (uint8_t*)"whisper";
    by_ref.len = 7;

    GeasRef ref;
    memset(&ref, 0, sizeof(ref));
    ref.host_ptr = &by_ref;
    ref.ty = GEAS_TY_STRING;

    if (geas_pledge_fulfill_sync(c, "shout", NULL, 0, &ref, 1, &out) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("fulfill shout through a ref");
    }
    if (!check_ok_string(&out, "WHISPER")) {
        geas_runtime_shutdown(rt);
        return fail("shout result mismatch");
    }
    if (by_ref.len != 7 || memcmp(by_ref.ptr, "WHISPER", 7) != 0) {
        geas_runtime_shutdown(rt);
        return fail("default write back did not land in host memory");
    }

    /* Both pledges have latched Ok now. */
    if (geas_contract_state(c) != GEAS_FULFILLED) {
        geas_runtime_shutdown(rt);
        return fail("state after fulfill");
    }

    /* The same ref through a future and a write back callback: the shouted
     * bytes are copied into host owned storage at the wait, and nothing the
     * host keeps points into the instance. */
    char sink[SHOUT_CAP] = {0};
    GeasString by_ref2;
    by_ref2.ptr = (uint8_t*)"quiet";
    by_ref2.len = 5;
    GeasRef ref2;
    memset(&ref2, 0, sizeof(ref2));
    ref2.host_ptr = &by_ref2;
    ref2.ty = GEAS_TY_STRING;
    ref2.cap = SHOUT_CAP;
    ref2.write_back = copy_shout_out;
    ref2.user = sink;

    GeasFuture* fr = geas_pledge_fulfill(c, "shout", NULL, 0, &ref2, 1);
    if (!fr) {
        geas_runtime_shutdown(rt);
        return fail("shout fulfill returned no future");
    }
    if (strcmp(sink, "") != 0) {
        geas_runtime_shutdown(rt);
        return fail("write back ran before the wait collected the outcome");
    }
    GeasValue rout;
    if (geas_future_wait(fr, &rout) != GEAS_OK ||
        !check_ok_string(&rout, "QUIET")) {
        geas_runtime_shutdown(rt);
        return fail("shout through the future");
    }
    if (strcmp(sink, "QUIET") != 0) {
        geas_runtime_shutdown(rt);
        return fail("callback write back did not land in host memory");
    }

    /* An unknown pledge is a name error, an unknown contract likewise. */
    GeasValue scratch;
    if (geas_pledge_fulfill_sync(c, "nope", NULL, 0, NULL, 0, &scratch) !=
        GEAS_ERR_NAME) {
        geas_runtime_shutdown(rt);
        return fail("unknown pledge did not report GEAS_ERR_NAME");
    }
    GeasContract* c2 = NULL;
    if (geas_contract_sign(rt, "Nope", NULL, 0, 0, &c2) != GEAS_ERR_NAME) {
        geas_runtime_shutdown(rt);
        return fail("unknown contract did not report GEAS_ERR_NAME");
    }

    /* ---- the override path: sign with a vow binding ---- */

    GeasVowBinding prefix;
    prefix.name = "prefix";
    prefix.value = str_arg("hey, ");
    GeasContract* c3 = NULL;
    if (geas_contract_sign(rt, "Greeter", &prefix, 1, 0, &c3) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("sign with vow override");
    }
    if (geas_pledge_fulfill_sync(c3, "greet", &name, 1, NULL, 0, &out) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("fulfill greet on the override instance");
    }
    if (!check_ok_string(&out, "hey, world")) {
        geas_runtime_shutdown(rt);
        return fail("override greeting mismatch");
    }

    /* The host reads the vow back the same way a thunk does. */
    const GeasValue* vref = geas_vow_ref(c3, "prefix");
    if (!vref || vref->ty != GEAS_TY_STRING || vref->as.s.len != 5) {
        geas_runtime_shutdown(rt);
        return fail("vow read through the instance");
    }

    /* A binding naming no vow is a name error; a binding of the wrong type
     * is a type error. Neither leaves an instance behind. */
    GeasVowBinding bogus;
    bogus.name = "nope";
    bogus.value = str_arg("x");
    GeasContract* c4 = NULL;
    if (geas_contract_sign(rt, "Greeter", &bogus, 1, 0, &c4) != GEAS_ERR_NAME) {
        geas_runtime_shutdown(rt);
        return fail("unknown vow name did not report GEAS_ERR_NAME");
    }
    GeasVowBinding wrongty;
    wrongty.name = "prefix";
    memset(&wrongty.value, 0, sizeof(wrongty.value));
    wrongty.value.ty = GEAS_TY_INT;
    wrongty.value.as.i = 7;
    if (geas_contract_sign(rt, "Greeter", &wrongty, 1, 0, &c4) != GEAS_ERR_TYPE) {
        geas_runtime_shutdown(rt);
        return fail("wrong vow type did not report GEAS_ERR_TYPE");
    }

    /* ---- the future path: fulfill, wait once, never twice ---- */

    GeasFuture* f = geas_pledge_fulfill(c3, "greet", &name, 1, NULL, 0);
    if (!f) {
        geas_runtime_shutdown(rt);
        return fail("fulfill returned no future");
    }
    GeasValue fout;
    if (geas_future_wait(f, &fout) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("future wait");
    }
    if (!check_ok_string(&fout, "hey, world")) {
        geas_runtime_shutdown(rt);
        return fail("future greeting mismatch");
    }
    if (geas_future_wait(f, &fout) != GEAS_ERR_STATE) {
        geas_runtime_shutdown(rt);
        return fail("double wait did not report GEAS_ERR_STATE");
    }

    /* ---- the signature: the right hash signs, a wrong one is refused ---- */

    uint64_t hash = geas_contract_hash(c3);
    GeasContract* c5 = NULL;
    if (geas_contract_sign(rt, "Greeter", NULL, 0, hash, &c5) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("sign under the correct expected hash");
    }
    GeasContract* c6 = NULL;
    if (geas_contract_sign(rt, "Greeter", NULL, 0, hash + 1, &c6) !=
        GEAS_ERR_VERSION) {
        geas_runtime_shutdown(rt);
        return fail("wrong expected hash did not report GEAS_ERR_VERSION");
    }

    /* ---- break, then prove the latch ---- */

    if (geas_contract_break(c) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("break");
    }
    if (geas_contract_state(c) != GEAS_BROKEN) {
        geas_runtime_shutdown(rt);
        return fail("state after break");
    }
    if (geas_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &scratch) !=
        GEAS_ERR_STATE) {
        geas_runtime_shutdown(rt);
        return fail("fulfill after break did not report GEAS_ERR_STATE");
    }
    GeasFuture* f2 = geas_pledge_fulfill(c, "greet", &name, 1, NULL, 0);
    if (!f2 || geas_future_wait(f2, &scratch) != GEAS_ERR_STATE) {
        geas_runtime_shutdown(rt);
        return fail("future after break did not report GEAS_ERR_STATE");
    }

    /* ---- two host threads, one instance: fulfillments serialize ---- */

    GeasContract* tc1 = NULL;
    GeasContract* tc2 = NULL;
    if (geas_contract_sign(rt, "Greeter", NULL, 0, 0, &tc1) != GEAS_OK ||
        geas_contract_sign(rt, "Greeter", NULL, 0, 0, &tc2) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("sign the concurrency instances");
    }

    GreetJob same[2] = {
        { tc1, "hello, world", GREET_ITERS, 0 },
        { tc1, "hello, world", GREET_ITERS, 0 },
    };
    pthread_t th[2];
    if (pthread_create(&th[0], NULL, greet_worker, &same[0]) != 0 ||
        pthread_create(&th[1], NULL, greet_worker, &same[1]) != 0) {
        geas_runtime_shutdown(rt);
        return fail("spawning the same-instance workers");
    }
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    if (same[0].failures || same[1].failures) {
        geas_runtime_shutdown(rt);
        return fail("concurrent fulfillments on one instance");
    }

    /* ---- two instances in parallel ---- */

    GreetJob apart[2] = {
        { tc1, "hello, world", GREET_ITERS, 0 },
        { tc2, "hello, world", GREET_ITERS, 0 },
    };
    if (pthread_create(&th[0], NULL, greet_worker, &apart[0]) != 0 ||
        pthread_create(&th[1], NULL, greet_worker, &apart[1]) != 0) {
        geas_runtime_shutdown(rt);
        return fail("spawning the cross-instance workers");
    }
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    if (apart[0].failures || apart[1].failures) {
        geas_runtime_shutdown(rt);
        return fail("parallel fulfillments across instances");
    }

    /* ---- the break race: fulfill in flight, break, wait ---- */

    /* The fulfillment is queued, a waiter thread races the break for it.
     * The runtime promises exactly two outcomes and no third: the pledge
     * delivered before the break won, or the break forfeited the future and
     * the wait reports GEAS_ERR_STATE. Either way nothing crashes and no
     * freed memory is touched, which is what ASan and TSan are watching. */
    GeasContract* rc = NULL;
    if (geas_contract_sign(rt, "Greeter", NULL, 0, 0, &rc) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("sign the race instance");
    }
    WaitJob race;
    race.f = geas_pledge_fulfill(rc, "greet", &name, 1, NULL, 0);
    race.status = GEAS_ERR_OOM;
    if (!race.f) {
        geas_runtime_shutdown(rt);
        return fail("race fulfill returned no future");
    }
    if (pthread_create(&th[0], NULL, wait_worker, &race) != 0) {
        geas_runtime_shutdown(rt);
        return fail("spawning the race waiter");
    }
    if (geas_contract_break(rc) != GEAS_OK) {
        pthread_join(th[0], NULL);
        geas_runtime_shutdown(rt);
        return fail("break during an in-flight fulfillment");
    }
    pthread_join(th[0], NULL);
    if (race.status != GEAS_OK && race.status != GEAS_ERR_STATE) {
        geas_runtime_shutdown(rt);
        return fail("break race produced a status outside {Ok, ERR_STATE}");
    }

    /* ---- freeze: the registration surface shuts, the rest lives on ---- */

    if (geas_runtime_freeze(rt) != GEAS_OK || geas_runtime_freeze(rt) != GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("freeze is not idempotent");
    }
    if (geas_pledge_bind(rt, "Greeter.shout", host_shout) != GEAS_ERR_STATE) {
        geas_runtime_shutdown(rt);
        return fail("bind after freeze did not report GEAS_ERR_STATE");
    }
    if (geas_module_load(rt, "target/geas-out/libhello.geas.so") !=
        GEAS_ERR_STATE) {
        geas_runtime_shutdown(rt);
        return fail("module load after freeze did not report GEAS_ERR_STATE");
    }

    /* Signing and fulfilling stay open after the freeze. */
    GeasContract* fc = NULL;
    if (geas_contract_sign(rt, ent.contract, NULL, 0, ent.shape_hash, &fc) !=
        GEAS_OK) {
        geas_runtime_shutdown(rt);
        return fail("sign after freeze");
    }
    if (geas_pledge_fulfill_sync(fc, "greet", &name, 1, NULL, 0, &out) !=
            GEAS_OK ||
        !check_ok_string(&out, "hello, world")) {
        geas_runtime_shutdown(rt);
        return fail("fulfill after freeze");
    }

    /* The canonical dump: non-empty, and it names the pledge the host
     * resolved through the table. */
    size_t dneed = 0;
    if (geas_iname_dump(rt, NULL, 0, &dneed) != GEAS_ERR_OOM || dneed <= 1) {
        geas_runtime_shutdown(rt);
        return fail("dump size query");
    }
    char* dump = malloc(dneed);
    if (!dump) {
        geas_runtime_shutdown(rt);
        return fail("allocating the dump buffer");
    }
    if (geas_iname_dump(rt, dump, dneed, &dneed) != GEAS_OK ||
        strlen(dump) + 1 != dneed || !strstr(dump, GREET_MANGLED)) {
        free(dump);
        geas_runtime_shutdown(rt);
        return fail("dump content");
    }
    free(dump);

    geas_runtime_shutdown(rt);
    fprintf(stderr, "[host] ok\n");
    return 0;
}
