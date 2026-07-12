/* test_std.c: the standard library gate. It loads the compiled std_user
 * module, whose imports merged four ashstd modules into one build, and
 * drives the merged surface from C: MathOps and ListOps signed by name even
 * though only the root file names StdUser, the integer and float pledges on
 * each, the error sums riding the Err box as tagged values, Option answers
 * from the reductions, and StdUser's sort3, whose ordering runs through the
 * compare clause the contract incorporated from ashstd.traits. Runs under
 * ASan and LSan, so every instance allocation the lowered bodies make is
 * watched. */

#include <ash/ash.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, what)                                    \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "[test_std] FAIL: %s\n", what);  \
            g_fail = 1;                                      \
        }                                                    \
    } while (0)

static AshValue int_val(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

static AshValue float_val(double f) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_FLOAT;
    v.as.f = f;
    return v;
}

static AshValue int_list(AshValue* elems, uint64_t n) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_LIST;
    v.as.list.data = elems;
    v.as.list.len = n;
    v.as.list.cap = n;
    v.as.list.elem_ty = ASH_TY_INT;
    return v;
}

/* The boxed Int behind an Ok result. */
static int ok_int(const AshValue* v, int64_t want) {
    if (v->ty != ASH_TY_RESULT || v->tag != 0 || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    return p->ty == ASH_TY_INT && p->as.i == want;
}

static int ok_float(const AshValue* v, double want) {
    if (v->ty != ASH_TY_RESULT || v->tag != 0 || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    return p->ty == ASH_TY_FLOAT && p->as.f == want;
}

static int ok_bool(const AshValue* v, int want) {
    if (v->ty != ASH_TY_RESULT || v->tag != 0 || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    return p->ty == ASH_TY_BOOL && p->as.b == (want ? 1 : 0);
}

static int ok_str(const AshValue* v, const char* want) {
    if (v->ty != ASH_TY_RESULT || v->tag != 0 || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    if (p->ty != ASH_TY_STRING) return 0;
    if (p->as.s.len != strlen(want)) return 0;
    return memcmp(p->as.s.ptr, want, p->as.s.len) == 0;
}

/* The sum behind an Err result: the box holds the error value and its tag is
 * the variant's declaration index. */
static int err_sum(const AshValue* v, uint32_t variant) {
    if (v->ty != ASH_TY_RESULT || v->tag != 1 || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    return p->ty == ASH_TY_SUM && p->tag == variant;
}

static int some_int(const AshValue* v, int64_t want) {
    if (v->ty != ASH_TY_OPTION || v->tag != 1 || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    return p->ty == ASH_TY_INT && p->as.i == want;
}

static int is_none(const AshValue* v) {
    return v->ty == ASH_TY_OPTION && v->tag == 0;
}

/* The boxed List<Int> behind an Ok result, element for element. */
static int ok_int_list(const AshValue* v, const int64_t* want, uint64_t n) {
    if (v->ty != ASH_TY_RESULT || v->tag != 0 || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    if (p->ty != ASH_TY_LIST || p->as.list.len != n) return 0;
    const AshValue* el = (const AshValue*)p->as.list.data;
    for (uint64_t i = 0; i < n; i++) {
        if (el[i].ty != ASH_TY_INT || el[i].as.i != want[i]) return 0;
    }
    return 1;
}

static AshValue run(AshContract* c, const char* name, AshValue* args,
                    size_t nargs, const char* what) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, name, args, nargs, NULL, 0, &out) ==
              ASH_OK,
          what);
    return out;
}

/* AshMathError: DomainError 0, Overflowed 1. CommonError: NotFound 0,
 * Invalid 1, Exhausted 2. The declaration order is the tag order. */
enum { MATH_DOMAIN = 0, MATH_OVERFLOWED = 1 };
enum { COMMON_INVALID = 1 };

static void drive_mathops(AshRuntime* rt) {
    AshContract* c = NULL;
    CHECK(ash_contract_sign(rt, "MathOps", NULL, 0, 0, &c) == ASH_OK,
          "sign MathOps");
    if (!c) return;

    AshValue out;
    AshValue a1[1];
    AshValue a2[2];
    AshValue a3[3];

    a1[0] = int_val(-5);
    out = run(c, "abs", a1, 1, "abs -5 runs");
    CHECK(ok_int(&out, 5), "abs(-5) is Ok(5)");

    /* The one Int whose magnitude does not fit reports the overflow arm. */
    a1[0] = int_val(INT64_MIN);
    out = run(c, "abs", a1, 1, "abs INT64_MIN runs");
    CHECK(err_sum(&out, MATH_OVERFLOWED), "abs(INT64_MIN) is Err(Overflowed)");

    a1[0] = int_val(-9);
    out = run(c, "sign", a1, 1, "sign -9 runs");
    CHECK(ok_int(&out, -1), "sign(-9) is Ok(-1)");
    a1[0] = int_val(0);
    out = run(c, "sign", a1, 1, "sign 0 runs");
    CHECK(ok_int(&out, 0), "sign(0) is Ok(0)");

    a2[0] = int_val(3);
    a2[1] = int_val(8);
    out = run(c, "min", a2, 2, "min runs");
    CHECK(ok_int(&out, 3), "min(3,8) is Ok(3)");
    out = run(c, "max", a2, 2, "max runs");
    CHECK(ok_int(&out, 8), "max(3,8) is Ok(8)");

    a3[0] = int_val(15);
    a3[1] = int_val(0);
    a3[2] = int_val(10);
    out = run(c, "clamp", a3, 3, "clamp high runs");
    CHECK(ok_int(&out, 10), "clamp(15,0,10) is Ok(10)");
    a3[0] = int_val(-2);
    out = run(c, "clamp", a3, 3, "clamp low runs");
    CHECK(ok_int(&out, 0), "clamp(-2,0,10) is Ok(0)");
    a3[0] = int_val(5);
    a3[1] = int_val(9);
    a3[2] = int_val(1);
    out = run(c, "clamp", a3, 3, "clamp inverted runs");
    CHECK(err_sum(&out, MATH_DOMAIN),
          "clamp with lo > hi is Err(DomainError)");

    a2[0] = int_val(2);
    a2[1] = int_val(10);
    out = run(c, "pow_int", a2, 2, "pow_int runs");
    CHECK(ok_int(&out, 1024), "pow_int(2,10) is Ok(1024)");
    a2[1] = int_val(-1);
    out = run(c, "pow_int", a2, 2, "pow_int negative exp runs");
    CHECK(err_sum(&out, MATH_DOMAIN), "pow_int(2,-1) is Err(DomainError)");
    a2[1] = int_val(64);
    out = run(c, "pow_int", a2, 2, "pow_int overflow runs");
    CHECK(err_sum(&out, MATH_OVERFLOWED), "pow_int(2,64) is Err(Overflowed)");

    a1[0] = float_val(-2.5);
    out = run(c, "abs_f", a1, 1, "abs_f runs");
    CHECK(ok_float(&out, 2.5), "abs_f(-2.5) is Ok(2.5)");
    out = run(c, "sign_f", a1, 1, "sign_f runs");
    CHECK(ok_float(&out, -1.0), "sign_f(-2.5) is Ok(-1.0)");

    a3[0] = float_val(0.75);
    a3[1] = float_val(0.0);
    a3[2] = float_val(0.5);
    out = run(c, "clamp_f", a3, 3, "clamp_f runs");
    CHECK(ok_float(&out, 0.5), "clamp_f(0.75,0.0,0.5) is Ok(0.5)");

    /* approx_eq reads the epsilon vow the contract signed with. */
    a2[0] = float_val(1.0);
    a2[1] = float_val(1.0000001);
    out = run(c, "approx_eq", a2, 2, "approx_eq near runs");
    CHECK(ok_bool(&out, 1), "approx_eq within epsilon is Ok(true)");
    a2[1] = float_val(1.5);
    out = run(c, "approx_eq", a2, 2, "approx_eq far runs");
    CHECK(ok_bool(&out, 0), "approx_eq outside epsilon is Ok(false)");

    CHECK(ash_contract_break(c) == ASH_OK, "break MathOps");
}

static void drive_listops(AshRuntime* rt) {
    AshContract* c = NULL;
    CHECK(ash_contract_sign(rt, "ListOps", NULL, 0, 0, &c) == ASH_OK,
          "sign ListOps");
    if (!c) return;

    AshValue out;
    AshValue e[4] = { int_val(4), int_val(7), int_val(4), int_val(2) };
    AshValue a1[1];
    AshValue a2[2];

    a1[0] = int_list(e, 4);
    out = run(c, "sum", a1, 1, "sum runs");
    CHECK(ok_int(&out, 17), "sum [4,7,4,2] is Ok(17)");
    out = run(c, "product", a1, 1, "product runs");
    CHECK(ok_int(&out, 224), "product [4,7,4,2] is Ok(224)");
    out = run(c, "min_of", a1, 1, "min_of runs");
    CHECK(some_int(&out, 2), "min_of [4,7,4,2] is Some(2)");
    out = run(c, "max_of", a1, 1, "max_of runs");
    CHECK(some_int(&out, 7), "max_of [4,7,4,2] is Some(7)");

    a1[0] = int_list(NULL, 0);
    out = run(c, "min_of", a1, 1, "min_of [] runs");
    CHECK(is_none(&out), "min_of [] is None");
    out = run(c, "sum", a1, 1, "sum [] runs");
    CHECK(ok_int(&out, 0), "sum [] is Ok(0)");

    a2[0] = int_list(e, 4);
    a2[1] = int_val(7);
    out = run(c, "contains", a2, 2, "contains hit runs");
    CHECK(ok_bool(&out, 1), "contains 7 is Ok(true)");
    a2[1] = int_val(9);
    out = run(c, "contains", a2, 2, "contains miss runs");
    CHECK(ok_bool(&out, 0), "contains 9 is Ok(false)");

    a2[1] = int_val(4);
    out = run(c, "count_of", a2, 2, "count_of runs");
    CHECK(ok_int(&out, 2), "count_of 4 is Ok(2)");
    out = run(c, "index_of", a2, 2, "index_of hit runs");
    CHECK(some_int(&out, 0), "index_of 4 is Some(0)");
    a2[1] = int_val(2);
    out = run(c, "index_of", a2, 2, "index_of tail runs");
    CHECK(some_int(&out, 3), "index_of 2 is Some(3)");
    a2[1] = int_val(11);
    out = run(c, "index_of", a2, 2, "index_of miss runs");
    CHECK(is_none(&out), "index_of 11 is None");

    a2[1] = int_val(4);
    out = run(c, "all_eq", a2, 2, "all_eq mixed runs");
    CHECK(ok_bool(&out, 0), "all_eq over mixed is Ok(false)");
    AshValue same[3] = { int_val(4), int_val(4), int_val(4) };
    a2[0] = int_list(same, 3);
    out = run(c, "all_eq", a2, 2, "all_eq uniform runs");
    CHECK(ok_bool(&out, 1), "all_eq over uniform is Ok(true)");

    /* product overflow trips the checked multiply, Err(Invalid). */
    AshValue big[2] = { int_val(INT64_MAX / 2), int_val(3) };
    a1[0] = int_list(big, 2);
    out = run(c, "product", a1, 1, "product overflow runs");
    CHECK(err_sum(&out, COMMON_INVALID), "product overflow is Err(Invalid)");

    CHECK(ash_contract_break(c) == ASH_OK, "break ListOps");
}

static void drive_stduser(AshRuntime* rt) {
    AshContract* c = NULL;
    CHECK(ash_contract_sign(rt, "StdUser", NULL, 0, 0, &c) == ASH_OK,
          "sign StdUser");
    if (!c) return;

    AshValue out;
    AshValue a3[3] = { int_val(3), int_val(1), int_val(2) };
    out = run(c, "sort3", a3, 3, "sort3 runs");
    {
        const int64_t want[3] = { 1, 2, 3 };
        CHECK(ok_int_list(&out, want, 3), "sort3(3,1,2) is Ok([1,2,3])");
    }
    AshValue r3[3] = { int_val(9), int_val(9), int_val(-4) };
    out = run(c, "sort3", r3, 3, "sort3 with a tie runs");
    {
        const int64_t want[3] = { -4, 9, 9 };
        CHECK(ok_int_list(&out, want, 3), "sort3(9,9,-4) is Ok([-4,9,9])");
    }

    AshValue a1[1];
    a1[0] = int_val(12);
    out = run(c, "check_positive", a1, 1, "check_positive 12 runs");
    CHECK(ok_int(&out, 12), "check_positive(12) is Ok(12)");
    a1[0] = int_val(-3);
    out = run(c, "check_positive", a1, 1, "check_positive -3 runs");
    CHECK(err_sum(&out, COMMON_INVALID), "check_positive(-3) is Err(Invalid)");

    a1[0] = int_val(-3);
    out = run(c, "describe", a1, 1, "describe -3 runs");
    CHECK(ok_str(&out, "[stduser] negative"),
          "describe(-3) decorates through log_line");

    CHECK(ash_contract_break(c) == ASH_OK, "break StdUser");
}

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) {
        fprintf(stderr, "[test_std] FAIL: runtime init\n");
        return 1;
    }
    CHECK(ash_module_load(rt, "target/ashc-out/libstd_user.ash.so") == ASH_OK,
          "module load");

    drive_mathops(rt);
    drive_listops(rt);
    drive_stduser(rt);

    ash_runtime_shutdown(rt);
    if (g_fail) return 1;
    fprintf(stderr, "[test_std] ok\n");
    return 0;
}
