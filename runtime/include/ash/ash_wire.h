/* ash_wire.h: the wire codec of the network runtime, a library first and a
 * protocol second. Frames and values encode and decode here with no socket in
 * sight, so the format can be golden tested byte for byte before any network
 * exists. The format is docs/network.md's: a fixed 20 byte frame header, and
 * a canonical little endian serialization of the AshValue representation with
 * the pointers flattened out. Canonical means one value has one byte string;
 * encoding a decoded payload reproduces the input exactly.
 *
 * Nothing on the wire is trusted. Every length is checked against the buffer
 * before it is read, decoding caps its nesting at 64 levels, and the two tags
 * that never cross, ASH_TY_INSTANCE and ASH_TY_PLEDGE_REF, are refused by the
 * encoder with ASH_ERR_TYPE and read as malformed by the decoder. A malformed
 * payload reports ASH_ERR_TYPE, the shape mismatch status the ABI already
 * has; an oversized one reports ASH_ERR_OOM, the refusal docs/network.md
 * pins for the payload cap. */

#ifndef ASH_WIRE_H
#define ASH_WIRE_H

#include "ash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The frame header: 4 bytes of magic, u32 kind, u64 request id, u32 payload
 * length, all little endian, payload bytes following. */
#define ASH_WIRE_MAGIC        "ASHW"
#define ASH_WIRE_HEADER_LEN   20

/* A payload longer than 64 MiB is refused; a peer that ignores the cap
 * cannot be resynchronized. */
#define ASH_WIRE_MAX_PAYLOAD  (64u * 1024u * 1024u)

/* The decoder refuses a value nested deeper than this, so a hostile payload
 * cannot recurse the stack away. The encoder holds the same line: a value it
 * cannot encode within the cap could never cross anyway. */
#define ASH_WIRE_MAX_DEPTH    64

/* The thirteen message kinds, numbered on the wire. Requests flow client to
 * daemon, replies daemon to client, ERROR answers any request whose normal
 * reply cannot be produced. */
typedef enum AshWireKind {
    ASH_WIRE_HELLO         = 1,
    ASH_WIRE_HELLO_OK      = 2,
    ASH_WIRE_INAME_SYNC    = 3,
    ASH_WIRE_INAME_TABLE   = 4,
    ASH_WIRE_SIGN          = 5,
    ASH_WIRE_SIGNED        = 6,
    ASH_WIRE_FULFILL       = 7,
    ASH_WIRE_RESULT        = 8,
    ASH_WIRE_BREAK         = 9,
    ASH_WIRE_BROKEN        = 10,
    ASH_WIRE_PARTIAL_QUERY = 11,
    ASH_WIRE_PARTIAL       = 12,
    ASH_WIRE_ERROR         = 13
} AshWireKind;

/* A parsed frame header. The payload bytes are not part of the struct; the
 * caller reads payload_len bytes after the header itself. */
typedef struct AshWireFrame {
    uint32_t kind;
    uint64_t request_id;
    uint32_t payload_len;
} AshWireFrame;

/* Writes one frame header into out, which must hold ASH_WIRE_HEADER_LEN
 * bytes. A kind outside the table is ASH_ERR_TYPE, a payload length past the
 * cap is ASH_ERR_OOM, and nothing is written on either. */
AshStatus ash_wire_frame_write(uint32_t kind, uint64_t request_id,
                               uint32_t payload_len,
                               uint8_t out[ASH_WIRE_HEADER_LEN]);

/* Parses one frame header from in, which must hold ASH_WIRE_HEADER_LEN
 * bytes. Wrong magic or an unknown kind is malformed, ASH_ERR_TYPE; a
 * payload length past the cap is ASH_ERR_OOM. out is untouched on error. */
AshStatus ash_wire_frame_read(const uint8_t in[ASH_WIRE_HEADER_LEN],
                              AshWireFrame* out);

/* Encodes one value in the canonical form. The size protocol is
 * ash_iname_dump's: *need receives the exact encoded size; when cap is at
 * least that, the bytes are written to buf and the call returns ASH_OK,
 * otherwise nothing is written and the call returns ASH_ERR_OOM, so a NULL
 * buf with cap 0 sizes the buffer. A value carrying ASH_TY_INSTANCE or
 * ASH_TY_PLEDGE_REF anywhere, a tag no value of its type can carry, or
 * nesting past ASH_WIRE_MAX_DEPTH is ASH_ERR_TYPE, with *need unset. */
AshStatus ash_wire_encode_value(const AshValue* v, uint8_t* buf, size_t cap,
                                size_t* need);

/* Decodes one value from buf. Every allocation the value needs goes through
 * the instance helpers on owner, so the result is instance owned and dies at
 * that instance's break, the same home a network result has. *consumed, when
 * non NULL, receives the bytes the value occupied; trailing bytes after a
 * complete value are the caller's business. A malformed payload, a forbidden
 * or unknown tag, a length the buffer cannot honor, or nesting past
 * ASH_WIRE_MAX_DEPTH is ASH_ERR_TYPE; an allocation failure is ASH_ERR_OOM.
 * On error out reads as a zeroed Unit and *consumed as 0; bytes already
 * copied onto owner stay there until its break, the one walk reclaim rule. */
AshStatus ash_wire_decode_value(AshContract* owner, const uint8_t* buf,
                                size_t len, AshValue* out, size_t* consumed);

#ifdef __cplusplus
}
#endif

#endif /* ASH_WIRE_H */
