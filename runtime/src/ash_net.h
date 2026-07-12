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
 * without a FIN. */
int ash_net_dial(const char* addr);

/* Binds and listens on host:port, returning the listening socket or -1.
 * SO_REUSEADDR is set so a restart does not wait out TIME_WAIT. */
int ash_net_listen(const char* addr);

/* Sets the receive timeout in milliseconds, 0 clearing it so reads block
 * forever. The handshake arms this and the served loop clears it, the one
 * timeout the protocol has. Returns 0 on success, -1 on failure. */
int ash_net_set_rcvtimeo(int fd, uint32_t ms);

/* FNV-1a 64 over n bytes, the hash HELLO_OK carries over the canonical iname
 * dump text so a client can tell one served world from another. */
uint64_t ash_net_fnv1a64(const uint8_t* bytes, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ASH_NET_H */
