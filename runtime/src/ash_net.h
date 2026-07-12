/* ash_net.h: the socket plumbing the network runtime shares. This is an
 * internal header, not part of the public ash.h surface; it lives beside the
 * translation units that use it, wire.c's frames turned into bytes on a real
 * file descriptor. Both C parties link it: ashd the daemon and the client
 * side of libashrt, the same one implementation the wire codec already is.
 *
 * Every read and write here is a full transfer or a failure. A short read is
 * looped until the whole frame or payload arrives, EINTR is retried rather
 * than surfaced, and a socket that dies mid transfer is an error the caller
 * turns into ASH_ERR_NET. Nothing here knows a message kind; it moves bytes
 * and leaves meaning to the two ends. */

#ifndef ASH_NET_H
#define ASH_NET_H

#include <ash/ash_wire.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Little endian payload primitives, the same byte order the frame header and
 * the value codec use. Payload strings are a u32 byte length then the bytes,
 * no terminator, the fat value flattened. */
void     ash_net_put_u32(uint8_t* p, uint32_t v);
void     ash_net_put_u64(uint8_t* p, uint64_t v);
uint32_t ash_net_get_u32(const uint8_t* p);
uint64_t ash_net_get_u64(const uint8_t* p);

/* Writes all n bytes or fails. EINTR is retried; a closed or reset peer is a
 * failure. Returns 0 on success, -1 on any error or a peer that hung up. */
int ash_net_write_all(int fd, const void* buf, size_t n);

/* Reads exactly n bytes or fails. EINTR is retried, a partial read is looped
 * until whole, and end of stream before n bytes is a failure. A receive
 * timeout set with ash_net_set_rcvtimeo surfaces here as -1. Returns 0 on
 * success, -1 on error, EOF, or timeout. */
int ash_net_read_all(int fd, void* buf, size_t n);

/* Sends one frame: the 20 byte header then payload_len payload bytes. payload
 * may be NULL when payload_len is 0. Returns 0 on success, -1 on a bad kind or
 * oversized payload (nothing written), or a write failure. */
int ash_net_send_frame(int fd, uint32_t kind, uint64_t request_id,
                       const uint8_t* payload, uint32_t payload_len);

/* Receives one frame. Reads and parses the header, then reads the payload
 * into a freshly malloc'd buffer the caller frees; *payload is NULL when the
 * payload is empty. Returns 0 on success, -1 on an I/O error, EOF, or
 * timeout, and -2 on a malformed or oversized header, the stream then being
 * unsynchronizable so the caller must close. On any nonzero return *payload is
 * NULL and nothing is left allocated. */
int ash_net_recv_frame(int fd, AshWireFrame* out, uint8_t** payload);

/* Dials host:port and returns a connected stream socket, or -1. The address
 * is a host and a decimal port joined by the last colon, so a bare IPv4 or a
 * name both resolve; keepalive is turned on for the far peer that vanishes
 * without a FIN. This blocks in connect(2) with no bound; ash_net_dial_timeout
 * is the bounded form the handshake uses. */
int ash_net_dial(const char* addr);

/* Dials host:port giving the TCP connect at most ms milliseconds, so a peer
 * that silently drops SYNs cannot park the caller forever, the connect half of
 * the handshake clock. The connect runs non blocking and is waited on a poll,
 * EINTR retried; the socket is handed back in blocking mode. ms of 0 blocks
 * with no bound, the ash_net_dial behavior. Returns a connected stream socket
 * or -1 on a resolution, connect, or timeout failure. */
int ash_net_dial_timeout(const char* addr, uint32_t ms);

/* Binds and listens on host:port, returning the listening socket or -1.
 * SO_REUSEADDR is set so a restart does not wait out TIME_WAIT. */
int ash_net_listen(const char* addr);

/* Sets the receive timeout in milliseconds, 0 clearing it so reads block
 * forever. The handshake arms this and the served loop clears it, the one
 * timeout the protocol has. Returns 0 on success, -1 on failure. */
int ash_net_set_rcvtimeo(int fd, uint32_t ms);

/* Sets the send timeout in milliseconds, 0 clearing it so writes block until
 * the kernel takes the bytes. The client arms this across the handshake so a
 * peer whose window never opens cannot stall a HELLO or an INAME_SYNC send
 * forever, and clears it once the connection is served, where a write blocks
 * only as long as the transport needs. Returns 0 on success, -1 on failure. */
int ash_net_set_sndtimeo(int fd, uint32_t ms);

/* FNV-1a 64 over n bytes, the hash HELLO_OK carries over the canonical iname
 * dump text so a client can tell one served world from another. */
uint64_t ash_net_fnv1a64(const uint8_t* bytes, size_t n);

/* ---- message payload builders and readers ----
 *
 * The frame codec moves the header and a byte count; these turn the message
 * payloads of docs/network.md into and out of those bytes. Both C parties link
 * them, so a SIGN the client writes and the daemon reads is one format with one
 * implementation. Payload strings are a u32 byte length then the bytes, no
 * terminator, distinct from the u64 length a String value carries inside the
 * value encoding. */

/* A growable byte buffer a message is written into. err latches on any
 * allocation or overflow, so a caller may append a whole message and check
 * once at the end; a builder whose err is set has a NULL, zero length payload
 * and must not be sent. */
typedef struct AshWBuf {
    uint8_t* data;
    size_t   len;
    size_t   cap;
    int      err;
} AshWBuf;

void ash_wbuf_init(AshWBuf* w);
void ash_wbuf_free(AshWBuf* w);
void ash_wbuf_u32(AshWBuf* w, uint32_t v);
void ash_wbuf_u64(AshWBuf* w, uint64_t v);
void ash_wbuf_i64(AshWBuf* w, int64_t v);
void ash_wbuf_bytes(AshWBuf* w, const void* p, size_t n);
/* A u32 length prefixed payload string. */
void ash_wbuf_str(AshWBuf* w, const char* s, size_t n);
/* Encodes one AshValue in the canonical wire form. Sets err on a value that
 * cannot cross, an instance handle or a pledge ref, so a refused value fails
 * the whole message rather than sending half of it. */
void ash_wbuf_value(AshWBuf* w, const AshValue* v);

/* A bounds checked cursor over a received payload. Every read checks the bytes
 * that remain before it advances, so a truncated or lying payload is a clean
 * failure rather than a read past the buffer. Returns 1 on success, 0 when the
 * bytes are not there. */
typedef struct AshRBuf {
    const uint8_t* p;
    size_t         left;
} AshRBuf;

void ash_rbuf_init(AshRBuf* r, const uint8_t* p, size_t n);
int  ash_rbuf_u32(AshRBuf* r, uint32_t* out);
int  ash_rbuf_u64(AshRBuf* r, uint64_t* out);
int  ash_rbuf_i64(AshRBuf* r, int64_t* out);
/* A payload string: *out aims into the cursor's own bytes, borrowed for the
 * life of the payload buffer, *len is its byte length. Not NUL terminated. */
int  ash_rbuf_str(AshRBuf* r, const char** out, uint32_t* len);
/* How many payload bytes remain unread, so a value decode can be handed the
 * rest of the frame. */
size_t ash_rbuf_left(const AshRBuf* r);
/* Advances the cursor past n bytes a value decode consumed. Returns 1 when the
 * bytes were there. */
int  ash_rbuf_skip(AshRBuf* r, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ASH_NET_H */
