/* mesh.c: the serve side of libashrt, the accept loop and the per connection
 * dispatch lifted out of the ashd daemon into a library call so any host that
 * links the runtime is a mesh node. It is the far side of ash_runtime_connect:
 * connect signs and fulfills a peer's contracts, serve runs the pledges and
 * answers. The two were the client and the server of one Layer 2 program; here
 * serve becomes a call, so a single process can do both and the roles stop
 * being two programs and become two halves of one node.
 *
 * The whole of this file was ashd.c's file scoped state and thread bodies. The
 * daemon's globals, the listening socket, the stop flag, and the connection
 * table, move onto an AshServer handle, so two servers in one process own two
 * tables and two listeners and never touch each other's memory. ashd shrinks to
 * a main that parses its arguments, brings up a runtime, and makes one
 * ash_runtime_serve call.
 *
 * The concurrency model is thread per connection over blocking sockets, the
 * ashd model unchanged: the mesh serves a project's nodes, not ten thousand
 * sockets, and the model already composes with the runtime's own threaded
 * calls. ash_runtime_serve binds the address, snapshots the frozen dump, and
 * starts one accept thread; each accepted connection gets a thread that owns
 * its socket for the connection's life, and each fulfillment on it a detached
 * waiter. ash_server_stop is the mirror of the daemon's signal driven shutdown
 * lifted into a call: stop the accept loop, shut every live connection socket
 * to wake a thread blocked in a read, join them all, and free the handle. The
 * library installs no signal handler and touches no signal disposition; a host
 * that wants a SIGINT to stop a server installs its own and calls stop from it,
 * which is exactly what ashd's main does. */

#include <ash/ash.h>

#include "ash_net.h"
#include "ash_remote.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The one protocol version v1 speaks, the same integer ashd carried and the
 * client sends; the mesh changes no wire, so this is Layer 2's version. */
#define ASH_MESH_PROTO_VERSION 1

/* One accepted connection: its socket and its thread. fd goes to -1 under the
 * server's connection lock when the thread closes it, so a stop never shuts
 * down a descriptor a later accept reused. */
typedef struct Conn {
    pthread_t th;
    int       th_set;
    int       fd;
} Conn;

/* The server handle, the reentrant replacement for ashd's file scoped globals.
 * Everything a connection thread reads about the served world lives here: the
 * runtime, the expected token, and the frozen dump built once before the first
 * accept and never mutated after, so the threads share it without a lock. The
 * listener, the accept thread, the stop flag, and the connection table were the
 * daemon's g_ globals; on the handle they are per server, so two servers in one
 * process do not collide. */
struct AshServer {
    AshRuntime* rt;
    char*       token;        /* the expected token bytes, empty string for none */
    size_t      token_len;
    char*       dump;         /* the canonical iname dump text, no terminator */
    size_t      dump_len;
    uint64_t    dump_hash;    /* FNV-1a 64 of the dump text */
    uint32_t    handshake_ms; /* the first HELLO's clock, in milliseconds */

    int         listen_fd;
    pthread_t   accept_th;
    int         accept_started;

    /* The connection table and the stop flag, both guarded by conns_mu. stop is
     * set by ash_server_stop and read by the accept loop; guarding it under the
     * lock the table already needs keeps the handoff a clean happens before
     * rather than the signal handler's volatile the daemon relied on. */
    pthread_mutex_t conns_mu;
    int             stop;
    Conn*           conns;
    size_t          nconns;
    size_t          conns_cap;
};

/* One argument struct per spawned connection thread, the server and the
 * connection index the thread owns. The index is stable across a table realloc,
 * a pointer would not be. */
typedef struct ConnArg {
    AshServer* srv;
    size_t     idx;
} ConnArg;

/* Constant time token compare, so a wrong token cannot be discovered a byte at
 * a time by timing the refusal. Every byte of the longer input is folded in and
 * the length difference is folded in too, so neither the contents nor the
 * length leak through an early return. */
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

/* Answers a request the normal reply cannot serve. The status is normative and
 * the string is for humans; a failure to write it is the peer's problem, not
 * ours, so the return is ignored and the caller closes regardless. */
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
 * server speaks, and compares the token in constant time. Returns ASH_OK when
 * the peer may proceed, or the status an ERROR should carry. */
static AshStatus check_hello(const AshServer* srv, const uint8_t* pl,
                             uint32_t plen) {
    if (plen < 4) return ASH_ERR_TYPE;
    uint32_t version = ash_net_get_u32(pl);
    if (plen < 8) return ASH_ERR_TYPE;
    uint32_t tlen = ash_net_get_u32(pl + 4);
    if ((uint64_t)tlen + 8 > plen) return ASH_ERR_TYPE;
    if (version != ASH_MESH_PROTO_VERSION) return ASH_ERR_VERSION;
    const char* tok = (const char*)(pl + 8);
    if (!token_eq(tok, tlen, srv->token, srv->token_len)) return ASH_ERR_NET;
    return ASH_OK;
}

/* ---- the served connection ----
 *
 * Past the handshake a connection carries the fulfillment path, so it grows a
 * state the handshake had no need for. The connection thread is the reader: it
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
 * value out of a heap the break is freeing. This is the server's answer to the
 * one race the value delivery would otherwise open. */

typedef struct InstEntry {
    uint64_t     id;
    AshContract* inst;
    int          outstanding; /* in flight fulfill waiters on this instance */
} InstEntry;

typedef struct ConnState {
    AshServer*      srv;
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
        arena = ash_scratch_new(cs->srv->rt);
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
        st = ash_contract_sign(cs->srv->rt, namez, binds, nov, expected, &inst);
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

static ConnState* connstate_new(AshServer* srv, int fd) {
    ConnState* cs = (ConnState*)calloc(1, sizeof(ConnState));
    if (!cs) return NULL;
    cs->srv = srv;
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
 * cannot pin server memory, and the waiters drain first so nothing is read out
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
 * the server handle; no lock is needed because nothing writes it after the
 * first accept. The reader dispatches every request in arrival order. */
static void serve(ConnState* cs, uint64_t hello_id) {
    const AshServer* srv = cs->srv;
    uint8_t ok[12];
    ash_net_put_u32(ok, ASH_MESH_PROTO_VERSION);
    ash_net_put_u64(ok + 4, srv->dump_hash);
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
            uint32_t outlen = (uint32_t)(4 + srv->dump_len);
            uint8_t* out = (uint8_t*)malloc(outlen);
            if (out) {
                ash_net_put_u32(out, (uint32_t)srv->dump_len);
                if (srv->dump_len) memcpy(out + 4, srv->dump, srv->dump_len);
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
    AshServer* srv = ca->srv;
    size_t idx = ca->idx;
    free(ca);

    pthread_mutex_lock(&srv->conns_mu);
    int fd = srv->conns[idx].fd;
    pthread_mutex_unlock(&srv->conns_mu);

    /* The handshake is on a clock; a stalled HELLO reaps the thread, and the
     * send side carries the same bound so a client that never drains its socket
     * cannot park this thread on the HELLO_OK or the refusal write either. */
    ash_net_set_rcvtimeo(fd, srv->handshake_ms);
    ash_net_set_sndtimeo(fd, srv->handshake_ms);

    AshWireFrame fr;
    uint8_t* pl = NULL;
    int rc = ash_net_recv_frame(fd, &fr, &pl);
    if (rc == 0 && fr.kind == ASH_WIRE_HELLO) {
        AshStatus st = check_hello(srv, pl, fr.payload_len);
        if (st == ASH_OK) {
            /* The handshake finished, so the clocks come off: a fulfillment
             * runs as long as its pledge runs, no protocol deadline above it,
             * and a RESULT write waits on the transport rather than a clock. */
            ash_net_set_rcvtimeo(fd, 0);
            ash_net_set_sndtimeo(fd, 0);
            ConnState* cs = connstate_new(srv, fd);
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

    pthread_mutex_lock(&srv->conns_mu);
    close(fd);
    srv->conns[idx].fd = -1;
    pthread_mutex_unlock(&srv->conns_mu);
    return NULL;
}

/* Reserves a connection slot under the lock, returning its index or -1 when the
 * table cannot grow. The slot's fd is set before the thread is spawned so the
 * thread reads a live descriptor. */
static long conn_reserve(AshServer* srv, int fd) {
    pthread_mutex_lock(&srv->conns_mu);
    if (srv->nconns == srv->conns_cap) {
        size_t cap = srv->conns_cap ? srv->conns_cap * 2 : 16;
        Conn* grown = (Conn*)realloc(srv->conns, cap * sizeof(Conn));
        if (!grown) {
            pthread_mutex_unlock(&srv->conns_mu);
            return -1;
        }
        srv->conns = grown;
        srv->conns_cap = cap;
    }
    size_t idx = srv->nconns++;
    srv->conns[idx].fd = fd;
    srv->conns[idx].th_set = 0;
    pthread_mutex_unlock(&srv->conns_mu);
    return (long)idx;
}

/* Builds the frozen dump the server serves: the canonical text and the FNV-1a
 * 64 the HELLO_OK carries. Called once after freeze, before the first accept. */
static int build_dump(AshServer* srv) {
    size_t need = 0;
    if (ash_iname_dump(srv->rt, NULL, 0, &need) != ASH_ERR_OOM || need == 0) {
        return -1;
    }
    char* buf = (char*)malloc(need);
    if (!buf) return -1;
    if (ash_iname_dump(srv->rt, buf, need, &need) != ASH_OK) {
        free(buf);
        return -1;
    }
    srv->dump = buf;
    srv->dump_len = need - 1; /* the served text excludes the terminating NUL */
    srv->dump_hash = ash_net_fnv1a64((const uint8_t*)buf, srv->dump_len);
    return 0;
}

/* The accept loop, ashd's main loop lifted onto a background thread. It reads
 * the stop flag under the connection lock rather than a signal handler's
 * volatile, since ash_server_stop sets it from another thread and shutting the
 * listener down wakes the accept at once. Every accepted connection is reserved
 * a slot and handed a thread; a transient accept error is not fatal, and a stop
 * breaks the loop. */
static void* accept_thread(void* arg) {
    AshServer* srv = (AshServer*)arg;
    for (;;) {
        pthread_mutex_lock(&srv->conns_mu);
        int stop = srv->stop;
        pthread_mutex_unlock(&srv->conns_mu);
        if (stop) break;

        int cfd = accept(srv->listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            pthread_mutex_lock(&srv->conns_mu);
            stop = srv->stop;
            pthread_mutex_unlock(&srv->conns_mu);
            if (stop) break;
            continue; /* a transient accept error is not fatal to the loop */
        }
        long idx = conn_reserve(srv, cfd);
        if (idx < 0) {
            close(cfd);
            continue;
        }
        ConnArg* ca = (ConnArg*)malloc(sizeof(ConnArg));
        if (!ca) {
            pthread_mutex_lock(&srv->conns_mu);
            close(cfd);
            srv->conns[idx].fd = -1;
            pthread_mutex_unlock(&srv->conns_mu);
            continue;
        }
        ca->srv = srv;
        ca->idx = (size_t)idx;
        pthread_t tid;
        if (pthread_create(&tid, NULL, conn_thread, ca) != 0) {
            free(ca);
            pthread_mutex_lock(&srv->conns_mu);
            close(cfd);
            srv->conns[idx].fd = -1;
            pthread_mutex_unlock(&srv->conns_mu);
            continue;
        }
        pthread_mutex_lock(&srv->conns_mu);
        srv->conns[idx].th = tid;
        srv->conns[idx].th_set = 1;
        pthread_mutex_unlock(&srv->conns_mu);
    }
    return NULL;
}

AshStatus ash_runtime_serve(AshRuntime* rt, const char* addr, const char* token,
                            AshServer** out) {
    if (!rt || !addr || !out) return ASH_ERR_TYPE;
    *out = NULL;

    AshServer* srv = (AshServer*)calloc(1, sizeof(AshServer));
    if (!srv) return ASH_ERR_OOM;
    srv->rt = rt;
    srv->listen_fd = -1;
    srv->handshake_ms = ash_runtime_handshake_ms(rt);
    if (pthread_mutex_init(&srv->conns_mu, NULL) != 0) {
        free(srv);
        return ASH_ERR_OOM;
    }

    /* The server owns its token bytes, a copy, so the caller's buffer need not
     * outlive the call; a NULL token is a tokenless endpoint, an empty string
     * the constant time compare accepts only an empty token against. */
    size_t tlen = token ? strlen(token) : 0;
    srv->token = (char*)malloc(tlen + 1);
    if (!srv->token) {
        pthread_mutex_destroy(&srv->conns_mu);
        free(srv);
        return ASH_ERR_OOM;
    }
    if (tlen) memcpy(srv->token, token, tlen);
    srv->token[tlen] = '\0';
    srv->token_len = tlen;

    /* Freeze first, then snapshot the dump: serve fixes the offered surface the
     * moment a node is reachable, so a runtime is loaded, bound, connected to
     * its peers, and only then served. Freeze is idempotent, so a host that
     * already froze pays nothing here. */
    ash_runtime_freeze(rt);
    if (build_dump(srv) != 0) {
        free(srv->token);
        pthread_mutex_destroy(&srv->conns_mu);
        free(srv);
        return ASH_ERR_OOM;
    }

    srv->listen_fd = ash_net_listen(addr);
    if (srv->listen_fd < 0) {
        free(srv->dump);
        free(srv->token);
        pthread_mutex_destroy(&srv->conns_mu);
        free(srv);
        return ASH_ERR_NET;
    }

    if (pthread_create(&srv->accept_th, NULL, accept_thread, srv) != 0) {
        close(srv->listen_fd);
        free(srv->dump);
        free(srv->token);
        pthread_mutex_destroy(&srv->conns_mu);
        free(srv);
        return ASH_ERR_OOM;
    }
    srv->accept_started = 1;

    *out = srv;
    return ASH_OK;
}

void ash_server_stop(AshServer* server) {
    if (!server) return;

    /* Stop accepting, and shut the listener down so an accept blocked in the
     * kernel returns at once; the loop reads the flag and breaks. The listen fd
     * is read under the lock the same accept loop takes, so the stop and the
     * shutdown race nothing. */
    pthread_mutex_lock(&server->conns_mu);
    server->stop = 1;
    int lfd = server->listen_fd;
    pthread_mutex_unlock(&server->conns_mu);
    if (lfd >= 0) shutdown(lfd, SHUT_RDWR);
    if (server->accept_started) pthread_join(server->accept_th, NULL);

    /* The accept loop is joined, so the table stops growing, but its connection
     * threads run on and each sets its own fd to -1 under the lock as it closes.
     * Wake every thread still blocked in a read by shutting its socket, the
     * shutdown loop under the lock so it never races a thread's own close, then
     * join them all with the lock dropped, since a joined thread takes the lock
     * on its way out. Each breaks the instances it signed as it unwinds, so no
     * server memory outlives the stop. */
    pthread_mutex_lock(&server->conns_mu);
    for (size_t i = 0; i < server->nconns; i++) {
        if (server->conns[i].fd >= 0) shutdown(server->conns[i].fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&server->conns_mu);
    for (size_t i = 0; i < server->nconns; i++) {
        pthread_mutex_lock(&server->conns_mu);
        int set = server->conns[i].th_set;
        pthread_t th = server->conns[i].th;
        pthread_mutex_unlock(&server->conns_mu);
        if (set) pthread_join(th, NULL);
    }

    if (server->listen_fd >= 0) close(server->listen_fd);
    free(server->conns);
    free(server->dump);
    free(server->token);
    pthread_mutex_destroy(&server->conns_mu);
    free(server);
}
