/* host.c: the C half of the walking skeleton. This is a foreign program, it
 * knows nothing about ashc, it only links libashrt and speaks the ABI header.
 * It loads the compiled module and walks the whole M3 surface: sign on the
 * declared defaults and demand exactly Ok("hello, world"), sign again with a
 * vow override and demand the override showed up, fulfill through a future
 * and wait it exactly once, prove a second wait is a state error, read the
 * signature the instance carries and prove a wrong expected hash is refused,
 * and exercise the lifecycle errors on both sides of break. It exits zero
 * only when every check held. valgrind runs this and expects silence. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>

static int fail(const char* what) {
    fprintf(stderr, "[host] FAIL: %s\n", what);
    return 1;
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

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return fail("runtime init");

    if (ash_module_load(rt, "target/ashc-out/libhello.ash.so") != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("module load");
    }

    /* ---- the default path: sign on the declared vow defaults ---- */

    AshContract* c = NULL;
    if (ash_contract_sign(rt, "Greeter", NULL, 0, 0, &c) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign");
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
    if (ash_pledge_fulfill_sync(c, "greet", &name, 1, &out) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("fulfill greet");
    }
    if (!check_ok_string(&out, "hello, world")) {
        ash_runtime_shutdown(rt);
        return fail("default greeting mismatch");
    }
    const AshValue* inner = (const AshValue*)out.as.box;
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

    /* ---- the override path: sign with a vow binding ---- */

    AshVowBinding prefix;
    prefix.name = "prefix";
    prefix.value = str_arg("hey, ");
    AshContract* c3 = NULL;
    if (ash_contract_sign(rt, "Greeter", &prefix, 1, 0, &c3) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("sign with vow override");
    }
    if (ash_pledge_fulfill_sync(c3, "greet", &name, 1, &out) != ASH_OK) {
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

    AshFuture* f = ash_pledge_fulfill(c3, "greet", &name, 1);
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
    if (ash_pledge_fulfill_sync(c, "greet", &name, 1, &scratch) != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("fulfill after break did not report ASH_ERR_STATE");
    }
    AshFuture* f2 = ash_pledge_fulfill(c, "greet", &name, 1);
    if (!f2 || ash_future_wait(f2, &scratch) != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("future after break did not report ASH_ERR_STATE");
    }

    ash_runtime_shutdown(rt);
    fprintf(stderr, "[host] ok\n");
    return 0;
}
