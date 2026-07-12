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
#include "ash_remote.h"

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

/* ---- the served connection ----
 *
 * Past the handshake a connection carries the fulfillment path, so it grows a
 * state the N1 daemon had no need for. The connection thread is the reader: it
 * decodes frames and issues sign, fulfill, break, and partial in arrival order,
 * which is what preserves per instance ordering, since the runtime instance
 * carries the same lock every instance carries. Each fulfillment gets a
 * detached waiter thread that blocks in ash_future_wait and sends its RESULT
 * under the write mutex, so a slow pledge on one instance never stalls the
 * reader or another instance's answer.
 *
 * Two counters coordinate teardown. live_waiters is every waiter across the
 * connection; a per instance outstanding count is the waiters on one instance.
 * A break, and a disconnect, drains the relevant waiters to zero before
 * ash_contract_break reclaims the instance heap, so no waiter is ever reading a
 * value out of a heap the break is freeing. This is the daemon's answer to the
 * one race the value delivery would otherwise open. */

typedef struct InstEntry {
    uint64_t     id;
    AshContract* inst;
    int          outstanding; /* in flight fulfill waiters on this instance */
} InstEntry;

typedef struct ConnState {
    Daemon*         d;
    int             fd;
    pthread_mutex_t wmu;   /* serializes every write to the socket */
    pthread_mutex_t mu;    /* guards the instance map and the counters */
    pthread_cond_t  cv;    /* a waiter finished; a drain may proceed */
    InstEntry*      insts;
    size_t          ninsts;
    size_t          insts_cap;
    uint64_t        next_id;   /* instance id allocator, ids start at 1 */
    int             live_waiters;
} ConnState;

/* Every write to a served connection goes here, so the reader's replies and a
 * waiter's RESULT never interleave their bytes on the wire. */
static int cs_send(ConnState* cs, uint32_t kind, uint64_t req_id,
                   const uint8_t* pl, uint32_t plen) {
    pthread_mutex_lock(&cs->wmu);
    int rc = ash_net_send_frame(cs->fd, kind, req_id, pl, plen);
    pthread_mutex_unlock(&cs->wmu);
    return rc;
}

static void cs_error(ConnState* cs, uint64_t req_id, AshStatus st,
                     const char* msg) {
    size_t mlen = msg ? strlen(msg) : 0;
    if (mlen > ASH_WIRE_MAX_PAYLOAD - 8) mlen = 0;
    uint32_t plen = (uint32_t)(8 + mlen);
    uint8_t* p = (uint8_t*)malloc(plen);
    if (!p) return;
    ash_net_put_u32(p, (uint32_t)st);
    ash_net_put_u32(p + 4, (uint32_t)mlen);
    if (mlen) memcpy(p + 8, msg, mlen);
    cs_send(cs, ASH_WIRE_ERROR, req_id, p, plen);
    free(p);
}

/* A RESULT carrying a status and no value, the shape a fulfillment error takes
 * so it arrives through the client's wait exactly like a local one. */
static void cs_result_status(ConnState* cs, uint64_t req_id, AshStatus st) {
    uint8_t p[4];
    ash_net_put_u32(p, (uint32_t)st);
    cs_send(cs, ASH_WIRE_RESULT, req_id, p, 4);
}

/* A NUL terminated copy of a payload string, since the runtime resolves names
 * by strcmp and a wire string is not terminated. */
static char* dupz(const char* p, uint32_t n) {
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n) memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

/* Registers a freshly signed instance and returns its connection scoped id, or
 * 0 on an allocation failure. Only the reader thread adds instances, so the map
 * never reallocs while a drain holds an entry. */
static uint64_t cs_add_instance(ConnState* cs, AshContract* inst) {
    pthread_mutex_lock(&cs->mu);
    if (cs->ninsts == cs->insts_cap) {
        size_t cap = cs->insts_cap ? cs->insts_cap * 2 : 16;
        InstEntry* g = (InstEntry*)realloc(cs->insts, cap * sizeof(InstEntry));
        if (!g) {
            pthread_mutex_unlock(&cs->mu);
            return 0;
        }
        cs->insts = g;
        cs->insts_cap = cap;
    }
    uint64_t id = cs->next_id++;
    cs->insts[cs->ninsts].id = id;
    cs->insts[cs->ninsts].inst = inst;
    cs->insts[cs->ninsts].outstanding = 0;
    cs->ninsts++;
    pthread_mutex_unlock(&cs->mu);
    return id;
}

static AshContract* cs_find(ConnState* cs, uint64_t id) {
    pthread_mutex_lock(&cs->mu);
    AshContract* r = NULL;
    for (size_t i = 0; i < cs->ninsts; i++) {
        if (cs->insts[i].id == id) {
            r = cs->insts[i].inst;
            break;
        }
    }
    pthread_mutex_unlock(&cs->mu);
    return r;
}

/* Adjusts one instance's outstanding waiter count under the lock. */
static void cs_outstanding(ConnState* cs, uint64_t id, int delta) {
    for (size_t i = 0; i < cs->ninsts; i++) {
        if (cs->insts[i].id == id) {
            cs->insts[i].outstanding += delta;
            if (cs->insts[i].outstanding < 0) cs->insts[i].outstanding = 0;
            break;
        }
    }
}

typedef struct WaiterArg {
    ConnState* cs;
    AshFuture* f;
    uint64_t   req_id;
    uint64_t   inst_id;
} WaiterArg;

/* Encodes and sends a delivered outcome. The instance heap the value lives on
 * is held valid by this waiter's outstanding count, which a break drains to
 * zero before it frees anything, so the encode never races the reclaim. */
static void waiter_send(ConnState* cs, uint64_t req_id, AshStatus st,
                        const AshValue* out) {
    AshWBuf w;
    ash_wbuf_init(&w);
    ash_wbuf_u32(&w, (uint32_t)st);
    if (st == ASH_OK) ash_wbuf_value(&w, out);
    if (w.err) {
        ash_wbuf_free(&w);
        cs_result_status(cs, req_id, ASH_ERR_TYPE);
        return;
    }
    cs_send(cs, ASH_WIRE_RESULT, req_id, w.data, (uint32_t)w.len);
    ash_wbuf_free(&w);
}

static void* waiter_main(void* arg) {
    WaiterArg* wa = (WaiterArg*)arg;
    ConnState* cs = wa->cs;
    AshValue out;
    memset(&out, 0, sizeof out);
    AshStatus st = ash_future_wait(wa->f, &out);
    waiter_send(cs, wa->req_id, st, &out);
    pthread_mutex_lock(&cs->mu);
    cs_outstanding(cs, wa->inst_id, -1);
    cs->live_waiters--;
    pthread_cond_broadcast(&cs->cv);
    pthread_mutex_unlock(&cs->mu);
    free(wa);
    return NULL;
}

/* Hands one fulfillment to a detached waiter, or, if the thread cannot be
 * spawned, waits it inline so the reply is never simply dropped. */
static void spawn_waiter(ConnState* cs, AshFuture* f, uint64_t req_id,
                         uint64_t inst_id) {
    WaiterArg* wa = (WaiterArg*)malloc(sizeof(WaiterArg));
    if (wa) {
        wa->cs = cs;
        wa->f = f;
        wa->req_id = req_id;
        wa->inst_id = inst_id;
        pthread_mutex_lock(&cs->mu);
        cs_outstanding(cs, inst_id, 1);
        cs->live_waiters++;
        pthread_mutex_unlock(&cs->mu);
        pthread_t th;
        if (pthread_create(&th, NULL, waiter_main, wa) == 0) {
            pthread_detach(th);
            return;
        }
        pthread_mutex_lock(&cs->mu);
        cs_outstanding(cs, inst_id, -1);
        cs->live_waiters--;
        pthread_cond_broadcast(&cs->cv);
        pthread_mutex_unlock(&cs->mu);
        free(wa);
    }
    AshValue out;
    memset(&out, 0, sizeof out);
    AshStatus st = ash_future_wait(f, &out);
    waiter_send(cs, req_id, st, &out);
}

/* SIGN: decode the overrides onto a scratch arena, run the local sign with them
 * as bindings, and answer SIGNED with the instance id and its effective vows.
 * Every validation and failure status is ash_contract_sign's own. */
static void handle_sign(ConnState* cs, uint64_t req_id, const uint8_t* pl,
                        uint32_t plen) {
    AshRBuf r;
    ash_rbuf_init(&r, pl, plen);
    const char* cname;
    uint32_t clen;
    uint64_t expected;
    uint32_t nov;
    if (!ash_rbuf_str(&r, &cname, &clen) || !ash_rbuf_u64(&r, &expected) ||
        !ash_rbuf_u32(&r, &nov)) {
        cs_error(cs, req_id, ASH_ERR_TYPE, "malformed SIGN");
        return;
    }
    char* namez = dupz(cname, clen);
    if (!namez) {
        cs_error(cs, req_id, ASH_ERR_OOM, "out of memory");
        return;
    }
    AshContract* arena = NULL;
    AshVowBinding* binds = NULL;
    char** bnames = NULL;
    AshStatus st = ASH_OK;
    if (nov) {
        arena = ash_scratch_new(cs->d->rt);
        binds = (AshVowBinding*)calloc(nov, sizeof(AshVowBinding));
        bnames = (char**)calloc(nov, sizeof(char*));
        if (!arena || !binds || !bnames) st = ASH_ERR_OOM;
    }
    for (uint32_t i = 0; st == ASH_OK && i < nov; i++) {
        const char* vn;
        uint32_t vnl;
        if (!ash_rbuf_str(&r, &vn, &vnl)) {
            st = ASH_ERR_TYPE;
            break;
        }
        bnames[i] = dupz(vn, vnl);
        if (!bnames[i]) {
            st = ASH_ERR_OOM;
            break;
        }
        AshValue v;
        size_t consumed = 0;
        if (ash_wire_decode_value(arena, r.p, r.left, &v, &consumed) != ASH_OK) {
            st = ASH_ERR_TYPE;
            break;
        }
        ash_rbuf_skip(&r, consumed);
        binds[i].name = bnames[i];
        binds[i].value = v;
    }
    AshContract* inst = NULL;
    if (st == ASH_OK) {
        st = ash_contract_sign(cs->d->rt, namez, binds, nov, expected, &inst);
    }
    /* The sign deep copied the vow values, so the arena and its decodes go now. */
    if (arena) ash_scratch_free(arena);
    if (bnames) {
        for (uint32_t i = 0; i < nov; i++) free(bnames[i]);
        free(bnames);
    }
    free(binds);
    free(namez);
    if (st != ASH_OK) {
        cs_error(cs, req_id, st, "sign refused");
        return;
    }
    uint64_t id = cs_add_instance(cs, inst);
    if (id == 0) {
        ash_contract_break(inst);
        cs_error(cs, req_id, ASH_ERR_OOM, "out of memory");
        return;
    }
    AshWBuf w;
    ash_wbuf_init(&w);
    ash_wbuf_u64(&w, id);
    ash_wbuf_i64(&w, ash_contract_signed_at(inst));
    ash_wbuf_u64(&w, ash_contract_hash(inst));
    size_t nv = ash_instance_nvows(inst);
    ash_wbuf_u32(&w, (uint32_t)nv);
    for (size_t i = 0; i < nv; i++) {
        const char* vn = ash_instance_vow_name(inst, i);
        const AshValue* vv = ash_instance_vow_value(inst, i);
        ash_wbuf_str(&w, vn ? vn : "", vn ? strlen(vn) : 0);
        if (vv) {
            ash_wbuf_value(&w, vv);
        } else {
            w.err = 1;
        }
    }
    if (w.err) {
        ash_wbuf_free(&w);
        cs_error(cs, req_id, ASH_ERR_OOM, "vow encode");
        return;
    }
    cs_send(cs, ASH_WIRE_SIGNED, req_id, w.data, (uint32_t)w.len);
    ash_wbuf_free(&w);
}

/* FULFILL: decode the arguments onto the target instance and start the local
 * fulfillment, whose future a detached waiter turns into a RESULT. Refs never
 * reach here; the client refuses them before they cross the wire. */
static void handle_fulfill(ConnState* cs, uint64_t req_id, const uint8_t* pl,
                           uint32_t plen) {
    AshRBuf r;
    ash_rbuf_init(&r, pl, plen);
    uint64_t id;
    const char* pn;
    uint32_t pnl;
    uint32_t nargs;
    if (!ash_rbuf_u64(&r, &id) || !ash_rbuf_str(&r, &pn, &pnl) ||
        !ash_rbuf_u32(&r, &nargs)) {
        cs_result_status(cs, req_id, ASH_ERR_TYPE);
        return;
    }
    AshContract* inst = cs_find(cs, id);
    if (!inst) {
        cs_result_status(cs, req_id, ASH_ERR_STATE);
        return;
    }
    char* pnz = dupz(pn, pnl);
    if (!pnz) {
        cs_result_status(cs, req_id, ASH_ERR_OOM);
        return;
    }
    AshValue* args = NULL;
    AshStatus st = ASH_OK;
    if (nargs) {
        args = (AshValue*)calloc(nargs, sizeof(AshValue));
        if (!args) {
            free(pnz);
            cs_result_status(cs, req_id, ASH_ERR_OOM);
            return;
        }
        for (uint32_t i = 0; i < nargs; i++) {
            AshValue v;
            size_t consumed = 0;
            if (ash_wire_decode_value(inst, r.p, r.left, &v, &consumed) !=
                ASH_OK) {
                st = ASH_ERR_TYPE;
                break;
            }
            ash_rbuf_skip(&r, consumed);
            args[i] = v;
        }
    }
    if (st != ASH_OK) {
        free(args);
        free(pnz);
        cs_result_status(cs, req_id, st);
        return;
    }
    AshFuture* f = ash_pledge_fulfill(inst, pnz, args, nargs, NULL, 0);
    free(args);
    free(pnz);
    if (!f) {
        cs_result_status(cs, req_id, ASH_ERR_OOM);
        return;
    }
    spawn_waiter(cs, f, req_id, id);
}

/* BREAK: drain this instance's in flight waiters, then break it and answer
 * BROKEN. The drain is what lets the break free the heap with no waiter still
 * reading a value out of it. */
static void handle_break(ConnState* cs, uint64_t req_id, const uint8_t* pl,
                         uint32_t plen) {
    AshRBuf r;
    ash_rbuf_init(&r, pl, plen);
    uint64_t id;
    if (!ash_rbuf_u64(&r, &id)) {
        cs_error(cs, req_id, ASH_ERR_TYPE, "malformed BREAK");
        return;
    }
    AshContract* inst = cs_find(cs, id);
    if (!inst) {
        uint8_t p[4];
        ash_net_put_u32(p, (uint32_t)ASH_ERR_STATE);
        cs_send(cs, ASH_WIRE_BROKEN, req_id, p, 4);
        return;
    }
    pthread_mutex_lock(&cs->mu);
    for (;;) {
        int out = 0;
        for (size_t i = 0; i < cs->ninsts; i++) {
            if (cs->insts[i].id == id) {
                out = cs->insts[i].outstanding;
                break;
            }
        }
        if (out == 0) break;
        pthread_cond_wait(&cs->cv, &cs->mu);
    }
    pthread_mutex_unlock(&cs->mu);
    AshStatus st = ash_contract_break(inst);
    uint8_t p[4];
    ash_net_put_u32(p, (uint32_t)st);
    cs_send(cs, ASH_WIRE_BROKEN, req_id, p, 4);
}

/* PARTIAL_QUERY: one frame snapshot of the instance's item states and its
 * broken pledges' errors. Items are grouped by state, which the client reads by
 * state, and every read takes the instance lock the local surface takes. */
static void handle_partial(ConnState* cs, uint64_t req_id, const uint8_t* pl,
                           uint32_t plen) {
    AshRBuf r;
    ash_rbuf_init(&r, pl, plen);
    uint64_t id;
    if (!ash_rbuf_u64(&r, &id)) {
        cs_error(cs, req_id, ASH_ERR_TYPE, "malformed PARTIAL_QUERY");
        return;
    }
    AshContract* inst = cs_find(cs, id);
    if (!inst) {
        cs_error(cs, req_id, ASH_ERR_STATE, "no such instance");
        return;
    }
    AshWBuf w;
    ash_wbuf_init(&w);
    ash_wbuf_u32(&w, (uint32_t)ash_contract_state(inst));
    AshItemState states[3] = { ASH_ITEM_PENDING, ASH_ITEM_FULFILLED,
                               ASH_ITEM_BROKEN };
    size_t total = 0;
    for (int s = 0; s < 3; s++) total += ash_partial_count(inst, states[s]);
    ash_wbuf_u32(&w, (uint32_t)total);
    for (int s = 0; s < 3; s++) {
        size_t n = ash_partial_count(inst, states[s]);
        for (size_t i = 0; i < n; i++) {
            const char* nm = ash_partial_name(inst, states[s], i);
            if (!nm) nm = "";
            ash_wbuf_str(&w, nm, strlen(nm));
            ash_wbuf_u32(&w, (uint32_t)states[s]);
        }
    }
    size_t nerr = ash_partial_nerrors(inst);
    ash_wbuf_u32(&w, (uint32_t)nerr);
    for (size_t i = 0; i < nerr; i++) {
        const char* en = NULL;
        const AshValue* ev = NULL;
        if (ash_partial_error(inst, i, &en, &ev) == ASH_OK && en && ev) {
            ash_wbuf_str(&w, en, strlen(en));
            ash_wbuf_value(&w, ev);
        } else {
            AshValue unit;
            memset(&unit, 0, sizeof unit);
            unit.ty = ASH_TY_UNIT;
            ash_wbuf_str(&w, "", 0);
            ash_wbuf_value(&w, &unit);
        }
    }
    if (w.err) {
        ash_wbuf_free(&w);
        cs_error(cs, req_id, ASH_ERR_OOM, "partial encode");
        return;
    }
    cs_send(cs, ASH_WIRE_PARTIAL, req_id, w.data, (uint32_t)w.len);
    ash_wbuf_free(&w);
}

static ConnState* connstate_new(Daemon* d, int fd) {
    ConnState* cs = (ConnState*)calloc(1, sizeof(ConnState));
    if (!cs) return NULL;
    cs->d = d;
    cs->fd = fd;
    cs->next_id = 1;
    if (pthread_mutex_init(&cs->wmu, NULL) != 0) {
        free(cs);
        return NULL;
    }
    if (pthread_mutex_init(&cs->mu, NULL) != 0) {
        pthread_mutex_destroy(&cs->wmu);
        free(cs);
        return NULL;
    }
    if (pthread_cond_init(&cs->cv, NULL) != 0) {
        pthread_mutex_destroy(&cs->mu);
        pthread_mutex_destroy(&cs->wmu);
        free(cs);
        return NULL;
    }
    return cs;
}

/* The connection's death breaks every instance it signed, so an absent client
 * cannot pin daemon memory, and the waiters drain first so nothing is read out
 * of a heap the break reclaims. */
static void connstate_free(ConnState* cs) {
    pthread_mutex_lock(&cs->mu);
    while (cs->live_waiters > 0) pthread_cond_wait(&cs->cv, &cs->mu);
    pthread_mutex_unlock(&cs->mu);
    for (size_t i = 0; i < cs->ninsts; i++) ash_contract_break(cs->insts[i].inst);
    free(cs->insts);
    pthread_cond_destroy(&cs->cv);
    pthread_mutex_destroy(&cs->mu);
    pthread_mutex_destroy(&cs->wmu);
    free(cs);
}

/* Serves HELLO_OK then the message loop. The frozen dump is read straight off
 * the daemon; no lock is needed because nothing writes it after the first
 * accept. The reader dispatches every request in arrival order. */
static void serve(ConnState* cs, uint64_t hello_id) {
    const Daemon* d = cs->d;
    uint8_t ok[12];
    ash_net_put_u32(ok, ASHD_PROTO_VERSION);
    ash_net_put_u64(ok + 4, d->dump_hash);
    if (cs_send(cs, ASH_WIRE_HELLO_OK, hello_id, ok, sizeof ok) != 0) return;
    for (;;) {
        AshWireFrame fr;
        uint8_t* pl = NULL;
        int rc = ash_net_recv_frame(cs->fd, &fr, &pl);
        if (rc == -1) return; /* the peer left */
        if (rc == -2) {
            cs_error(cs, 0, ASH_ERR_TYPE, "malformed frame");
            free(pl);
            return;
        }
        switch (fr.kind) {
        case ASH_WIRE_INAME_SYNC: {
            uint32_t outlen = (uint32_t)(4 + d->dump_len);
            uint8_t* out = (uint8_t*)malloc(outlen);
            if (out) {
                ash_net_put_u32(out, (uint32_t)d->dump_len);
                if (d->dump_len) memcpy(out + 4, d->dump, d->dump_len);
                cs_send(cs, ASH_WIRE_INAME_TABLE, fr.request_id, out, outlen);
                free(out);
            } else {
                cs_error(cs, fr.request_id, ASH_ERR_OOM, "out of memory");
            }
            break;
        }
        case ASH_WIRE_SIGN:
            handle_sign(cs, fr.request_id, pl, fr.payload_len);
            break;
        case ASH_WIRE_FULFILL:
            handle_fulfill(cs, fr.request_id, pl, fr.payload_len);
            break;
        case ASH_WIRE_BREAK:
            handle_break(cs, fr.request_id, pl, fr.payload_len);
            break;
        case ASH_WIRE_PARTIAL_QUERY:
            handle_partial(cs, fr.request_id, pl, fr.payload_len);
            break;
        default:
            /* A reply kind, or one no v1 client sends unsolicited, is a state
             * error, and the connection stays open. */
            cs_error(cs, fr.request_id, ASH_ERR_STATE, "unexpected message kind");
            break;
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

    /* The handshake is on a clock; a stalled HELLO reaps the thread, and the
     * send side carries the same bound so a client that never drains its socket
     * cannot park this thread on the HELLO_OK or the refusal write either. */
    ash_net_set_rcvtimeo(fd, d->handshake_ms);
    ash_net_set_sndtimeo(fd, d->handshake_ms);

    AshWireFrame fr;
    uint8_t* pl = NULL;
    int rc = ash_net_recv_frame(fd, &fr, &pl);
    if (rc == 0 && fr.kind == ASH_WIRE_HELLO) {
        AshStatus st = check_hello(d, pl, fr.payload_len);
        if (st == ASH_OK) {
            /* The handshake finished, so the clocks come off: a fulfillment
             * runs as long as its pledge runs, no protocol deadline above it,
             * and a RESULT write waits on the transport rather than a clock. */
            ash_net_set_rcvtimeo(fd, 0);
            ash_net_set_sndtimeo(fd, 0);
            ConnState* cs = connstate_new(d, fd);
            if (cs) {
                serve(cs, fr.request_id);
                connstate_free(cs);
            }
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
