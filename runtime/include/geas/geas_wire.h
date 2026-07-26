/* geas_wire.h: the canonical wire codec, a library first and a protocol second.
 * Frames and values encode and decode here with no socket in sight, so the
 * format is golden tested byte for byte on its own. It is a fixed 20 byte frame
 * header, and a canonical little endian serialization of the GeasValue
 * representation with the pointers flattened out, the same serialization the
 * park format writes an instance's durable state through. Canonical means one
 * value has one byte string; encoding a decoded payload reproduces the input
 * exactly.
 *
 * Nothing in an encoded payload is trusted. Every length is checked against the
 * buffer before it is read, decoding caps its nesting at 64 levels, and the two
 * tags that never cross, GEAS_TY_INSTANCE and GEAS_TY_PLEDGE_REF, are refused by
 * the encoder with GEAS_ERR_TYPE and read as malformed by the decoder. A
 * malformed payload reports GEAS_ERR_TYPE, the shape mismatch status the ABI
 * already has; an oversized one reports GEAS_ERR_OOM, the refusal the codec pins
 * at its payload cap. */

#ifndef GEAS_WIRE_H
#define GEAS_WIRE_H

#include "geas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The frame header: 4 bytes of magic, u32 kind, u64 request id, u32 payload
 * length, all little endian, payload bytes following. */
#define GEAS_WIRE_MAGIC        "ASHW"
#define GEAS_WIRE_HEADER_LEN   20

/* A payload longer than 64 MiB is refused; a peer that ignores the cap
 * cannot be resynchronized. */
#define GEAS_WIRE_MAX_PAYLOAD  (64u * 1024u * 1024u)

/* The decoder refuses a value nested deeper than this, so a hostile payload
 * cannot recurse the stack away. The encoder holds the same line: a value it
 * cannot encode within the cap could never cross anyway. */
#define GEAS_WIRE_MAX_DEPTH    64

/* The thirteen message kinds, numbered on the wire. Requests flow client to
 * daemon, replies daemon to client, ERROR answers any request whose normal
 * reply cannot be produced. */
typedef enum GeasWireKind {
    GEAS_WIRE_HELLO         = 1,
    GEAS_WIRE_HELLO_OK      = 2,
    GEAS_WIRE_INAME_SYNC    = 3,
    GEAS_WIRE_INAME_TABLE   = 4,
    GEAS_WIRE_SIGN          = 5,
    GEAS_WIRE_SIGNED        = 6,
    GEAS_WIRE_FULFILL       = 7,
    GEAS_WIRE_RESULT        = 8,
    GEAS_WIRE_BREAK         = 9,
    GEAS_WIRE_BROKEN        = 10,
    GEAS_WIRE_PARTIAL_QUERY = 11,
    GEAS_WIRE_PARTIAL       = 12,
    GEAS_WIRE_ERROR         = 13
} GeasWireKind;

/* A parsed frame header. The payload bytes are not part of the struct; the
 * caller reads payload_len bytes after the header itself. */
typedef struct GeasWireFrame {
    uint32_t kind;
    uint64_t request_id;
    uint32_t payload_len;
} GeasWireFrame;

/* Writes one frame header into out, which must hold GEAS_WIRE_HEADER_LEN
 * bytes. A kind outside the table is GEAS_ERR_TYPE, a payload length past the
 * cap is GEAS_ERR_OOM, and nothing is written on either. */
GeasStatus geas_wire_frame_write(uint32_t kind, uint64_t request_id,
                               uint32_t payload_len,
                               uint8_t out[GEAS_WIRE_HEADER_LEN]);

/* Parses one frame header from in, which must hold GEAS_WIRE_HEADER_LEN
 * bytes. Wrong magic or an unknown kind is malformed, GEAS_ERR_TYPE; a
 * payload length past the cap is GEAS_ERR_OOM. out is untouched on error. */
GeasStatus geas_wire_frame_read(const uint8_t in[GEAS_WIRE_HEADER_LEN],
                              GeasWireFrame* out);

/* Encodes one value in the canonical form. The size protocol is
 * geas_iname_dump's: *need receives the exact encoded size; when cap is at
 * least that, the bytes are written to buf and the call returns GEAS_OK,
 * otherwise nothing is written and the call returns GEAS_ERR_OOM, so a NULL
 * buf with cap 0 sizes the buffer. A value carrying GEAS_TY_INSTANCE or
 * GEAS_TY_PLEDGE_REF anywhere, a tag no value of its type can carry, or
 * nesting past GEAS_WIRE_MAX_DEPTH is GEAS_ERR_TYPE, with *need unset. */
GeasStatus geas_wire_encode_value(const GeasValue* v, uint8_t* buf, size_t cap,
                                size_t* need);

/* Decodes one value from buf. Every allocation the value needs goes through
 * the instance helpers on owner, so the result is instance owned and dies at
 * that instance's break, the same home a network result has. *consumed, when
 * non NULL, receives the bytes the value occupied; trailing bytes after a
 * complete value are the caller's business. A malformed payload, a forbidden
 * or unknown tag, a length the buffer cannot honor, or nesting past
 * GEAS_WIRE_MAX_DEPTH is GEAS_ERR_TYPE; an allocation failure is GEAS_ERR_OOM.
 * On error out reads as a zeroed Unit and *consumed as 0; bytes already
 * copied onto owner stay there until its break, the one walk reclaim rule. */
GeasStatus geas_wire_decode_value(GeasContract* owner, const uint8_t* buf,
                                size_t len, GeasValue* out, size_t* consumed);

#ifdef __cplusplus
}
#endif

#endif /* GEAS_WIRE_H */
