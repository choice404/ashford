/* host.c: the C half of the walking skeleton. This is a foreign program, it
 * knows nothing about ashc, it only links libashrt and speaks the ABI header.
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
 * land on Ok or ASH_ERR_STATE and nothing else. M6 makes the first sign a
 * real discovery: the host resolves greet's mangled name through the iname
 * table, signs under the contract name and shape hash the entry carries,
 * and at the end freezes the runtime and proves that binding is refused
 * while signing and fulfilling still work, and that the canonical dump is
 * non-empty and names the pledge it resolved. It exits zero only when
 * every check held. valgrind runs this and expects silence. */

#include <ash/ash.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The mangled name of Greeter.greet, hardcoded the way a foreign host ships
 * it today; the generated header that spells it for the host is M9 work. */
#define GREET_MANGLED "__ash_ash_Greeter_greet_17cef80f14421b9b_v1"

static int fail(const char* what) {
    fprintf(stderr, "[host] FAIL: %s\n", what);
    return 1;
}

/* The host implementation bound over Greeter.shout. It runs in the uniform
 * thunk frame like any compiled body: everything it builds goes through the
 * instance in ctx, and the name parameter is its own instance owned slot, so
 * shouting mutates the slot in place and the runtime's write back carries
 * the change out when the caller passed the name by reference. */
static AshStatus host_shout(void* ctx, const AshValue* args, size_t nargs,
                            AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 1) return ASH_ERR_TYPE;
    if (args[0].ty != ASH_TY_STRING) return ASH_ERR_TYPE;
    AshValue up = ash_string_copy(c, args[0].as.s.ptr, args[0].as.s.len);
    if (args[0].as.s.len && !up.as.s.ptr) return ASH_ERR_OOM;
    for (uint64_t i = 0; i < up.as.s.len; i++) {
        uint8_t ch = up.as.s.ptr[i];
        if (ch >= 'a' && ch <= 'z') up.as.s.ptr[i] = (uint8_t)(ch - 32);
    }
    ((AshValue*)args)[0] = up;
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    *box = up;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

/* A write back callback: instead of letting the default point the host's
 * AshString at instance owned bytes, copy them into a buffer the host owns,
 * reached through user, so nothing aliases instance memory after the call. */
#define SHOUT_CAP 32
static void copy_shout_out(void* host_ptr, const AshValue* v, void* user) {
    (void)host_ptr;
    char* buf = (char*)user;
    size_t n = v->as.s.len < SHOUT_CAP - 1 ? v->as.s.len : SHOUT_CAP - 1;
    memcpy(buf, v->as.s.ptr, n);
    buf[n] = '\0';
}

static AshValue str_arg(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

/* Demands that a value is Ok(<want>) for a string payload. */
static int check_ok_string(const AshValue* out, const char* want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* inner = (const AshValue*)out->as.box;
    if (!inner || inner->ty != ASH_TY_STRING) return 0;
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
    AshContract* c;
    const char*  want;
    int          iters;
    int          failures;
} GreetJob;

static void* greet_worker(void* arg) {
    GreetJob* job = (GreetJob*)arg;
    AshValue name = str_arg("world");
    for (int i = 0; i < job->iters; i++) {
        AshValue out;
        if (ash_pledge_fulfill_sync(job->c, "greet", &name, 1, NULL, 0,
                                    &out) != ASH_OK ||
            !check_ok_string(&out, job->want)) {
            job->failures++;
        }
    }
    return NULL;
}

/* The waiter half of the break race. It waits one future and records the
 * status; the main thread breaks the instance concurrently. On ASH_OK it
 * must not read the payload, the instance heap may already be gone. */
typedef struct WaitJob {
    AshFuture* f;
    AshStatus  status;
} WaitJob;

static void* wait_worker(void* arg) {
    WaitJob* job = (WaitJob*)arg;
    AshValue out;
    job->status = ash_future_wait(job->f, &out);
    return NULL;
}

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return fail("runtime init");

    if (ash_module_load(rt, "target/ashc-out/libhello.ash.so") != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("module load");
    }

    /* ---- the abstract pledge: no sign until the host binds it ---- */

    AshContract* c0 = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, 0, &c0) != ASH_ERR_UNBOUND) {
        ash_runtime_shutdown(rt);
        return fail("sign before bind did not report ASH_ERR_UNBOUND");
    }
    if (ash_pledge_bind(rt, "Greeter.nope", host_shout) != ASH_ERR_NAME) {
        ash_runtime_shutdown(rt);
        return fail("binding an unknown pledge did not report ASH_ERR_NAME");
    }
    if (ash_pledge_bind(rt, "Greeter.shout", host_shout) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("bind Greeter.shout");
    }

    /* ---- discovery: resolve the pledge's mangled name first ---- */

    /* The iname table turns the mangled name the host shipped with into the
     * owning contract and the shape hash to sign under, so the first sign is
     * a checked handshake instead of a bare string. */
    AshInameEntry ent;
    if (ash_iname_lookup(rt, GREET_MANGLED, &ent) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("iname lookup of the greet mangled name");
    }
    if (ent.kind != ASH_INAME_PLEDGE || strcmp(ent.contract, "Greeter") != 0 ||
        !ent.symbol || strcmp(ent.symbol, "greet") != 0 || ent.nargs != 1 ||
        ent.version != 1 || ent.shape_hash == 0) {
        ash_runtime_shutdown(rt);
        return fail("iname entry for greet carries the wrong facts");
    }
    AshInameEntry miss;
    if (ash_iname_lookup(rt, "__ash_ash_Greeter_greet_0000000000000000_v1",
                         &miss) != ASH_ERR_NAME) {
        ash_runtime_shutdown(rt);
        return fail("a wrong mangled name did not report ASH_ERR_NAME");
    }

    /* ---- the default path: sign on the declared vow defaults ---- */

    AshContract* c = NULL;
    if (ash_contract_sign(rt, ent.contract, NULL, 0, ent.shape_hash, &c) !=
        ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign under the discovered contract and hash");
    }
    if (ash_contract_state(c) != ASH_SIGNED) {
        ash_runtime_shutdown(rt);
        return fail("state after sign");
    }
    if (ash_contract_hash(c) == 0) {
        ash_runtime_shutdown(rt);
        return fail("signed instance carries no shape hash");
    }
    if (ash_contract_signed_at(c) <= 0) {
        ash_runtime_shutdown(rt);
        return fail("signed instance carries no timestamp");
    }

    /* The argument is host owned; the runtime never keeps it. */
    AshValue name = str_arg("world");

    AshValue out;
    if (ash_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &out) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("fulfill greet");
    }
    if (!check_ok_string(&out, "hello, world")) {
        ash_runtime_shutdown(rt);
        return fail("default greeting mismatch");
    }
    const AshValue* inner = (const AshValue*)out.as.box;
    printf("%.*s\n", (int)inner->as.s.len, (const char*)inner->as.s.ptr);

    /* One of two pledges has latched Ok; the contract is not fulfilled yet. */
    if (ash_contract_state(c) != ASH_SIGNED) {
        ash_runtime_shutdown(rt);
        return fail("state after a partial fulfill");
    }

    /* ---- the bound pledge, called with the name by reference ---- */

    /* The host's own storage for the argument. The runtime copies the value
     * in at fulfill, the bound body shouts its instance owned slot, and the
     * default write back repoints this struct at the shouted bytes before
     * the call returns. */
    AshString by_ref;
    by_ref.ptr = (uint8_t*)"whisper";
    by_ref.len = 7;

    AshRef ref;
    memset(&ref, 0, sizeof(ref));
    ref.host_ptr = &by_ref;
    ref.ty = ASH_TY_STRING;

    if (ash_pledge_fulfill_sync(c, "shout", NULL, 0, &ref, 1, &out) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("fulfill shout through a ref");
    }
    if (!check_ok_string(&out, "WHISPER")) {
        ash_runtime_shutdown(rt);
        return fail("shout result mismatch");
    }
    if (by_ref.len != 7 || memcmp(by_ref.ptr, "WHISPER", 7) != 0) {
        ash_runtime_shutdown(rt);
        return fail("default write back did not land in host memory");
    }

    /* Both pledges have latched Ok now. */
    if (ash_contract_state(c) != ASH_FULFILLED) {
        ash_runtime_shutdown(rt);
        return fail("state after fulfill");
    }

    /* The same ref through a future and a write back callback: the shouted
     * bytes are copied into host owned storage at the wait, and nothing the
     * host keeps points into the instance. */
    char sink[SHOUT_CAP] = {0};
    AshString by_ref2;
    by_ref2.ptr = (uint8_t*)"quiet";
    by_ref2.len = 5;
    AshRef ref2;
    memset(&ref2, 0, sizeof(ref2));
    ref2.host_ptr = &by_ref2;
    ref2.ty = ASH_TY_STRING;
    ref2.cap = SHOUT_CAP;
    ref2.write_back = copy_shout_out;
    ref2.user = sink;

    AshFuture* fr = ash_pledge_fulfill(c, "shout", NULL, 0, &ref2, 1);
    if (!fr) {
        ash_runtime_shutdown(rt);
        return fail("shout fulfill returned no future");
    }
    if (strcmp(sink, "") != 0) {
        ash_runtime_shutdown(rt);
        return fail("write back ran before the wait collected the outcome");
    }
    AshValue rout;
    if (ash_future_wait(fr, &rout) != ASH_OK ||
        !check_ok_string(&rout, "QUIET")) {
        ash_runtime_shutdown(rt);
        return fail("shout through the future");
    }
    if (strcmp(sink, "QUIET") != 0) {
        ash_runtime_shutdown(rt);
        return fail("callback write back did not land in host memory");
    }

    /* An unknown pledge is a name error, an unknown contract likewise. */
    AshValue scratch;
    if (ash_pledge_fulfill_sync(c, "nope", NULL, 0, NULL, 0, &scratch) !=
        ASH_ERR_NAME) {
        ash_runtime_shutdown(rt);
        return fail("unknown pledge did not report ASH_ERR_NAME");
    }
    AshContract* c2 = NULL;
    if (ash_contract_sign(rt, "Nope", NULL, 0, 0, &c2) != ASH_ERR_NAME) {
        ash_runtime_shutdown(rt);
        return fail("unknown contract did not report ASH_ERR_NAME");
    }

    /* ---- the override path: sign with a vow binding ---- */

    AshVowBinding prefix;
    prefix.name = "prefix";
    prefix.value = str_arg("hey, ");
    AshContract* c3 = NULL;
    if (ash_contract_sign(rt, "Greeter", &prefix, 1, 0, &c3) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign with vow override");
    }
    if (ash_pledge_fulfill_sync(c3, "greet", &name, 1, NULL, 0, &out) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("fulfill greet on the override instance");
    }
    if (!check_ok_string(&out, "hey, world")) {
        ash_runtime_shutdown(rt);
        return fail("override greeting mismatch");
    }

    /* The host reads the vow back the same way a thunk does. */
    const AshValue* vref = ash_vow_ref(c3, "prefix");
    if (!vref || vref->ty != ASH_TY_STRING || vref->as.s.len != 5) {
        ash_runtime_shutdown(rt);
        return fail("vow read through the instance");
    }

    /* A binding naming no vow is a name error; a binding of the wrong type
     * is a type error. Neither leaves an instance behind. */
    AshVowBinding bogus;
    bogus.name = "nope";
    bogus.value = str_arg("x");
    AshContract* c4 = NULL;
    if (ash_contract_sign(rt, "Greeter", &bogus, 1, 0, &c4) != ASH_ERR_NAME) {
        ash_runtime_shutdown(rt);
        return fail("unknown vow name did not report ASH_ERR_NAME");
    }
    AshVowBinding wrongty;
    wrongty.name = "prefix";
    memset(&wrongty.value, 0, sizeof(wrongty.value));
    wrongty.value.ty = ASH_TY_INT;
    wrongty.value.as.i = 7;
    if (ash_contract_sign(rt, "Greeter", &wrongty, 1, 0, &c4) != ASH_ERR_TYPE) {
        ash_runtime_shutdown(rt);
        return fail("wrong vow type did not report ASH_ERR_TYPE");
    }

    /* ---- the future path: fulfill, wait once, never twice ---- */

    AshFuture* f = ash_pledge_fulfill(c3, "greet", &name, 1, NULL, 0);
    if (!f) {
        ash_runtime_shutdown(rt);
        return fail("fulfill returned no future");
    }
    AshValue fout;
    if (ash_future_wait(f, &fout) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("future wait");
    }
    if (!check_ok_string(&fout, "hey, world")) {
        ash_runtime_shutdown(rt);
        return fail("future greeting mismatch");
    }
    if (ash_future_wait(f, &fout) != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("double wait did not report ASH_ERR_STATE");
    }

    /* ---- the signature: the right hash signs, a wrong one is refused ---- */

    uint64_t hash = ash_contract_hash(c3);
    AshContract* c5 = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, hash, &c5) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign under the correct expected hash");
    }
    AshContract* c6 = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, hash + 1, &c6) !=
        ASH_ERR_VERSION) {
        ash_runtime_shutdown(rt);
        return fail("wrong expected hash did not report ASH_ERR_VERSION");
    }

    /* ---- break, then prove the latch ---- */

    if (ash_contract_break(c) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("break");
    }
    if (ash_contract_state(c) != ASH_BROKEN) {
        ash_runtime_shutdown(rt);
        return fail("state after break");
    }
    if (ash_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &scratch) !=
        ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("fulfill after break did not report ASH_ERR_STATE");
    }
    AshFuture* f2 = ash_pledge_fulfill(c, "greet", &name, 1, NULL, 0);
    if (!f2 || ash_future_wait(f2, &scratch) != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("future after break did not report ASH_ERR_STATE");
    }

    /* ---- two host threads, one instance: fulfillments serialize ---- */

    AshContract* tc1 = NULL;
    AshContract* tc2 = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, 0, &tc1) != ASH_OK ||
        ash_contract_sign(rt, "Greeter", NULL, 0, 0, &tc2) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign the concurrency instances");
    }

    GreetJob same[2] = {
        { tc1, "hello, world", GREET_ITERS, 0 },
        { tc1, "hello, world", GREET_ITERS, 0 },
    };
    pthread_t th[2];
    if (pthread_create(&th[0], NULL, greet_worker, &same[0]) != 0 ||
        pthread_create(&th[1], NULL, greet_worker, &same[1]) != 0) {
        ash_runtime_shutdown(rt);
        return fail("spawning the same-instance workers");
    }
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    if (same[0].failures || same[1].failures) {
        ash_runtime_shutdown(rt);
        return fail("concurrent fulfillments on one instance");
    }

    /* ---- two instances in parallel ---- */

    GreetJob apart[2] = {
        { tc1, "hello, world", GREET_ITERS, 0 },
        { tc2, "hello, world", GREET_ITERS, 0 },
    };
    if (pthread_create(&th[0], NULL, greet_worker, &apart[0]) != 0 ||
        pthread_create(&th[1], NULL, greet_worker, &apart[1]) != 0) {
        ash_runtime_shutdown(rt);
        return fail("spawning the cross-instance workers");
    }
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    if (apart[0].failures || apart[1].failures) {
        ash_runtime_shutdown(rt);
        return fail("parallel fulfillments across instances");
    }

    /* ---- the break race: fulfill in flight, break, wait ---- */

    /* The fulfillment is queued, a waiter thread races the break for it.
     * The runtime promises exactly two outcomes and no third: the pledge
     * delivered before the break won, or the break forfeited the future and
     * the wait reports ASH_ERR_STATE. Either way nothing crashes and no
     * freed memory is touched, which is what ASan and TSan are watching. */
    AshContract* rc = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, 0, &rc) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign the race instance");
    }
    WaitJob race;
    race.f = ash_pledge_fulfill(rc, "greet", &name, 1, NULL, 0);
    race.status = ASH_ERR_OOM;
    if (!race.f) {
        ash_runtime_shutdown(rt);
        return fail("race fulfill returned no future");
    }
    if (pthread_create(&th[0], NULL, wait_worker, &race) != 0) {
        ash_runtime_shutdown(rt);
        return fail("spawning the race waiter");
    }
    if (ash_contract_break(rc) != ASH_OK) {
        pthread_join(th[0], NULL);
        ash_runtime_shutdown(rt);
        return fail("break during an in-flight fulfillment");
    }
    pthread_join(th[0], NULL);
    if (race.status != ASH_OK && race.status != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("break race produced a status outside {Ok, ERR_STATE}");
    }

    /* ---- freeze: the registration surface shuts, the rest lives on ---- */

    if (ash_runtime_freeze(rt) != ASH_OK || ash_runtime_freeze(rt) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("freeze is not idempotent");
    }
    if (ash_pledge_bind(rt, "Greeter.shout", host_shout) != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("bind after freeze did not report ASH_ERR_STATE");
    }
    if (ash_module_load(rt, "target/ashc-out/libhello.ash.so") !=
        ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("module load after freeze did not report ASH_ERR_STATE");
    }

    /* Signing and fulfilling stay open after the freeze. */
    AshContract* fc = NULL;
    if (ash_contract_sign(rt, ent.contract, NULL, 0, ent.shape_hash, &fc) !=
        ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign after freeze");
    }
    if (ash_pledge_fulfill_sync(fc, "greet", &name, 1, NULL, 0, &out) !=
            ASH_OK ||
        !check_ok_string(&out, "hello, world")) {
        ash_runtime_shutdown(rt);
        return fail("fulfill after freeze");
    }

    /* The canonical dump: non-empty, and it names the pledge the host
     * resolved through the table. */
    size_t dneed = 0;
    if (ash_iname_dump(rt, NULL, 0, &dneed) != ASH_ERR_OOM || dneed <= 1) {
        ash_runtime_shutdown(rt);
        return fail("dump size query");
    }
    char* dump = malloc(dneed);
    if (!dump) {
        ash_runtime_shutdown(rt);
        return fail("allocating the dump buffer");
    }
    if (ash_iname_dump(rt, dump, dneed, &dneed) != ASH_OK ||
        strlen(dump) + 1 != dneed || !strstr(dump, GREET_MANGLED)) {
        free(dump);
        ash_runtime_shutdown(rt);
        return fail("dump content");
    }
    free(dump);

    ash_runtime_shutdown(rt);
    fprintf(stderr, "[host] ok\n");
    return 0;
}
