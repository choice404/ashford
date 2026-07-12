/* ashd.c: the Ashford network daemon. It is a small program over the runtime
 * that already exists. It brings up an AshRuntime, dlopens the modules named
 * on its command line and lets their registrars fill the iname table, freezes
 * the runtime so the table is immutable, and listens on a TCP address. From
 * that moment the table it serves is the table every client fetches at
 * handshake, byte for byte, the discovery surface docs/network.md describes.
 *
 * The concurrency model is thread per connection, blocking sockets, chosen in
 * the roadmap because v1 serves a handful of trusted clients and the runtime
 * underneath is already threaded. Each accepted connection gets a thread that
 * owns its socket for the connection's life. This N1 daemon serves the
 * handshake and the iname sync and nothing more: HELLO into HELLO_OK, then
 * INAME_SYNC into the whole dump, with SIGN and the rest answered by a state
 * error until N2 lands the fulfillment path.
 *
 * Shutdown is the mirror of ash_runtime_shutdown. SIGINT and SIGTERM stop the
 * accept loop, every live connection socket is shut down to wake a thread
 * blocked in a read, every connection thread is joined, and only then is the
 * runtime torn down. SIGPIPE is ignored so a write to a peer that already left
 * is an error return rather than a killed daemon. */

#include <ash/ash.h>

#include "ash_net.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The handshake timeout the daemon enforces on a first HELLO, the mirror of
 * the client's connect timeout: a peer that will not finish a HELLO is not a
 * peer, and the thread it holds is reaped rather than parked forever. This is
 * the default; --handshake-ms overrides it, which a test uses to prove the
 * reap without parking on a ten second clock. */
#define ASHD_HANDSHAKE_MS 10000

/* The one protocol version v1 speaks. */
#define ASHD_PROTO_VERSION 1

/* The frozen world every connection thread reads, built once before the first
 * accept and never mutated after, so the threads share it without a lock. */
typedef struct Daemon {
    AshRuntime* rt;
    char*       token;      /* the expected token bytes, empty string for none */
    size_t      token_len;
    char*       dump;       /* the canonical iname dump text, no terminator */
    size_t      dump_len;
    uint64_t    dump_hash;  /* FNV-1a 64 of the dump text */
    uint32_t    handshake_ms; /* the first HELLO's clock, in milliseconds */
} Daemon;

/* One accepted connection: its socket and its thread. fd goes to -1 under the
 * lock when the thread closes it, so shutdown never shuts down a descriptor a
 * later accept reused. */
typedef struct Conn {
    pthread_t th;
    int       th_set;
    int       fd;
} Conn;

static pthread_mutex_t g_conns_mu = PTHREAD_MUTEX_INITIALIZER;
static Conn*  g_conns;
static size_t g_nconns;
static size_t g_conns_cap;

/* Set by the signal handler, read by the accept loop. The listening socket is
 * shut down out of the handler too so a blocked accept returns at once. */
static volatile sig_atomic_t g_stop;
static int g_listen_fd = -1;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
}

/* One argument struct per spawned thread, the daemon and the connection index
 * the thread owns. The index is stable across an array realloc, a pointer
 * would not be. */
typedef struct ConnArg {
    Daemon* d;
    size_t  idx;
} ConnArg;

/* Constant time token compare, so a wrong token cannot be discovered a byte
 * at a time by timing the refusal. Every byte of the longer input is folded
 * in and the length difference is folded in too, so neither the contents nor
 * the length leak through an early return. */
static int token_eq(const char* a, size_t alen, const char* b, size_t blen) {
    size_t n = alen > blen ? alen : blen;
    volatile unsigned char diff = (unsigned char)(alen != blen);
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < alen ? (unsigned char)a[i] : 0;
        unsigned char cb = i < blen ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

/* Answers a request the normal reply cannot serve. The status is normative
 * and the string is for humans; a failure to write it is the peer's problem,
 * not ours, so the return is ignored and the caller closes regardless. */
static void send_error(int fd, uint64_t request_id, AshStatus status,
                       const char* msg) {
    size_t mlen = msg ? strlen(msg) : 0;
    if (mlen > ASH_WIRE_MAX_PAYLOAD - 8) mlen = 0;
    uint32_t plen = (uint32_t)(8 + mlen);
    uint8_t* p = (uint8_t*)malloc(plen);
    if (!p) return;
    ash_net_put_u32(p, (uint32_t)status);
    ash_net_put_u32(p + 4, (uint32_t)mlen);
    if (mlen) memcpy(p + 8, msg, mlen);
    ash_net_send_frame(fd, ASH_WIRE_ERROR, request_id, p, plen);
    free(p);
}

/* The HELLO validation, split out so the thread body reads as a sequence. It
 * parses the version and the token from the payload, checks the version the
 * daemon speaks, and compares the token in constant time. Returns ASH_OK when
 * the peer may proceed, or the status an ERROR should carry. */
static AshStatus check_hello(const Daemon* d, const uint8_t* pl, uint32_t plen) {
    if (plen < 4) return ASH_ERR_TYPE;
    uint32_t version = ash_net_get_u32(pl);
    if (plen < 8) return ASH_ERR_TYPE;
    uint32_t tlen = ash_net_get_u32(pl + 4);
    if ((uint64_t)tlen + 8 > plen) return ASH_ERR_TYPE;
    if (version != ASHD_PROTO_VERSION) return ASH_ERR_VERSION;
    const char* tok = (const char*)(pl + 8);
    if (!token_eq(tok, tlen, d->token, d->token_len)) return ASH_ERR_NET;
    return ASH_OK;
}

/* Serves HELLO_OK then the INAME_SYNC loop. The frozen dump is read straight
 * off the daemon; no lock is needed because nothing writes it after the first
 * accept. */
static void serve(const Daemon* d, int fd, uint64_t hello_id) {
    uint8_t ok[12];
    ash_net_put_u32(ok, ASHD_PROTO_VERSION);
    ash_net_put_u64(ok + 4, d->dump_hash);
    if (ash_net_send_frame(fd, ASH_WIRE_HELLO_OK, hello_id, ok, sizeof ok) != 0) {
        return;
    }
    for (;;) {
        AshWireFrame fr;
        uint8_t* pl = NULL;
        int rc = ash_net_recv_frame(fd, &fr, &pl);
        if (rc == -1) return; /* the peer left */
        if (rc == -2) {
            send_error(fd, 0, ASH_ERR_TYPE, "malformed frame");
            return;
        }
        if (fr.kind == ASH_WIRE_INAME_SYNC) {
            uint32_t plen = (uint32_t)(4 + d->dump_len);
            uint8_t* out = (uint8_t*)malloc(plen);
            if (out) {
                ash_net_put_u32(out, (uint32_t)d->dump_len);
                if (d->dump_len) memcpy(out + 4, d->dump, d->dump_len);
                ash_net_send_frame(fd, ASH_WIRE_INAME_TABLE, fr.request_id, out,
                                   plen);
                free(out);
            } else {
                send_error(fd, fr.request_id, ASH_ERR_OOM, "out of memory");
            }
        } else {
            /* N1 serves the handshake and the discovery sync only; the
             * fulfillment kinds land in N2. A named request gets a state
             * error and the connection stays open for more syncs. */
            send_error(fd, fr.request_id, ASH_ERR_STATE,
                       "only handshake and iname sync are served in this "
                       "protocol version");
        }
        free(pl);
    }
}

static void* conn_thread(void* arg) {
    ConnArg* ca = (ConnArg*)arg;
    Daemon* d = ca->d;
    size_t idx = ca->idx;
    free(ca);

    pthread_mutex_lock(&g_conns_mu);
    int fd = g_conns[idx].fd;
    pthread_mutex_unlock(&g_conns_mu);

    /* The handshake is on a clock; a stalled HELLO reaps the thread. */
    ash_net_set_rcvtimeo(fd, d->handshake_ms);

    AshWireFrame fr;
    uint8_t* pl = NULL;
    int rc = ash_net_recv_frame(fd, &fr, &pl);
    if (rc == 0 && fr.kind == ASH_WIRE_HELLO) {
        AshStatus st = check_hello(d, pl, fr.payload_len);
        if (st == ASH_OK) {
            /* The handshake finished, so the clock comes off: a fulfillment
             * runs as long as its pledge runs, no protocol deadline above it. */
            ash_net_set_rcvtimeo(fd, 0);
            serve(d, fd, fr.request_id);
        } else {
            const char* why = st == ASH_ERR_VERSION
                                  ? "unsupported protocol version"
                                  : (st == ASH_ERR_NET ? "authentication failed"
                                                       : "malformed HELLO");
            send_error(fd, fr.request_id, st, why);
        }
    } else if (rc == 0) {
        send_error(fd, fr.request_id, ASH_ERR_STATE, "expected HELLO");
    } else if (rc == -2) {
        send_error(fd, 0, ASH_ERR_TYPE, "malformed frame");
    }
    /* rc == -1 is a timeout or a peer that left before HELLO; nothing to say. */
    free(pl);

    pthread_mutex_lock(&g_conns_mu);
    close(fd);
    g_conns[idx].fd = -1;
    pthread_mutex_unlock(&g_conns_mu);
    return NULL;
}

/* Reserves a connection slot under the lock, returning its index or -1 when
 * the array cannot grow. The slot's fd is set before the thread is spawned so
 * the thread reads a live descriptor. */
static long conn_reserve(int fd) {
    pthread_mutex_lock(&g_conns_mu);
    if (g_nconns == g_conns_cap) {
        size_t cap = g_conns_cap ? g_conns_cap * 2 : 16;
        Conn* grown = (Conn*)realloc(g_conns, cap * sizeof(Conn));
        if (!grown) {
            pthread_mutex_unlock(&g_conns_mu);
            return -1;
        }
        g_conns = grown;
        g_conns_cap = cap;
    }
    size_t idx = g_nconns++;
    g_conns[idx].fd = fd;
    g_conns[idx].th_set = 0;
    pthread_mutex_unlock(&g_conns_mu);
    return (long)idx;
}

/* Reads a token file, trimming one trailing newline so an editor's newline
 * does not become part of the secret. The bytes are the daemon's expected
 * token; a NULL path leaves the daemon tokenless, accepting empty tokens. */
static int load_token(const char* path, char** out, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) got--;
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return 0;
}

/* Builds the frozen dump the daemon serves: the canonical text and the FNV-1a
 * 64 the HELLO_OK carries. Called once after freeze. */
static int build_dump(Daemon* d) {
    size_t need = 0;
    if (ash_iname_dump(d->rt, NULL, 0, &need) != ASH_ERR_OOM || need == 0) {
        return -1;
    }
    char* buf = (char*)malloc(need);
    if (!buf) return -1;
    if (ash_iname_dump(d->rt, buf, need, &need) != ASH_OK) {
        free(buf);
        return -1;
    }
    d->dump = buf;
    d->dump_len = need - 1; /* the served text excludes the terminating NUL */
    d->dump_hash = ash_net_fnv1a64((const uint8_t*)buf, d->dump_len);
    return 0;
}

static void usage(const char* prog) {
    fprintf(stderr,
            "usage: %s --listen host:port [--token-file F] "
            "[--handshake-ms N] [--module M.ash.so ...]\n",
            prog);
}

int main(int argc, char** argv) {
    const char* listen_addr = NULL;
    const char* token_file = NULL;
    const char* modules[64];
    size_t nmodules = 0;
    uint32_t handshake_ms = ASHD_HANDSHAKE_MS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_addr = argv[++i];
        } else if (strcmp(argv[i], "--token-file") == 0 && i + 1 < argc) {
            token_file = argv[++i];
        } else if (strcmp(argv[i], "--handshake-ms") == 0 && i + 1 < argc) {
            handshake_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--module") == 0 && i + 1 < argc) {
            if (nmodules == sizeof modules / sizeof modules[0]) {
                fprintf(stderr, "[ashd] too many modules\n");
                return 1;
            }
            modules[nmodules++] = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!listen_addr) {
        usage(argv[0]);
        return 1;
    }

    /* A write to a peer that already hung up must not kill the daemon; it is
     * a failed write the connection thread handles and closes on. */
    signal(SIGPIPE, SIG_IGN);

    Daemon d;
    memset(&d, 0, sizeof d);
    d.handshake_ms = handshake_ms;
    if (ash_runtime_init(NULL, &d.rt) != ASH_OK) {
        fprintf(stderr, "[ashd] runtime init failed\n");
        return 1;
    }
    for (size_t i = 0; i < nmodules; i++) {
        if (ash_module_load(d.rt, modules[i]) != ASH_OK) {
            fprintf(stderr, "[ashd] failed to load module %s\n", modules[i]);
            ash_runtime_shutdown(d.rt);
            return 1;
        }
    }
    if (token_file) {
        if (load_token(token_file, &d.token, &d.token_len) != 0) {
            fprintf(stderr, "[ashd] failed to read token file %s\n", token_file);
            ash_runtime_shutdown(d.rt);
            return 1;
        }
    } else {
        d.token = (char*)malloc(1);
        if (!d.token) {
            ash_runtime_shutdown(d.rt);
            return 1;
        }
        d.token[0] = '\0';
        d.token_len = 0;
    }

    /* Freeze first, then snapshot the dump: the table is immutable from here,
     * which is the whole promise a discovery surface makes. */
    ash_runtime_freeze(d.rt);
    if (build_dump(&d) != 0) {
        fprintf(stderr, "[ashd] failed to render the iname dump\n");
        free(d.token);
        ash_runtime_shutdown(d.rt);
        return 1;
    }

    g_listen_fd = ash_net_listen(listen_addr);
    if (g_listen_fd < 0) {
        fprintf(stderr, "[ashd] failed to listen on %s\n", listen_addr);
        free(d.token);
        free(d.dump);
        ash_runtime_shutdown(d.rt);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "[ashd] listening on %s\n", listen_addr);

    while (!g_stop) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (g_stop) break;
            continue; /* a transient accept error is not fatal to the loop */
        }
        long idx = conn_reserve(cfd);
        if (idx < 0) {
            close(cfd);
            continue;
        }
        ConnArg* ca = (ConnArg*)malloc(sizeof(ConnArg));
        if (!ca) {
            pthread_mutex_lock(&g_conns_mu);
            close(cfd);
            g_conns[idx].fd = -1;
            pthread_mutex_unlock(&g_conns_mu);
            continue;
        }
        ca->d = &d;
        ca->idx = (size_t)idx;
        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_thread, ca) != 0) {
            free(ca);
            pthread_mutex_lock(&g_conns_mu);
            close(cfd);
            g_conns[idx].fd = -1;
            pthread_mutex_unlock(&g_conns_mu);
            continue;
        }
        pthread_mutex_lock(&g_conns_mu);
        g_conns[idx].th = tid;
        g_conns[idx].th_set = 1;
        pthread_mutex_unlock(&g_conns_mu);
    }

    /* Graceful shutdown, the mirror of the runtime's own: stop accepting,
     * wake every connection thread blocked in a read by shutting its socket,
     * join them all, then tear down the runtime with nothing still calling
     * in. */
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_mutex_lock(&g_conns_mu);
    for (size_t i = 0; i < g_nconns; i++) {
        if (g_conns[i].fd >= 0) shutdown(g_conns[i].fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_conns_mu);
    for (size_t i = 0; i < g_nconns; i++) {
        pthread_mutex_lock(&g_conns_mu);
        int set = g_conns[i].th_set;
        pthread_t th = g_conns[i].th;
        pthread_mutex_unlock(&g_conns_mu);
        if (set) pthread_join(th, NULL);
    }
    free(g_conns);
    g_conns = NULL;
    g_nconns = g_conns_cap = 0;

    free(d.token);
    free(d.dump);
    ash_runtime_shutdown(d.rt);
    return 0;
}
