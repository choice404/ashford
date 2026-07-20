/* test_park.c: the parked instance gate. An instance's durable state, the
 * vows, the latches, the Err payloads, and the transactional fates, goes
 * into a store row through ash_instance_park and stands back up in a fresh
 * runtime through ash_instance_resume, so a signature outlives its process.
 * The checks pin the round trip: a partial payment resumes with its vow
 * override, its latch set, and its walk still open, and runs to fulfilled;
 * an automatically broken instance resumes with its Err payload readable
 * and further fulfillment refused; a store backed Ledger resumes against
 * its own dsn vow, reads the committed balance, and refuses to rerun the
 * transactional episode it already closed. The refusals hold their line:
 * park mid transaction, park with an unwaited future, park after the
 * caller's own break, resume of a key nobody parked, resume under a lying
 * hash, and resume in a runtime that never registered the contract. Runs
 * under ASan and LSan so both runtimes, the row, and every decoded value
 * are watched to zero leaks. */

#include <ash/ash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond, what)                                     \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "[test_park] FAIL: %s\n", what);  \
            g_fail = 1;                                       \
        }                                                     \
    } while (0)

/* The host charge the partial gate binds: Ok(true) for a positive amount,
 * Err(7) otherwise. */
static AshStatus host_charge(void* ctx, const AshValue* args, size_t nargs,
                             AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 2) return ASH_ERR_TYPE;
    if (args[0].ty != ASH_TY_STRING || args[1].ty != ASH_TY_FLOAT)
        return ASH_ERR_TYPE;
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_RESULT;
    if (args[1].as.f > 0.0) {
        box->ty = ASH_TY_BOOL;
        box->as.b = 1;
        out->tag = 0;
    } else {
        box->ty = ASH_TY_INT;
        box->as.i = 7;
        out->tag = 1;
    }
    out->as.box = box;
    return ASH_OK;
}

static AshValue str_val(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static AshValue float_val(double f) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_FLOAT;
    v.as.f = f;
    return v;
}

static AshValue bool_val(int b) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_BOOL;
    v.as.b = b ? 1 : 0;
    return v;
}

static AshValue int_val(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

static AshValue run(AshContract* c, const char* name, const AshValue* args,
                    size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out) ==
              ASH_OK,
          name);
    return out;
}

static AshStatus run_status(AshContract* c, const char* name,
                            const AshValue* args, size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    return ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out);
}

static int is_result(const AshValue* v, uint32_t tag) {
    return v->ty == ASH_TY_RESULT && v->tag == tag;
}

static int ok_float(const AshValue* r, double* out) {
    if (r->ty != ASH_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const AshValue* p = (const AshValue*)r->as.box;
    if (p->ty != ASH_TY_FLOAT) return 0;
    *out = p->as.f;
    return 1;
}

/* A payment runtime: the module loaded and the abstract charge bound. */
static AshRuntime* payment_rt(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK || !rt) return NULL;
    CHECK(ash_module_load(rt, "target/ashc-out/libpayment.ash.so") == ASH_OK,
          "load payment module");
    CHECK(ash_pledge_bind(rt, "PaymentService.charge", host_charge) == ASH_OK,
          "bind charge");
    return rt;
}

int main(void) {
    char park_path[] = "target/ashpark_XXXXXX";
    int pfd = mkstemp(park_path);
    char led_path[] = "target/ashparkled_XXXXXX";
    int lfd = mkstemp(led_path);
    if (pfd < 0 || lfd < 0) {
        fprintf(stderr, "[test_park] FAIL: mkstemp\n");
        return 1;
    }
    close(pfd);
    close(lfd);
    char park_dsn[96];
    snprintf(park_dsn, sizeof(park_dsn), "file:%s", park_path);
    char led_dsn[96];
    snprintf(led_dsn, sizeof(led_dsn), "file:%s", led_path);

    AshValue card = str_val("4111 1111");
    AshValue out;

    /* ---- a partial walk parks, and its instance outlives the runtime ---- */

    AshRuntime* rt1 = payment_rt();
    if (!rt1) return 1;
    AshVowBinding eur = { "currency", str_val("EUR") };
    AshContract* c1 = NULL;
    CHECK(ash_contract_sign(rt1, "PaymentService", &eur, 1, 0, &c1) == ASH_OK,
          "sign c1");
    out = run(c1, "validate_card", &card, 1);
    CHECK(is_result(&out, 0), "validate_card Ok");
    AshValue amt = float_val(25.0);
    out = run(c1, "validate_amount", &amt, 1);
    CHECK(is_result(&out, 0), "validate_amount Ok");
    CHECK(ash_contract_state(c1) == ASH_PARTIAL, "c1 partial before the park");
    int64_t c1_signed_at = ash_contract_signed_at(c1);

    CHECK(ash_instance_park(NULL, park_dsn, "s1") == ASH_ERR_TYPE,
          "park of NULL is ASH_ERR_TYPE");
    CHECK(ash_instance_park(c1, park_dsn, "s1") == ASH_OK, "park c1");

    /* Parking is a write, not an ending: the instance still drives. */
    CHECK(ash_contract_state(c1) == ASH_PARTIAL, "c1 still partial after park");
    CHECK(ash_contract_break(c1) == ASH_OK, "break c1");
    ash_runtime_shutdown(rt1);

    /* ---- the resume, in a process the sign never saw ---- */

    AshRuntime* rt2 = payment_rt();
    if (!rt2) return 1;

    AshContract* r1 = NULL;
    CHECK(ash_instance_resume(rt2, park_dsn, "nobody", 0, &r1) == ASH_ERR_NAME,
          "resume of an unparked key is ASH_ERR_NAME");
    CHECK(ash_instance_resume(rt2, park_dsn, "s1", 12345, &r1) ==
              ASH_ERR_VERSION,
          "resume under a lying hash is ASH_ERR_VERSION");
    CHECK(ash_instance_resume(rt2, park_dsn, "s1", 0, &r1) == ASH_OK,
          "resume s1");
    CHECK(r1 && ash_contract_state(r1) == ASH_PARTIAL, "r1 resumes partial");
    CHECK(ash_contract_signed_at(r1) == c1_signed_at,
          "r1 keeps the original signing time");
    const AshValue* vw = ash_vow_ref(r1, "currency");
    CHECK(vw && vw->ty == ASH_TY_STRING && vw->as.s.len == 3 &&
              memcmp(vw->as.s.ptr, "EUR", 3) == 0,
          "the vow override survived the park");
    CHECK(ash_partial_count(r1, ASH_ITEM_FULFILLED) == 1 &&
              strcmp(ash_partial_name(r1, ASH_ITEM_FULFILLED, 0),
                     "Validation") == 0,
          "r1 fulfilled lists Validation");
    CHECK(ash_partial_count(r1, ASH_ITEM_PENDING) == 2,
          "r1 pending lists Processing and notify_user");

    /* The walk resumes on latches set before the park, and lands. */
    AshValue charge_args[2] = { card, float_val(25.0) };
    out = run(r1, "charge", charge_args, 2);
    CHECK(is_result(&out, 0), "charge Ok on the resumed instance");
    AshValue okv = bool_val(1);
    out = run(r1, "notify_user", &okv, 1);
    CHECK(is_result(&out, 0), "notify_user Ok on the resumed instance");
    CHECK(ash_contract_state(r1) == ASH_FULFILLED, "r1 runs to fulfilled");
    CHECK(ash_contract_break(r1) == ASH_OK, "break r1");

    /* ---- an automatic break parks with its payload readable ---- */

    AshContract* c2 = NULL;
    CHECK(ash_contract_sign(rt2, "PaymentService", NULL, 0, 0, &c2) == ASH_OK,
          "sign c2");
    AshValue bad[2] = { card, float_val(-2.0) };
    out = run(c2, "charge", bad, 2);
    CHECK(is_result(&out, 1), "charge Err breaks c2");
    CHECK(ash_contract_state(c2) == ASH_BROKEN, "c2 broken by its own line");
    CHECK(ash_instance_park(c2, park_dsn, "s2") == ASH_OK,
          "an automatically broken instance parks");

    /* The caller's own break reclaims the heap; after it there is nothing
     * left to write down, and park says so. */
    CHECK(ash_contract_break(c2) == ASH_OK, "break c2");
    CHECK(ash_instance_park(c2, park_dsn, "s2b") == ASH_ERR_STATE,
          "park after an explicit break is ASH_ERR_STATE");
    ash_runtime_shutdown(rt2);

    AshRuntime* rt3 = payment_rt();
    if (!rt3) return 1;
    AshContract* r2 = NULL;
    CHECK(ash_instance_resume(rt3, park_dsn, "s2", 0, &r2) == ASH_OK,
          "resume s2");
    CHECK(r2 && ash_contract_state(r2) == ASH_BROKEN, "r2 resumes broken");
    CHECK(ash_partial_nerrors(r2) == 1, "r2 carries one error");
    const char* ep = NULL;
    const AshValue* ev = NULL;
    CHECK(ash_partial_error(r2, 0, &ep, &ev) == ASH_OK &&
              strcmp(ep, "charge") == 0 && ev && ev->ty == ASH_TY_INT &&
              ev->as.i == 7,
          "the Err payload crossed the park");
    CHECK(run_status(r2, "validate_card", &card, 1) == ASH_ERR_STATE,
          "fulfillment on a resumed broken instance is ASH_ERR_STATE");
    CHECK(ash_contract_break(r2) == ASH_OK, "break r2");

    /* ---- an unwaited future refuses the park ---- */

    AshContract* c3 = NULL;
    CHECK(ash_contract_sign(rt3, "PaymentService", NULL, 0, 0, &c3) == ASH_OK,
          "sign c3");
    AshFuture* fut = ash_pledge_fulfill(c3, "validate_card", &card, 1, NULL, 0);
    CHECK(fut != NULL, "async fulfill issues a receipt");
    CHECK(ash_instance_park(c3, park_dsn, "s3") == ASH_ERR_STATE,
          "park with a walk in the air is ASH_ERR_STATE");
    memset(&out, 0, sizeof(out));
    CHECK(ash_future_wait(fut, &out) == ASH_OK, "the receipt delivers");
    CHECK(ash_instance_park(c3, park_dsn, "s3") == ASH_OK,
          "park lands once the walk is on the ground");
    CHECK(ash_contract_break(c3) == ASH_OK, "break c3");
    ash_runtime_shutdown(rt3);

    /* ---- a runtime that never registered the contract ---- */

    AshRuntime* rt4 = NULL;
    CHECK(ash_runtime_init(NULL, &rt4) == ASH_OK && rt4, "bare runtime init");
    AshContract* r4 = NULL;
    CHECK(ash_instance_resume(rt4, park_dsn, "s1", 0, &r4) == ASH_ERR_NAME,
          "resume without the module is ASH_ERR_NAME");
    ash_runtime_shutdown(rt4);

    /* ---- the store backed instance: the dsn vow crosses the park ---- */

    AshRuntime* rt5 = NULL;
    CHECK(ash_runtime_init(NULL, &rt5) == ASH_OK && rt5, "ledger runtime init");
    CHECK(ash_module_load(rt5, "target/ashc-out/libledger.ash.so") == ASH_OK,
          "load ledger module");
    AshVowBinding dsn_ovr = { "dsn", str_val(led_dsn) };
    AshContract* led = NULL;
    CHECK(ash_contract_sign(rt5, "Ledger", &dsn_ovr, 1, 0, &led) == ASH_OK,
          "sign Ledger");
    AshValue open_args[3] = { int_val(1), str_val("ada"), float_val(100.0) };
    out = run(led, "open", open_args, 3);
    CHECK(is_result(&out, 0), "open account 1");

    /* The transactional episode: park refuses mid flight, parks once the
     * commit landed, and the resumed instance never reruns it. */
    AshValue debit_args[2] = { int_val(1), float_val(30.0) };
    out = run(led, "debit", debit_args, 2);
    CHECK(is_result(&out, 0), "debit 30 opens the episode");
    CHECK(ash_instance_park(led, park_dsn, "led1") == ASH_ERR_STATE,
          "park mid transaction is ASH_ERR_STATE");
    AshValue credit_args[2] = { int_val(1), float_val(30.0) };
    out = run(led, "credit", credit_args, 2);
    CHECK(is_result(&out, 0), "credit 30 closes the episode");
    CHECK(ash_instance_park(led, park_dsn, "led1") == ASH_OK,
          "park the ledger once the episode committed");
    CHECK(ash_contract_break(led) == ASH_OK, "break the ledger");
    ash_runtime_shutdown(rt5);

    AshRuntime* rt6 = NULL;
    CHECK(ash_runtime_init(NULL, &rt6) == ASH_OK && rt6, "resume runtime init");
    CHECK(ash_module_load(rt6, "target/ashc-out/libledger.ash.so") == ASH_OK,
          "load ledger module again");
    AshContract* rled = NULL;
    CHECK(ash_instance_resume(rt6, park_dsn, "led1", 0, &rled) == ASH_OK,
          "resume the ledger");
    const AshValue* dv = rled ? ash_vow_ref(rled, "dsn") : NULL;
    CHECK(dv && dv->ty == ASH_TY_STRING && dv->as.s.len == strlen(led_dsn) &&
              memcmp(dv->as.s.ptr, led_dsn, dv->as.s.len) == 0,
          "the dsn vow crossed the park");
    AshValue key1 = int_val(1);
    out = run(rled, "balance", &key1, 1);
    double bal = -1.0;
    CHECK(ok_float(&out, &bal) && bal == 100.0,
          "the resumed ledger reads the committed balance");
    CHECK(run_status(rled, "debit", debit_args, 2) == ASH_ERR_STATE,
          "the closed episode never reruns on the resumed instance");
    CHECK(ash_contract_break(rled) == ASH_OK, "break the resumed ledger");
    ash_runtime_shutdown(rt6);

    unlink(park_path);
    unlink(led_path);

    if (g_fail) return 1;
    printf("[test_park] ok\n");
    return 0;
}
