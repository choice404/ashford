/* test_header.c: the emit-header gate's C half. This host includes the
 * header ashc emitted for skeleton/hello.ash and drives discovery with the
 * two names it publishes and nothing else hardcoded: resolve the pledge's
 * mangled symbol through the iname table, check the entry hands back exactly
 * the shape hash the header spells, sign under that hash, and prove a wrong
 * hash is refused with ASH_ERR_VERSION. That is the product claim of the
 * generated header: a C host compiles against it and signs safely without
 * ever computing a hash itself. Runs under ASan and LSan. */

#include <ash/ash.h>
#include "hello.ash.h"

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
static AshStatus stub_shout(void* ctx, const AshValue* args, size_t nargs,
                            AshValue* out) {
    (void)ctx;
    (void)args;
    if (nargs != 1) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_UNIT;
    return ASH_OK;
}

int main(void) {
    AshRuntime* rt = NULL;
    CHECK(ash_runtime_init(NULL, &rt) == ASH_OK, "runtime init");
    CHECK(ash_module_load(rt, "target/ashc-out/libhello.ash.so") == ASH_OK,
          "load the hello module");

    AshInameEntry e;
    memset(&e, 0, sizeof(e));
    CHECK(ash_iname_lookup(rt, ASH_MANGLED_Greeter_greet, &e) == ASH_OK,
          "resolve the header's mangled greet");
    CHECK(e.kind == ASH_INAME_PLEDGE, "greet resolves as a pledge");
    CHECK(strcmp(e.contract, "Greeter") == 0, "greet belongs to Greeter");
    CHECK(e.shape_hash == ASH_HASH_Greeter,
          "the iname hash matches the header's ASH_HASH_Greeter");

    CHECK(ash_iname_lookup(rt, ASH_MANGLED_Greeter_shout, &e) == ASH_OK,
          "resolve the header's mangled shout");
    CHECK(e.shape_hash == ASH_HASH_Greeter, "shout carries the same hash");

    CHECK(ash_pledge_bind(rt, "Greeter.shout", stub_shout) == ASH_OK,
          "bind a body over the abstract shout");
    ash_runtime_freeze(rt);

    AshContract* c = NULL;
    CHECK(ash_contract_sign(rt, e.contract, NULL, 0, ASH_HASH_Greeter, &c) ==
              ASH_OK,
          "sign Greeter under the header's hash");
    CHECK(ash_contract_sign(rt, e.contract, NULL, 0, ASH_HASH_Greeter ^ 1,
                            &c) == ASH_ERR_VERSION,
          "a wrong expected hash is refused");

    CHECK(ash_contract_break(c) == ASH_OK, "break the signed instance");
    ash_runtime_shutdown(rt);

    if (g_failures) {
        fprintf(stderr, "[test_header] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_header] ok\n");
    return 0;
}
