/* test_header.c: the emit-header gate's C half. This host includes the
 * header geas emitted for skeleton/hello.geas and drives discovery with the
 * two names it publishes and nothing else hardcoded: resolve the pledge's
 * mangled symbol through the iname table, check the entry hands back exactly
 * the shape hash the header spells, sign under that hash, and prove a wrong
 * hash is refused with GEAS_ERR_VERSION. That is the product claim of the
 * generated header: a C host compiles against it and signs safely without
 * ever computing a hash itself. Runs under ASan and LSan. */

#include <geas/geas.h>
#include "hello.geas.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "[test_header] FAIL: %s (%s:%d)\n", what,       \
                    __FILE__, __LINE__);                                    \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* Greeter.shout is abstract; signing needs a body bound over it. */
static GeasStatus stub_shout(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out) {
    (void)ctx;
    (void)args;
    if (nargs != 1) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_UNIT;
    return GEAS_OK;
}

int main(void) {
    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "runtime init");
    CHECK(geas_module_load(rt, "target/geas-out/libhello.geas.so") == GEAS_OK,
          "load the hello module");

    GeasInameEntry e;
    memset(&e, 0, sizeof(e));
    CHECK(geas_iname_lookup(rt, GEAS_MANGLED_Greeter_greet, &e) == GEAS_OK,
          "resolve the header's mangled greet");
    CHECK(e.kind == GEAS_INAME_PLEDGE, "greet resolves as a pledge");
    CHECK(strcmp(e.contract, "Greeter") == 0, "greet belongs to Greeter");
    CHECK(e.shape_hash == GEAS_HASH_Greeter,
          "the iname hash matches the header's GEAS_HASH_Greeter");

    CHECK(geas_iname_lookup(rt, GEAS_MANGLED_Greeter_shout, &e) == GEAS_OK,
          "resolve the header's mangled shout");
    CHECK(e.shape_hash == GEAS_HASH_Greeter, "shout carries the same hash");

    CHECK(geas_pledge_bind(rt, "Greeter.shout", stub_shout) == GEAS_OK,
          "bind a body over the abstract shout");
    geas_runtime_freeze(rt);

    GeasContract* c = NULL;
    CHECK(geas_contract_sign(rt, e.contract, NULL, 0, GEAS_HASH_Greeter, &c) ==
              GEAS_OK,
          "sign Greeter under the header's hash");
    CHECK(geas_contract_sign(rt, e.contract, NULL, 0, GEAS_HASH_Greeter ^ 1,
                            &c) == GEAS_ERR_VERSION,
          "a wrong expected hash is refused");

    CHECK(geas_contract_break(c) == GEAS_OK, "break the signed instance");
    geas_runtime_shutdown(rt);

    if (g_failures) {
        fprintf(stderr, "[test_header] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_header] ok\n");
    return 0;
}
