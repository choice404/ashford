/* test_lang.c: the language lowering gate. It loads the compiled gauntlet
 * module and drives every pledge of skeleton/lang.ash from C, asserting the
 * value each construct produces: loop accumulation over a host built list,
 * both propagation paths of '?', every arm of the sum and Result matches,
 * assignment through record fields and list slots, deep equality, and the
 * out of bounds index, which the ABI pins as ASH_ERR_TYPE from the thunk.
 * Runs under ASan and LSan, so every instance allocation the lowered bodies
 * make is watched. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, what)                                     \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "[test_lang] FAIL: %s\n", what);  \
            g_fail = 1;                                       \
        }                                                     \
    } while (0)

static AshValue int_val(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

static AshValue bool_val(int b) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_BOOL;
    v.as.b = b ? 1 : 0;
    return v;
}

/* A host built List<Int> pointing at the caller's element array; the fulfill
 * deep copies it onto the instance, so stack storage is fine. */
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

/* The boxed Int behind an Ok or Err result. */
static int result_int(const AshValue* v, uint32_t tag, int64_t want) {
    if (v->ty != ASH_TY_RESULT || v->tag != tag || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    return p->ty == ASH_TY_INT && p->as.i == want;
}

static int result_bool(const AshValue* v, uint32_t tag, int want) {
    if (v->ty != ASH_TY_RESULT || v->tag != tag || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    return p->ty == ASH_TY_BOOL && p->as.b == (want ? 1 : 0);
}

static int result_str(const AshValue* v, uint32_t tag, const char* want) {
    if (v->ty != ASH_TY_RESULT || v->tag != tag || !v->as.box) return 0;
    const AshValue* p = (const AshValue*)v->as.box;
    if (p->ty != ASH_TY_STRING) return 0;
    if (p->as.s.len != strlen(want)) return 0;
    return memcmp(p->as.s.ptr, want, p->as.s.len) == 0;
}

static AshValue run1(AshContract* c, const char* name, AshValue arg,
                     const char* what) {
    AshValue out;
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, name, &arg, 1, NULL, 0, &out) == ASH_OK,
          what);
    return out;
}

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) {
        fprintf(stderr, "[test_lang] FAIL: runtime init\n");
        return 1;
    }
    CHECK(ash_module_load(rt, "target/ashc-out/liblang.ash.so") == ASH_OK,
          "module load");

    AshContract* c = NULL;
    CHECK(ash_contract_sign(rt, "LangGauntlet", NULL, 0, 0, &c) == ASH_OK,
          "sign LangGauntlet");
    if (!c) {
        ash_runtime_shutdown(rt);
        return 1;
    }

    AshValue out;

    /* ---- sum_list: for loop accumulation, empty list included ---- */

    AshValue e123[3] = { int_val(1), int_val(2), int_val(3) };
    out = run1(c, "sum_list", int_list(e123, 3), "sum_list [1,2,3]");
    CHECK(result_int(&out, 0, 6), "sum_list [1,2,3] is Ok(6)");
    out = run1(c, "sum_list", int_list(NULL, 0), "sum_list []");
    CHECK(result_int(&out, 0, 0), "sum_list [] is Ok(0)");

    /* ---- classify: Bool match, sum payload binding, record slots ---- */

    out = run1(c, "classify", int_val(-4), "classify -4");
    CHECK(result_int(&out, 1, 1), "classify -4 takes the Fail arm, Err(1)");
    out = run1(c, "classify", int_val(3), "classify 3");
    CHECK(result_str(&out, 0, "small"), "classify 3 is Ok(small)");
    out = run1(c, "classify", int_val(20), "classify 20");
    CHECK(result_str(&out, 0, "big"), "classify 20 is Ok(big)");

    /* ---- fizz: while, continue, %, String +, unary minus ---- */

    out = run1(c, "fizz", int_val(6), "fizz 6");
    CHECK(result_str(&out, 0, "..f..f"), "fizz 6 is Ok(..f..f)");
    out = run1(c, "fizz", int_val(0), "fizz 0");
    CHECK(result_str(&out, 0, ""), "fizz 0 runs the while zero times");
    out = run1(c, "fizz", int_val(-3), "fizz -3");
    CHECK(result_int(&out, 1, 3), "fizz -3 is Err(3)");

    /* ---- find: for with break ---- */

    AshValue e45[2] = { int_val(4), int_val(5) };
    AshValue find_hit[2] = { int_list(e45, 2), int_val(5) };
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, "find", find_hit, 2, NULL, 0, &out) ==
              ASH_OK,
          "find hit runs");
    CHECK(result_bool(&out, 0, 1), "find 5 in [4,5] is Ok(true)");
    AshValue find_miss[2] = { int_list(e45, 2), int_val(6) };
    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, "find", find_miss, 2, NULL, 0, &out) ==
              ASH_OK,
          "find miss runs");
    CHECK(result_bool(&out, 0, 0), "find 6 in [4,5] is Ok(false)");

    /* ---- pick: list literal, index assign, index read, OOB rule ---- */

    out = run1(c, "pick", int_val(0), "pick 0");
    CHECK(result_int(&out, 0, 10), "pick 0 is Ok(10)");
    out = run1(c, "pick", int_val(1), "pick 1");
    CHECK(result_int(&out, 0, 21), "pick 1 reads the assigned slot, Ok(21)");
    memset(&out, 0, sizeof(out));
    AshValue oob = int_val(5);
    CHECK(ash_pledge_fulfill_sync(c, "pick", &oob, 1, NULL, 0, &out) ==
              ASH_ERR_TYPE,
          "pick 5 out of bounds is ASH_ERR_TYPE");
    memset(&out, 0, sizeof(out));
    AshValue neg = int_val(-1);
    CHECK(ash_pledge_fulfill_sync(c, "pick", &neg, 1, NULL, 0, &out) ==
              ASH_ERR_TYPE,
          "pick -1 is ASH_ERR_TYPE through the unsigned cast");

    /* ---- head_doubled: both paths of '?' ---- */

    out = run1(c, "head_doubled", int_list(NULL, 0), "head_doubled []");
    CHECK(out.ty == ASH_TY_OPTION && out.tag == 0,
          "head_doubled [] propagates None");
    AshValue e21[1] = { int_val(21) };
    out = run1(c, "head_doubled", int_list(e21, 1), "head_doubled [21]");
    CHECK(out.ty == ASH_TY_OPTION && out.tag == 1 && out.as.box &&
              ((const AshValue*)out.as.box)->as.i == 42,
          "head_doubled [21] is Some(42)");

    /* ---- nest: match as value, nested match, Result patterns ---- */

    out = run1(c, "nest", int_val(0), "nest 0");
    CHECK(result_int(&out, 0, 1), "nest 0 walks the Err arm, Ok(1)");
    out = run1(c, "nest", int_val(3), "nest 3");
    CHECK(result_int(&out, 0, 4), "nest 3 takes the small branch, Ok(4)");
    out = run1(c, "nest", int_val(7), "nest 7");
    CHECK(result_int(&out, 0, 71), "nest 7 takes the big branch, Ok(71)");

    /* ---- deep: nested constructor patterns, Ok(Some(x)) ---- */

    out = run1(c, "deep", int_val(4), "deep 4");
    CHECK(result_int(&out, 0, 5), "deep 4 matches Ok(Some(x)), Ok(5)");
    out = run1(c, "deep", int_val(1), "deep 1");
    CHECK(result_int(&out, 0, 0), "deep 1 matches Ok(None), Ok(0)");
    out = run1(c, "deep", int_val(0), "deep 0");
    CHECK(result_int(&out, 1, 7), "deep 0 falls to the catch all, Err(7)");

    /* ---- misc: scalar literal forms, deep equality, vow read ---- */

    out = run1(c, "misc", bool_val(1), "misc true");
    CHECK(result_str(&out, 0, "lang:tcbus"),
          "misc true is Ok(lang:tcbus)");
    out = run1(c, "misc", bool_val(0), "misc false");
    CHECK(result_int(&out, 1, 2), "misc false is Err(2)");

    /* ---- guard: '&&' short circuit, xs[i] never touched out of range ---- */

    AshValue g303[3] = { int_val(0), int_val(5), int_val(0) };
    /* i=9 is out of range; a short circuited '&&' stops at i < 3 and never
     * evaluates xs[9], so the call succeeds with Ok(false) rather than the
     * ASH_ERR_TYPE an eager xs[9] would raise. This is the short circuit
     * proof: an eager lowering faults here. */
    {
        AshValue gargs[2] = { int_list(g303, 3), int_val(9) };
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(c, "guard", gargs, 2, NULL, 0, &out) ==
                  ASH_OK,
              "guard i=9 short circuits, no fault");
        CHECK(result_bool(&out, 0, 0), "guard i=9 is Ok(false), not ASH_ERR_TYPE");
    }
    {
        AshValue gargs[2] = { int_list(g303, 3), int_val(0) };
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(c, "guard", gargs, 2, NULL, 0, &out) ==
                  ASH_OK,
              "guard i=0 runs");
        CHECK(result_bool(&out, 0, 1),
              "guard i=0 in range and xs[0]==0 is Ok(true)");
    }
    {
        AshValue gargs[2] = { int_list(g303, 3), int_val(1) };
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(c, "guard", gargs, 2, NULL, 0, &out) ==
                  ASH_OK,
              "guard i=1 runs");
        CHECK(result_bool(&out, 0, 0),
              "guard i=1 in range but xs[1]==5 is Ok(false)");
    }
    {
        AshValue gargs[2] = { int_list(g303, 3), int_val(-1) };
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(c, "guard", gargs, 2, NULL, 0, &out) ==
                  ASH_OK,
              "guard i=-1 short circuits, no fault");
        CHECK(result_bool(&out, 0, 0), "guard i=-1 is Ok(false)");
    }

    /* ---- orguard: '||' short circuit, xs[i] untouched when i>=3 ---- */

    {
        AshValue oargs[2] = { int_list(g303, 3), int_val(9) };
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(c, "orguard", oargs, 2, NULL, 0, &out) ==
                  ASH_OK,
              "orguard i=9 short circuits, no fault");
        CHECK(result_bool(&out, 0, 1),
              "orguard i=9 is Ok(true), xs[9] never evaluated");
    }
    {
        AshValue oargs[2] = { int_list(g303, 3), int_val(1) };
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(c, "orguard", oargs, 2, NULL, 0, &out) ==
                  ASH_OK,
              "orguard i=1 runs");
        CHECK(result_bool(&out, 0, 0),
              "orguard i=1 evaluates xs[1]==5, Ok(false)");
    }

    /* ---- value semantics: a bound copy is independent of its source ---- */

    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, "rec_copy", NULL, 0, NULL, 0, &out) ==
              ASH_OK,
          "rec_copy runs");
    CHECK(result_int(&out, 0, 199),
          "rec_copy: q.x=99 leaves p.x==1, Ok(1*100+99)");

    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, "list_copy", NULL, 0, NULL, 0, &out) ==
              ASH_OK,
          "list_copy runs");
    CHECK(result_int(&out, 0, 199),
          "list_copy: b[0]=99 leaves a[0]==1, Ok(1*100+99)");

    memset(&out, 0, sizeof(out));
    CHECK(ash_pledge_fulfill_sync(c, "nested_copy", NULL, 0, NULL, 0, &out) ==
              ASH_OK,
          "nested_copy runs");
    CHECK(result_int(&out, 0, 199),
          "nested_copy: row read out of the slot is its own copy, Ok(199)");

    CHECK(ash_contract_break(c) == ASH_OK, "break the instance");
    ash_runtime_shutdown(rt);
    if (g_fail) return 1;
    fprintf(stderr, "[test_lang] ok\n");
    return 0;
}
