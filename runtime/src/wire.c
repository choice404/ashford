/* wire.c: the canonical wire codec, docs/network.md made executable. This
 * translation unit knows no sockets; it turns frame headers and AshValues
 * into little endian bytes and back, and both C parties of the network
 * runtime, ashd and the client side of libashrt, link this one
 * implementation.
 *
 * The status vocabulary is deliberately old. A malformed payload is
 * ASH_ERR_TYPE, the same word a thunk uses for a shape it cannot accept; an
 * oversized payload or a failed allocation is ASH_ERR_OOM, the refusal
 * docs/network.md pins for the 64 MiB cap. No new status exists for parsing,
 * because a parse failure is a type failure wearing a longer coat.
 *
 * The decoder trusts nothing. Every count is checked against the bytes that
 * remain before anything is allocated, using the fact that no encoded value
 * is smaller than its 8 byte header, so a count field cannot demand more
 * memory than the payload could possibly justify. Size accumulation in the
 * encoder checks every addition for overflow. Nesting is capped at
 * ASH_WIRE_MAX_DEPTH on both sides. */

#include <ash/ash_wire.h>

#include <stdint.h>
#include <string.h>

/* ---- little endian primitives ---- */

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t* p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t* p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* ---- frame header ---- */

AshStatus ash_wire_frame_write(uint32_t kind, uint64_t request_id,
                               uint32_t payload_len,
                               uint8_t out[ASH_WIRE_HEADER_LEN]) {
    if (!out) return ASH_ERR_TYPE;
    if (kind < ASH_WIRE_HELLO || kind > ASH_WIRE_ERROR) return ASH_ERR_TYPE;
    if (payload_len > ASH_WIRE_MAX_PAYLOAD) return ASH_ERR_OOM;
    memcpy(out, ASH_WIRE_MAGIC, 4);
    put_u32(out + 4, kind);
    put_u64(out + 8, request_id);
    put_u32(out + 16, payload_len);
    return ASH_OK;
}

AshStatus ash_wire_frame_read(const uint8_t in[ASH_WIRE_HEADER_LEN],
                              AshWireFrame* out) {
    if (!in || !out) return ASH_ERR_TYPE;
    if (memcmp(in, ASH_WIRE_MAGIC, 4) != 0) return ASH_ERR_TYPE;
    uint32_t kind = get_u32(in + 4);
    if (kind < ASH_WIRE_HELLO || kind > ASH_WIRE_ERROR) return ASH_ERR_TYPE;
    uint32_t payload_len = get_u32(in + 16);
    if (payload_len > ASH_WIRE_MAX_PAYLOAD) return ASH_ERR_OOM;
    out->kind = kind;
    out->request_id = get_u64(in + 8);
    out->payload_len = payload_len;
    return ASH_OK;
}

/* ---- shared tag rules ---- */

/* A tag a value on the wire may carry. INSTANCE and PLEDGE_REF never cross;
 * anything past the enum is garbage. */
static int tag_crosses(uint32_t ty) {
    return ty <= ASH_TY_SUM && ty != ASH_TY_PLEDGE_REF;
}

/* The keyable scalars a map's elem_ty may name. */
static int tag_keyable(uint32_t ty) {
    switch (ty) {
    case ASH_TY_INT:
    case ASH_TY_UINT:
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:
    case ASH_TY_CHAR:
    case ASH_TY_STRING:
        return 1;
    default:
        return 0;
    }
}

/* ---- encode: the sizing pass ---- */

/* All validation lives here; the write pass below assumes a value this pass
 * blessed. Overflow checked accumulation, because a hostile length field in
 * a value a host built by hand is still a length field. */
static AshStatus size_add(size_t* total, uint64_t n) {
    if (n > SIZE_MAX - *total) return ASH_ERR_OOM;
    *total += (size_t)n;
    return ASH_OK;
}

static AshStatus encode_size(const AshValue* v, uint32_t depth,
                             size_t* total) {
    if (depth > ASH_WIRE_MAX_DEPTH) return ASH_ERR_TYPE;
    AshStatus st = size_add(total, 8); /* the u32 ty, u32 tag header */
    if (st != ASH_OK) return st;

    switch ((AshTypeTag)v->ty) {
    case ASH_TY_UNIT:
        return v->tag == 0 ? ASH_OK : ASH_ERR_TYPE;
    case ASH_TY_INT:
    case ASH_TY_UINT:
    case ASH_TY_FLOAT:
        return v->tag == 0 ? size_add(total, 8) : ASH_ERR_TYPE;
    case ASH_TY_BOOL:
        if (v->tag != 0 || v->as.b > 1) return ASH_ERR_TYPE;
        return size_add(total, 1);
    case ASH_TY_BYTE:
        return v->tag == 0 ? size_add(total, 1) : ASH_ERR_TYPE;
    case ASH_TY_CHAR:
        if (v->tag != 0 || v->as.ch > 0x10FFFF ||
            (v->as.ch >= 0xD800 && v->as.ch <= 0xDFFF))
            return ASH_ERR_TYPE;
        return size_add(total, 4);
    case ASH_TY_STRING:
        if (v->tag != 0) return ASH_ERR_TYPE;
        if (v->as.s.len && !v->as.s.ptr) return ASH_ERR_TYPE;
        if ((st = size_add(total, 8)) != ASH_OK) return st;
        return size_add(total, v->as.s.len);
    case ASH_TY_LIST: {
        if (v->tag != 0) return ASH_ERR_TYPE;
        if (!tag_crosses(v->as.list.elem_ty)) return ASH_ERR_TYPE;
        if (v->as.list.len && !v->as.list.data) return ASH_ERR_TYPE;
        if ((st = size_add(total, 12)) != ASH_OK) return st;
        const AshValue* e = (const AshValue*)v->as.list.data;
        for (uint64_t i = 0; i < v->as.list.len; i++) {
            if (e[i].ty != v->as.list.elem_ty) return ASH_ERR_TYPE;
            if ((st = encode_size(&e[i], depth + 1, total)) != ASH_OK)
                return st;
        }
        return ASH_OK;
    }
    case ASH_TY_MAP: {
        if (v->tag != 0) return ASH_ERR_TYPE;
        if (!tag_keyable(v->as.list.elem_ty)) return ASH_ERR_TYPE;
        if (v->as.list.len % 2) return ASH_ERR_TYPE;
        if (v->as.list.len && !v->as.list.data) return ASH_ERR_TYPE;
        if ((st = size_add(total, 12)) != ASH_OK) return st;
        const AshValue* e = (const AshValue*)v->as.list.data;
        for (uint64_t i = 0; i < v->as.list.len; i++) {
            if (i % 2 == 0 && e[i].ty != v->as.list.elem_ty)
                return ASH_ERR_TYPE;
            if ((st = encode_size(&e[i], depth + 1, total)) != ASH_OK)
                return st;
        }
        return ASH_OK;
    }
    case ASH_TY_TUPLE:
    case ASH_TY_RECORD:
    case ASH_TY_SUM: {
        if (v->ty != ASH_TY_SUM && v->tag != 0) return ASH_ERR_TYPE;
        if (v->as.list.len && !v->as.list.data) return ASH_ERR_TYPE;
        if ((st = size_add(total, 8)) != ASH_OK) return st;
        const AshValue* e = (const AshValue*)v->as.list.data;
        for (uint64_t i = 0; i < v->as.list.len; i++) {
            if ((st = encode_size(&e[i], depth + 1, total)) != ASH_OK)
                return st;
        }
        return ASH_OK;
    }
    case ASH_TY_OPTION:
        if (v->tag > 1) return ASH_ERR_TYPE;
        if (v->tag == 0) return v->as.box ? ASH_ERR_TYPE : ASH_OK;
        if (!v->as.box) return ASH_ERR_TYPE;
        return encode_size((const AshValue*)v->as.box, depth + 1, total);
    case ASH_TY_RESULT:
        if (v->tag > 1 || !v->as.box) return ASH_ERR_TYPE;
        return encode_size((const AshValue*)v->as.box, depth + 1, total);
    default:
        /* ASH_TY_INSTANCE, ASH_TY_PLEDGE_REF, and anything unknown. */
        return ASH_ERR_TYPE;
    }
}

/* ---- encode: the write pass ---- */

/* Runs only over a value the sizing pass accepted, into a buffer the sizing
 * pass measured, so nothing here can fail. */
static void encode_write(const AshValue* v, uint8_t** p) {
    put_u32(*p, v->ty);
    put_u32(*p + 4, v->tag);
    *p += 8;

    switch ((AshTypeTag)v->ty) {
    case ASH_TY_UNIT:
        return;
    case ASH_TY_INT:
        put_u64(*p, (uint64_t)v->as.i);
        *p += 8;
        return;
    case ASH_TY_UINT:
        put_u64(*p, v->as.u);
        *p += 8;
        return;
    case ASH_TY_FLOAT: {
        uint64_t bits;
        memcpy(&bits, &v->as.f, 8);
        put_u64(*p, bits);
        *p += 8;
        return;
    }
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:
        **p = v->as.b;
        *p += 1;
        return;
    case ASH_TY_CHAR:
        put_u32(*p, v->as.ch);
        *p += 4;
        return;
    case ASH_TY_STRING:
        put_u64(*p, v->as.s.len);
        *p += 8;
        if (v->as.s.len) memcpy(*p, v->as.s.ptr, (size_t)v->as.s.len);
        *p += v->as.s.len;
        return;
    case ASH_TY_LIST:
    case ASH_TY_MAP: {
        put_u64(*p, v->as.list.len);
        put_u32(*p + 8, v->as.list.elem_ty);
        *p += 12;
        const AshValue* e = (const AshValue*)v->as.list.data;
        for (uint64_t i = 0; i < v->as.list.len; i++) encode_write(&e[i], p);
        return;
    }
    case ASH_TY_TUPLE:
    case ASH_TY_RECORD:
    case ASH_TY_SUM: {
        put_u64(*p, v->as.list.len);
        *p += 8;
        const AshValue* e = (const AshValue*)v->as.list.data;
        for (uint64_t i = 0; i < v->as.list.len; i++) encode_write(&e[i], p);
        return;
    }
    case ASH_TY_OPTION:
        if (v->tag == 1) encode_write((const AshValue*)v->as.box, p);
        return;
    case ASH_TY_RESULT:
        encode_write((const AshValue*)v->as.box, p);
        return;
    default:
        return; /* unreachable past the sizing pass */
    }
}

AshStatus ash_wire_encode_value(const AshValue* v, uint8_t* buf, size_t cap,
                                size_t* need) {
    if (!v || !need) return ASH_ERR_TYPE;
    size_t total = 0;
    AshStatus st = encode_size(v, 1, &total);
    if (st != ASH_OK) return st;
    *need = total;
    if (!buf || cap < total) return ASH_ERR_OOM;
    uint8_t* p = buf;
    encode_write(v, &p);
    return ASH_OK;
}

/* ---- decode ---- */

/* A cursor over the untrusted bytes. take() is the one gate every read goes
 * through, so a length can lie all it wants and still never reach past the
 * buffer. */
typedef struct WireCur {
    const uint8_t* p;
    size_t left;
} WireCur;

static int take(WireCur* c, size_t n, const uint8_t** out) {
    if (c->left < n) return 0;
    *out = c->p;
    c->p += n;
    c->left -= n;
    return 1;
}

/* A count field claims count encoded values follow. The smallest encoded
 * value is its 8 byte header, so a count the remaining bytes cannot cover at
 * 8 bytes each is a lie, refused before a single byte is allocated. */
static int count_plausible(const WireCur* c, uint64_t count) {
    return count <= c->left / 8;
}

static AshStatus decode_rec(AshContract* owner, WireCur* c, uint32_t depth,
                            AshValue* out) {
    if (depth > ASH_WIRE_MAX_DEPTH) return ASH_ERR_TYPE;
    const uint8_t* h;
    if (!take(c, 8, &h)) return ASH_ERR_TYPE;
    uint32_t ty = get_u32(h);
    uint32_t tag = get_u32(h + 4);
    memset(out, 0, sizeof(*out));
    out->ty = ty;
    out->tag = tag;

    switch ((AshTypeTag)ty) {
    case ASH_TY_UNIT:
        return tag == 0 ? ASH_OK : ASH_ERR_TYPE;
    case ASH_TY_INT:
        if (tag != 0 || !take(c, 8, &h)) return ASH_ERR_TYPE;
        out->as.i = (int64_t)get_u64(h);
        return ASH_OK;
    case ASH_TY_UINT:
        if (tag != 0 || !take(c, 8, &h)) return ASH_ERR_TYPE;
        out->as.u = get_u64(h);
        return ASH_OK;
    case ASH_TY_FLOAT: {
        if (tag != 0 || !take(c, 8, &h)) return ASH_ERR_TYPE;
        uint64_t bits = get_u64(h);
        memcpy(&out->as.f, &bits, 8);
        return ASH_OK;
    }
    case ASH_TY_BOOL:
        if (tag != 0 || !take(c, 1, &h)) return ASH_ERR_TYPE;
        if (*h > 1) return ASH_ERR_TYPE;
        out->as.b = *h;
        return ASH_OK;
    case ASH_TY_BYTE:
        if (tag != 0 || !take(c, 1, &h)) return ASH_ERR_TYPE;
        out->as.b = *h;
        return ASH_OK;
    case ASH_TY_CHAR: {
        if (tag != 0 || !take(c, 4, &h)) return ASH_ERR_TYPE;
        uint32_t ch = get_u32(h);
        if (ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF))
            return ASH_ERR_TYPE; /* not a Unicode scalar value */
        out->as.ch = ch;
        return ASH_OK;
    }
    case ASH_TY_STRING: {
        if (tag != 0 || !take(c, 8, &h)) return ASH_ERR_TYPE;
        uint64_t len = get_u64(h);
        if (len > c->left) return ASH_ERR_TYPE;
        const uint8_t* bytes;
        take(c, (size_t)len, &bytes);
        *out = ash_string_copy(owner, bytes, len);
        if (len && !out->as.s.ptr) return ASH_ERR_OOM;
        return ASH_OK;
    }
    case ASH_TY_LIST: {
        if (tag != 0 || !take(c, 12, &h)) return ASH_ERR_TYPE;
        uint64_t count = get_u64(h);
        uint32_t elem_ty = get_u32(h + 8);
        if (!tag_crosses(elem_ty)) return ASH_ERR_TYPE;
        if (!count_plausible(c, count)) return ASH_ERR_TYPE;
        AshStatus st = ash_list_new(owner, elem_ty, count, out);
        if (st != ASH_OK) return st;
        for (uint64_t i = 0; i < count; i++) {
            AshValue elem;
            if ((st = decode_rec(owner, c, depth + 1, &elem)) != ASH_OK)
                return st;
            if (elem.ty != elem_ty) return ASH_ERR_TYPE;
            if ((st = ash_list_push(owner, out, &elem)) != ASH_OK) return st;
        }
        return ASH_OK;
    }
    case ASH_TY_TUPLE: {
        if (tag != 0 || !take(c, 8, &h)) return ASH_ERR_TYPE;
        uint64_t arity = get_u64(h);
        if (!count_plausible(c, arity)) return ASH_ERR_TYPE;
        AshStatus st = ash_tuple_new(owner, arity, out);
        if (st != ASH_OK) return st;
        AshValue* slots = (AshValue*)out->as.list.data;
        for (uint64_t i = 0; i < arity; i++) {
            if ((st = decode_rec(owner, c, depth + 1, &slots[i])) != ASH_OK)
                return st;
        }
        return ASH_OK;
    }
    case ASH_TY_RECORD:
    case ASH_TY_SUM: {
        if (ty == ASH_TY_RECORD && tag != 0) return ASH_ERR_TYPE;
        if (!take(c, 8, &h)) return ASH_ERR_TYPE;
        uint64_t count = get_u64(h);
        if (!count_plausible(c, count)) return ASH_ERR_TYPE;
        AshValue* fields = NULL;
        if (count) {
            fields = (AshValue*)ash_bytes(owner,
                                          count * sizeof(AshValue));
            if (!fields) return ASH_ERR_OOM;
            memset(fields, 0, (size_t)count * sizeof(AshValue));
        }
        out->as.list.data = fields;
        out->as.list.len = count;
        out->as.list.cap = count;
        for (uint64_t i = 0; i < count; i++) {
            AshStatus st = decode_rec(owner, c, depth + 1, &fields[i]);
            if (st != ASH_OK) return st;
        }
        return ASH_OK;
    }
    case ASH_TY_MAP: {
        if (tag != 0 || !take(c, 12, &h)) return ASH_ERR_TYPE;
        uint64_t slots = get_u64(h);
        uint32_t key_ty = get_u32(h + 8);
        if (slots % 2) return ASH_ERR_TYPE;
        if (!tag_keyable(key_ty)) return ASH_ERR_TYPE;
        if (!count_plausible(c, slots)) return ASH_ERR_TYPE;
        *out = ash_map_new(owner, key_ty);
        AshValue* data = NULL;
        if (slots) {
            data = (AshValue*)ash_bytes(owner, slots * sizeof(AshValue));
            if (!data) return ASH_ERR_OOM;
            memset(data, 0, (size_t)slots * sizeof(AshValue));
        }
        out->as.list.data = data;
        out->as.list.len = slots;
        out->as.list.cap = slots;
        for (uint64_t i = 0; i < slots; i++) {
            AshStatus st = decode_rec(owner, c, depth + 1, &data[i]);
            if (st != ASH_OK) return st;
            if (i % 2 == 0) {
                if (data[i].ty != key_ty) return ASH_ERR_TYPE;
                /* A map the runtime built has no duplicate keys, so a wire
                 * map carrying one is malformed, not a merge request. The
                 * scan is linear per key, the same v1 tradeoff the map
                 * itself makes. */
                for (uint64_t j = 0; j < i; j += 2) {
                    if (ash_value_eq(&data[j], &data[i]))
                        return ASH_ERR_TYPE;
                }
            }
        }
        return ASH_OK;
    }
    case ASH_TY_OPTION: {
        if (tag > 1) return ASH_ERR_TYPE;
        if (tag == 0) return ASH_OK; /* None: eight bytes of header only */
        AshValue* box = ash_box(owner);
        if (!box) return ASH_ERR_OOM;
        out->as.box = box;
        return decode_rec(owner, c, depth + 1, box);
    }
    case ASH_TY_RESULT: {
        if (tag > 1) return ASH_ERR_TYPE;
        AshValue* box = ash_box(owner);
        if (!box) return ASH_ERR_OOM;
        out->as.box = box;
        return decode_rec(owner, c, depth + 1, box);
    }
    default:
        /* ASH_TY_INSTANCE, ASH_TY_PLEDGE_REF, and anything unknown never
         * cross; meeting one is malformed. */
        return ASH_ERR_TYPE;
    }
}

AshStatus ash_wire_decode_value(AshContract* owner, const uint8_t* buf,
                                size_t len, AshValue* out, size_t* consumed) {
    if (consumed) *consumed = 0;
    if (!owner || !out) return ASH_ERR_TYPE;
    if (!buf && len) return ASH_ERR_TYPE;
    WireCur c = { buf, len };
    AshStatus st = decode_rec(owner, &c, 1, out);
    if (st != ASH_OK) {
        memset(out, 0, sizeof(*out));
        return st;
    }
    if (consumed) *consumed = len - c.left;
    return ASH_OK;
}
