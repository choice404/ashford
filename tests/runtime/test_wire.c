/* test_wire.c: the value codec gate. The codec is a library, the canonical
 * encoding the park row serializes through, and this test pins it. Three
 * claims are pinned. Goldens: every fixture in tests/wire is the canonical
 * encoding of a value this file builds by hand, and the encoder must
 * reproduce each one exactly. Canonicity: decoding a golden and encoding the
 * result reproduces the golden, byte for byte. Refusal: a negative corpus of
 * truncations, forbidden tags, lying lengths, and a depth bomb must each land
 * a status, never a crash, with every partial allocation still owned by the
 * decode arena and reclaimed at its break. Runs under ASan and LSan.
 *
 * Regenerating the goldens after a deliberate format change:
 *
 *   ./target/geas-out/test_wire --emit tests/wire
 *
 * and review the byte diffs like any other golden. */

#include <geas/geas_wire.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "[test_wire] FAIL: %s (%s:%d)\n", what,        \
                    __FILE__, __LINE__);                                   \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/* ---- little endian writers for hand built corpus bytes ---- */

typedef struct Buf {
    uint8_t b[640];
    size_t  n;
} Buf;

static void bput32(Buf* w, uint32_t v) {
    w->b[w->n++] = (uint8_t)v;
    w->b[w->n++] = (uint8_t)(v >> 8);
    w->b[w->n++] = (uint8_t)(v >> 16);
    w->b[w->n++] = (uint8_t)(v >> 24);
}

static void bput64(Buf* w, uint64_t v) {
    bput32(w, (uint32_t)v);
    bput32(w, (uint32_t)(v >> 32));
}

static void bhdr(Buf* w, uint32_t ty, uint32_t tag) {
    bput32(w, ty);
    bput32(w, tag);
}

/* ---- scalar value shorthand ---- */

static GeasValue zeroed(uint32_t ty) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ty;
    return v;
}

static GeasValue int_val(int64_t i) {
    GeasValue v = zeroed(GEAS_TY_INT);
    v.as.i = i;
    return v;
}

static GeasValue bool_val(uint8_t b) {
    GeasValue v = zeroed(GEAS_TY_BOOL);
    v.as.b = b;
    return v;
}

static GeasValue str_borrow(const char* s) {
    GeasValue v = zeroed(GEAS_TY_STRING);
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

/* ---- the fixtures, one builder per golden ---- */

static GeasValue mk_unit(GeasContract* a) {
    (void)a;
    return zeroed(GEAS_TY_UNIT);
}

static GeasValue mk_int(GeasContract* a) {
    (void)a;
    return int_val(-42);
}

static GeasValue mk_uint(GeasContract* a) {
    (void)a;
    GeasValue v = zeroed(GEAS_TY_UINT);
    v.as.u = 0xDEADBEEFCAFEBABEULL;
    return v;
}

static GeasValue mk_float(GeasContract* a) {
    (void)a;
    GeasValue v = zeroed(GEAS_TY_FLOAT);
    v.as.f = -2.5;
    return v;
}

static GeasValue mk_bool(GeasContract* a) {
    (void)a;
    return bool_val(1);
}

static GeasValue mk_byte(GeasContract* a) {
    (void)a;
    GeasValue v = zeroed(GEAS_TY_BYTE);
    v.as.b = 0xA5;
    return v;
}

static GeasValue mk_char(GeasContract* a) {
    (void)a;
    GeasValue v = zeroed(GEAS_TY_CHAR);
    v.as.ch = 0x1F600; /* a scalar value past the BMP, four UTF-8 bytes */
    return v;
}

/* "héllo, 世界": multibyte UTF-8, length counted in bytes. */
static const char k_str[] = "h\xC3\xA9llo, \xE4\xB8\x96\xE7\x95\x8C";

static GeasValue mk_string(GeasContract* a) {
    return geas_string_copy(a, (const uint8_t*)k_str, strlen(k_str));
}

/* [[1, 2], [3]]: a list of lists, the nested composite arm. */
static GeasValue mk_list(GeasContract* a) {
    GeasValue outer;
    geas_list_new(a, GEAS_TY_LIST, 2, &outer);
    GeasValue in1, in2;
    geas_list_new(a, GEAS_TY_INT, 2, &in1);
    GeasValue e = int_val(1);
    geas_list_push(a, &in1, &e);
    e = int_val(2);
    geas_list_push(a, &in1, &e);
    geas_list_new(a, GEAS_TY_INT, 1, &in2);
    e = int_val(3);
    geas_list_push(a, &in2, &e);
    geas_list_push(a, &outer, &in1);
    geas_list_push(a, &outer, &in2);
    return outer;
}

/* ("alpha", 10, true) */
static GeasValue mk_tuple(GeasContract* a) {
    GeasValue t;
    geas_tuple_new(a, 3, &t);
    GeasValue* slots = (GeasValue*)t.as.list.data;
    slots[0] = geas_string_copy(a, (const uint8_t*)"alpha", 5);
    slots[1] = int_val(10);
    slots[2] = bool_val(1);
    return t;
}

/* {"card", 42}: two fields in declaration order. */
static GeasValue mk_record(GeasContract* a) {
    GeasValue* fields = (GeasValue*)geas_bytes(a, 2 * sizeof(GeasValue));
    fields[0] = geas_string_copy(a, (const uint8_t*)"card", 4);
    fields[1] = int_val(42);
    GeasValue v = zeroed(GEAS_TY_RECORD);
    v.as.list.data = fields;
    v.as.list.len = 2;
    v.as.list.cap = 2;
    return v;
}

/* #1("overflow", 7): a payload carrying variant. */
static GeasValue mk_sum(GeasContract* a) {
    GeasValue* payload = (GeasValue*)geas_bytes(a, 2 * sizeof(GeasValue));
    payload[0] = geas_string_copy(a, (const uint8_t*)"overflow", 8);
    payload[1] = int_val(7);
    GeasValue v = zeroed(GEAS_TY_SUM);
    v.tag = 1;
    v.as.list.data = payload;
    v.as.list.len = 2;
    v.as.list.cap = 2;
    return v;
}

static GeasValue mk_none(GeasContract* a) {
    (void)a;
    return zeroed(GEAS_TY_OPTION);
}

static GeasValue mk_some(GeasContract* a) {
    GeasValue v = zeroed(GEAS_TY_OPTION);
    v.tag = 1;
    v.as.box = geas_box(a);
    *(GeasValue*)v.as.box = int_val(7);
    return v;
}

static GeasValue mk_result_ok(GeasContract* a) {
    GeasValue v = zeroed(GEAS_TY_RESULT);
    v.as.box = geas_box(a);
    *(GeasValue*)v.as.box = bool_val(1);
    return v;
}

/* Err(Some("deep")): the boxed shapes nest. */
static GeasValue mk_result_err(GeasContract* a) {
    GeasValue some = zeroed(GEAS_TY_OPTION);
    some.tag = 1;
    some.as.box = geas_box(a);
    *(GeasValue*)some.as.box = geas_string_copy(a, (const uint8_t*)"deep", 4);
    GeasValue v = zeroed(GEAS_TY_RESULT);
    v.tag = 1;
    v.as.box = geas_box(a);
    *(GeasValue*)v.as.box = some;
    return v;
}

/* {"ada": 37, "bob": 40}: insertion order is the order on the wire. */
static GeasValue mk_map(GeasContract* a) {
    GeasValue m = geas_map_new(a, GEAS_TY_STRING);
    GeasValue k = str_borrow("ada");
    GeasValue v = int_val(37);
    geas_map_set(a, &m, &k, &v);
    k = str_borrow("bob");
    v = int_val(40);
    geas_map_set(a, &m, &k, &v);
    return m;
}

typedef struct Fixture {
    const char* name;
    GeasValue (*build)(GeasContract*);
} Fixture;

static const Fixture k_fixtures[] = {
    { "unit", mk_unit },
    { "int", mk_int },
    { "uint", mk_uint },
    { "float", mk_float },
    { "bool", mk_bool },
    { "byte", mk_byte },
    { "char", mk_char },
    { "string", mk_string },
    { "list", mk_list },
    { "tuple", mk_tuple },
    { "record", mk_record },
    { "sum", mk_sum },
    { "option_none", mk_none },
    { "option_some", mk_some },
    { "result_ok", mk_result_ok },
    { "result_err", mk_result_err },
    { "map", mk_map },
};

enum { N_FIXTURES = sizeof(k_fixtures) / sizeof(k_fixtures[0]) };

/* ---- file helpers ---- */

static uint8_t* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t* buf = (uint8_t*)malloc(n ? (size_t)n : 1);
    if (buf && n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

static int write_file(const char* path, const uint8_t* buf, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t wrote = len ? fwrite(buf, 1, len, f) : 0;
    fclose(f);
    return wrote == len;
}

/* ---- the frame header gate ---- */

static void test_frame(void) {
    uint8_t hdr[GEAS_WIRE_HEADER_LEN];
    CHECK(geas_wire_frame_write(GEAS_WIRE_FULFILL, 0x1122334455667788ULL, 32,
                               hdr) == GEAS_OK,
          "frame write");
    CHECK(memcmp(hdr, "ASHW", 4) == 0, "magic bytes lead the frame");
    CHECK(hdr[4] == 7 && hdr[5] == 0 && hdr[6] == 0 && hdr[7] == 0,
          "kind is little endian at offset 4");
    CHECK(hdr[8] == 0x88 && hdr[15] == 0x11,
          "request id is little endian at offset 8");
    CHECK(hdr[16] == 32 && hdr[17] == 0 && hdr[18] == 0 && hdr[19] == 0,
          "payload length is little endian at offset 16");

    GeasWireFrame fr;
    CHECK(geas_wire_frame_read(hdr, &fr) == GEAS_OK, "frame read");
    CHECK(fr.kind == GEAS_WIRE_FULFILL &&
              fr.request_id == 0x1122334455667788ULL && fr.payload_len == 32,
          "frame fields round trip");

    uint8_t bad[GEAS_WIRE_HEADER_LEN];
    memcpy(bad, hdr, sizeof(bad));
    bad[0] = 'X';
    CHECK(geas_wire_frame_read(bad, &fr) == GEAS_ERR_TYPE,
          "wrong magic is malformed");

    memcpy(bad, hdr, sizeof(bad));
    bad[4] = 14; /* one past GEAS_WIRE_ERROR */
    CHECK(geas_wire_frame_read(bad, &fr) == GEAS_ERR_TYPE,
          "unknown kind is malformed");
    bad[4] = 0;
    CHECK(geas_wire_frame_read(bad, &fr) == GEAS_ERR_TYPE,
          "kind zero is malformed");

    memcpy(bad, hdr, sizeof(bad));
    bad[16] = 0x01;
    bad[17] = 0x00;
    bad[18] = 0x00;
    bad[19] = 0x04; /* 64 MiB + 1 */
    CHECK(geas_wire_frame_read(bad, &fr) == GEAS_ERR_OOM,
          "a payload past the cap is refused with GEAS_ERR_OOM");

    CHECK(geas_wire_frame_write(GEAS_WIRE_HELLO, 1, GEAS_WIRE_MAX_PAYLOAD,
                               hdr) == GEAS_OK,
          "the cap itself is allowed");
    CHECK(geas_wire_frame_write(GEAS_WIRE_HELLO, 1, GEAS_WIRE_MAX_PAYLOAD + 1,
                               hdr) == GEAS_ERR_OOM,
          "one past the cap is refused at write too");
    CHECK(geas_wire_frame_write(0, 1, 0, hdr) == GEAS_ERR_TYPE,
          "kind zero refuses at write");
    CHECK(geas_wire_frame_write(14, 1, 0, hdr) == GEAS_ERR_TYPE,
          "kind fourteen refuses at write");
}

/* ---- the golden gate, per fixture ---- */

static void test_fixture(GeasContract* enc, GeasContract* dec, const char* dir,
                         const Fixture* fx) {
    GeasValue v = fx->build(enc);

    /* The size protocol: the sizing call reports the exact need, a cap one
     * short is refused with nothing written. */
    size_t need = 0;
    CHECK(geas_wire_encode_value(&v, NULL, 0, &need) == GEAS_ERR_OOM,
          "sizing call reports GEAS_ERR_OOM");
    CHECK(need >= 8, "every encoding carries at least its header");

    uint8_t* enc_buf = (uint8_t*)malloc(need + 1);
    enc_buf[0] = 0x5A;
    if (need > 1) {
        CHECK(geas_wire_encode_value(&v, enc_buf, need - 1, &need) ==
                  GEAS_ERR_OOM,
              "a short cap is refused");
        CHECK(enc_buf[0] == 0x5A, "a short cap writes nothing");
    }
    CHECK(geas_wire_encode_value(&v, enc_buf, need, &need) == GEAS_OK,
          "encode with room succeeds");

    /* The golden: the checked in bytes are the canonical encoding. */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bin", dir, fx->name);
    size_t glen = 0;
    uint8_t* golden = read_file(path, &glen);
    CHECK(golden != NULL, path);
    if (!golden) {
        free(enc_buf);
        return;
    }
    CHECK(glen == need && memcmp(enc_buf, golden, need) == 0,
          "encoder reproduces the golden byte for byte");
    if (glen != need) {
        free(golden);
        free(enc_buf);
        return;
    }

    /* Decode onto the decode arena and compare structurally. */
    GeasValue back;
    size_t consumed = 0;
    CHECK(geas_wire_decode_value(dec, golden, glen, &back, &consumed) ==
              GEAS_OK,
          "golden decodes");
    CHECK(consumed == glen, "decode consumes the whole golden");
    CHECK(geas_value_eq(&v, &back), "decoded value equals the source");

    /* Canonicity: encode(decode(bytes)) == bytes. */
    size_t need2 = 0;
    CHECK(geas_wire_encode_value(&back, NULL, 0, &need2) == GEAS_ERR_OOM &&
              need2 == glen,
          "re-encode sizes like the golden");
    uint8_t* re = (uint8_t*)malloc(need2 ? need2 : 1);
    CHECK(geas_wire_encode_value(&back, re, need2, &need2) == GEAS_OK &&
              memcmp(re, golden, glen) == 0,
          "re-encode reproduces the golden byte for byte");
    free(re);

    /* Trailing bytes after a complete value are the caller's business. */
    memcpy(enc_buf, golden, glen);
    enc_buf[glen] = 0xEE;
    CHECK(geas_wire_decode_value(dec, enc_buf, glen + 1, &back, &consumed) ==
                  GEAS_OK &&
              consumed == glen,
          "a trailing byte is left unconsumed");

    /* Every strict prefix is an incomplete value and must refuse. */
    for (size_t cut = 0; cut < glen; cut++) {
        GeasValue partial;
        size_t used = 1;
        GeasStatus st = geas_wire_decode_value(dec, golden, cut, &partial,
                                             &used);
        if (st == GEAS_OK || used != 0) {
            CHECK(0, "a truncated golden must refuse");
            break;
        }
    }

    free(golden);
    free(enc_buf);
}

/* ---- the negative corpus ---- */

static void expect_malformed(GeasContract* dec, const Buf* w,
                             const char* what) {
    GeasValue out;
    size_t used = 1;
    GeasStatus st = geas_wire_decode_value(dec, w->b, w->n, &out, &used);
    CHECK(st != GEAS_OK && used == 0, what);
}

static void test_negative(GeasContract* dec) {
    Buf w;

    memset(&w, 0, sizeof(w));
    bhdr(&w, 999, 0);
    expect_malformed(dec, &w, "an unknown type tag is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_INSTANCE, 0);
    expect_malformed(dec, &w, "GEAS_TY_INSTANCE never crosses");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_PLEDGE_REF, 0);
    expect_malformed(dec, &w, "GEAS_TY_PLEDGE_REF never crosses");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_INT, 1);
    bput64(&w, 5);
    expect_malformed(dec, &w, "a nonzero tag on a scalar is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_OPTION, 2);
    expect_malformed(dec, &w, "an Option tag past Some is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_BOOL, 0);
    w.b[w.n++] = 2;
    expect_malformed(dec, &w, "a Bool byte past one is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_CHAR, 0);
    bput32(&w, 0xD800);
    expect_malformed(dec, &w, "a surrogate is not a scalar value");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_CHAR, 0);
    bput32(&w, 0x110000);
    expect_malformed(dec, &w, "a char past U+10FFFF is malformed");

    /* A string length far past the buffer, and one past it by a single
     * byte: both lies, both refused before a read. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_STRING, 0);
    bput64(&w, 0xFFFFFFFFFFFFFFFFULL);
    w.b[w.n++] = 'x';
    expect_malformed(dec, &w, "a huge string length is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_STRING, 0);
    bput64(&w, 6);
    memcpy(w.b + w.n, "hello", 5);
    w.n += 5;
    expect_malformed(dec, &w, "a string length one past its bytes lies");

    /* A list count no payload could cover: refused by arithmetic, no giant
     * allocation ever attempted. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_LIST, 0);
    bput64(&w, 0x7FFFFFFFFFFFFFFFULL);
    bput32(&w, GEAS_TY_INT);
    expect_malformed(dec, &w, "an implausible list count is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_LIST, 0);
    bput64(&w, 1);
    bput32(&w, GEAS_TY_INSTANCE);
    bhdr(&w, GEAS_TY_INSTANCE, 0);
    expect_malformed(dec, &w, "a forbidden element tag is malformed");

    /* An element disagreeing with the declared elem_ty. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_LIST, 0);
    bput64(&w, 1);
    bput32(&w, GEAS_TY_INT);
    bhdr(&w, GEAS_TY_BOOL, 0);
    w.b[w.n++] = 1;
    expect_malformed(dec, &w, "an element off its declared type lies");

    /* Maps: an odd slot count, an unkeyable key tag, a key slot off the
     * declared tag, and a duplicate key. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_MAP, 0);
    bput64(&w, 3);
    bput32(&w, GEAS_TY_INT);
    expect_malformed(dec, &w, "an odd map slot count is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_MAP, 0);
    bput64(&w, 0);
    bput32(&w, GEAS_TY_FLOAT);
    expect_malformed(dec, &w, "an unkeyable key tag is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_MAP, 0);
    bput64(&w, 2);
    bput32(&w, GEAS_TY_STRING);
    bhdr(&w, GEAS_TY_INT, 0);
    bput64(&w, 1);
    bhdr(&w, GEAS_TY_INT, 0);
    bput64(&w, 2);
    expect_malformed(dec, &w, "a key slot off the declared tag lies");

    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_MAP, 0);
    bput64(&w, 4);
    bput32(&w, GEAS_TY_INT);
    bhdr(&w, GEAS_TY_INT, 0);
    bput64(&w, 7);
    bhdr(&w, GEAS_TY_INT, 0);
    bput64(&w, 1);
    bhdr(&w, GEAS_TY_INT, 0);
    bput64(&w, 7);
    bhdr(&w, GEAS_TY_INT, 0);
    bput64(&w, 2);
    expect_malformed(dec, &w, "a duplicate map key is malformed");

    /* A tuple that promises two elements and delivers one. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, GEAS_TY_TUPLE, 0);
    bput64(&w, 2);
    bhdr(&w, GEAS_TY_UNIT, 0);
    expect_malformed(dec, &w, "an undelivered arity is malformed");
}

/* ---- the depth cap, both sides ---- */

/* levels nested Options, Some all the way down to a final None, as bytes. */
static void nest_bytes(Buf* w, uint32_t levels) {
    memset(w, 0, sizeof(*w));
    for (uint32_t i = 0; i + 1 < levels; i++) bhdr(w, GEAS_TY_OPTION, 1);
    bhdr(w, GEAS_TY_OPTION, 0);
}

/* The same nest as a value on the arena. */
static GeasValue nest_value(GeasContract* a, uint32_t levels) {
    GeasValue v = zeroed(GEAS_TY_OPTION);
    for (uint32_t i = 0; i + 1 < levels; i++) {
        GeasValue* box = geas_box(a);
        *box = v;
        v = zeroed(GEAS_TY_OPTION);
        v.tag = 1;
        v.as.box = box;
    }
    return v;
}

static void test_depth(GeasContract* enc, GeasContract* dec) {
    Buf w;
    GeasValue out;
    size_t used = 0;

    nest_bytes(&w, GEAS_WIRE_MAX_DEPTH);
    CHECK(geas_wire_decode_value(dec, w.b, w.n, &out, &used) == GEAS_OK,
          "a nest at the cap decodes");

    nest_bytes(&w, GEAS_WIRE_MAX_DEPTH + 1);
    used = 1;
    CHECK(geas_wire_decode_value(dec, w.b, w.n, &out, &used) == GEAS_ERR_TYPE &&
              used == 0,
          "a nest past the cap is malformed");

    size_t need = 0;
    GeasValue deep = nest_value(enc, GEAS_WIRE_MAX_DEPTH);
    CHECK(geas_wire_encode_value(&deep, NULL, 0, &need) == GEAS_ERR_OOM,
          "a value at the cap sizes");
    deep = nest_value(enc, GEAS_WIRE_MAX_DEPTH + 1);
    CHECK(geas_wire_encode_value(&deep, NULL, 0, &need) == GEAS_ERR_TYPE,
          "a value past the cap refuses to encode");
}

/* ---- encoder refusals ---- */

static void test_encode_refusals(void) {
    size_t need = 0;
    GeasValue v = zeroed(GEAS_TY_INSTANCE);
    CHECK(geas_wire_encode_value(&v, NULL, 0, &need) == GEAS_ERR_TYPE,
          "encoder refuses GEAS_TY_INSTANCE");
    v = zeroed(GEAS_TY_PLEDGE_REF);
    CHECK(geas_wire_encode_value(&v, NULL, 0, &need) == GEAS_ERR_TYPE,
          "encoder refuses GEAS_TY_PLEDGE_REF");

    /* A hand built list whose element disagrees with its elem_ty. */
    GeasValue elem = bool_val(1);
    GeasValue list = zeroed(GEAS_TY_LIST);
    list.as.list.data = &elem;
    list.as.list.len = 1;
    list.as.list.cap = 1;
    list.as.list.elem_ty = GEAS_TY_INT;
    CHECK(geas_wire_encode_value(&list, NULL, 0, &need) == GEAS_ERR_TYPE,
          "encoder refuses an element off its elem_ty");

    CHECK(geas_wire_encode_value(NULL, NULL, 0, &need) == GEAS_ERR_TYPE,
          "encoder refuses a NULL value");
}

/* ---- the emit mode: regenerate the goldens ---- */

static int emit_goldens(GeasContract* enc, const char* dir) {
    for (size_t i = 0; i < N_FIXTURES; i++) {
        GeasValue v = k_fixtures[i].build(enc);
        size_t need = 0;
        if (geas_wire_encode_value(&v, NULL, 0, &need) != GEAS_ERR_OOM) {
            fprintf(stderr, "[test_wire] emit: sizing %s failed\n",
                    k_fixtures[i].name);
            return 1;
        }
        uint8_t* buf = (uint8_t*)malloc(need);
        if (!buf ||
            geas_wire_encode_value(&v, buf, need, &need) != GEAS_OK) {
            fprintf(stderr, "[test_wire] emit: encoding %s failed\n",
                    k_fixtures[i].name);
            free(buf);
            return 1;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.bin", dir, k_fixtures[i].name);
        if (!write_file(path, buf, need)) {
            fprintf(stderr, "[test_wire] emit: writing %s failed\n", path);
            free(buf);
            return 1;
        }
        fprintf(stderr, "[test_wire] wrote %s (%zu bytes)\n", path, need);
        free(buf);
    }
    return 0;
}

/* ---- main ---- */

static const GeasContractDesc k_arena = {
    .name = "WireArena", .shape_hash = 0x11ULL, .version = 1,
};

int main(int argc, char** argv) {
    const char* dir = "tests/wire";
    int emit = 0;
    if (argc >= 2 && strcmp(argv[1], "--emit") == 0) {
        emit = 1;
        if (argc >= 3) dir = argv[2];
    } else if (argc >= 2) {
        dir = argv[1];
    }

    GeasRuntime* rt = NULL;
    CHECK(geas_runtime_init(NULL, &rt) == GEAS_OK, "runtime init");
    CHECK(geas_register_contract(rt, &k_arena) == GEAS_OK, "register arena");
    GeasContract* enc = NULL;
    GeasContract* dec = NULL;
    CHECK(geas_contract_sign(rt, "WireArena", NULL, 0, 0, &enc) == GEAS_OK,
          "sign the encode arena");
    CHECK(geas_contract_sign(rt, "WireArena", NULL, 0, 0, &dec) == GEAS_OK,
          "sign the decode arena");

    int rc = 0;
    if (enc && dec) {
        if (emit) {
            rc = emit_goldens(enc, dir);
        } else {
            test_frame();
            for (size_t i = 0; i < N_FIXTURES; i++) {
                test_fixture(enc, dec, dir, &k_fixtures[i]);
            }
            test_negative(dec);
            test_depth(enc, dec);
            test_encode_refusals();
        }
    }

    /* The break reclaims everything the decodes hung on the arenas, the
     * failed decodes included; LSan holds the codec to zero leaks. */
    CHECK(geas_contract_break(enc) == GEAS_OK, "break the encode arena");
    CHECK(geas_contract_break(dec) == GEAS_OK, "break the decode arena");
    geas_runtime_shutdown(rt);

    if (rc) return rc;
    if (g_failures) {
        fprintf(stderr, "[test_wire] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_wire] %s\n", emit ? "goldens emitted" : "ok");
    return 0;
}
