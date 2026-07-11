/* host.c: the C half of the walking skeleton. This is a foreign program, it
 * knows nothing about ashc, it only links libashrt and speaks the ABI header.
 * It loads the compiled module, signs Greeter, fulfills greet("world"),
 * demands exactly Ok("hello, world"), exercises the state errors on both
 * sides of the lifecycle, breaks the contract, and exits zero only when every
 * check held. valgrind runs this and expects silence. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>

static int fail(const char* what) {
    fprintf(stderr, "[host] FAIL: %s\n", what);
    return 1;
}

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return fail("runtime init");

    if (ash_module_load(rt, "target/ashc-out/libhello.ash.so") != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("module load");
    }

    AshContract* c = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, 0, &c) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign");
    }
    if (ash_contract_state(c) != ASH_SIGNED) {
        ash_runtime_shutdown(rt);
        return fail("state after sign");
    }

    /* The argument is host owned; the runtime never keeps it. */
    AshValue name;
    memset(&name, 0, sizeof(name));
    name.ty = ASH_TY_STRING;
    name.as.s.ptr = (uint8_t*)"world";
    name.as.s.len = 5;

    AshValue out;
    if (ash_pledge_fulfill_sync(c, "greet", &name, 1, &out) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("fulfill greet");
    }
    if (out.ty != ASH_TY_RESULT || out.tag != 0) {
        ash_runtime_shutdown(rt);
        return fail("greet did not return Ok");
    }
    const AshValue* inner = (const AshValue*)out.as.box;
    if (!inner || inner->ty != ASH_TY_STRING) {
        ash_runtime_shutdown(rt);
        return fail("Ok payload is not a string");
    }
    const char* want = "hello, world";
    if (inner->as.s.len != strlen(want) ||
        memcmp(inner->as.s.ptr, want, inner->as.s.len) != 0) {
        ash_runtime_shutdown(rt);
        return fail("greeting mismatch");
    }
    printf("%.*s\n", (int)inner->as.s.len, (const char*)inner->as.s.ptr);

    if (ash_contract_state(c) != ASH_FULFILLED) {
        ash_runtime_shutdown(rt);
        return fail("state after fulfill");
    }

    /* An unknown pledge is a name error, an unknown contract likewise. */
    AshValue scratch;
    if (ash_pledge_fulfill_sync(c, "nope", NULL, 0, &scratch) != ASH_ERR_NAME) {
        ash_runtime_shutdown(rt);
        return fail("unknown pledge did not report ASH_ERR_NAME");
    }
    AshContract* c2 = NULL;
    if (ash_contract_sign(rt, "Nope", NULL, 0, 0, &c2) != ASH_ERR_NAME) {
        ash_runtime_shutdown(rt);
        return fail("unknown contract did not report ASH_ERR_NAME");
    }

    /* Break, then prove the latch: fulfilling a broken contract is a state
     * error, not a crash and not a rerun. */
    if (ash_contract_break(c) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("break");
    }
    if (ash_contract_state(c) != ASH_BROKEN) {
        ash_runtime_shutdown(rt);
        return fail("state after break");
    }
    if (ash_pledge_fulfill_sync(c, "greet", &name, 1, &scratch) != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("fulfill after break did not report ASH_ERR_STATE");
    }

    ash_runtime_shutdown(rt);
    fprintf(stderr, "[host] ok\n");
    return 0;
}
