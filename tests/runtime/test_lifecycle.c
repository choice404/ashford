/* test_lifecycle.c: the instance surface gate. The compiled lifecycle module
 * walks sign, status(), the vow read through an instance, park, break, and
 * resume entirely inside the language, and answers the stations as one
 * string; this host only signs Driver, points its park_dsn vow at a temp
 * file, and asserts the answer. The two refusal pledges pin the fault
 * convention: a park against a dsn that will not open faults the enclosing
 * fulfillment with GEAS_ERR_STORE, and a resume of a key nobody parked
 * faults it with GEAS_ERR_NAME, the statuses the C surface answers, riding
 * the thunk's exit convention. Runs under ASan and LSan; the instance a
 * faulted pledge leaves behind is the runtime shutdown's to reclaim, which
 * the leak check proves it does. */

#include <geas/geas.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0;

#define CHECK(cond, what)                                          \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "[test_lifecycle] FAIL: %s\n", what);  \
            g_fail = 1;                                            \
        }                                                          \
    } while (0)

static GeasValue str_val(const char* s) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static int ok_string(const GeasValue* r, const char* want) {
    if (r->ty != GEAS_TY_RESULT || r->tag != 0 || !r->as.box) return 0;
    const GeasValue* p = (const GeasValue*)r->as.box;
    if (p->ty != GEAS_TY_STRING) return 0;
    if (p->as.s.len != strlen(want)) return 0;
    return memcmp(p->as.s.ptr, want, p->as.s.len) == 0;
}

int main(void) {
    char db_path[] = "target/ashlife_XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) {
        fprintf(stderr, "[test_lifecycle] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);
    char dsn[96];
    snprintf(dsn, sizeof(dsn), "file:%s", db_path);

    GeasRuntime* rt = NULL;
    if (geas_runtime_init(NULL, &rt) != GEAS_OK || !rt) {
        fprintf(stderr, "[test_lifecycle] FAIL: runtime init\n");
        return 1;
    }
    CHECK(geas_module_load(rt, "target/geas-out/liblifecycle.geas.so") == GEAS_OK,
          "load lifecycle module");

    GeasVowBinding dsn_ovr = { "park_dsn", str_val(dsn) };
    GeasContract* drv = NULL;
    CHECK(geas_contract_sign(rt, "Driver", &dsn_ovr, 1, 0, &drv) == GEAS_OK,
          "sign Driver");

    /* The whole walk, one answer: status at sign, after the fulfillment,
     * and after the resume, with the vow read back through both the signed
     * and the resumed instance. */
    GeasValue out;
    memset(&out, 0, sizeof(out));
    CHECK(geas_pledge_fulfill_sync(drv, "walk", NULL, 0, NULL, 0, &out) ==
              GEAS_OK,
          "walk delivers");
    CHECK(ok_string(&out, "Signed,Fulfilled,Fulfilled,alpha,alpha"),
          "the stations read Signed,Fulfilled,Fulfilled,alpha,alpha");

    /* The partial surface as a value: the record before any fulfillment and
     * after one, states and name lists read as fields inside the language. */
    memset(&out, 0, sizeof(out));
    CHECK(geas_pledge_fulfill_sync(drv, "panel", NULL, 0, NULL, 0, &out) ==
              GEAS_OK,
          "panel delivers");
    CHECK(ok_string(&out, "Signed,Partial,first,second"),
          "the panel reads Signed,Partial,first,second");

    /* The fault convention: the language's park and resume answer the same
     * statuses the C surface answers, through the thunk's exit. */
    memset(&out, 0, sizeof(out));
    CHECK(geas_pledge_fulfill_sync(drv, "bad_park", NULL, 0, NULL, 0, &out) ==
              GEAS_ERR_STORE,
          "a park against a dead dsn faults GEAS_ERR_STORE");
    memset(&out, 0, sizeof(out));
    CHECK(geas_pledge_fulfill_sync(drv, "bad_resume", NULL, 0, NULL, 0, &out) ==
              GEAS_ERR_NAME,
          "a resume of an unparked key faults GEAS_ERR_NAME");

    CHECK(geas_contract_break(drv) == GEAS_OK, "break Driver");
    geas_runtime_shutdown(rt);
    unlink(db_path);

    if (g_fail) return 1;
    printf("[test_lifecycle] ok\n");
    return 0;
}
