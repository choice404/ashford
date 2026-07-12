/* test_partial.c: the M7 requirements and partial result gate. It drives the
 * compiled payment module end to end: the host binds the abstract charge
 * pledge and steers it between Ok and Err, and the checks pin the grammar's
 * laws. A pledge latches on its first outcome and never moves again. A
 * subcontract is fulfilled when every pledge inside it is and broken when
 * every pledge inside it is. The policy evaluates after every fulfillment in
 * priority order break, fulfill, partial. The partial surface reports item
 * states in descriptor order, named subcontracts then loose pledges, and
 * hands over the first Err payload of every broken pledge. An automatic
 * break keeps the instance heap alive so those payloads stay readable; an
 * explicit break reclaims them. Runs under ASan and LSan, so the automatic
 * break's kept heap is also proven reclaimed at shutdown. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, what)                                        \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "[test_partial] FAIL: %s\n", what);  \
            g_fail = 1;                                          \
        }                                                        \
    } while (0)

/* The host implementation of PaymentService.charge: Ok(true) for a positive
 * amount, Err(7) otherwise, so one binding walks every path. */
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

static int is_result(const AshValue* v, uint32_t tag) {
    return v->ty == ASH_TY_RESULT && v->tag == tag;
}

/* One named item's presence at index i of the k list. */
static int name_at(AshContract* c, AshItemState k, size_t i,
                   const char* want) {
    const char* got = ash_partial_name(c, k, i);
    return got && strcmp(got, want) == 0;
}

/* Fulfills one pledge synchronously and demands the thunk ran. */
static AshValue run_pledge(AshContract* c, const char* name,
                           const AshValue* args, size_t nargs) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out) ==
              ASH_OK,
          name);
    return out;
}

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) {
        fprintf(stderr, "[test_partial] FAIL: runtime init\n");
        return 1;
    }
    CHECK(ash_module_load(rt, "target/ashc-out/libpayment.ash.so") == ASH_OK,
          "module load");
    CHECK(ash_pledge_bind(rt, "PaymentService.charge", host_charge) == ASH_OK,
          "bind charge");

    AshValue card = str_val("4111 1111");
    AshValue out;

    /* ---- the partial path: Validation lands, Processing breaks ---- */

    AshContract* c1 = NULL;
    CHECK(ash_contract_sign(rt, "PaymentService", NULL, 0, 0, &c1) == ASH_OK,
          "sign c1");
    CHECK(ash_contract_state(c1) == ASH_SIGNED, "c1 signed");
    CHECK(ash_partial_count(c1, ASH_ITEM_PENDING) == 3, "c1 3 pending items");
    CHECK(ash_partial_count(c1, ASH_ITEM_FULFILLED) == 0, "c1 none fulfilled");
    CHECK(ash_partial_count(c1, ASH_ITEM_BROKEN) == 0, "c1 none broken");
    CHECK(ash_partial_nerrors(c1) == 0, "c1 no errors");
    CHECK(name_at(c1, ASH_ITEM_PENDING, 0, "Validation") &&
              name_at(c1, ASH_ITEM_PENDING, 1, "Processing") &&
              name_at(c1, ASH_ITEM_PENDING, 2, "notify_user"),
          "c1 pending order is subs then loose pledges");
    CHECK(ash_partial_name(c1, ASH_ITEM_PENDING, 3) == NULL,
          "c1 pending index out of range is NULL");

    /* One Validation pledge alone does not fulfill the subcontract. */
    out = run_pledge(c1, "validate_card", &card, 1);
    CHECK(is_result(&out, 0), "validate_card Ok");
    CHECK(ash_contract_state(c1) == ASH_SIGNED,
          "half a subcontract moves nothing");
    CHECK(ash_partial_count(c1, ASH_ITEM_PENDING) == 3,
          "Validation still pending on one pledge");

    /* The second lands the subcontract; partial: Validation || ... fires. */
    AshValue amount = float_val(25.0);
    out = run_pledge(c1, "validate_amount", &amount, 1);
    CHECK(is_result(&out, 0), "validate_amount Ok");
    CHECK(ash_contract_state(c1) == ASH_PARTIAL, "c1 partial");
    CHECK(ash_partial_count(c1, ASH_ITEM_FULFILLED) == 1 &&
              name_at(c1, ASH_ITEM_FULFILLED, 0, "Validation"),
          "fulfilled lists Validation");
    CHECK(ash_partial_count(c1, ASH_ITEM_PENDING) == 2 &&
              name_at(c1, ASH_ITEM_PENDING, 0, "Processing") &&
              name_at(c1, ASH_ITEM_PENDING, 1, "notify_user"),
          "pending lists Processing and notify_user");

    /* charge fails: Processing breaks and the error is attached. */
    AshValue charge_args[2] = { card, float_val(-1.0) };
    out = run_pledge(c1, "charge", charge_args, 2);
    CHECK(is_result(&out, 1), "charge returned Err");
    CHECK(ash_contract_state(c1) == ASH_PARTIAL,
          "break line does not fire while Validation holds");
    CHECK(ash_partial_count(c1, ASH_ITEM_BROKEN) == 1 &&
              name_at(c1, ASH_ITEM_BROKEN, 0, "Processing"),
          "broken lists Processing");
    CHECK(ash_partial_nerrors(c1) == 1, "one error attached");
    const char* pname = NULL;
    const AshValue* perr = NULL;
    CHECK(ash_partial_error(c1, 0, &pname, &perr) == ASH_OK, "error 0 reads");
    CHECK(pname && strcmp(pname, "charge") == 0, "error names charge");
    CHECK(perr && perr->ty == ASH_TY_INT && perr->as.i == 7,
          "error carries Err(7)");
    CHECK(ash_partial_error(c1, 1, &pname, &perr) == ASH_ERR_NAME,
          "error index out of range is ASH_ERR_NAME");

    /* The latch law: a later Ok still runs and still returns, but the
     * latched Broken never moves. */
    charge_args[1] = float_val(25.0);
    out = run_pledge(c1, "charge", charge_args, 2);
    CHECK(is_result(&out, 0), "late charge Ok returns to the caller");
    CHECK(ash_partial_count(c1, ASH_ITEM_BROKEN) == 1 &&
              ash_partial_nerrors(c1) == 1,
          "Processing stays latched Broken");
    CHECK(ash_contract_state(c1) == ASH_PARTIAL, "state unchanged by relatch");

    /* notify_user lands; fulfill needs Processing, so partial holds. */
    AshValue okv = bool_val(1);
    out = run_pledge(c1, "notify_user", &okv, 1);
    CHECK(is_result(&out, 0), "notify_user Ok");
    CHECK(ash_contract_state(c1) == ASH_PARTIAL, "c1 stays partial");
    CHECK(ash_partial_count(c1, ASH_ITEM_FULFILLED) == 2 &&
              name_at(c1, ASH_ITEM_FULFILLED, 0, "Validation") &&
              name_at(c1, ASH_ITEM_FULFILLED, 1, "notify_user"),
          "fulfilled lists Validation then notify_user");
    CHECK(ash_partial_count(c1, ASH_ITEM_PENDING) == 0, "nothing pending");

    /* An explicit break reclaims the heap; the latches survive it, the
     * stored payload does not. */
    CHECK(ash_contract_break(c1) == ASH_OK, "explicit break c1");
    CHECK(ash_contract_state(c1) == ASH_BROKEN, "c1 broken");
    CHECK(ash_partial_count(c1, ASH_ITEM_FULFILLED) == 2 &&
              ash_partial_count(c1, ASH_ITEM_BROKEN) == 1,
          "latches readable after explicit break");
    CHECK(ash_partial_error(c1, 0, &pname, &perr) == ASH_OK &&
              perr->ty == ASH_TY_UNIT,
          "explicit break reclaimed the payload");

    /* ---- the fulfilled path: everything Ok on a fresh instance ---- */

    AshContract* c2 = NULL;
    CHECK(ash_contract_sign(rt, "PaymentService", NULL, 0, 0, &c2) == ASH_OK,
          "sign c2");
    out = run_pledge(c2, "validate_card", &card, 1);
    CHECK(is_result(&out, 0), "c2 validate_card Ok");
    amount = float_val(10.0);
    out = run_pledge(c2, "validate_amount", &amount, 1);
    CHECK(is_result(&out, 0), "c2 validate_amount Ok");
    charge_args[1] = float_val(5.0);
    out = run_pledge(c2, "charge", charge_args, 2);
    CHECK(is_result(&out, 0), "c2 charge Ok");
    CHECK(ash_contract_state(c2) == ASH_PARTIAL, "c2 partial before notify");
    out = run_pledge(c2, "notify_user", &okv, 1);
    CHECK(is_result(&out, 0), "c2 notify_user Ok");
    CHECK(ash_contract_state(c2) == ASH_FULFILLED, "c2 fulfilled");
    CHECK(ash_partial_count(c2, ASH_ITEM_FULFILLED) == 3 &&
              ash_partial_count(c2, ASH_ITEM_PENDING) == 0 &&
              ash_partial_count(c2, ASH_ITEM_BROKEN) == 0,
          "c2 every item fulfilled");
    CHECK(ash_partial_nerrors(c2) == 0, "c2 no errors");

    /* ---- the automatic break: the break line fires by itself ---- */

    AshContract* c3 = NULL;
    CHECK(ash_contract_sign(rt, "PaymentService", NULL, 0, 0, &c3) == ASH_OK,
          "sign c3");
    charge_args[1] = float_val(-2.0);
    out = run_pledge(c3, "charge", charge_args, 2);
    CHECK(is_result(&out, 1), "c3 charge Err");
    CHECK(ash_contract_state(c3) == ASH_BROKEN,
          "break line fired with no explicit break()");
    AshValue scratch;
    CHECK(ash_pledge_fulfill_sync(c3, "validate_card", &card, 1, NULL, 0,
                                  &scratch) == ASH_ERR_STATE,
          "fulfillment after automatic break is ASH_ERR_STATE");

    /* The automatic break kept the heap: the error payload is readable. */
    CHECK(ash_partial_nerrors(c3) == 1, "c3 one error");
    CHECK(ash_partial_error(c3, 0, &pname, &perr) == ASH_OK &&
              pname && strcmp(pname, "charge") == 0 && perr &&
              perr->ty == ASH_TY_INT && perr->as.i == 7,
          "automatic break kept the Err payload readable");
    CHECK(ash_partial_count(c3, ASH_ITEM_BROKEN) == 1 &&
              name_at(c3, ASH_ITEM_BROKEN, 0, "Processing"),
          "c3 broken lists Processing");
    CHECK(ash_partial_count(c3, ASH_ITEM_PENDING) == 2,
          "c3 the untouched items stay pending");

    /* An explicit break after the automatic one reclaims what it kept. */
    CHECK(ash_contract_break(c3) == ASH_OK, "explicit break after automatic");
    CHECK(ash_partial_error(c3, 0, &pname, &perr) == ASH_OK &&
              perr->ty == ASH_TY_UNIT,
          "the kept payload is reclaimed by the explicit break");

    ash_runtime_shutdown(rt);
    if (g_fail) return 1;
    fprintf(stderr, "[test_partial] ok\n");
    return 0;
}
