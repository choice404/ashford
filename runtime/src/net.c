/* net.c: the socket plumbing behind ash_net.h. It turns the wire codec's
 * frames into bytes on a file descriptor and back, with the full transfer
 * discipline the header promises: every read loops until whole, every write
 * loops until drained, EINTR is retried, and a dead peer is an error rather
 * than a partial frame. The daemon and the client side of libashrt both link
 * this, so the two ends move bytes exactly the same way.
 *
 * No message meaning lives here. A frame is a header and a byte count; what
 * the bytes mean is the caller's, which is what keeps this file testable and
 * small. */

#include "ash_net.h"

#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- little endian primitives ---- */

void ash_net_put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void ash_net_put_u64(uint8_t* p, uint64_t v) {
    ash_net_put_u32(p, (uint32_t)v);
    ash_net_put_u32(p + 4, (uint32_t)(v >> 32));
}

uint32_t ash_net_get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t ash_net_get_u64(const uint8_t* p) {
    return (uint64_t)ash_net_get_u32(p) | ((uint64_t)ash_net_get_u32(p + 4) << 32);
}

/* ---- full transfer ---- */

int ash_net_write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

int ash_net_read_all(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1; /* a receive timeout arrives here as EAGAIN */
        }
        if (r == 0) return -1; /* peer hung up before the frame was whole */
        off += (size_t)r;
    }
    return 0;
}

/* ---- frames ---- */

int ash_net_send_frame(int fd, uint32_t kind, uint64_t request_id,
                       const uint8_t* payload, uint32_t payload_len) {
    uint8_t header[ASH_WIRE_HEADER_LEN];
    if (ash_wire_frame_write(kind, request_id, payload_len, header) != ASH_OK) {
        return -1;
    }
    if (ash_net_write_all(fd, header, ASH_WIRE_HEADER_LEN) != 0) return -1;
    if (payload_len > 0) {
        if (!payload) return -1;
        if (ash_net_write_all(fd, payload, payload_len) != 0) return -1;
    }
    return 0;
}

int ash_net_recv_frame(int fd, AshWireFrame* out, uint8_t** payload) {
    if (payload) *payload = NULL;
    uint8_t header[ASH_WIRE_HEADER_LEN];
    if (ash_net_read_all(fd, header, ASH_WIRE_HEADER_LEN) != 0) return -1;
    AshStatus st = ash_wire_frame_read(header, out);
    if (st != ASH_OK) return -2; /* bad magic, unknown kind, or oversized */
    if (out->payload_len == 0) return 0;
    uint8_t* buf = (uint8_t*)malloc(out->payload_len);
    if (!buf) return -1;
    if (ash_net_read_all(fd, buf, out->payload_len) != 0) {
        free(buf);
        return -1;
    }
    if (payload) {
        *payload = buf;
    } else {
        free(buf);
    }
    return 0;
}

/* ---- addressing ---- */

/* Splits host:port on the last colon so a name or a dotted quad both work and
 * the port is unambiguous. host_out and port_out are caller buffers. */
static int split_addr(const char* addr, char* host_out, size_t host_cap,
                      char* port_out, size_t port_cap) {
    const char* colon = strrchr(addr, ':');
    if (!colon || colon == addr || colon[1] == '\0') return -1;
    size_t hlen = (size_t)(colon - addr);
    if (hlen + 1 > host_cap) return -1;
    memcpy(host_out, addr, hlen);
    host_out[hlen] = '\0';
    size_t plen = strlen(colon + 1);
    if (plen + 1 > port_cap) return -1;
    memcpy(port_out, colon + 1, plen + 1);
    return 0;
}

int ash_net_dial(const char* addr) {
    char host[256], port[32];
    if (!addr || split_addr(addr, host, sizeof host, port, sizeof port) != 0) {
        return -1;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    }
    return fd;
}

int ash_net_listen(const char* addr) {
    char host[256], port[32];
    if (!addr || split_addr(addr, host, sizeof host, port, sizeof port) != 0) {
        return -1;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, 16) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int ash_net_set_rcvtimeo(int fd, uint32_t ms) {
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000u);
    tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) return -1;
    return 0;
}

/* ---- hashing ---- */

uint64_t ash_net_fnv1a64(const uint8_t* bytes, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)bytes[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}
