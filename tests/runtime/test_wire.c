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
 *   ./target/ashc-out/test_wire --emit tests/wire
 *
 * and review the byte diffs like any other golden. */

#include <ash/ash_wire.h>

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

static AshValue zeroed(uint32_t ty) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ty;
    return v;
}

static AshValue int_val(int64_t i) {
    AshValue v = zeroed(ASH_TY_INT);
    v.as.i = i;
    return v;
}

static AshValue bool_val(uint8_t b) {
    AshValue v = zeroed(ASH_TY_BOOL);
    v.as.b = b;
    return v;
}

static AshValue str_borrow(const char* s) {
    AshValue v = zeroed(ASH_TY_STRING);
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

/* ---- the fixtures, one builder per golden ---- */

static AshValue mk_unit(AshContract* a) {
    (void)a;
    return zeroed(ASH_TY_UNIT);
}

static AshValue mk_int(AshContract* a) {
    (void)a;
    return int_val(-42);
}

static AshValue mk_uint(AshContract* a) {
    (void)a;
    AshValue v = zeroed(ASH_TY_UINT);
    v.as.u = 0xDEADBEEFCAFEBABEULL;
    return v;
}

static AshValue mk_float(AshContract* a) {
    (void)a;
    AshValue v = zeroed(ASH_TY_FLOAT);
    v.as.f = -2.5;
    return v;
}

static AshValue mk_bool(AshContract* a) {
    (void)a;
    return bool_val(1);
}

static AshValue mk_byte(AshContract* a) {
    (void)a;
    AshValue v = zeroed(ASH_TY_BYTE);
    v.as.b = 0xA5;
    return v;
}

static AshValue mk_char(AshContract* a) {
    (void)a;
    AshValue v = zeroed(ASH_TY_CHAR);
    v.as.ch = 0x1F600; /* a scalar value past the BMP, four UTF-8 bytes */
    return v;
}

/* "héllo, 世界": multibyte UTF-8, length counted in bytes. */
static const char k_str[] = "h\xC3\xA9llo, \xE4\xB8\x96\xE7\x95\x8C";

static AshValue mk_string(AshContract* a) {
    return ash_string_copy(a, (const uint8_t*)k_str, strlen(k_str));
}

/* [[1, 2], [3]]: a list of lists, the nested composite arm. */
static AshValue mk_list(AshContract* a) {
    AshValue outer;
    ash_list_new(a, ASH_TY_LIST, 2, &outer);
    AshValue in1, in2;
    ash_list_new(a, ASH_TY_INT, 2, &in1);
    AshValue e = int_val(1);
    ash_list_push(a, &in1, &e);
    e = int_val(2);
    ash_list_push(a, &in1, &e);
    ash_list_new(a, ASH_TY_INT, 1, &in2);
    e = int_val(3);
    ash_list_push(a, &in2, &e);
    ash_list_push(a, &outer, &in1);
    ash_list_push(a, &outer, &in2);
    return outer;
}

/* ("alpha", 10, true) */
static AshValue mk_tuple(AshContract* a) {
    AshValue t;
    ash_tuple_new(a, 3, &t);
    AshValue* slots = (AshValue*)t.as.list.data;
    slots[0] = ash_string_copy(a, (const uint8_t*)"alpha", 5);
    slots[1] = int_val(10);
    slots[2] = bool_val(1);
    return t;
}

/* {"card", 42}: two fields in declaration order. */
static AshValue mk_record(AshContract* a) {
    AshValue* fields = (AshValue*)ash_bytes(a, 2 * sizeof(AshValue));
    fields[0] = ash_string_copy(a, (const uint8_t*)"card", 4);
    fields[1] = int_val(42);
    AshValue v = zeroed(ASH_TY_RECORD);
    v.as.list.data = fields;
    v.as.list.len = 2;
    v.as.list.cap = 2;
    return v;
}

/* #1("overflow", 7): a payload carrying variant. */
static AshValue mk_sum(AshContract* a) {
    AshValue* payload = (AshValue*)ash_bytes(a, 2 * sizeof(AshValue));
    payload[0] = ash_string_copy(a, (const uint8_t*)"overflow", 8);
    payload[1] = int_val(7);
    AshValue v = zeroed(ASH_TY_SUM);
    v.tag = 1;
    v.as.list.data = payload;
    v.as.list.len = 2;
    v.as.list.cap = 2;
    return v;
}

static AshValue mk_none(AshContract* a) {
    (void)a;
    return zeroed(ASH_TY_OPTION);
}

static AshValue mk_some(AshContract* a) {
    AshValue v = zeroed(ASH_TY_OPTION);
    v.tag = 1;
    v.as.box = ash_box(a);
    *(AshValue*)v.as.box = int_val(7);
    return v;
}

static AshValue mk_result_ok(AshContract* a) {
    AshValue v = zeroed(ASH_TY_RESULT);
    v.as.box = ash_box(a);
    *(AshValue*)v.as.box = bool_val(1);
    return v;
}

/* Err(Some("deep")): the boxed shapes nest. */
static AshValue mk_result_err(AshContract* a) {
    AshValue some = zeroed(ASH_TY_OPTION);
    some.tag = 1;
    some.as.box = ash_box(a);
    *(AshValue*)some.as.box = ash_string_copy(a, (const uint8_t*)"deep", 4);
    AshValue v = zeroed(ASH_TY_RESULT);
    v.tag = 1;
    v.as.box = ash_box(a);
    *(AshValue*)v.as.box = some;
    return v;
}

/* {"ada": 37, "bob": 40}: insertion order is the order on the wire. */
static AshValue mk_map(AshContract* a) {
    AshValue m = ash_map_new(a, ASH_TY_STRING);
    AshValue k = str_borrow("ada");
    AshValue v = int_val(37);
    ash_map_set(a, &m, &k, &v);
    k = str_borrow("bob");
    v = int_val(40);
    ash_map_set(a, &m, &k, &v);
    return m;
}

typedef struct Fixture {
    const char* name;
    AshValue (*build)(AshContract*);
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
    uint8_t hdr[ASH_WIRE_HEADER_LEN];
    CHECK(ash_wire_frame_write(ASH_WIRE_FULFILL, 0x1122334455667788ULL, 32,
                               hdr) == ASH_OK,
          "frame write");
    CHECK(memcmp(hdr, "ASHW", 4) == 0, "magic bytes lead the frame");
    CHECK(hdr[4] == 7 && hdr[5] == 0 && hdr[6] == 0 && hdr[7] == 0,
          "kind is little endian at offset 4");
    CHECK(hdr[8] == 0x88 && hdr[15] == 0x11,
          "request id is little endian at offset 8");
    CHECK(hdr[16] == 32 && hdr[17] == 0 && hdr[18] == 0 && hdr[19] == 0,
          "payload length is little endian at offset 16");

    AshWireFrame fr;
    CHECK(ash_wire_frame_read(hdr, &fr) == ASH_OK, "frame read");
    CHECK(fr.kind == ASH_WIRE_FULFILL &&
              fr.request_id == 0x1122334455667788ULL && fr.payload_len == 32,
          "frame fields round trip");

    uint8_t bad[ASH_WIRE_HEADER_LEN];
    memcpy(bad, hdr, sizeof(bad));
    bad[0] = 'X';
    CHECK(ash_wire_frame_read(bad, &fr) == ASH_ERR_TYPE,
          "wrong magic is malformed");

    memcpy(bad, hdr, sizeof(bad));
    bad[4] = 14; /* one past ASH_WIRE_ERROR */
    CHECK(ash_wire_frame_read(bad, &fr) == ASH_ERR_TYPE,
          "unknown kind is malformed");
    bad[4] = 0;
    CHECK(ash_wire_frame_read(bad, &fr) == ASH_ERR_TYPE,
          "kind zero is malformed");

    memcpy(bad, hdr, sizeof(bad));
    bad[16] = 0x01;
    bad[17] = 0x00;
    bad[18] = 0x00;
    bad[19] = 0x04; /* 64 MiB + 1 */
    CHECK(ash_wire_frame_read(bad, &fr) == ASH_ERR_OOM,
          "a payload past the cap is refused with ASH_ERR_OOM");

    CHECK(ash_wire_frame_write(ASH_WIRE_HELLO, 1, ASH_WIRE_MAX_PAYLOAD,
                               hdr) == ASH_OK,
          "the cap itself is allowed");
    CHECK(ash_wire_frame_write(ASH_WIRE_HELLO, 1, ASH_WIRE_MAX_PAYLOAD + 1,
                               hdr) == ASH_ERR_OOM,
          "one past the cap is refused at write too");
    CHECK(ash_wire_frame_write(0, 1, 0, hdr) == ASH_ERR_TYPE,
          "kind zero refuses at write");
    CHECK(ash_wire_frame_write(14, 1, 0, hdr) == ASH_ERR_TYPE,
          "kind fourteen refuses at write");
}

/* ---- the golden gate, per fixture ---- */

static void test_fixture(AshContract* enc, AshContract* dec, const char* dir,
                         const Fixture* fx) {
    AshValue v = fx->build(enc);

    /* The size protocol: the sizing call reports the exact need, a cap one
     * short is refused with nothing written. */
    size_t need = 0;
    CHECK(ash_wire_encode_value(&v, NULL, 0, &need) == ASH_ERR_OOM,
          "sizing call reports ASH_ERR_OOM");
    CHECK(need >= 8, "every encoding carries at least its header");

    uint8_t* enc_buf = (uint8_t*)malloc(need + 1);
    enc_buf[0] = 0x5A;
    if (need > 1) {
        CHECK(ash_wire_encode_value(&v, enc_buf, need - 1, &need) ==
                  ASH_ERR_OOM,
              "a short cap is refused");
        CHECK(enc_buf[0] == 0x5A, "a short cap writes nothing");
    }
    CHECK(ash_wire_encode_value(&v, enc_buf, need, &need) == ASH_OK,
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
    AshValue back;
    size_t consumed = 0;
    CHECK(ash_wire_decode_value(dec, golden, glen, &back, &consumed) ==
              ASH_OK,
          "golden decodes");
    CHECK(consumed == glen, "decode consumes the whole golden");
    CHECK(ash_value_eq(&v, &back), "decoded value equals the source");

    /* Canonicity: encode(decode(bytes)) == bytes. */
    size_t need2 = 0;
    CHECK(ash_wire_encode_value(&back, NULL, 0, &need2) == ASH_ERR_OOM &&
              need2 == glen,
          "re-encode sizes like the golden");
    uint8_t* re = (uint8_t*)malloc(need2 ? need2 : 1);
    CHECK(ash_wire_encode_value(&back, re, need2, &need2) == ASH_OK &&
              memcmp(re, golden, glen) == 0,
          "re-encode reproduces the golden byte for byte");
    free(re);

    /* Trailing bytes after a complete value are the caller's business. */
    memcpy(enc_buf, golden, glen);
    enc_buf[glen] = 0xEE;
    CHECK(ash_wire_decode_value(dec, enc_buf, glen + 1, &back, &consumed) ==
                  ASH_OK &&
              consumed == glen,
          "a trailing byte is left unconsumed");

    /* Every strict prefix is an incomplete value and must refuse. */
    for (size_t cut = 0; cut < glen; cut++) {
        AshValue partial;
        size_t used = 1;
        AshStatus st = ash_wire_decode_value(dec, golden, cut, &partial,
                                             &used);
        if (st == ASH_OK || used != 0) {
            CHECK(0, "a truncated golden must refuse");
            break;
        }
    }

    free(golden);
    free(enc_buf);
}

/* ---- the negative corpus ---- */

static void expect_malformed(AshContract* dec, const Buf* w,
                             const char* what) {
    AshValue out;
    size_t used = 1;
    AshStatus st = ash_wire_decode_value(dec, w->b, w->n, &out, &used);
    CHECK(st != ASH_OK && used == 0, what);
}

static void test_negative(AshContract* dec) {
    Buf w;

    memset(&w, 0, sizeof(w));
    bhdr(&w, 999, 0);
    expect_malformed(dec, &w, "an unknown type tag is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_INSTANCE, 0);
    expect_malformed(dec, &w, "ASH_TY_INSTANCE never crosses");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_PLEDGE_REF, 0);
    expect_malformed(dec, &w, "ASH_TY_PLEDGE_REF never crosses");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_INT, 1);
    bput64(&w, 5);
    expect_malformed(dec, &w, "a nonzero tag on a scalar is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_OPTION, 2);
    expect_malformed(dec, &w, "an Option tag past Some is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_BOOL, 0);
    w.b[w.n++] = 2;
    expect_malformed(dec, &w, "a Bool byte past one is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_CHAR, 0);
    bput32(&w, 0xD800);
    expect_malformed(dec, &w, "a surrogate is not a scalar value");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_CHAR, 0);
    bput32(&w, 0x110000);
    expect_malformed(dec, &w, "a char past U+10FFFF is malformed");

    /* A string length far past the buffer, and one past it by a single
     * byte: both lies, both refused before a read. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_STRING, 0);
    bput64(&w, 0xFFFFFFFFFFFFFFFFULL);
    w.b[w.n++] = 'x';
    expect_malformed(dec, &w, "a huge string length is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_STRING, 0);
    bput64(&w, 6);
    memcpy(w.b + w.n, "hello", 5);
    w.n += 5;
    expect_malformed(dec, &w, "a string length one past its bytes lies");

    /* A list count no payload could cover: refused by arithmetic, no giant
     * allocation ever attempted. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_LIST, 0);
    bput64(&w, 0x7FFFFFFFFFFFFFFFULL);
    bput32(&w, ASH_TY_INT);
    expect_malformed(dec, &w, "an implausible list count is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_LIST, 0);
    bput64(&w, 1);
    bput32(&w, ASH_TY_INSTANCE);
    bhdr(&w, ASH_TY_INSTANCE, 0);
    expect_malformed(dec, &w, "a forbidden element tag is malformed");

    /* An element disagreeing with the declared elem_ty. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_LIST, 0);
    bput64(&w, 1);
    bput32(&w, ASH_TY_INT);
    bhdr(&w, ASH_TY_BOOL, 0);
    w.b[w.n++] = 1;
    expect_malformed(dec, &w, "an element off its declared type lies");

    /* Maps: an odd slot count, an unkeyable key tag, a key slot off the
     * declared tag, and a duplicate key. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_MAP, 0);
    bput64(&w, 3);
    bput32(&w, ASH_TY_INT);
    expect_malformed(dec, &w, "an odd map slot count is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_MAP, 0);
    bput64(&w, 0);
    bput32(&w, ASH_TY_FLOAT);
    expect_malformed(dec, &w, "an unkeyable key tag is malformed");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_MAP, 0);
    bput64(&w, 2);
    bput32(&w, ASH_TY_STRING);
    bhdr(&w, ASH_TY_INT, 0);
    bput64(&w, 1);
    bhdr(&w, ASH_TY_INT, 0);
    bput64(&w, 2);
    expect_malformed(dec, &w, "a key slot off the declared tag lies");

    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_MAP, 0);
    bput64(&w, 4);
    bput32(&w, ASH_TY_INT);
    bhdr(&w, ASH_TY_INT, 0);
    bput64(&w, 7);
    bhdr(&w, ASH_TY_INT, 0);
    bput64(&w, 1);
    bhdr(&w, ASH_TY_INT, 0);
    bput64(&w, 7);
    bhdr(&w, ASH_TY_INT, 0);
    bput64(&w, 2);
    expect_malformed(dec, &w, "a duplicate map key is malformed");

    /* A tuple that promises two elements and delivers one. */
    memset(&w, 0, sizeof(w));
    bhdr(&w, ASH_TY_TUPLE, 0);
    bput64(&w, 2);
    bhdr(&w, ASH_TY_UNIT, 0);
    expect_malformed(dec, &w, "an undelivered arity is malformed");
}

/* ---- the depth cap, both sides ---- */

/* levels nested Options, Some all the way down to a final None, as bytes. */
static void nest_bytes(Buf* w, uint32_t levels) {
    memset(w, 0, sizeof(*w));
    for (uint32_t i = 0; i + 1 < levels; i++) bhdr(w, ASH_TY_OPTION, 1);
    bhdr(w, ASH_TY_OPTION, 0);
}

/* The same nest as a value on the arena. */
static AshValue nest_value(AshContract* a, uint32_t levels) {
    AshValue v = zeroed(ASH_TY_OPTION);
    for (uint32_t i = 0; i + 1 < levels; i++) {
        AshValue* box = ash_box(a);
        *box = v;
        v = zeroed(ASH_TY_OPTION);
        v.tag = 1;
        v.as.box = box;
    }
    return v;
}

static void test_depth(AshContract* enc, AshContract* dec) {
    Buf w;
    AshValue out;
    size_t used = 0;

    nest_bytes(&w, ASH_WIRE_MAX_DEPTH);
    CHECK(ash_wire_decode_value(dec, w.b, w.n, &out, &used) == ASH_OK,
          "a nest at the cap decodes");

    nest_bytes(&w, ASH_WIRE_MAX_DEPTH + 1);
    used = 1;
    CHECK(ash_wire_decode_value(dec, w.b, w.n, &out, &used) == ASH_ERR_TYPE &&
              used == 0,
          "a nest past the cap is malformed");

    size_t need = 0;
    AshValue deep = nest_value(enc, ASH_WIRE_MAX_DEPTH);
    CHECK(ash_wire_encode_value(&deep, NULL, 0, &need) == ASH_ERR_OOM,
          "a value at the cap sizes");
    deep = nest_value(enc, ASH_WIRE_MAX_DEPTH + 1);
    CHECK(ash_wire_encode_value(&deep, NULL, 0, &need) == ASH_ERR_TYPE,
          "a value past the cap refuses to encode");
}

/* ---- encoder refusals ---- */

static void test_encode_refusals(void) {
    size_t need = 0;
    AshValue v = zeroed(ASH_TY_INSTANCE);
    CHECK(ash_wire_encode_value(&v, NULL, 0, &need) == ASH_ERR_TYPE,
          "encoder refuses ASH_TY_INSTANCE");
    v = zeroed(ASH_TY_PLEDGE_REF);
    CHECK(ash_wire_encode_value(&v, NULL, 0, &need) == ASH_ERR_TYPE,
          "encoder refuses ASH_TY_PLEDGE_REF");

    /* A hand built list whose element disagrees with its elem_ty. */
    AshValue elem = bool_val(1);
    AshValue list = zeroed(ASH_TY_LIST);
    list.as.list.data = &elem;
    list.as.list.len = 1;
    list.as.list.cap = 1;
    list.as.list.elem_ty = ASH_TY_INT;
    CHECK(ash_wire_encode_value(&list, NULL, 0, &need) == ASH_ERR_TYPE,
          "encoder refuses an element off its elem_ty");

    CHECK(ash_wire_encode_value(NULL, NULL, 0, &need) == ASH_ERR_TYPE,
          "encoder refuses a NULL value");
}

/* ---- the emit mode: regenerate the goldens ---- */

static int emit_goldens(AshContract* enc, const char* dir) {
    for (size_t i = 0; i < N_FIXTURES; i++) {
        AshValue v = k_fixtures[i].build(enc);
        size_t need = 0;
        if (ash_wire_encode_value(&v, NULL, 0, &need) != ASH_ERR_OOM) {
            fprintf(stderr, "[test_wire] emit: sizing %s failed\n",
                    k_fixtures[i].name);
            return 1;
        }
        uint8_t* buf = (uint8_t*)malloc(need);
        if (!buf ||
            ash_wire_encode_value(&v, buf, need, &need) != ASH_OK) {
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

static const AshContractDesc k_arena = {
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

    AshRuntime* rt = NULL;
    CHECK(ash_runtime_init(NULL, &rt) == ASH_OK, "runtime init");
    CHECK(ash_register_contract(rt, &k_arena) == ASH_OK, "register arena");
    AshContract* enc = NULL;
    AshContract* dec = NULL;
    CHECK(ash_contract_sign(rt, "WireArena", NULL, 0, 0, &enc) == ASH_OK,
          "sign the encode arena");
    CHECK(ash_contract_sign(rt, "WireArena", NULL, 0, 0, &dec) == ASH_OK,
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
    CHECK(ash_contract_break(enc) == ASH_OK, "break the encode arena");
    CHECK(ash_contract_break(dec) == ASH_OK, "break the decode arena");
    ash_runtime_shutdown(rt);

    if (rc) return rc;
    if (g_failures) {
        fprintf(stderr, "[test_wire] %d check(s) failed\n", g_failures);
        return 1;
    }
    fprintf(stderr, "[test_wire] %s\n", emit ? "goldens emitted" : "ok");
    return 0;
}
