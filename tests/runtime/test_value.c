/* test_value.c: the runtime's deep value gate. No compiled module here, the
 * descriptors are handwritten so the test drives libashrt the way any host
 * embedding it directly would: register a contract, sign an instance as an
 * arena, build nested values with the deep helpers, deep copy them, and
 * prove the copy shares nothing with its source. The copy-in half runs a
 * real pledge and mutates the host's bytes after the call to prove the thunk
 * only ever saw instance owned memory. Runs under ASan and LSan; a single
 * leaked block fails the build. */

#include <ash/ash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "[test_value] FAIL: %s (%s:%d)\n", what,       \
                    __FILE__, __LINE__);                                   \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static AshValue str_val(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static AshValue int_val(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

/* Structural equality, deep, for everything deep copy supports. */
static int value_eq(const AshValue* a, const AshValue* b) {
    if (a->ty != b->ty || a->tag != b->tag) return 0;
    switch ((AshTypeTag)a->ty) {
    case ASH_TY_UNIT:   return 1;
    case ASH_TY_INT:    return a->as.i == b->as.i;
    case ASH_TY_UINT:   return a->as.u == b->as.u;
    case ASH_TY_FLOAT:  return a->as.f == b->as.f;
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:   return a->as.b == b->as.b;
    case ASH_TY_CHAR:   return a->as.ch == b->as.ch;
    case ASH_TY_STRING: return ash_string_eq(a, b);
    case ASH_TY_LIST:
    case ASH_TY_TUPLE: {
        if (a->as.list.len != b->as.list.len) return 0;
        for (uint64_t i = 0; i < a->as.list.len; i++) {
            if (!value_eq(ash_list_get(a, i), ash_list_get(b, i))) return 0;
        }
        return 1;
    }
    case ASH_TY_OPTION:
    case ASH_TY_RESULT: {
        if (!a->as.box && !b->as.box) return 1;
        if (!a->as.box || !b->as.box) return 0;
        return value_eq((const AshValue*)a->as.box,
                        (const AshValue*)b->as.box);
    }
    default:
        return 0;
    }
}

/* ---- the capture pledge: the copy-in isolation witness ---- */

/* The thunk stashes what it saw so the test can look at it after mutating
 * the host bytes the argument came from. The pointer aims into the frame on
 * the instance, valid until break. */
static const AshValue* g_seen = NULL;

static AshStatus capture_fn(void* ctx, const AshValue* args, size_t nargs,
                            AshValue* out) {
    (void)ctx;
    if (nargs != 1 || args[0].ty != ASH_TY_STRING) return ASH_ERR_TYPE;
    g_seen = &args[0];
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_UNIT;
    return ASH_OK;
}

static const AshPledgeDesc k_capture_pledges[] = {
    { "capture", "__ash_test_capture", 1, capture_fn, -1 },
};

static const AshContractDesc k_arena = {
    .name = "Arena", .shape_hash = 0x1ULL, .version = 1,
};

static const AshContractDesc k_capture = {
    .name = "Capture", .shape_hash = 0x2ULL, .version = 1,
    .npledges = 1, .pledges = k_capture_pledges,
};

/* ---- the tests ---- */

static void test_list_of_tuples(AshContract* src_arena, AshContract* dst_arena) {
    AshValue list;
    CHECK(ash_list_new(src_arena, ASH_TY_TUPLE, 2, &list) == ASH_OK,
          "list_new");

    const char* names[] = { "alpha", "beta", "gamma" };
    for (int i = 0; i < 3; i++) {
        AshValue tup;
        CHECK(ash_tuple_new(src_arena, 2, &tup) == ASH_OK, "tuple_new");
        AshValue* slots = (AshValue*)tup.as.list.data;
        AshValue s = str_val(names[i]);
        CHECK(ash_value_deep_copy(src_arena, &s, &slots[0]) == ASH_OK,
              "deep copy string into tuple");
        slots[1] = int_val(i * 10);
        CHECK(ash_list_push(src_arena, &list, &tup) == ASH_OK, "list_push");
    }
    CHECK(list.as.list.len == 3, "list length after pushes");
    CHECK(list.as.list.cap >= 3, "list grew past its initial capacity");

    /* Push of a mismatched element is refused. */
    AshValue wrong = int_val(7);
    CHECK(ash_list_push(src_arena, &list, &wrong) == ASH_ERR_TYPE,
          "push of the wrong element type");

    /* Out of range and non-list gets are NULL. */
    CHECK(ash_list_get(&list, 3) == NULL, "get past the end");
    CHECK(ash_list_get(&wrong, 0) == NULL, "get on a non-list");

    AshValue copy;
    CHECK(ash_value_deep_copy(dst_arena, &list, &copy) == ASH_OK,
          "deep copy of the nested list");
    CHECK(value_eq(&list, &copy), "copy is structurally equal");
    CHECK(copy.as.list.data != list.as.list.data,
          "copy owns its own element array");

    const AshValue* t0 = ash_list_get(&list, 0);
    const AshValue* c0 = ash_list_get(&copy, 0);
    CHECK(t0 && c0 && t0->as.list.data != c0->as.list.data,
          "copied tuple owns its own slots");
    const AshValue* s0 = ash_list_get(t0, 0);
    const AshValue* cs0 = ash_list_get(c0, 0);
    CHECK(s0 && cs0 && s0->as.s.ptr != cs0->as.s.ptr,
          "copied string owns its own bytes");
}

static void test_option_result_nesting(AshContract* arena,
                                       AshContract* dst_arena) {
    /* Err(Some("deep")) : a Result wrapping an Option wrapping a String. */
    AshValue inner_str = ash_string_copy(arena, (const uint8_t*)"deep", 4);

    AshValue some;
    memset(&some, 0, sizeof(some));
    some.ty = ASH_TY_OPTION;
    some.tag = 1;
    some.as.box = ash_box(arena);
    *(AshValue*)some.as.box = inner_str;

    AshValue err;
    memset(&err, 0, sizeof(err));
    err.ty = ASH_TY_RESULT;
    err.tag = 1;
    err.as.box = ash_box(arena);
    *(AshValue*)err.as.box = some;

    AshValue copy;
    CHECK(ash_value_deep_copy(dst_arena, &err, &copy) == ASH_OK,
          "deep copy of Err(Some(String))");
    CHECK(value_eq(&err, &copy), "nested copy is structurally equal");
    CHECK(copy.as.box != err.as.box, "copied Result owns its own box");
    const AshValue* copt = (const AshValue*)copy.as.box;
    const AshValue* oopt = (const AshValue*)err.as.box;
    CHECK(copt->as.box != oopt->as.box, "copied Option owns its own box");

    /* None carries a null box and copies to one. */
    AshValue none;
    memset(&none, 0, sizeof(none));
    none.ty = ASH_TY_OPTION;
    CHECK(ash_value_deep_copy(dst_arena, &none, &copy) == ASH_OK,
          "deep copy of None");
    CHECK(copy.ty == ASH_TY_OPTION && copy.tag == 0 && copy.as.box == NULL,
          "None copies to None");

    /* Maps are v1 unsupported and refuse loudly. */
    AshValue map;
    memset(&map, 0, sizeof(map));
    map.ty = ASH_TY_MAP;
    CHECK(ash_value_deep_copy(dst_arena, &map, &copy) == ASH_ERR_TYPE,
          "deep copy of a map reports ASH_ERR_TYPE");
}

static void test_copy_in_isolation(AshContract* cap) {
    /* The host's mutable bytes. The fulfill deep copies them onto the
     * instance; scribbling over them afterwards must not reach the thunk's
     * view. */
    char buf[] = "original";
    AshValue arg;
    memset(&arg, 0, sizeof(arg));
    arg.ty = ASH_TY_STRING;
    arg.as.s.ptr = (uint8_t*)buf;
    arg.as.s.len = 8;

    AshValue out;
    CHECK(ash_pledge_fulfill_sync(cap, "capture", &arg, 1, NULL, 0, &out) ==
              ASH_OK,
          "capture fulfill");
    memset(buf, 'X', 8);

    CHECK(g_seen != NULL, "the thunk saw a frame");
    if (g_seen) {
        CHECK(g_seen->ty == ASH_TY_STRING && g_seen->as.s.len == 8,
              "captured slot shape");
        CHECK(memcmp(g_seen->as.s.ptr, "original", 8) == 0,
              "captured bytes untouched by the host mutation");
        CHECK(g_seen->as.s.ptr != (uint8_t*)buf,
              "captured slot does not alias host memory");
    }
}

int main(void) {
    AshRuntime* rt = NULL;
    CHECK(ash_runtime_init(NULL, &rt) == ASH_OK, "runtime init");
    CHECK(ash_register_contract(rt, &k_arena) == ASH_OK, "register Arena");
    CHECK(ash_register_contract(rt, &k_capture) == ASH_OK, "register Capture");

    AshContract* src = NULL;
    AshContract* dst = NULL;
    AshContract* cap = NULL;
    CHECK(ash_contract_sign(rt, "Arena", NULL, 0, 0, &src) == ASH_OK,
          "sign the source arena");
    CHECK(ash_contract_sign(rt, "Arena", NULL, 0, 0, &dst) == ASH_OK,
          "sign the destination arena");
    CHECK(ash_contract_sign(rt, "Capture", NULL, 0, 0, &cap) == ASH_OK,
          "sign Capture");

    if (src && dst && cap) {
        test_list_of_tuples(src, dst);
        test_option_result_nesting(src, dst);
        test_copy_in_isolation(cap);
    }

    CHECK(ash_contract_break(src) == ASH_OK, "break the source arena");
    CHECK(ash_contract_break(dst) == ASH_OK, "break the destination arena");
    CHECK(ash_contract_break(cap) == ASH_OK, "break Capture");
    ash_runtime_shutdown(rt);

    if (g_failures) {
        fprintf(stderr, "[test_value] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_value] ok\n");
    return 0;
}
