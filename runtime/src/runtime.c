/* runtime.c: the M7 intermediary runtime. One translation unit on purpose;
 * the split into contract.c, iname.c, and friends happens when there is more
 * than one contract's worth of machinery to split. What exists today is the
 * whole path a compiled module and a foreign host share: load a module,
 * register its descriptors, bind host implementations over abstract or
 * compiled pledges, sign a contract with vow overrides over the declared
 * defaults, dispatch fulfillments through the uniform thunk frame on a real
 * thread pool, latch the pledge outcome, and reclaim everything at break.
 *
 * M7 adds the requirements evaluator and the partial result. Every pledge
 * carries its own latch now, fulfilled on the first Ok, broken on an Err
 * before any Ok, immutable after either, and the contract state is
 * recomputed after every fulfillment by evaluating the descriptor's policy
 * lines in priority order break, fulfill, partial over those latches. The
 * partial surface reports the item states and the first Err payload of every
 * broken pledge under the same instance lock.
 *
 * M6 adds the iname table and the freeze. Registration fills a sorted
 * registry of contract types keyed by mangled name, one entry per contract
 * and one per pledge; ash_runtime_freeze latches the registration surface
 * shut, after which load, register, and bind report ASH_ERR_STATE while
 * sign and fulfill continue unchanged. Every iname read takes the runtime
 * lock, the simple discipline TSan can vouch for.
 *
 * Memory follows one rule. Every allocation a pledge makes goes through the
 * instance's block list, vow values and frames included, so
 * ash_contract_break frees the lot in one walk and valgrind stays clean
 * whatever the pledge did. The one exception threading forced: the future
 * struct itself is heap memory the runtime tracks per instance and frees at
 * shutdown, so a wait that races a break lands on a live struct and reports
 * ASH_ERR_STATE instead of touching freed memory. Everything a future's
 * value points at is still instance owned and still dies at break, which is
 * why the wait-before-break rule keeps mattering to a host that wants the
 * bytes.
 *
 * M5 makes fulfillment concurrent for real. ash_pledge_fulfill validates and
 * copies in on the caller's thread, exactly the M4 boundary, then queues the
 * work; a pool worker runs the thunk; the wait blocks on the future's
 * condvar and performs the ref write back on the waiting thread. Three locks
 * carry the whole design: the runtime lock over the descriptor, instance,
 * and binding tables; the per-instance recursive mutex that serializes every
 * fulfillment, latch, break, and block list allocation touching one
 * instance; and the per-future mutex under its condvar. The pool's queue
 * lock is a leaf taken with no other lock held.
 *
 * Cross-contract calls change the reentrancy story. A pledge body may sign
 * another contract, fulfill its pledges, and break it, so a thunk holding
 * its own instance lock now takes the runtime lock (sign) and another
 * instance's lock (fulfill). ash_pledge_fulfill_sync detects a pool worker
 * through a thread-local flag and runs the nested fulfillment inline on that
 * worker instead of queueing it, since a pool whose every worker is blocked
 * waiting on a queued nested call would starve itself into deadlock. The
 * lock graph stays acyclic for compiled code: the runtime lock is held only
 * over table walks and a fresh, unpublished instance's own mutex, never
 * blocking on a contended instance, and a thunk's nested instances form a
 * tree per thread over the recursive mutexes. Two host bound bodies on
 * different threads fulfilling against each other's shared instances in
 * opposite orders can still deadlock; that cycle needs instances handed
 * around outside the language, and v1 documents it instead of detecting
 * it. */

#include <ash/ash.h>

#include "ash_net.h"
#include "ash_remote.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ASH_HANDSHAKE_MS_DEFAULT 10000

#define ASH_MAX_CONTRACT_TYPES 64
#define ASH_MAX_MODULES        64
#define ASH_MAX_INSTANCES      256
#define ASH_MAX_BINDINGS       128

#define ASH_POOL_DEFAULT_THREADS 4
#define ASH_POOL_MAX_THREADS     256

/* A block the instance owns. Blocks form a singly linked list headed in the
 * instance; the header rides in front of the caller's bytes. */
typedef struct AshBlock {
    struct AshBlock* next;
} AshBlock;

/* The per-pledge latch values. A pledge latches on its first Ok or first
 * Err and never moves again; the contract state is recomputed from these
 * latches after every fulfillment. */
enum {
    PLEDGE_PENDING   = 0,
    PLEDGE_FULFILLED = 1,
    PLEDGE_BROKEN    = 2
};

/* A client's connection to one daemon, defined in full with the remote path at
 * the end of the file. A proxy instance signed over it routes its lifecycle
 * across the wire; the struct is forward declared here so the instance can hold
 * one. */
typedef struct AshConn AshConn;

struct AshContract {
    AshRuntime*            rt;
    const AshContractDesc* desc;
    AshContractState       state;
    AshBlock*              owned;
    AshValue*              vow_vals;  /* one per desc vow, instance owned */
    AshPledgeFn*           fns;       /* dispatch table resolved at sign */
    struct AshFuture*      futures;   /* every future this instance issued */
    uint64_t               shape_hash;
    int64_t                signed_at;

    /* Remote proxy fields, all zero for a local instance. conn is the one test
     * that tells a proxy from a local instance: non NULL means every lifecycle
     * call routes to the wire instead of the pool. remote_id is the daemon's
     * handle for this instance, meaningless off the connection. The vow set the
     * SIGNED reply carried is stored here so ash_vow_ref, hash, and signed_at
     * are local reads, sound because vows are immutable from the sign. */
    AshConn*               conn;
    uint64_t               remote_id;
    struct AshContract*    proxy_next;   /* conn's list of its proxies */
    char**                 rvow_names;   /* instance owned, remote_nvows of them */
    AshValue*              rvow_vals;
    uint32_t               remote_nvows;

    /* The latches, one slot per descriptor pledge, and the first Err payload
     * each broken pledge carried. Both arrays are plain heap freed at
     * shutdown, not instance blocks, so the partial surface stays readable
     * after a break; the payload structs point into the instance heap, so an
     * explicit break zeroes them when it frees that heap, while an automatic
     * break leaves both alone on purpose, the errors are what it reports. */
    uint8_t*               pledge_state; /* PLEDGE_*, one per pledge */
    AshValue*              pledge_err;   /* first Err payload per pledge */
    pthread_mutex_t        mu;        /* recursive; the instance lock */
};

/* A future is the receipt of one fulfillment. Between the fulfill and the
 * worker it is also the task: the dispatch fn, the prepared frame, and the
 * queue link ride inside it, so the pool queue allocates nothing. The struct
 * is heap memory tracked on the instance's futures list and freed at
 * shutdown, or earlier by the synchronous path once its one wait has
 * delivered; a break forfeits every unwaited future to ASH_ERR_STATE and
 * clears its pointers into the instance heap before that heap goes away. */
struct AshFuture {
    struct AshFuture* next;    /* instance futures list */
    struct AshFuture* qnext;   /* pool queue link */
    AshContract*      c;
    AshPledgeFn       fn;
    uint32_t          pidx;    /* descriptor index of the pledge, for latch */
    AshValue*         frame;   /* instance owned, one slot per parameter */
    size_t            frame_nargs;
    AshStatus         status;
    uint32_t          done;
    uint32_t          waited;
    AshValue          value;
    AshRef*           refs;      /* instance owned copy of the caller's refs */
    AshValue*         ref_slots; /* the mutable trailing slots of the frame */
    size_t            nrefs;
    uint32_t          refcnt;    /* holders: the receipt, plus the pool */
    uint64_t          req_id;    /* remote fulfill: the request id its RESULT
                                  * echoes; qnext links it in the connection's
                                  * pending map instead of the pool queue */
    pthread_mutex_t   mu;
    pthread_cond_t    cv;
};

/* A host implementation bound over one pledge descriptor. The overlay lives
 * on the runtime because the descriptor tables are const data inside the
 * module image. */
typedef struct AshBinding {
    const AshPledgeDesc* pd;
    AshPledgeFn          fn;
} AshBinding;

/* A block of heap the runtime keeps for the life of a connection: the retained
 * iname dump text a remote merge tokenizes in place, which the merged entries
 * borrow into exactly the way a local pledge entry borrows into a module
 * image. They are freed wholesale at shutdown. */
typedef struct AshNetBuf {
    struct AshNetBuf* next;
    void*             p;
    size_t            n;
} AshNetBuf;

struct AshRuntime {
    const AshContractDesc* descs[ASH_MAX_CONTRACT_TYPES];
    size_t                 ndescs;
    void*                  modules[ASH_MAX_MODULES];
    size_t                 nmodules;
    AshContract*           instances[ASH_MAX_INSTANCES];
    size_t                 ninstances;
    AshBinding             bindings[ASH_MAX_BINDINGS];
    size_t                 nbindings;

    /* The iname table: one entry per registered contract and one per pledge,
     * kept sorted by mangled name so lookup is a binary search and the dump
     * is byte stable. A contract entry's mangled string is runtime owned
     * heap; a pledge entry borrows its descriptor's string. frozen latches
     * the registration surface shut; sign and fulfill never read it. */
    AshInameEntry*         inames;
    size_t                 ninames;
    size_t                 iname_cap;
    int                    frozen;
    pthread_mutex_t        lock;      /* guards the tables above */

    /* The handshake timeout ash_runtime_connect gives a daemon, resolved from
     * the config at init, and the retained buffers remote merges borrow into.
     * net_bufs is guarded by lock, the same discipline the iname table keeps. */
    uint32_t               handshake_ms;
    AshNetBuf*             net_bufs;

    /* The remote surface: one open connection per daemon, and one row per
     * remote contract naming the connection that serves it, so a sign by plain
     * contract name resolves to an origin the same way find_desc resolves a
     * local one. Both are guarded by lock. */
    AshConn*               conns;
    struct RemoteContract* remotes;
    size_t                 nremotes;
    size_t                 remotes_cap;

    /* The pool: nworkers threads draining one unbounded intrusive queue of
     * futures. qmu and qcv are a leaf lock pair, never held with another. */
    pthread_t*             workers;
    uint32_t               nworkers;
    struct AshFuture*      qhead;
    struct AshFuture*      qtail;
    int                    qstop;
    pthread_mutex_t        qmu;
    pthread_cond_t         qcv;
};

typedef AshStatus (*AshRegisterFn)(AshRuntime*);

/* The latch readers the partial surface shares with the requirements
 * evaluator further down; both run under the instance lock. */
static int pledge_is_loose(const AshContractDesc* d, uint32_t i);
static int sub_all(const AshContract* c, uint32_t s, uint8_t want);

/* Whether a pointer falls inside one of the runtime's retained net buffers,
 * the test that tells a remote iname string from a locally owned one at
 * shutdown. Defined with the connect path at the end of the file. */
static int net_owned(const AshRuntime* rt, const void* ptr);

/* The remote lifecycle, all defined with the connect path at the end of the
 * file. A proxy routes sign, fulfill, break, state, and the partial surface to
 * the wire; conn_shutdown tears one connection down, forfeiting its in flight
 * work to ASH_ERR_NET the way a local break forfeits to ASH_ERR_STATE. */
typedef struct RemoteContract {
    const char* name;        /* the plain contract name, runtime owned */
    uint64_t    shape_hash;
    uint32_t    version;
    AshConn*    conn;
} RemoteContract;

/* A pending synchronous reply, the sign, break, or partial call's receipt. The
 * caller links one keyed by request id, sends its frame, and blocks on the cv
 * until the reader delivers the answer or the connection dies. */
typedef struct PendingReply {
    struct PendingReply* next;
    uint64_t             req_id;
    int                  done;
    int                  dead;   /* the connection failed before the answer */
    AshWireFrame         fr;
    uint8_t*             payload;
    pthread_mutex_t      mu;
    pthread_cond_t       cv;
} PendingReply;

/* One connection to one daemon. A single reader thread owns every read; every
 * write goes under wmu so a detached decode never interleaves bytes with a
 * fulfill. Requests register in one of two maps keyed by request id, futures
 * for fulfillments and replies for the synchronous calls, and the reader routes
 * each answer to its waiter. proxies is the list of instances signed here, so a
 * disconnect can latch them all Broken. */
struct AshConn {
    struct AshConn*   next;      /* rt->conns */
    AshRuntime*       rt;
    int               fd;
    pthread_mutex_t   wmu;       /* serializes frame writes */
    pthread_t         reader;
    int               reader_started;

    pthread_mutex_t   pmu;       /* guards everything below */
    uint64_t          next_req;
    int               dead;
    int               shutdown_done;
    struct AshFuture* pending_futures;  /* linked via qnext */
    PendingReply*     pending_replies;
    AshContract*      proxies;          /* linked via proxy_next */
};

static AshStatus remote_sign(AshRuntime* rt, RemoteContract* rc,
                             const AshVowBinding* vows, size_t nvows,
                             uint64_t expected_hash, AshContract** out);
static AshFuture* remote_fulfill(AshContract* c, const char* pledge_name,
                                 const AshValue* args, size_t nargs,
                                 const AshRef* refs, size_t nrefs);
static AshStatus remote_break(AshContract* c);
static AshContractState remote_state(AshContract* c);
static size_t remote_partial_count(AshContract* c, AshItemState k);
static const char* remote_partial_name(AshContract* c, AshItemState k, size_t i);
static size_t remote_partial_nerrors(AshContract* c);
static AshStatus remote_partial_error(AshContract* c, size_t i,
                                      const char** pledge_name,
                                      const AshValue** err);
static void conn_shutdown(AshConn* conn);
static void conn_free(AshConn* conn);
static RemoteContract* find_remote(AshRuntime* rt, const char* name);

/* ---- locking primitives ---- */

/* The instance lock is recursive on purpose: a pool worker holds it across
 * the whole thunk run, and the allocation helpers the thunk calls lock it
 * again. The same helpers called by a host outside any fulfillment, the
 * arena pattern test_value drives, take the lock cold and are just as safe. */
static int mutex_init_recursive(pthread_mutex_t* mu) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return -1;
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int rc = pthread_mutex_init(mu, &attr);
    pthread_mutexattr_destroy(&attr);
    return rc;
}

/* ---- futures: allocation, completion, forfeit ---- */

static void future_free(struct AshFuture* f) {
    pthread_mutex_destroy(&f->mu);
    pthread_cond_destroy(&f->cv);
    free(f);
}

/* A fresh future linked onto its instance. NULL on any allocation failure;
 * nothing is left behind in that case. */
static struct AshFuture* future_new(AshContract* c) {
    struct AshFuture* f = calloc(1, sizeof(struct AshFuture));
    if (!f) return NULL;
    if (pthread_mutex_init(&f->mu, NULL) != 0) {
        free(f);
        return NULL;
    }
    if (pthread_cond_init(&f->cv, NULL) != 0) {
        pthread_mutex_destroy(&f->mu);
        free(f);
        return NULL;
    }
    f->c = c;
    f->refcnt = 1;
    pthread_mutex_lock(&c->mu);
    f->next = c->futures;
    c->futures = f;
    pthread_mutex_unlock(&c->mu);
    return f;
}

/* Drops one hold on the future and frees it when nothing holds it anymore.
 * Two parties can hold a future at once, the receipt the host waits and the
 * pool queue it rides through; a break can finish the receipt's side before
 * the worker has even dequeued the task, so neither side may free
 * unilaterally. A future still linked on its instance keeps a hold too; only
 * the synchronous path unlinks and drops it early, everything else drops at
 * shutdown. */
static void future_unref(struct AshFuture* f) {
    pthread_mutex_lock(&f->mu);
    uint32_t left = --f->refcnt;
    pthread_mutex_unlock(&f->mu);
    if (left == 0) future_free(f);
}

/* Publishes an outcome exactly once. A future a break already forfeited
 * keeps its ASH_ERR_STATE; the worker's later completion is a no-op. */
static void future_finish(struct AshFuture* f, AshStatus st,
                          const AshValue* val) {
    pthread_mutex_lock(&f->mu);
    if (!f->done) {
        f->status = st;
        if (val) f->value = *val;
        f->done = 1;
        pthread_cond_broadcast(&f->cv);
    }
    pthread_mutex_unlock(&f->mu);
}

/* Break's side of the race. An unwaited future, delivered or not, forfeits
 * to ASH_ERR_STATE and drops every pointer into the instance heap, because
 * that heap is about to be freed and a late wait must find nothing to touch.
 * A waited future is left alone; its one delivery already happened. The
 * caller holds the instance lock, so no thunk is mid-run on this instance
 * and any waiter mid-write-back holds f->mu and finishes before the mark. */
static void future_forfeit(struct AshFuture* f) {
    pthread_mutex_lock(&f->mu);
    if (!f->waited) {
        f->status = ASH_ERR_STATE;
        memset(&f->value, 0, sizeof(f->value));
        f->frame = NULL;
        f->refs = NULL;
        f->ref_slots = NULL;
        f->nrefs = 0;
        f->done = 1;
        pthread_cond_broadcast(&f->cv);
    }
    pthread_mutex_unlock(&f->mu);
}

/* Unlinks one delivered future and drops the receipt's hold on it, the
 * synchronous path's cleanup so a sync-heavy host does not accumulate
 * receipts until shutdown. The pool may still hold the future when a break
 * finished it while it sat queued; the worker's own drop frees it then. */
static void future_release(struct AshFuture* f) {
    AshContract* c = f->c;
    pthread_mutex_lock(&c->mu);
    struct AshFuture** p = &c->futures;
    while (*p && *p != f) p = &(*p)->next;
    if (*p) *p = f->next;
    pthread_mutex_unlock(&c->mu);
    future_unref(f);
}

/* ---- the pool ---- */

static void run_task(struct AshFuture* f);

static void pool_enqueue(AshRuntime* rt, struct AshFuture* f) {
    pthread_mutex_lock(&rt->qmu);
    f->qnext = NULL;
    if (rt->qtail) rt->qtail->qnext = f;
    else rt->qhead = f;
    rt->qtail = f;
    pthread_cond_signal(&rt->qcv);
    pthread_mutex_unlock(&rt->qmu);
}

/* Raised on pool worker threads and read by ash_pledge_fulfill_sync: a
 * synchronous fulfillment started from inside a thunk runs inline on the
 * worker rather than riding the queue it is draining. */
static __thread int t_pool_worker;

/* Workers drain the queue until shutdown raises qstop, and even then they
 * finish what is queued before exiting, so shutdown never strands a future
 * an outstanding wait is parked on. */
static void* pool_worker(void* arg) {
    AshRuntime* rt = (AshRuntime*)arg;
    t_pool_worker = 1;
    for (;;) {
        pthread_mutex_lock(&rt->qmu);
        while (!rt->qhead && !rt->qstop) {
            pthread_cond_wait(&rt->qcv, &rt->qmu);
        }
        struct AshFuture* f = rt->qhead;
        if (f) {
            rt->qhead = f->qnext;
            if (!rt->qhead) rt->qtail = NULL;
        }
        pthread_mutex_unlock(&rt->qmu);
        if (!f) return NULL;
        run_task(f);
        future_unref(f); /* the pool's hold ends with the task */
    }
}

/* ---- the iname table ---- */

/* Binary search over the sorted table. Returns 1 with *pos the index on a
 * hit, 0 with *pos the insertion point on a miss. Called under rt->lock. */
static int iname_find(const AshRuntime* rt, const char* mangled, size_t* pos) {
    size_t lo = 0;
    size_t hi = rt->ninames;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(rt->inames[mid].mangled, mangled);
        if (cmp == 0) {
            *pos = mid;
            return 1;
        }
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    *pos = lo;
    return 0;
}

/* Inserts one entry at its sorted position. A mangled name already in the
 * table is ASH_ERR_NAME; growth failure is ASH_ERR_OOM. Under rt->lock. */
static AshStatus iname_insert(AshRuntime* rt, const AshInameEntry* e) {
    size_t pos;
    if (iname_find(rt, e->mangled, &pos)) return ASH_ERR_NAME;
    if (rt->ninames == rt->iname_cap) {
        size_t cap = rt->iname_cap ? rt->iname_cap * 2 : 16;
        AshInameEntry* grown = realloc(rt->inames, cap * sizeof(*grown));
        if (!grown) return ASH_ERR_OOM;
        rt->inames = grown;
        rt->iname_cap = cap;
    }
    memmove(rt->inames + pos + 1, rt->inames + pos,
            (rt->ninames - pos) * sizeof(AshInameEntry));
    rt->inames[pos] = *e;
    rt->ninames++;
    return ASH_OK;
}

/* The contract level mangled name the runtime synthesizes, since the
 * compiler mangles pledges only: __ash_ash_{contract}__{shapehash16}_v{ver},
 * the pledge format with an empty symbol slot and the shape hash where a
 * pledge carries its signature hash. Heap the runtime owns until shutdown. */
static char* iname_contract_mangled(const AshContractDesc* desc) {
    int n = snprintf(NULL, 0, "__ash_ash_%s__%016llx_v%u", desc->name,
                     (unsigned long long)desc->shape_hash, desc->version);
    if (n < 0) return NULL;
    char* s = malloc((size_t)n + 1);
    if (!s) return NULL;
    snprintf(s, (size_t)n + 1, "__ash_ash_%s__%016llx_v%u", desc->name,
             (unsigned long long)desc->shape_hash, desc->version);
    return s;
}

/* Removes every entry one contract contributed, the rollback when a later
 * insert of the same registration fails. Under rt->lock. */
static void iname_remove_contract(AshRuntime* rt, const char* contract) {
    size_t w = 0;
    for (size_t r = 0; r < rt->ninames; r++) {
        AshInameEntry* e = &rt->inames[r];
        if (strcmp(e->contract, contract) == 0) {
            if (e->kind == ASH_INAME_CONTRACT) free((char*)e->mangled);
            continue;
        }
        rt->inames[w++] = *e;
    }
    rt->ninames = w;
}

/* Fills the table for one registration: the contract entry first, then one
 * entry per pledge that carries a mangled name; a handwritten descriptor
 * whose pledges carry none contributes only its contract entry. All or
 * nothing: any failure removes what this call added. Under rt->lock. */
static AshStatus iname_register(AshRuntime* rt, const AshContractDesc* desc) {
    char* cm = iname_contract_mangled(desc);
    if (!cm) return ASH_ERR_OOM;
    AshInameEntry ce;
    memset(&ce, 0, sizeof(ce));
    ce.mangled = cm;
    ce.kind = ASH_INAME_CONTRACT;
    ce.contract = desc->name;
    ce.symbol = NULL;
    ce.shape_hash = desc->shape_hash;
    ce.version = desc->version;
    AshStatus st = iname_insert(rt, &ce);
    if (st != ASH_OK) {
        free(cm);
        return st;
    }
    for (uint32_t i = 0; i < desc->npledges; i++) {
        const AshPledgeDesc* pd = &desc->pledges[i];
        if (!pd->mangled) continue;
        AshInameEntry pe;
        memset(&pe, 0, sizeof(pe));
        pe.mangled = pd->mangled;
        pe.kind = ASH_INAME_PLEDGE;
        pe.contract = desc->name;
        pe.symbol = pd->name;
        pe.shape_hash = desc->shape_hash;
        pe.version = desc->version;
        pe.nargs = pd->nargs;
        st = iname_insert(rt, &pe);
        if (st != ASH_OK) {
            iname_remove_contract(rt, desc->name);
            return st;
        }
    }
    return ASH_OK;
}

/* ---- runtime lifecycle ---- */

AshStatus ash_runtime_init(const AshRuntimeConfig* cfg, AshRuntime** out) {
    if (!out) return ASH_ERR_TYPE;
    uint32_t nworkers = ASH_POOL_DEFAULT_THREADS;
    if (cfg && cfg->max_threads != 0) {
        if (cfg->max_threads > ASH_POOL_MAX_THREADS) return ASH_ERR_TYPE;
        nworkers = cfg->max_threads;
    }
    AshRuntime* rt = calloc(1, sizeof(AshRuntime));
    if (!rt) return ASH_ERR_OOM;
    if (pthread_mutex_init(&rt->lock, NULL) != 0) {
        free(rt);
        return ASH_ERR_OOM;
    }
    if (pthread_mutex_init(&rt->qmu, NULL) != 0) {
        pthread_mutex_destroy(&rt->lock);
        free(rt);
        return ASH_ERR_OOM;
    }
    if (pthread_cond_init(&rt->qcv, NULL) != 0) {
        pthread_mutex_destroy(&rt->qmu);
        pthread_mutex_destroy(&rt->lock);
        free(rt);
        return ASH_ERR_OOM;
    }
    rt->workers = calloc(nworkers, sizeof(pthread_t));
    if (!rt->workers) {
        pthread_cond_destroy(&rt->qcv);
        pthread_mutex_destroy(&rt->qmu);
        pthread_mutex_destroy(&rt->lock);
        free(rt);
        return ASH_ERR_OOM;
    }
    for (uint32_t i = 0; i < nworkers; i++) {
        if (pthread_create(&rt->workers[i], NULL, pool_worker, rt) != 0) {
            pthread_mutex_lock(&rt->qmu);
            rt->qstop = 1;
            pthread_cond_broadcast(&rt->qcv);
            pthread_mutex_unlock(&rt->qmu);
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(rt->workers[j], NULL);
            }
            free(rt->workers);
            pthread_cond_destroy(&rt->qcv);
            pthread_mutex_destroy(&rt->qmu);
            pthread_mutex_destroy(&rt->lock);
            free(rt);
            return ASH_ERR_OOM;
        }
    }
    rt->nworkers = nworkers;
    rt->handshake_ms = (cfg && cfg->handshake_ms != 0) ? cfg->handshake_ms
                                                       : ASH_HANDSHAKE_MS_DEFAULT;
    *out = rt;
    return ASH_OK;
}

static void contract_free_owned(AshContract* c) {
    AshBlock* b = c->owned;
    while (b) {
        AshBlock* next = b->next;
        free(b);
        b = next;
    }
    c->owned = NULL;
    c->vow_vals = NULL;
    c->fns = NULL;
}

void ash_runtime_shutdown(AshRuntime* rt) {
    if (!rt) return;
    /* Drain and join first. After the joins no worker exists, so the rest of
     * shutdown is single threaded and needs no locks. */
    pthread_mutex_lock(&rt->qmu);
    rt->qstop = 1;
    pthread_cond_broadcast(&rt->qcv);
    pthread_mutex_unlock(&rt->qmu);
    for (uint32_t i = 0; i < rt->nworkers; i++) {
        pthread_join(rt->workers[i], NULL);
    }
    free(rt->workers);
    /* Tear the connections down before the instances they proxy: each
     * conn_shutdown forfeits its in flight work and joins its reader thread, so
     * once the loop returns no reader can be decoding a result onto an instance
     * this function is about to free. The proxy instances themselves are freed
     * by the instance loop below, the same walk that frees a local one. */
    {
        AshConn* conn = rt->conns;
        while (conn) {
            AshConn* next = conn->next;
            conn_shutdown(conn);
            conn_free(conn);
            conn = next;
        }
        rt->conns = NULL;
    }
    for (size_t i = 0; i < rt->ninstances; i++) {
        AshContract* c = rt->instances[i];
        struct AshFuture* f = c->futures;
        while (f) {
            struct AshFuture* next = f->next;
            future_free(f);
            f = next;
        }
        contract_free_owned(c);
        free(c->pledge_state);
        free(c->pledge_err);
        pthread_mutex_destroy(&c->mu);
        free(c);
    }
    for (size_t i = 0; i < rt->ninames; i++) {
        /* A local contract entry's mangled name is its own heap; a remote
         * entry's strings live in a retained net buffer freed below, so it is
         * skipped here to keep the free single owned. */
        if (rt->inames[i].kind == ASH_INAME_CONTRACT &&
            !net_owned(rt, rt->inames[i].mangled)) {
            free((char*)rt->inames[i].mangled);
        }
    }
    free(rt->inames);
    free(rt->remotes);
    AshNetBuf* nb = rt->net_bufs;
    while (nb) {
        AshNetBuf* next = nb->next;
        free(nb->p);
        free(nb);
        nb = next;
    }
    for (size_t i = 0; i < rt->nmodules; i++) {
        dlclose(rt->modules[i]);
    }
    pthread_cond_destroy(&rt->qcv);
    pthread_mutex_destroy(&rt->qmu);
    pthread_mutex_destroy(&rt->lock);
    free(rt);
}

AshStatus ash_module_load(AshRuntime* rt, const char* so_path) {
    if (!rt || !so_path) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    int frozen = rt->frozen;
    pthread_mutex_unlock(&rt->lock);
    if (frozen) return ASH_ERR_STATE;
    void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return ASH_ERR_LOAD;
    AshRegisterFn reg = (AshRegisterFn)dlsym(handle, "ash_module_register");
    if (!reg) {
        dlclose(handle);
        return ASH_ERR_LOAD;
    }
    AshStatus st = reg(rt);
    if (st != ASH_OK) {
        dlclose(handle);
        return st;
    }
    pthread_mutex_lock(&rt->lock);
    if (rt->nmodules == ASH_MAX_MODULES) {
        pthread_mutex_unlock(&rt->lock);
        dlclose(handle);
        return ASH_ERR_OOM;
    }
    rt->modules[rt->nmodules++] = handle;
    pthread_mutex_unlock(&rt->lock);
    return ASH_OK;
}

AshStatus ash_register_contract(AshRuntime* rt, const AshContractDesc* desc) {
    if (!rt || !desc || !desc->name) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    if (rt->frozen) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_STATE;
    }
    if (rt->ndescs == ASH_MAX_CONTRACT_TYPES) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_OOM;
    }
    for (size_t i = 0; i < rt->ndescs; i++) {
        if (strcmp(rt->descs[i]->name, desc->name) == 0) {
            pthread_mutex_unlock(&rt->lock);
            return ASH_ERR_NAME;
        }
    }
    /* The iname entries go in before the descriptor commits, so a mangled
     * name collision or an allocation failure leaves the runtime exactly as
     * it was and the registration reports the failure whole. */
    AshStatus st = iname_register(rt, desc);
    if (st != ASH_OK) {
        pthread_mutex_unlock(&rt->lock);
        return st;
    }
    rt->descs[rt->ndescs++] = desc;
    pthread_mutex_unlock(&rt->lock);
    return ASH_OK;
}

/* ---- the iname surface ---- */

AshStatus ash_runtime_freeze(AshRuntime* rt) {
    if (!rt) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    rt->frozen = 1;
    pthread_mutex_unlock(&rt->lock);
    return ASH_OK;
}

AshStatus ash_iname_lookup(AshRuntime* rt, const char* mangled,
                           AshInameEntry* out) {
    if (!rt || !mangled || !out) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    size_t pos;
    int hit = iname_find(rt, mangled, &pos);
    if (hit) *out = rt->inames[pos];
    pthread_mutex_unlock(&rt->lock);
    return hit ? ASH_OK : ASH_ERR_NAME;
}

size_t ash_iname_count(AshRuntime* rt) {
    if (!rt) return 0;
    pthread_mutex_lock(&rt->lock);
    size_t n = rt->ninames;
    pthread_mutex_unlock(&rt->lock);
    return n;
}

AshStatus ash_iname_at(AshRuntime* rt, size_t i, AshInameEntry* out) {
    if (!rt || !out) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    if (i >= rt->ninames) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_NAME;
    }
    *out = rt->inames[i];
    pthread_mutex_unlock(&rt->lock);
    return ASH_OK;
}

/* One canonical line per entry. The format is pinned by docs/abi.md; a
 * change here is a wire change. */
static int iname_line(const AshInameEntry* e, char* buf, size_t cap) {
    return snprintf(buf, cap, "%s %s %016llx v%u\n", e->mangled,
                    e->kind == ASH_INAME_CONTRACT ? "contract" : "pledge",
                    (unsigned long long)e->shape_hash, e->version);
}

AshStatus ash_iname_dump(AshRuntime* rt, char* buf, size_t cap, size_t* need) {
    if (!rt || !need) return ASH_ERR_TYPE;
    if (!buf) cap = 0;
    pthread_mutex_lock(&rt->lock);
    size_t total = 1; /* the terminating NUL */
    for (size_t i = 0; i < rt->ninames; i++) {
        int n = iname_line(&rt->inames[i], NULL, 0);
        if (n < 0) {
            pthread_mutex_unlock(&rt->lock);
            return ASH_ERR_OOM;
        }
        total += (size_t)n;
    }
    *need = total;
    if (cap < total) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_OOM;
    }
    size_t off = 0;
    for (size_t i = 0; i < rt->ninames; i++) {
        int n = iname_line(&rt->inames[i], buf + off, cap - off);
        if (n < 0 || (size_t)n >= cap - off) {
            pthread_mutex_unlock(&rt->lock);
            return ASH_ERR_OOM;
        }
        off += (size_t)n;
    }
    buf[off] = '\0';
    pthread_mutex_unlock(&rt->lock);
    return ASH_OK;
}

/* ---- contracts ---- */

static const AshContractDesc* find_desc(const AshRuntime* rt, const char* name) {
    for (size_t i = 0; i < rt->ndescs; i++) {
        if (strcmp(rt->descs[i]->name, name) == 0) return rt->descs[i];
    }
    return NULL;
}

static const AshVowDesc* find_vow_desc(const AshContractDesc* desc,
                                       const char* name) {
    for (uint32_t i = 0; i < desc->nvows; i++) {
        if (strcmp(desc->vows[i].name, name) == 0) return &desc->vows[i];
    }
    return NULL;
}

/* Copies one vow value onto the instance, deeply, so the instance never
 * aliases host memory or another instance's heap whatever shape the vow is.
 * An instance handle is refused outright: a vow is a value locked at sign,
 * and a handle is neither copyable nor a value. */
static AshStatus copy_vow_value(AshContract* c, const AshValue* src,
                                AshValue* dst) {
    if (src->ty == ASH_TY_INSTANCE) return ASH_ERR_TYPE;
    return ash_value_deep_copy(c, src, dst);
}

/* Fills the instance's vow storage: defaults first, then the overrides, each
 * override checked by name and by type, and every vow accounted for. */
static AshStatus bind_vows(AshContract* c, const AshVowBinding* vows,
                           size_t nvows) {
    const AshContractDesc* desc = c->desc;
    if (desc->nvows == 0) {
        return (nvows == 0) ? ASH_OK : ASH_ERR_NAME;
    }
    c->vow_vals = (AshValue*)ash_bytes(c, desc->nvows * sizeof(AshValue));
    if (!c->vow_vals) return ASH_ERR_OOM;
    memset(c->vow_vals, 0, desc->nvows * sizeof(AshValue));

    uint8_t bound[ASH_MAX_CONTRACT_TYPES] = {0};
    if (desc->nvows > ASH_MAX_CONTRACT_TYPES) return ASH_ERR_OOM;

    for (size_t i = 0; i < nvows; i++) {
        if (!vows[i].name) return ASH_ERR_NAME;
        const AshVowDesc* vd = find_vow_desc(desc, vows[i].name);
        if (!vd) return ASH_ERR_NAME;
        if (vows[i].value.ty != vd->ty) return ASH_ERR_TYPE;
        size_t slot = (size_t)(vd - desc->vows);
        AshStatus st = copy_vow_value(c, &vows[i].value, &c->vow_vals[slot]);
        if (st != ASH_OK) return st;
        bound[slot] = 1;
    }
    for (uint32_t j = 0; j < desc->nvows; j++) {
        if (bound[j]) continue;
        if (!desc->vows[j].has_default) return ASH_ERR_UNBOUND;
        AshStatus st = copy_vow_value(c, &desc->vows[j].default_value,
                                      &c->vow_vals[j]);
        if (st != ASH_OK) return st;
    }
    return ASH_OK;
}

/* The host binding over a pledge descriptor, or NULL when nothing bound.
 * Called under the runtime lock. */
static AshPledgeFn find_binding(const AshRuntime* rt, const AshPledgeDesc* pd) {
    for (size_t i = 0; i < rt->nbindings; i++) {
        if (rt->bindings[i].pd == pd) return rt->bindings[i].fn;
    }
    return NULL;
}

/* Resolves the dispatch table into plain heap under the runtime lock, one fn
 * per pledge, the host binding beating the compiled body. A pledge with
 * neither refuses the whole sign. The buffer is deliberately not instance
 * memory: sign never touches an instance mutex while the runtime lock is
 * held, the ordering rule cross-contract calls rely on. */
static AshStatus resolve_dispatch(const AshRuntime* rt,
                                  const AshContractDesc* desc,
                                  AshPledgeFn** fns_out) {
    *fns_out = NULL;
    if (desc->npledges == 0) return ASH_OK;
    AshPledgeFn* fns = calloc(desc->npledges, sizeof(AshPledgeFn));
    if (!fns) return ASH_ERR_OOM;
    for (uint32_t i = 0; i < desc->npledges; i++) {
        AshPledgeFn fn = find_binding(rt, &desc->pledges[i]);
        if (!fn) fn = desc->pledges[i].fn;
        if (!fn) {
            free(fns);
            return ASH_ERR_UNBOUND;
        }
        fns[i] = fn;
    }
    *fns_out = fns;
    return ASH_OK;
}

/* Sign runs in three phases so the runtime lock never sits above an instance
 * mutex. Phase one resolves the descriptor and dispatch table under the
 * runtime lock alone; phase two builds the instance with no locks held,
 * where the allocation helpers take only the fresh instance's own mutex,
 * which nothing else can reach yet; phase three publishes it under the
 * runtime lock, where the capacity check belongs because that is where the
 * slot is taken. A pledge body signing through ash_instance_runtime already
 * holds its own instance lock, and this shape keeps that edge one way:
 * instance above runtime, never the reverse. */
AshStatus ash_contract_sign(AshRuntime* rt, const char* contract_name,
                            const AshVowBinding* vows, size_t nvows,
                            uint64_t expected_hash, AshContract** out) {
    if (!rt || !contract_name || !out) return ASH_ERR_TYPE;
    if (nvows > 0 && !vows) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    const AshContractDesc* desc = find_desc(rt, contract_name);
    if (!desc) {
        /* No local descriptor: the name may be a remote origin's, in which case
         * the sign routes to the wire and its whole rulebook runs daemon side.
         * A copy of the row is taken under the lock so a later connect's realloc
         * of the remotes array cannot move it out from under the send. */
        RemoteContract* rc = find_remote(rt, contract_name);
        RemoteContract rcv;
        if (rc) rcv = *rc;
        pthread_mutex_unlock(&rt->lock);
        if (rc) return remote_sign(rt, &rcv, vows, nvows, expected_hash, out);
        return ASH_ERR_NAME;
    }
    if (expected_hash != 0 && expected_hash != desc->shape_hash) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_VERSION;
    }
    AshPledgeFn* fns = NULL;
    AshStatus st = resolve_dispatch(rt, desc, &fns);
    pthread_mutex_unlock(&rt->lock);
    if (st != ASH_OK) return st;

    AshContract* c = calloc(1, sizeof(AshContract));
    if (!c) {
        free(fns);
        return ASH_ERR_OOM;
    }
    if (mutex_init_recursive(&c->mu) != 0) {
        free(c);
        free(fns);
        return ASH_ERR_OOM;
    }
    c->rt = rt;
    c->desc = desc;
    if (desc->npledges > 0) {
        c->fns = (AshPledgeFn*)ash_bytes(c, desc->npledges * sizeof(AshPledgeFn));
        if (!c->fns) st = ASH_ERR_OOM;
        else memcpy(c->fns, fns, desc->npledges * sizeof(AshPledgeFn));
    }
    free(fns);
    if (st == ASH_OK) st = bind_vows(c, vows, nvows);
    if (st == ASH_OK && desc->npledges > 0) {
        c->pledge_state = calloc(desc->npledges, sizeof(uint8_t));
        c->pledge_err = calloc(desc->npledges, sizeof(AshValue));
        if (!c->pledge_state || !c->pledge_err) st = ASH_ERR_OOM;
    }
    if (st == ASH_OK) {
        c->state = ASH_SIGNED;
        c->shape_hash = desc->shape_hash;
        c->signed_at = (int64_t)time(NULL);
        pthread_mutex_lock(&rt->lock);
        if (rt->ninstances == ASH_MAX_INSTANCES) {
            st = ASH_ERR_OOM;
        } else {
            rt->instances[rt->ninstances++] = c;
        }
        pthread_mutex_unlock(&rt->lock);
    }
    if (st != ASH_OK) {
        contract_free_owned(c);
        free(c->pledge_state);
        free(c->pledge_err);
        pthread_mutex_destroy(&c->mu);
        free(c);
        return st;
    }
    *out = c;
    return ASH_OK;
}

AshContractState ash_contract_state(const AshContract* c) {
    if (!c) return ASH_UNSIGNED;
    AshContract* mc = (AshContract*)c;
    if (mc->conn) return remote_state(mc);
    pthread_mutex_lock(&mc->mu);
    AshContractState s = mc->state;
    pthread_mutex_unlock(&mc->mu);
    return s;
}

uint64_t ash_contract_hash(const AshContract* c) {
    return c ? c->shape_hash : 0;
}

int64_t ash_contract_signed_at(const AshContract* c) {
    return c ? c->signed_at : 0;
}

/* The backref a cross-contract sign needs: a compiled thunk reaches the
 * runtime through its own ctx, and a host bound body may do the same. The
 * field is written once under the runtime lock at sign and never moves, so
 * the read needs no lock. */
AshRuntime* ash_instance_runtime(const AshContract* c) {
    return c ? c->rt : NULL;
}

/* Break under the instance lock, which is the whole in-flight story: a thunk
 * mid-run on a worker holds this lock, so the break waits it out; a task
 * still queued finds the state already Broken when its worker gets the lock
 * and never touches the freed heap. Every unwaited future is forfeited to
 * ASH_ERR_STATE before the heap goes, so a late wait delivers a clean error
 * instead of freed memory. A fulfillment racing the break resolves to one of
 * exactly two outcomes: delivered before the break, or ASH_ERR_STATE. */
AshStatus ash_contract_break(AshContract* c) {
    if (!c) return ASH_ERR_TYPE;
    if (c->conn) return remote_break(c);
    pthread_mutex_lock(&c->mu);
    if (c->state == ASH_UNSIGNED) {
        pthread_mutex_unlock(&c->mu);
        return ASH_ERR_STATE;
    }
    for (struct AshFuture* f = c->futures; f; f = f->next) {
        future_forfeit(f);
    }
    /* The stored Err payloads point into the heap about to be freed, so an
     * explicit break zeroes them; the latches themselves survive so the
     * partial surface still reports which pledges landed and which broke. */
    if (c->pledge_err && c->desc->npledges > 0) {
        memset(c->pledge_err, 0, c->desc->npledges * sizeof(AshValue));
    }
    contract_free_owned(c);
    c->state = ASH_BROKEN;
    pthread_mutex_unlock(&c->mu);
    return ASH_OK;
}

/* ---- the partial result ---- */

/* A named subcontract item's state: fulfilled when every pledge inside it
 * latched Ok, broken when every pledge inside it latched Err, pending
 * otherwise. Called under the instance lock. */
static AshItemState sub_item_state(const AshContract* c, uint32_t s) {
    if (sub_all(c, s, PLEDGE_FULFILLED)) return ASH_ITEM_FULFILLED;
    if (sub_all(c, s, PLEDGE_BROKEN)) return ASH_ITEM_BROKEN;
    return ASH_ITEM_PENDING;
}

static AshItemState loose_item_state(const AshContract* c, uint32_t i) {
    if (!c->pledge_state) return ASH_ITEM_PENDING;
    if (c->pledge_state[i] == PLEDGE_FULFILLED) return ASH_ITEM_FULFILLED;
    if (c->pledge_state[i] == PLEDGE_BROKEN) return ASH_ITEM_BROKEN;
    return ASH_ITEM_PENDING;
}

/* The one walk both partial item calls share: items in descriptor order,
 * named subcontracts first then loose pledges, counting the ones whose state
 * reads k, and capturing the want-th match's name when the caller asked for
 * one. Anonymous subcontracts group their pledges for the policy but have no
 * name a PartialResult could report, so they are not items. Called under the
 * instance lock. */
static size_t partial_scan(const AshContract* c, AshItemState k, size_t want,
                           const char** name_out) {
    const AshContractDesc* d = c->desc;
    size_t n = 0;
    for (uint32_t s = 0; s < d->nsubs; s++) {
        if (!d->subs || !d->subs[s]) continue;
        if (sub_item_state(c, s) != k) continue;
        if (name_out && n == want) *name_out = d->subs[s];
        n++;
    }
    for (uint32_t i = 0; i < d->npledges; i++) {
        if (!pledge_is_loose(d, i)) continue;
        if (loose_item_state(c, i) != k) continue;
        if (name_out && n == want) *name_out = d->pledges[i].name;
        n++;
    }
    return n;
}

size_t ash_partial_count(AshContract* c, AshItemState k) {
    if (!c) return 0;
    if (c->conn) return remote_partial_count(c, k);
    pthread_mutex_lock(&c->mu);
    size_t n = partial_scan(c, k, 0, NULL);
    pthread_mutex_unlock(&c->mu);
    return n;
}

const char* ash_partial_name(AshContract* c, AshItemState k, size_t i) {
    if (!c) return NULL;
    if (c->conn) return remote_partial_name(c, k, i);
    const char* name = NULL;
    pthread_mutex_lock(&c->mu);
    partial_scan(c, k, i, &name);
    pthread_mutex_unlock(&c->mu);
    return name;
}

size_t ash_partial_nerrors(AshContract* c) {
    if (!c) return 0;
    if (c->conn) return remote_partial_nerrors(c);
    size_t n = 0;
    pthread_mutex_lock(&c->mu);
    if (c->pledge_state) {
        for (uint32_t i = 0; i < c->desc->npledges; i++) {
            if (c->pledge_state[i] == PLEDGE_BROKEN) n++;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return n;
}

AshStatus ash_partial_error(AshContract* c, size_t i,
                            const char** pledge_name, const AshValue** err) {
    if (!c) return ASH_ERR_TYPE;
    if (c->conn) return remote_partial_error(c, i, pledge_name, err);
    AshStatus st = ASH_ERR_NAME;
    pthread_mutex_lock(&c->mu);
    if (c->pledge_state) {
        size_t n = 0;
        for (uint32_t p = 0; p < c->desc->npledges; p++) {
            if (c->pledge_state[p] != PLEDGE_BROKEN) continue;
            if (n == i) {
                if (pledge_name) *pledge_name = c->desc->pledges[p].name;
                if (err) *err = &c->pledge_err[p];
                st = ASH_OK;
                break;
            }
            n++;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

/* ---- vows ---- */

/* Safe from a thunk, which already holds the instance lock, and from a host
 * thread, which takes it here. The returned pointer is instance owned; a
 * host that holds it across a break holds a dangling pointer, the same
 * ownership rule every instance pointer follows. */
const AshValue* ash_vow_ref(AshContract* c, const char* name) {
    if (!c || !name) return NULL;
    if (c->conn) {
        /* A proxy's vows are the effective set the SIGNED reply carried, stored
         * by name at sign; the read is local because vows never move after. */
        const AshValue* v = NULL;
        pthread_mutex_lock(&c->mu);
        for (uint32_t i = 0; i < c->remote_nvows; i++) {
            if (strcmp(c->rvow_names[i], name) == 0) {
                v = &c->rvow_vals[i];
                break;
            }
        }
        pthread_mutex_unlock(&c->mu);
        return v;
    }
    pthread_mutex_lock(&c->mu);
    const AshValue* v = NULL;
    if (c->vow_vals) {
        const AshVowDesc* vd = find_vow_desc(c->desc, name);
        if (vd) v = &c->vow_vals[vd - c->desc->vows];
    }
    pthread_mutex_unlock(&c->mu);
    return v;
}

/* ---- pledges ---- */

static const AshPledgeDesc* find_pledge(const AshContractDesc* desc,
                                        const char* name) {
    for (uint32_t i = 0; i < desc->npledges; i++) {
        if (strcmp(desc->pledges[i].name, name) == 0) return &desc->pledges[i];
    }
    return NULL;
}

/* Resolves "Contract.pledge" or a mangled symbol to its descriptor entry.
 * Called under the runtime lock. */
static const AshPledgeDesc* resolve_pledge_name(const AshRuntime* rt,
                                                const char* name) {
    const char* dot = strchr(name, '.');
    if (dot && dot != name && dot[1] != '\0') {
        size_t clen = (size_t)(dot - name);
        for (size_t i = 0; i < rt->ndescs; i++) {
            const AshContractDesc* d = rt->descs[i];
            if (strncmp(d->name, name, clen) != 0 || d->name[clen] != '\0')
                continue;
            return find_pledge(d, dot + 1);
        }
        return NULL;
    }
    for (size_t i = 0; i < rt->ndescs; i++) {
        const AshContractDesc* d = rt->descs[i];
        for (uint32_t j = 0; j < d->npledges; j++) {
            if (d->pledges[j].mangled &&
                strcmp(d->pledges[j].mangled, name) == 0)
                return &d->pledges[j];
        }
    }
    return NULL;
}

AshStatus ash_pledge_bind(AshRuntime* rt, const char* pledge_name,
                          AshPledgeFn fn) {
    if (!rt || !pledge_name || !fn) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    if (rt->frozen) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_STATE;
    }
    const AshPledgeDesc* pd = resolve_pledge_name(rt, pledge_name);
    if (!pd) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_NAME;
    }
    for (size_t i = 0; i < rt->nbindings; i++) {
        if (rt->bindings[i].pd == pd) {
            rt->bindings[i].fn = fn;
            pthread_mutex_unlock(&rt->lock);
            return ASH_OK;
        }
    }
    if (rt->nbindings == ASH_MAX_BINDINGS) {
        pthread_mutex_unlock(&rt->lock);
        return ASH_ERR_OOM;
    }
    rt->bindings[rt->nbindings].pd = pd;
    rt->bindings[rt->nbindings].fn = fn;
    rt->nbindings++;
    pthread_mutex_unlock(&rt->lock);
    return ASH_OK;
}

/* Copies the host value a ref points at into an instance owned frame slot.
 * v1 passes scalars and strings by reference; the composite types wait for a
 * host repr worth standardizing. */
static AshStatus ref_copy_in(AshContract* c, const AshRef* r, AshValue* slot) {
    if (!r->host_ptr) return ASH_ERR_TYPE;
    memset(slot, 0, sizeof(*slot));
    slot->ty = r->ty;
    switch ((AshTypeTag)r->ty) {
    case ASH_TY_INT:   slot->as.i  = *(const int64_t*)r->host_ptr;  return ASH_OK;
    case ASH_TY_UINT:  slot->as.u  = *(const uint64_t*)r->host_ptr; return ASH_OK;
    case ASH_TY_FLOAT: slot->as.f  = *(const double*)r->host_ptr;   return ASH_OK;
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:  slot->as.b  = *(const uint8_t*)r->host_ptr;  return ASH_OK;
    case ASH_TY_CHAR:  slot->as.ch = *(const uint32_t*)r->host_ptr; return ASH_OK;
    case ASH_TY_STRING: {
        const AshString* hs = (const AshString*)r->host_ptr;
        *slot = ash_string_copy(c, hs->ptr, hs->len);
        if (hs->len && !slot->as.s.ptr) return ASH_ERR_OOM;
        return ASH_OK;
    }
    default:
        return ASH_ERR_TYPE;
    }
}

/* Writes one slot's final value back to host memory, the default protocol
 * when the ref carries no callback: scalars in place, strings as a whole
 * AshString struct whose bytes stay instance owned. Runs on the thread that
 * collects the outcome, while the host is blocked in the ash call. */
static AshStatus ref_write_back(const AshRef* r, const AshValue* slot) {
    if (slot->ty != r->ty) return ASH_ERR_TYPE;
    if (r->write_back) {
        r->write_back(r->host_ptr, slot, r->user);
        return ASH_OK;
    }
    switch ((AshTypeTag)r->ty) {
    case ASH_TY_INT:   *(int64_t*)r->host_ptr   = slot->as.i;  return ASH_OK;
    case ASH_TY_UINT:  *(uint64_t*)r->host_ptr  = slot->as.u;  return ASH_OK;
    case ASH_TY_FLOAT: *(double*)r->host_ptr    = slot->as.f;  return ASH_OK;
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:  *(uint8_t*)r->host_ptr   = slot->as.b;  return ASH_OK;
    case ASH_TY_CHAR:  *(uint32_t*)r->host_ptr  = slot->as.ch; return ASH_OK;
    case ASH_TY_STRING: *(AshString*)r->host_ptr = slot->as.s; return ASH_OK;
    default:
        return ASH_ERR_TYPE;
    }
}

/* Applies every write back of a delivered fulfillment. Slot types are
 * checked first so a pledge that broke the protocol writes nothing at all. */
static AshStatus write_back_refs(const AshRef* refs, const AshValue* slots,
                                 size_t nrefs) {
    for (size_t i = 0; i < nrefs; i++) {
        if (slots[i].ty != refs[i].ty) return ASH_ERR_TYPE;
    }
    for (size_t i = 0; i < nrefs; i++) {
        AshStatus st = ref_write_back(&refs[i], &slots[i]);
        if (st != ASH_OK) return st;
    }
    return ASH_OK;
}

/* Builds the frame a thunk sees: one instance owned slot per declared
 * parameter, the value arguments deep copied first, the refs copied in
 * behind them. Everything happens on the caller's thread, so no host memory
 * is ever read after the fulfill call returns. */
static AshStatus prepare_frame(AshContract* c, const AshValue* args,
                               size_t nargs, const AshRef* refs, size_t nrefs,
                               AshValue** frame_out) {
    size_t total = nargs + nrefs;
    *frame_out = NULL;
    if (total == 0) return ASH_OK;
    AshValue* frame = (AshValue*)ash_bytes(c, total * sizeof(AshValue));
    if (!frame) return ASH_ERR_OOM;
    for (size_t i = 0; i < nargs; i++) {
        AshStatus st = ash_value_deep_copy(c, &args[i], &frame[i]);
        if (st != ASH_OK) return st;
    }
    for (size_t i = 0; i < nrefs; i++) {
        AshStatus st = ref_copy_in(c, &refs[i], &frame[nargs + i]);
        if (st != ASH_OK) return st;
    }
    *frame_out = frame;
    return ASH_OK;
}

/* ---- the requirements evaluator ---- */

/* Whether a pledge sits outside every subcontract. A sub index outside the
 * subs table reads as loose, which is what a zero-filled handwritten
 * descriptor gets. */
static int pledge_is_loose(const AshContractDesc* d, uint32_t i) {
    int32_t s = d->pledges[i].sub;
    return s < 0 || (uint32_t)s >= d->nsubs;
}

/* Whether every pledge of subcontract s has latched want. An empty
 * subcontract holds no latch to test and reads false for both fulfilled and
 * broken. Called under the instance lock. */
static int sub_all(const AshContract* c, uint32_t s, uint8_t want) {
    const AshContractDesc* d = c->desc;
    int seen = 0;
    if (!c->pledge_state) return 0;
    for (uint32_t i = 0; i < d->npledges; i++) {
        if (d->pledges[i].sub != (int32_t)s || (uint32_t)d->pledges[i].sub >= d->nsubs)
            continue;
        seen = 1;
        if (c->pledge_state[i] != want) return 0;
    }
    return seen;
}

/* One atom's truth. A sub atom tests every pledge of the subcontract, a
 * pledge atom tests that pledge's own latch; kind picks fulfilled or broken,
 * and a bare grammar atom is always the fulfilled test, so "false" covers
 * pending and broken alike, the grammar's "!x means not fulfilled". */
static int atom_true(const AshContract* c, const AshReqAtom* a) {
    uint8_t want = (a->kind == ASH_ATOM_BROKEN) ? PLEDGE_BROKEN
                                                : PLEDGE_FULFILLED;
    if (a->sub >= 0) return sub_all(c, (uint32_t)a->sub, want);
    if (a->pledge >= 0 && (uint32_t)a->pledge < c->desc->npledges &&
        c->pledge_state) {
        return c->pledge_state[a->pledge] == want;
    }
    return 0;
}

/* Evaluates one postfix policy line. An empty line is a line the source did
 * not write and never fires. The stack bound is generous, the source caps a
 * contract at 16 distinct atoms; a malformed program that would overflow or
 * underflow reads false rather than anything worse. */
#define REQ_STACK_MAX 128

static int eval_line(const AshContract* c, const AshReqOp* ops, uint32_t n) {
    uint8_t st[REQ_STACK_MAX];
    int sp = 0;
    if (n == 0 || !ops) return 0;
    for (uint32_t i = 0; i < n; i++) {
        switch (ops[i].op) {
        case ASH_REQ_ATOM:
            if (sp >= REQ_STACK_MAX) return 0;
            if (ops[i].atom >= c->desc->natoms) return 0;
            st[sp++] = (uint8_t)atom_true(c, &c->desc->atoms[ops[i].atom]);
            break;
        case ASH_REQ_NOT:
            if (sp < 1) return 0;
            st[sp - 1] = !st[sp - 1];
            break;
        case ASH_REQ_AND:
            if (sp < 2) return 0;
            st[sp - 2] = st[sp - 2] && st[sp - 1];
            sp--;
            break;
        case ASH_REQ_OR:
            if (sp < 2) return 0;
            st[sp - 2] = st[sp - 2] || st[sp - 1];
            sp--;
            break;
        default:
            return 0;
        }
    }
    return sp == 1 ? st[0] : 0;
}

/* The structural default policy for a descriptor that carries no
 * requirements data at all, the handwritten descriptor case: fulfill when
 * every subcontract and every loose pledge is fulfilled, partial when at
 * least one subcontract is, break when everything is broken. ashc compiled
 * modules never land here, the compiler serializes the source block or
 * synthesizes these same defaults as trees. Called under the instance
 * lock. */
static int default_line(const AshContract* c, uint8_t want) {
    const AshContractDesc* d = c->desc;
    int seen = 0;
    if (!c->pledge_state) return 0;
    for (uint32_t s = 0; s < d->nsubs; s++) {
        seen = 1;
        if (!sub_all(c, s, want)) return 0;
    }
    for (uint32_t i = 0; i < d->npledges; i++) {
        if (!pledge_is_loose(d, i)) continue;
        seen = 1;
        if (c->pledge_state[i] != want) return 0;
    }
    return seen;
}

static int default_partial(const AshContract* c) {
    for (uint32_t s = 0; s < c->desc->nsubs; s++) {
        if (sub_all(c, s, PLEDGE_FULFILLED)) return 1;
    }
    return 0;
}

/* Recomputes the contract state from the latches, in the grammar's priority
 * order: break, then fulfill, then partial, the first line that matches
 * setting the state, and Signed when none does. Broken is terminal, every
 * later fulfillment is refused with ASH_ERR_STATE before a thunk runs.
 *
 * The break line is armed by the first broken pledge. A break line written
 * over negated atoms, the README's !Validation && !Processing shape, is true
 * the moment the contract signs, since nothing is fulfilled yet; firing it
 * on the first Ok would tear down a contract nothing broke. A contract
 * cannot break before something broke, which is also what the synthesized
 * default, everything broken, already implies, so the arming rule makes the
 * written and the defaulted policy read the same way.
 *
 * An automatic Broken keeps the owned heap alive, the Err payloads the
 * partial surface reports live there; only an explicit break() reclaims.
 * Called under the instance lock, after every fulfillment outcome. */
static void eval_policy(AshContract* c) {
    const AshContractDesc* d = c->desc;
    if (c->state == ASH_BROKEN) return;
    int armed = 0;
    if (c->pledge_state) {
        for (uint32_t i = 0; i < d->npledges; i++) {
            if (c->pledge_state[i] == PLEDGE_BROKEN) {
                armed = 1;
                break;
            }
        }
    }
    int has_trees = d->has_reqs || d->natoms > 0 || d->nfulfill > 0 ||
                    d->npartial > 0 || d->nbreak > 0;
    int brk, ful, par;
    if (has_trees) {
        brk = armed && eval_line(c, d->req_break, d->nbreak);
        ful = eval_line(c, d->req_fulfill, d->nfulfill);
        par = eval_line(c, d->req_partial, d->npartial);
    } else {
        brk = armed && default_line(c, PLEDGE_BROKEN);
        ful = default_line(c, PLEDGE_FULFILLED);
        par = default_partial(c);
    }
    if (brk) c->state = ASH_BROKEN;
    else if (ful) c->state = ASH_FULFILLED;
    else if (par) c->state = ASH_PARTIAL;
    else c->state = ASH_SIGNED;
}

/* The per-pledge latch, the grammar's law: fulfilled on the first Ok, broken
 * on an Err that lands before any Ok, and never a change after either. The
 * first Err's payload is kept beside the latch; the box it may point into is
 * instance owned, so the struct copy stays valid exactly as long as the
 * instance heap does. Called under the instance lock. */
static void latch_pledge(AshContract* c, uint32_t pidx, const AshValue* out) {
    if (!c->pledge_state || pidx >= c->desc->npledges) return;
    if (c->pledge_state[pidx] != PLEDGE_PENDING) return;
    if (out->ty == ASH_TY_RESULT && out->tag == 1) {
        c->pledge_state[pidx] = PLEDGE_BROKEN;
        if (out->as.box) c->pledge_err[pidx] = *(const AshValue*)out->as.box;
    } else {
        c->pledge_state[pidx] = PLEDGE_FULFILLED;
    }
}

/* One fulfillment on a pool worker. The instance lock covers the state
 * check, the thunk run, and the latch, so fulfillments against one instance
 * serialize while distinct instances run truly in parallel, and a break can
 * never free the heap under a running thunk. The completion happens inside
 * the same critical section: the lock handoff to the waiting thread is what
 * makes the thunk's slot writes visible to the write back. */
static void run_task(struct AshFuture* f) {
    AshContract* c = f->c;
    pthread_mutex_lock(&c->mu);
    if (c->state != ASH_SIGNED && c->state != ASH_PARTIAL &&
        c->state != ASH_FULFILLED) {
        future_finish(f, ASH_ERR_STATE, NULL);
    } else {
        AshValue out;
        memset(&out, 0, sizeof(out));
        AshStatus st = f->fn((void*)c, f->frame, f->frame_nargs, &out);
        if (st == ASH_OK) {
            latch_pledge(c, f->pidx, &out);
            eval_policy(c);
        }
        future_finish(f, st, &out);
    }
    pthread_mutex_unlock(&c->mu);
}

/* Starts a fulfillment: validate and copy in on the caller's thread, under
 * the instance lock, then hand the future to the pool. Every failure short
 * of allocating the future itself is delivered through the wait, the M4
 * contract that survives concurrency unchanged. */
AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs,
                              const AshRef* refs, size_t nrefs) {
    if (!c || !pledge_name) return NULL;
    if (c->conn) {
        return remote_fulfill(c, pledge_name, args, nargs, refs, nrefs);
    }
    struct AshFuture* f = future_new(c);
    if (!f) return NULL;
    AshStatus st = ASH_OK;
    int ready = 0;
    pthread_mutex_lock(&c->mu);
    do {
        if ((nargs > 0 && !args) || (nrefs > 0 && !refs)) {
            st = ASH_ERR_TYPE;
            break;
        }
        if (c->state != ASH_SIGNED && c->state != ASH_PARTIAL &&
            c->state != ASH_FULFILLED) {
            st = ASH_ERR_STATE;
            break;
        }
        const AshPledgeDesc* p = find_pledge(c->desc, pledge_name);
        if (!p) {
            st = ASH_ERR_NAME;
            break;
        }
        if (nargs + nrefs != p->nargs) {
            st = ASH_ERR_TYPE;
            break;
        }
        if (nrefs > 0) {
            f->refs = (AshRef*)ash_bytes(c, nrefs * sizeof(AshRef));
            if (!f->refs) {
                st = ASH_ERR_OOM;
                break;
            }
            memcpy(f->refs, refs, nrefs * sizeof(AshRef));
            f->nrefs = nrefs;
        }
        AshValue* frame = NULL;
        st = prepare_frame(c, args, nargs, refs, nrefs, &frame);
        if (st != ASH_OK) break;
        f->fn = c->fns[p - c->desc->pledges];
        f->pidx = (uint32_t)(p - c->desc->pledges);
        f->frame = frame;
        f->frame_nargs = p->nargs;
        f->ref_slots = (nrefs > 0 && frame) ? frame + nargs : NULL;
        ready = 1;
    } while (0);
    pthread_mutex_unlock(&c->mu);
    if (!ready) {
        future_finish(f, st, NULL);
        return f;
    }
    /* The pool takes its own hold before the future is visible to a worker;
     * nothing else can see the count yet, so the plain write is safe and the
     * queue lock publishes it. */
    f->refcnt = 2;
    pool_enqueue(c->rt, f);
    return f;
}

/* Blocks until the outcome exists, delivers it exactly once, and performs
 * the ref write back on this thread while the host is inside the call. The
 * write back runs under the future's mutex, which is also what a break must
 * take to forfeit this future, so the instance heap the slots live in cannot
 * be freed out from under a write back in progress. */
AshStatus ash_future_wait(AshFuture* f, AshValue* out) {
    if (!f || !out) return ASH_ERR_TYPE;
    pthread_mutex_lock(&f->mu);
    while (!f->done) {
        pthread_cond_wait(&f->cv, &f->mu);
    }
    if (f->waited) {
        pthread_mutex_unlock(&f->mu);
        return ASH_ERR_STATE;
    }
    f->waited = 1;
    AshStatus st = f->status;
    if (st == ASH_OK && f->nrefs > 0 && f->ref_slots) {
        AshStatus wb = write_back_refs(f->refs, f->ref_slots, f->nrefs);
        if (wb != ASH_OK) {
            pthread_mutex_unlock(&f->mu);
            return wb;
        }
    }
    *out = f->value;
    pthread_mutex_unlock(&f->mu);
    return st;
}

/* The reentrant path: a synchronous fulfillment started inside a thunk runs
 * whole on the current worker thread, the same validate, copy in, run,
 * latch, and write back walk the queued path performs, under the callee
 * instance's lock. No future exists because no one could wait on it; the
 * caller is this thread. The caller's own instance lock is already held
 * above this frame, which is exactly the instance-to-instance edge the
 * header comment audits. */
static AshStatus fulfill_inline(AshContract* c, const char* pledge_name,
                                const AshValue* args, size_t nargs,
                                const AshRef* refs, size_t nrefs,
                                AshValue* out) {
    AshStatus st = ASH_OK;
    pthread_mutex_lock(&c->mu);
    do {
        if ((nargs > 0 && !args) || (nrefs > 0 && !refs)) {
            st = ASH_ERR_TYPE;
            break;
        }
        if (c->state != ASH_SIGNED && c->state != ASH_PARTIAL &&
            c->state != ASH_FULFILLED) {
            st = ASH_ERR_STATE;
            break;
        }
        const AshPledgeDesc* p = find_pledge(c->desc, pledge_name);
        if (!p) {
            st = ASH_ERR_NAME;
            break;
        }
        if (nargs + nrefs != p->nargs) {
            st = ASH_ERR_TYPE;
            break;
        }
        AshValue* frame = NULL;
        st = prepare_frame(c, args, nargs, refs, nrefs, &frame);
        if (st != ASH_OK) break;
        AshValue res;
        memset(&res, 0, sizeof(res));
        AshPledgeFn fn = c->fns[p - c->desc->pledges];
        st = fn((void*)c, frame, p->nargs, &res);
        if (st != ASH_OK) break;
        latch_pledge(c, (uint32_t)(p - c->desc->pledges), &res);
        eval_policy(c);
        if (nrefs > 0 && frame) {
            AshStatus wb = write_back_refs(refs, frame + nargs, nrefs);
            if (wb != ASH_OK) {
                st = wb;
                break;
            }
        }
        *out = res;
    } while (0);
    pthread_mutex_unlock(&c->mu);
    return st;
}

/* The synchronous form is exactly fulfill plus wait on the same path, so
 * the two can never drift; the only extra step is releasing the delivered
 * receipt immediately, since no one else can be holding it. Called from a
 * pool worker, which means from inside a pledge body, it runs inline on
 * this thread instead: queueing would park the worker on work only the
 * pool can run, and a pool of blocked workers deadlocks. */
AshStatus ash_pledge_fulfill_sync(AshContract* c, const char* pledge_name,
                                  const AshValue* args, size_t nargs,
                                  const AshRef* refs, size_t nrefs,
                                  AshValue* out) {
    if (!c || !pledge_name || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (t_pool_worker) {
        return fulfill_inline(c, pledge_name, args, nargs, refs, nrefs, out);
    }
    AshFuture* f = ash_pledge_fulfill(c, pledge_name, args, nargs, refs, nrefs);
    if (!f) return ASH_ERR_OOM;
    AshStatus st = ash_future_wait(f, out);
    future_release(f);
    return st;
}

/* ---- allocation helpers ---- */

/* Every helper is safe from a thunk, where the worker already holds the
 * recursive instance lock, and from a host thread outside any fulfillment,
 * where the lock is taken cold right here. Only the block list is guarded;
 * a single AshValue is not a shared object, and two threads mutating the
 * same list or string value remain a host bug. */
uint8_t* ash_bytes(AshContract* c, uint64_t n) {
    if (!c) return NULL;
    AshBlock* b = malloc(sizeof(AshBlock) + n);
    if (!b) return NULL;
    pthread_mutex_lock(&c->mu);
    b->next = c->owned;
    c->owned = b;
    pthread_mutex_unlock(&c->mu);
    return (uint8_t*)(b + 1);
}

AshValue* ash_box(AshContract* c) {
    AshValue* v = (AshValue*)ash_bytes(c, sizeof(AshValue));
    if (v) memset(v, 0, sizeof(*v));
    return v;
}

AshValue ash_string_copy(AshContract* c, const uint8_t* utf8, uint64_t len) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    uint8_t* dst = ash_bytes(c, len);
    if (dst && len) memcpy(dst, utf8, len);
    v.as.s.ptr = dst;
    v.as.s.len = dst ? len : 0;
    return v;
}

AshStatus ash_string_concat(AshContract* c, const AshValue* a,
                            const AshValue* b, AshValue* out) {
    if (!c || !a || !b || !out) return ASH_ERR_TYPE;
    if (a->ty != ASH_TY_STRING || b->ty != ASH_TY_STRING) return ASH_ERR_TYPE;
    uint64_t n = a->as.s.len + b->as.s.len;
    uint8_t* buf = ash_bytes(c, n);
    if (!buf) return ASH_ERR_OOM;
    if (a->as.s.len) memcpy(buf, a->as.s.ptr, a->as.s.len);
    if (b->as.s.len) memcpy(buf + a->as.s.len, b->as.s.ptr, b->as.s.len);
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_STRING;
    out->as.s.ptr = buf;
    out->as.s.len = n;
    return ASH_OK;
}

int ash_string_eq(const AshValue* a, const AshValue* b) {
    if (!a || !b) return 0;
    if (a->ty != ASH_TY_STRING || b->ty != ASH_TY_STRING) return 0;
    if (a->as.s.len != b->as.s.len) return 0;
    if (a->as.s.len == 0) return 1;
    return memcmp(a->as.s.ptr, b->as.s.ptr, a->as.s.len) == 0;
}

/* ---- deep values ---- */

AshStatus ash_list_new(AshContract* c, uint32_t elem_ty, uint64_t cap,
                       AshValue* out) {
    if (!c || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_LIST;
    out->as.list.elem_ty = elem_ty;
    if (cap == 0) return ASH_OK;
    AshValue* data = (AshValue*)ash_bytes(c, cap * sizeof(AshValue));
    if (!data) return ASH_ERR_OOM;
    memset(data, 0, cap * sizeof(AshValue));
    out->as.list.data = data;
    out->as.list.cap = cap;
    return ASH_OK;
}

AshStatus ash_list_push(AshContract* c, AshValue* list, const AshValue* elem) {
    if (!c || !list || !elem) return ASH_ERR_TYPE;
    if (list->ty != ASH_TY_LIST) return ASH_ERR_TYPE;
    if (elem->ty != list->as.list.elem_ty) return ASH_ERR_TYPE;
    if (list->as.list.len == list->as.list.cap) {
        uint64_t cap = list->as.list.cap ? list->as.list.cap * 2 : 4;
        AshValue* data = (AshValue*)ash_bytes(c, cap * sizeof(AshValue));
        if (!data) return ASH_ERR_OOM;
        if (list->as.list.len) {
            memcpy(data, list->as.list.data,
                   list->as.list.len * sizeof(AshValue));
        }
        list->as.list.data = data;
        list->as.list.cap = cap;
    }
    ((AshValue*)list->as.list.data)[list->as.list.len++] = *elem;
    return ASH_OK;
}

const AshValue* ash_list_get(const AshValue* v, uint64_t idx) {
    if (!v) return NULL;
    if (v->ty != ASH_TY_LIST && v->ty != ASH_TY_TUPLE) return NULL;
    if (idx >= v->as.list.len) return NULL;
    return (const AshValue*)v->as.list.data + idx;
}

/* Overwrites one live slot in place. No allocation happens here, so no
 * contract rides the call; the element must already be instance owned, the
 * same rule ash_list_push states. Out of range is ASH_ERR_TYPE, the status a
 * compiled index assignment returns from its pledge on a bad index. */
AshStatus ash_list_set(AshValue* list, uint64_t idx, const AshValue* elem) {
    if (!list || !elem) return ASH_ERR_TYPE;
    if (list->ty != ASH_TY_LIST) return ASH_ERR_TYPE;
    if (elem->ty != list->as.list.elem_ty) return ASH_ERR_TYPE;
    if (idx >= list->as.list.len) return ASH_ERR_TYPE;
    ((AshValue*)list->as.list.data)[idx] = *elem;
    return ASH_OK;
}

/* ---- maps ---- */

/* A map rides the list arm: the data pointer holds interleaved key, value
 * pairs, entries[2i] the key and entries[2i+1] its value, len counts slots so
 * it is always twice the pair count, and elem_ty is the key's tag alone; the
 * value type is the checker's knowledge, not the runtime's. Pairs stay in
 * insertion order, which is the order any serialization sees. Lookup and
 * insert are a linear scan over the keys through ash_value_eq, O(n) in the
 * pair count, the v1 tradeoff that keeps the repr one arm deep. */

AshValue ash_map_new(AshContract* c, uint32_t key_ty) {
    (void)c; /* an empty map allocates nothing; the ctx rides for symmetry */
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_MAP;
    v.as.list.elem_ty = key_ty;
    return v;
}

/* The slot index of k's value inside m, or -1 for a miss. Assumes m is a map
 * and k matches its key tag; the public entry points check first. */
static int64_t map_find(const AshValue* m, const AshValue* k) {
    const AshValue* e = (const AshValue*)m->as.list.data;
    for (uint64_t i = 0; i + 1 < m->as.list.len; i += 2) {
        if (ash_value_eq(&e[i], k)) return (int64_t)(i + 1);
    }
    return -1;
}

AshStatus ash_map_set(AshContract* c, AshValue* m, const AshValue* k,
                      const AshValue* v) {
    if (!c || !m || !k || !v) return ASH_ERR_TYPE;
    if (m->ty != ASH_TY_MAP) return ASH_ERR_TYPE;
    if (k->ty != m->as.list.elem_ty) return ASH_ERR_TYPE;
    /* Both halves are deep copied before anything is committed, so an OOM
     * mid-copy leaves the map exactly as it was. */
    AshValue vc;
    AshStatus st = ash_value_deep_copy(c, v, &vc);
    if (st != ASH_OK) return st;
    int64_t hit = map_find(m, k);
    if (hit >= 0) {
        ((AshValue*)m->as.list.data)[hit] = vc;
        return ASH_OK;
    }
    AshValue kc;
    st = ash_value_deep_copy(c, k, &kc);
    if (st != ASH_OK) return st;
    if (m->as.list.len + 2 > m->as.list.cap) {
        uint64_t cap = m->as.list.cap ? m->as.list.cap * 2 : 8;
        AshValue* data = (AshValue*)ash_bytes(c, cap * sizeof(AshValue));
        if (!data) return ASH_ERR_OOM;
        if (m->as.list.len) {
            memcpy(data, m->as.list.data, m->as.list.len * sizeof(AshValue));
        }
        m->as.list.data = data;
        m->as.list.cap = cap;
    }
    AshValue* e = (AshValue*)m->as.list.data;
    e[m->as.list.len] = kc;
    e[m->as.list.len + 1] = vc;
    m->as.list.len += 2;
    return ASH_OK;
}

int ash_map_get(const AshValue* m, const AshValue* k, const AshValue** out) {
    if (!m || !k || !out) return 0;
    if (m->ty != ASH_TY_MAP) return 0;
    if (k->ty != m->as.list.elem_ty) return 0;
    int64_t hit = map_find(m, k);
    if (hit < 0) return 0;
    *out = (const AshValue*)m->as.list.data + hit;
    return 1;
}

/* Structural equality, the recursion mirroring ash_value_deep_copy: what the
 * copy can reach, the compare can test. A tag mismatch reads unequal. A map
 * compares pair by pair in insertion order, keys and values both, the same
 * order semantics serialization promises, so the answer is deterministic. */
int ash_value_eq(const AshValue* a, const AshValue* b) {
    if (!a || !b) return 0;
    if (a->ty != b->ty) return 0;
    switch ((AshTypeTag)a->ty) {
    case ASH_TY_UNIT:  return 1;
    case ASH_TY_INT:   return a->as.i == b->as.i;
    case ASH_TY_UINT:  return a->as.u == b->as.u;
    case ASH_TY_FLOAT: return a->as.f == b->as.f;
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:  return a->as.b == b->as.b;
    case ASH_TY_CHAR:  return a->as.ch == b->as.ch;
    case ASH_TY_STRING:
        return ash_string_eq(a, b);
    case ASH_TY_LIST:
    case ASH_TY_MAP:
    case ASH_TY_TUPLE:
    case ASH_TY_RECORD:
    case ASH_TY_SUM: {
        if (a->tag != b->tag) return 0;
        if (a->as.list.len != b->as.list.len) return 0;
        const AshValue* xa = (const AshValue*)a->as.list.data;
        const AshValue* xb = (const AshValue*)b->as.list.data;
        for (uint64_t i = 0; i < a->as.list.len; i++) {
            if (!ash_value_eq(&xa[i], &xb[i])) return 0;
        }
        return 1;
    }
    case ASH_TY_OPTION:
    case ASH_TY_RESULT: {
        if (a->tag != b->tag) return 0;
        if (!a->as.box && !b->as.box) return 1;
        if (!a->as.box || !b->as.box) return 0;
        return ash_value_eq((const AshValue*)a->as.box,
                            (const AshValue*)b->as.box);
    }
    case ASH_TY_INSTANCE:
        /* An instance value is a reference handle; equality is identity. */
        return a->as.box == b->as.box;
    case ASH_TY_PLEDGE_REF:
    default:
        return 0;
    }
}

AshStatus ash_tuple_new(AshContract* c, uint64_t count, AshValue* out) {
    if (!c || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_TUPLE;
    if (count == 0) return ASH_OK;
    AshValue* data = (AshValue*)ash_bytes(c, count * sizeof(AshValue));
    if (!data) return ASH_ERR_OOM;
    memset(data, 0, count * sizeof(AshValue));
    out->as.list.data = data;
    out->as.list.len = count;
    out->as.list.cap = count;
    return ASH_OK;
}

/* The recursive workhorse behind copy-in. Scalars are the struct copy, a
 * string copies its bytes, list, map, tuple, record, and sum payloads copy
 * element by element on the shared list arm, a map's interleaved keys and
 * values riding along like any other elements, and Option and Result rebox
 * their payload. */
AshStatus ash_value_deep_copy(AshContract* c, const AshValue* src,
                              AshValue* dst) {
    if (!c || !src || !dst) return ASH_ERR_TYPE;
    switch ((AshTypeTag)src->ty) {
    case ASH_TY_UNIT:
    case ASH_TY_INT:
    case ASH_TY_UINT:
    case ASH_TY_FLOAT:
    case ASH_TY_BOOL:
    case ASH_TY_BYTE:
    case ASH_TY_CHAR:
    case ASH_TY_PLEDGE_REF:
    case ASH_TY_INSTANCE:
        /* An instance value is a reference handle, the one deliberate value
         * semantics exception: the copy shares the instance. Internal only;
         * the ABI never carries this tag across the boundary. */
        *dst = *src;
        return ASH_OK;
    case ASH_TY_STRING:
        *dst = ash_string_copy(c, src->as.s.ptr, src->as.s.len);
        if (src->as.s.len && !dst->as.s.ptr) return ASH_ERR_OOM;
        return ASH_OK;
    case ASH_TY_LIST:
    case ASH_TY_MAP:
    case ASH_TY_TUPLE:
    case ASH_TY_RECORD:
    case ASH_TY_SUM: {
        uint64_t n = src->as.list.len;
        memset(dst, 0, sizeof(*dst));
        dst->ty = src->ty;
        dst->tag = src->tag;
        dst->as.list.elem_ty = src->as.list.elem_ty;
        dst->as.list.len = n;
        dst->as.list.cap = n;
        if (n == 0) return ASH_OK;
        AshValue* data = (AshValue*)ash_bytes(c, n * sizeof(AshValue));
        if (!data) return ASH_ERR_OOM;
        const AshValue* from = (const AshValue*)src->as.list.data;
        for (uint64_t i = 0; i < n; i++) {
            AshStatus st = ash_value_deep_copy(c, &from[i], &data[i]);
            if (st != ASH_OK) return st;
        }
        dst->as.list.data = data;
        return ASH_OK;
    }
    case ASH_TY_OPTION:
    case ASH_TY_RESULT: {
        memset(dst, 0, sizeof(*dst));
        dst->ty = src->ty;
        dst->tag = src->tag;
        if (!src->as.box) return ASH_OK;
        AshValue* boxed = ash_box(c);
        if (!boxed) return ASH_ERR_OOM;
        AshStatus st = ash_value_deep_copy(c, (const AshValue*)src->as.box,
                                           boxed);
        if (st != ASH_OK) return st;
        dst->as.box = boxed;
        return ASH_OK;
    }
    default:
        return ASH_ERR_TYPE;
    }
}

/* ---- the debug renderer ---- */

/* A counting sink: pos advances for every byte the render wants, and bytes
 * land in buf only while they fit. The render runs once with a NULL buf to
 * size the text and once more to write it, so a too-small cap writes
 * nothing, the same promise ash_iname_dump makes. */
typedef struct RenderSink {
    char*  buf;
    size_t cap;
    size_t pos;
} RenderSink;

static void sink_put(RenderSink* s, const char* bytes, size_t n) {
    if (s->buf && s->pos < s->cap) {
        size_t room = s->cap - s->pos;
        memcpy(s->buf + s->pos, bytes, n < room ? n : room);
    }
    s->pos += n;
}

static void sink_str(RenderSink* s, const char* z) {
    sink_put(s, z, strlen(z));
}

static void sink_fmt(RenderSink* s, const char* fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sink_put(s, tmp, (size_t)n);
}

/* String bytes in the debug spelling: quote and backslash escaped, control
 * bytes as \xNN, everything else raw, UTF-8 passing through untouched. */
static void render_string(RenderSink* s, const AshString* str) {
    sink_put(s, "\"", 1);
    for (uint64_t i = 0; i < str->len; i++) {
        uint8_t b = str->ptr[i];
        if (b == '"') {
            sink_put(s, "\\\"", 2);
        } else if (b == '\\') {
            sink_put(s, "\\\\", 2);
        } else if (b < 0x20 || b == 0x7f) {
            sink_fmt(s, "\\x%02x", (unsigned)b);
        } else {
            sink_put(s, (const char*)&str->ptr[i], 1);
        }
    }
    sink_put(s, "\"", 1);
}

#define ASH_RENDER_DEPTH 8

static void render_value(RenderSink* s, const AshValue* v, unsigned depth);

static void render_elems(RenderSink* s, const AshValue* v, unsigned depth,
                         const char* open, const char* close) {
    sink_str(s, open);
    const AshValue* xs = (const AshValue*)v->as.list.data;
    for (uint64_t i = 0; i < v->as.list.len; i++) {
        if (i) sink_str(s, ", ");
        render_value(s, &xs[i], depth + 1);
    }
    sink_str(s, close);
}

/* A map in its canonical spelling: {k: v, ...} pairs in insertion order,
 * which is the only order a map has. */
static void render_map(RenderSink* s, const AshValue* v, unsigned depth) {
    sink_put(s, "{", 1);
    const AshValue* xs = (const AshValue*)v->as.list.data;
    for (uint64_t i = 0; i + 1 < v->as.list.len; i += 2) {
        if (i) sink_str(s, ", ");
        render_value(s, &xs[i], depth + 1);
        sink_str(s, ": ");
        render_value(s, &xs[i + 1], depth + 1);
    }
    sink_put(s, "}", 1);
}

static void render_box(RenderSink* s, const AshValue* v, unsigned depth,
                       const char* name) {
    sink_str(s, name);
    sink_put(s, "(", 1);
    if (v->as.box) {
        render_value(s, (const AshValue*)v->as.box, depth + 1);
    } else {
        sink_put(s, "?", 1);
    }
    sink_put(s, ")", 1);
}

static void render_value(RenderSink* s, const AshValue* v, unsigned depth) {
    if (depth > ASH_RENDER_DEPTH) {
        sink_str(s, "...");
        return;
    }
    switch ((AshTypeTag)v->ty) {
    case ASH_TY_UNIT:   sink_str(s, "()"); return;
    case ASH_TY_INT:    sink_fmt(s, "%lld", (long long)v->as.i); return;
    case ASH_TY_UINT:   sink_fmt(s, "%llu", (unsigned long long)v->as.u); return;
    case ASH_TY_FLOAT:  sink_fmt(s, "%g", v->as.f); return;
    case ASH_TY_BOOL:   sink_str(s, v->as.b ? "true" : "false"); return;
    case ASH_TY_BYTE:   sink_fmt(s, "%u", (unsigned)v->as.b); return;
    case ASH_TY_CHAR:   sink_fmt(s, "U+%04X", (unsigned)v->as.ch); return;
    case ASH_TY_STRING: render_string(s, &v->as.s); return;
    case ASH_TY_LIST:   render_elems(s, v, depth, "[", "]"); return;
    case ASH_TY_TUPLE:  render_elems(s, v, depth, "(", ")"); return;
    case ASH_TY_RECORD: render_elems(s, v, depth, "{", "}"); return;
    case ASH_TY_SUM:
        sink_fmt(s, "#%u", (unsigned)v->tag);
        if (v->as.list.len) render_elems(s, v, depth, "(", ")");
        return;
    case ASH_TY_OPTION:
        if (v->tag == 0) { sink_str(s, "None"); return; }
        render_box(s, v, depth, "Some");
        return;
    case ASH_TY_RESULT:
        render_box(s, v, depth, v->tag == 0 ? "Ok" : "Err");
        return;
    case ASH_TY_MAP:        render_map(s, v, depth); return;
    case ASH_TY_PLEDGE_REF: sink_str(s, "<pledge>"); return;
    case ASH_TY_INSTANCE:   sink_str(s, "<instance>"); return;
    default:                sink_str(s, "<?>"); return;
    }
}

/* Renders a value in its canonical debug spelling, the text the emitted
 * standalone wrapper prints for a Main.run Err. The size protocol is
 * ash_iname_dump's: *need receives the full size including the NUL, a cap at
 * least that writes the text, anything smaller writes nothing and reports
 * ASH_ERR_OOM, so a NULL buf with cap 0 sizes the buffer. */
AshStatus ash_value_render(const AshValue* v, char* buf, size_t cap,
                           size_t* need) {
    if (!v || !need) return ASH_ERR_TYPE;
    if (!buf) cap = 0;
    RenderSink size_pass = { NULL, 0, 0 };
    render_value(&size_pass, v, 0);
    *need = size_pass.pos + 1;
    if (cap < *need) return ASH_ERR_OOM;
    RenderSink write_pass = { buf, cap, 0 };
    render_value(&write_pass, v, 0);
    buf[write_pass.pos] = '\0';
    return ASH_OK;
}

/* ---- the network client ---- */

/* The retained buffer bookkeeping and the remote table merge behind
 * ash_runtime_connect. A merge parses the daemon's canonical dump text back
 * into iname entries and inserts them beside the local ones, the reverse of
 * ash_iname_dump. The dump text carries a mangled name, a kind, a shape hash,
 * and a version, which is everything discovery and the dump need; the owning
 * contract name, the pledge symbol, and the argument count are not in the
 * dump, so a remote entry leaves them empty until a later layer that fulfills
 * across the wire needs them. */

/* Whether a pointer falls inside one of the runtime's retained net buffers.
 * Called under rt->lock at shutdown, and cheap because the buffer list is one
 * entry per connection. */
static int net_owned(const AshRuntime* rt, const void* ptr) {
    const uint8_t* q = (const uint8_t*)ptr;
    for (const AshNetBuf* b = rt->net_bufs; b; b = b->next) {
        const uint8_t* base = (const uint8_t*)b->p;
        if (q >= base && q < base + b->n) return 1;
    }
    return 0;
}

/* Hands one heap block to the runtime to hold until shutdown. Under rt->lock.
 * On failure the caller still owns the block. */
static AshStatus net_track(AshRuntime* rt, void* p, size_t n) {
    AshNetBuf* b = (AshNetBuf*)malloc(sizeof(AshNetBuf));
    if (!b) return ASH_ERR_OOM;
    b->p = p;
    b->n = n;
    b->next = rt->net_bufs;
    rt->net_bufs = b;
    return ASH_OK;
}

/* Removes one entry by mangled name, the rollback when a later insert of the
 * same merge fails. Under rt->lock. */
static void iname_remove_one(AshRuntime* rt, const char* mangled) {
    size_t pos;
    if (!iname_find(rt, mangled, &pos)) return;
    memmove(rt->inames + pos, rt->inames + pos + 1,
            (rt->ninames - pos - 1) * sizeof(AshInameEntry));
    rt->ninames--;
}

/* ---- the daemon seam: a decode arena and vow enumeration ----
 *
 * ashd reaches these through ash_remote.h. The scratch instance is a bare
 * AshContract the wire decoder can own values on when there is no real instance
 * yet, which is the case for a SIGN frame's vow overrides. The vow enumeration
 * walks a signed instance's effective vows so the daemon can put the whole set
 * in a SIGNED reply. */

AshContract* ash_scratch_new(AshRuntime* rt) {
    AshContract* c = (AshContract*)calloc(1, sizeof(AshContract));
    if (!c) return NULL;
    if (mutex_init_recursive(&c->mu) != 0) {
        free(c);
        return NULL;
    }
    c->rt = rt;
    c->state = ASH_SIGNED; /* so the allocation helpers do not balk */
    return c;
}

void ash_scratch_free(AshContract* c) {
    if (!c) return;
    contract_free_owned(c);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

size_t ash_instance_nvows(const AshContract* c) {
    if (!c || !c->desc || c->state == ASH_UNSIGNED) return 0;
    return c->desc->nvows;
}

const char* ash_instance_vow_name(const AshContract* c, size_t i) {
    if (!c || !c->desc || i >= c->desc->nvows) return NULL;
    return c->desc->vows[i].name;
}

const AshValue* ash_instance_vow_value(const AshContract* c, size_t i) {
    if (!c || !c->vow_vals || !c->desc || i >= c->desc->nvows) return NULL;
    return &c->vow_vals[i];
}

/* ---- the client's remote surface ---- */

static AshStatus error_status(const AshWireFrame* fr, const uint8_t* pl) {
    if (fr->payload_len >= 4 && pl) return (AshStatus)ash_net_get_u32(pl);
    return ASH_ERR_NET;
}

static RemoteContract* find_remote(AshRuntime* rt, const char* name) {
    for (size_t i = 0; i < rt->nremotes; i++) {
        if (strcmp(rt->remotes[i].name, name) == 0) return &rt->remotes[i];
    }
    return NULL;
}

/* Adds one remote contract row, the name kept runtime owned. Under rt->lock. */
static AshStatus remotes_add(AshRuntime* rt, const char* name, uint64_t hash,
                             uint32_t ver, AshConn* conn) {
    if (rt->nremotes == rt->remotes_cap) {
        size_t cap = rt->remotes_cap ? rt->remotes_cap * 2 : 8;
        RemoteContract* g =
            (RemoteContract*)realloc(rt->remotes, cap * sizeof(RemoteContract));
        if (!g) return ASH_ERR_OOM;
        rt->remotes = g;
        rt->remotes_cap = cap;
    }
    rt->remotes[rt->nremotes].name = name;
    rt->remotes[rt->nremotes].shape_hash = hash;
    rt->remotes[rt->nremotes].version = ver;
    rt->remotes[rt->nremotes].conn = conn;
    rt->nremotes++;
    return ASH_OK;
}

/* Recovers the plain contract name from a contract kind mangled name, the
 * reverse of iname_contract_mangled: __ash_ash_{name}__{16 hex}_v{digits}. The
 * suffix is parsed from the right so a name with underscores stays whole; a
 * mangled name that does not fit the shape returns NULL. Heap the caller owns. */
static char* parse_contract_name(const char* m) {
    const char* pre = "__ash_ash_";
    size_t plen = strlen(pre);
    if (strncmp(m, pre, plen) != 0) return NULL;
    size_t len = strlen(m);
    const char* end = m + len;
    const char* d = end;
    while (d > m && d[-1] >= '0' && d[-1] <= '9') d--;
    if (d == end) return NULL;                 /* no version digits */
    if (d - 2 < m || d[-1] != 'v' || d[-2] != '_') return NULL;
    const char* vstart = d - 2;                /* the "_v" */
    if (vstart - 16 < m) return NULL;
    const char* hstart = vstart - 16;
    for (const char* h = hstart; h < vstart; h++) {
        char ch = *h;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return NULL;
    }
    if (hstart - 2 < m + (long)plen) return NULL;
    if (hstart[-1] != '_' || hstart[-2] != '_') return NULL;
    const char* nstart = m + plen;
    const char* nend = hstart - 2;
    if (nend <= nstart) return NULL;
    size_t nlen = (size_t)(nend - nstart);
    char* out = (char*)malloc(nlen + 1);
    if (!out) return NULL;
    memcpy(out, nstart, nlen);
    out[nlen] = '\0';
    return out;
}

/* Parses the daemon's canonical dump text into iname entries and merges them
 * all or nothing, the reverse of ash_iname_dump. The text is copied into a
 * retained buffer and tokenized in place so each mangled name borrows into it
 * exactly as a local pledge entry borrows into a module image. A contract entry
 * also recovers its plain name, which the dump does not carry, so a later sign
 * by that name resolves to this connection's origin. A name that already
 * exists, local or from an earlier connection, fails the whole merge with
 * ASH_ERR_NAME and inserts nothing. */
static AshStatus iname_merge_remote(AshRuntime* rt, AshConn* conn,
                                    const uint8_t* text, uint32_t len) {
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) return ASH_ERR_OOM;
    if (len) memcpy(buf, text, len);
    buf[len] = '\0';

    AshInameEntry* ents = NULL;
    char** cnames = NULL;      /* parsed contract name per entry, NULL for pledges */
    size_t nents = 0, cap = 0;
    AshStatus st = ASH_OK;
    char* p = buf;
    while (*p) {
        char* line = p;
        char* nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = line + strlen(line);
        }
        if (*line == '\0') continue;
        char* sp = strchr(line, ' ');
        if (!sp) {
            st = ASH_ERR_TYPE;
            break;
        }
        *sp = '\0';
        char kindbuf[16];
        unsigned long long hash = 0;
        unsigned ver = 0;
        if (sscanf(sp + 1, "%15s %llx v%u", kindbuf, &hash, &ver) != 3) {
            st = ASH_ERR_TYPE;
            break;
        }
        uint32_t kind;
        if (strcmp(kindbuf, "contract") == 0) {
            kind = ASH_INAME_CONTRACT;
        } else if (strcmp(kindbuf, "pledge") == 0) {
            kind = ASH_INAME_PLEDGE;
        } else {
            st = ASH_ERR_TYPE;
            break;
        }
        char* cname = NULL;
        if (kind == ASH_INAME_CONTRACT) {
            cname = parse_contract_name(line);
            if (!cname) {
                st = ASH_ERR_TYPE;
                break;
            }
        }
        if (nents == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            AshInameEntry* ge = realloc(ents, ncap * sizeof(*ge));
            char** gc = realloc(cnames, ncap * sizeof(*gc));
            if (!ge || !gc) {
                free(ge ? ge : ents);
                free(gc ? gc : cnames);
                free(cname);
                ents = NULL;
                cnames = NULL;
                free(buf);
                return ASH_ERR_OOM;
            }
            ents = ge;
            cnames = gc;
            cap = ncap;
        }
        AshInameEntry e;
        memset(&e, 0, sizeof e);
        e.mangled = line;
        e.kind = kind;
        e.shape_hash = (uint64_t)hash;
        e.version = (uint32_t)ver;
        ents[nents] = e;
        cnames[nents] = cname;
        nents++;
    }
    if (st != ASH_OK) {
        for (size_t i = 0; i < nents; i++) free(cnames[i]);
        free(ents);
        free(cnames);
        free(buf);
        return st;
    }

    pthread_mutex_lock(&rt->lock);
    size_t start_remotes = rt->nremotes;
    size_t inserted = 0;
    for (size_t i = 0; i < nents; i++) {
        size_t pos;
        if (iname_find(rt, ents[i].mangled, &pos)) {
            st = ASH_ERR_NAME;
            break;
        }
    }
    if (st == ASH_OK) {
        for (; inserted < nents; inserted++) {
            if (ents[inserted].kind == ASH_INAME_CONTRACT) {
                ents[inserted].contract = cnames[inserted];
            }
            st = iname_insert(rt, &ents[inserted]);
            if (st != ASH_OK) break;
            if (ents[inserted].kind == ASH_INAME_CONTRACT) {
                st = remotes_add(rt, cnames[inserted], ents[inserted].shape_hash,
                                 ents[inserted].version, conn);
                if (st != ASH_OK) {
                    /* the entry inserted but its origin row did not: drop the
                     * entry too so the rollback below is uniform */
                    iname_remove_one(rt, ents[inserted].mangled);
                    break;
                }
            }
        }
    }
    if (st == ASH_OK) {
        /* Commit the borrowed storage: the dump buffer and every parsed name go
         * into the runtime's retained set, so the merged entries' pointers hold
         * to shutdown. A failure here rolls the visible tables back; a buffer
         * already retained is simply freed at shutdown. */
        if (net_track(rt, buf, (size_t)len + 1) != ASH_OK) {
            st = ASH_ERR_OOM;
        } else {
            buf = NULL; /* owned by net_bufs now */
            for (size_t i = 0; i < nents; i++) {
                if (!cnames[i]) continue;
                if (net_track(rt, cnames[i], strlen(cnames[i]) + 1) != ASH_OK) {
                    st = ASH_ERR_OOM;
                    break;
                }
                cnames[i] = NULL; /* owned by net_bufs now */
            }
        }
    }
    if (st != ASH_OK) {
        for (size_t i = 0; i < inserted; i++) {
            iname_remove_one(rt, ents[i].mangled);
        }
        rt->nremotes = start_remotes;
        pthread_mutex_unlock(&rt->lock);
        for (size_t i = 0; i < nents; i++) free(cnames[i]);
        free(ents);
        free(cnames);
        free(buf);
        return st;
    }
    pthread_mutex_unlock(&rt->lock);
    free(ents);
    free(cnames);
    free(buf);
    return ASH_OK;
}

/* Sends one frame under the connection's write lock, the one gate every write
 * passes so a reader's decode reply and a caller's fulfill never interleave. */
static int conn_send(AshConn* conn, uint32_t kind, uint64_t req_id,
                     const uint8_t* pl, uint32_t plen) {
    pthread_mutex_lock(&conn->wmu);
    int rc = ash_net_send_frame(conn->fd, kind, req_id, pl, plen);
    pthread_mutex_unlock(&conn->wmu);
    return rc;
}

/* Finishes one fulfillment from its RESULT or ERROR frame. The value is decoded
 * onto the proxy under its own lock, the same home a local result has; a proxy
 * a break already latched Broken takes no value, so a late RESULT racing a
 * break resolves to ASH_ERR_STATE instead of allocating onto a dead heap. */
static void complete_fulfill(struct AshFuture* f, const AshWireFrame* fr,
                             const uint8_t* pl) {
    if (fr->kind == ASH_WIRE_ERROR) {
        future_finish(f, error_status(fr, pl), NULL);
        return;
    }
    if (fr->kind != ASH_WIRE_RESULT || fr->payload_len < 4) {
        future_finish(f, ASH_ERR_NET, NULL);
        return;
    }
    AshRBuf r;
    ash_rbuf_init(&r, pl, fr->payload_len);
    uint32_t status = 0;
    ash_rbuf_u32(&r, &status);
    if ((AshStatus)status != ASH_OK) {
        future_finish(f, (AshStatus)status, NULL);
        return;
    }
    AshContract* c = f->c;
    AshValue out;
    memset(&out, 0, sizeof out);
    pthread_mutex_lock(&c->mu);
    if (c->state == ASH_BROKEN) {
        pthread_mutex_unlock(&c->mu);
        future_finish(f, ASH_ERR_STATE, NULL);
        return;
    }
    AshStatus dst = ash_wire_decode_value(c, r.p, r.left, &out, NULL);
    pthread_mutex_unlock(&c->mu);
    if (dst != ASH_OK) {
        future_finish(f, ASH_ERR_NET, NULL);
    } else {
        future_finish(f, ASH_OK, &out);
    }
}

/* Hands a synchronous answer to its waiter, transferring the payload buffer. */
static void deliver_reply(PendingReply* rep, const AshWireFrame* fr,
                          uint8_t* pl) {
    pthread_mutex_lock(&rep->mu);
    rep->fr = *fr;
    rep->payload = pl;
    rep->done = 1;
    pthread_cond_signal(&rep->cv);
    pthread_mutex_unlock(&rep->mu);
}

/* Routes one received frame to the request its id answers. A request id lives
 * in exactly one map, so the id alone decides fulfill versus synchronous reply,
 * whatever the kind, which is what lets an ERROR answer either. */
static void conn_dispatch(AshConn* conn, const AshWireFrame* fr, uint8_t* pl) {
    struct AshFuture* fut = NULL;
    PendingReply* rep = NULL;
    pthread_mutex_lock(&conn->pmu);
    struct AshFuture** pf = &conn->pending_futures;
    while (*pf) {
        if ((*pf)->req_id == fr->request_id) {
            fut = *pf;
            *pf = fut->qnext;
            break;
        }
        pf = &(*pf)->qnext;
    }
    if (!fut) {
        PendingReply** pr = &conn->pending_replies;
        while (*pr) {
            if ((*pr)->req_id == fr->request_id) {
                rep = *pr;
                *pr = rep->next;
                break;
            }
            pr = &(*pr)->next;
        }
    }
    pthread_mutex_unlock(&conn->pmu);

    if (fut) {
        complete_fulfill(fut, fr, pl);
        free(pl);
    } else if (rep) {
        deliver_reply(rep, fr, pl); /* takes the payload */
    } else {
        free(pl); /* an answer to nothing, dropped */
    }
}

/* Tears one connection down exactly once: every in flight fulfillment delivers
 * ASH_ERR_NET, every pending synchronous call learns the peer is gone, and
 * every proxy signed here latches Broken so a later fulfill is a local
 * ASH_ERR_STATE with no wire. The instance heaps are left for shutdown, so a
 * result a host already waited stays readable, the wait-before-break rule
 * unchanged across the network. */
static void conn_shutdown(AshConn* conn) {
    pthread_mutex_lock(&conn->pmu);
    if (conn->shutdown_done) {
        pthread_mutex_unlock(&conn->pmu);
        return;
    }
    conn->shutdown_done = 1;
    conn->dead = 1;
    struct AshFuture* futs = conn->pending_futures;
    conn->pending_futures = NULL;
    PendingReply* reps = conn->pending_replies;
    conn->pending_replies = NULL;
    AshContract* proxies = conn->proxies;
    conn->proxies = NULL;
    pthread_mutex_unlock(&conn->pmu);

    /* Wake a reader blocked in a read, or unblock the send side that detected
     * the failure; the fd is closed later by conn_free. */
    shutdown(conn->fd, SHUT_RDWR);

    while (futs) {
        struct AshFuture* next = futs->qnext;
        future_finish(futs, ASH_ERR_NET, NULL);
        futs = next;
    }
    while (reps) {
        PendingReply* next = reps->next;
        pthread_mutex_lock(&reps->mu);
        reps->dead = 1;
        reps->done = 1;
        pthread_cond_signal(&reps->cv);
        pthread_mutex_unlock(&reps->mu);
        reps = next;
    }
    while (proxies) {
        AshContract* next = proxies->proxy_next;
        pthread_mutex_lock(&proxies->mu);
        if (proxies->state != ASH_BROKEN && proxies->state != ASH_UNSIGNED) {
            proxies->state = ASH_BROKEN;
        }
        pthread_mutex_unlock(&proxies->mu);
        proxies = next;
    }
}

static void* conn_reader(void* arg) {
    AshConn* conn = (AshConn*)arg;
    for (;;) {
        AshWireFrame fr;
        uint8_t* pl = NULL;
        int rc = ash_net_recv_frame(conn->fd, &fr, &pl);
        if (rc != 0) {
            free(pl);
            break; /* EOF, error, or a malformed frame the stream cannot survive */
        }
        conn_dispatch(conn, &fr, pl);
    }
    conn_shutdown(conn);
    return NULL;
}

static AshConn* conn_new(AshRuntime* rt, int fd) {
    AshConn* conn = (AshConn*)calloc(1, sizeof(AshConn));
    if (!conn) return NULL;
    conn->rt = rt;
    conn->fd = fd;
    conn->next_req = 3; /* the handshake used ids 1 and 2 */
    if (pthread_mutex_init(&conn->wmu, NULL) != 0) {
        free(conn);
        return NULL;
    }
    if (pthread_mutex_init(&conn->pmu, NULL) != 0) {
        pthread_mutex_destroy(&conn->wmu);
        free(conn);
        return NULL;
    }
    return conn;
}

static void conn_free(AshConn* conn) {
    if (conn->reader_started) pthread_join(conn->reader, NULL);
    close(conn->fd);
    pthread_mutex_destroy(&conn->wmu);
    pthread_mutex_destroy(&conn->pmu);
    free(conn);
}

/* One synchronous request and its reply, the shape sign, break, and partial
 * share. The reply registers before the send so an answer can never arrive
 * before its receipt exists; a dead connection returns ASH_ERR_NET. On ASH_OK
 * the caller owns *out_pl and frees it. */
static AshStatus conn_call(AshConn* conn, uint32_t kind, const AshWBuf* body,
                           AshWireFrame* out_fr, uint8_t** out_pl) {
    *out_pl = NULL;
    if (body->err) return ASH_ERR_OOM;
    PendingReply rep;
    memset(&rep, 0, sizeof rep);
    if (pthread_mutex_init(&rep.mu, NULL) != 0) return ASH_ERR_OOM;
    if (pthread_cond_init(&rep.cv, NULL) != 0) {
        pthread_mutex_destroy(&rep.mu);
        return ASH_ERR_OOM;
    }

    pthread_mutex_lock(&conn->pmu);
    if (conn->dead) {
        pthread_mutex_unlock(&conn->pmu);
        pthread_mutex_destroy(&rep.mu);
        pthread_cond_destroy(&rep.cv);
        return ASH_ERR_NET;
    }
    rep.req_id = conn->next_req++;
    rep.next = conn->pending_replies;
    conn->pending_replies = &rep;
    pthread_mutex_unlock(&conn->pmu);

    if (conn_send(conn, kind, rep.req_id, body->data, (uint32_t)body->len) != 0) {
        conn_shutdown(conn); /* delivers dead to rep among the rest */
    }

    pthread_mutex_lock(&rep.mu);
    while (!rep.done) pthread_cond_wait(&rep.cv, &rep.mu);
    int dead = rep.dead;
    AshWireFrame fr = rep.fr;
    uint8_t* pl = rep.payload;
    pthread_mutex_unlock(&rep.mu);

    pthread_mutex_destroy(&rep.mu);
    pthread_cond_destroy(&rep.cv);
    if (dead) {
        free(pl);
        return ASH_ERR_NET;
    }
    *out_fr = fr;
    *out_pl = pl;
    return ASH_OK;
}

static AshStatus remote_sign(AshRuntime* rt, RemoteContract* rc,
                             const AshVowBinding* vows, size_t nvows,
                             uint64_t expected_hash, AshContract** out) {
    AshConn* conn = rc->conn;
    AshWBuf w;
    ash_wbuf_init(&w);
    ash_wbuf_str(&w, rc->name, strlen(rc->name));
    ash_wbuf_u64(&w, expected_hash);
    ash_wbuf_u32(&w, (uint32_t)nvows);
    for (size_t i = 0; i < nvows; i++) {
        if (!vows[i].name) {
            ash_wbuf_free(&w);
            return ASH_ERR_NAME;
        }
        ash_wbuf_str(&w, vows[i].name, strlen(vows[i].name));
        ash_wbuf_value(&w, &vows[i].value);
    }
    if (w.err) {
        ash_wbuf_free(&w);
        return ASH_ERR_TYPE; /* a vow value that cannot cross, or OOM */
    }

    AshWireFrame fr;
    uint8_t* pl = NULL;
    AshStatus st = conn_call(conn, ASH_WIRE_SIGN, &w, &fr, &pl);
    ash_wbuf_free(&w);
    if (st != ASH_OK) return st;
    if (fr.kind == ASH_WIRE_ERROR) {
        AshStatus e = error_status(&fr, pl);
        free(pl);
        return e;
    }
    if (fr.kind != ASH_WIRE_SIGNED) {
        free(pl);
        return ASH_ERR_NET;
    }

    AshRBuf r;
    ash_rbuf_init(&r, pl, fr.payload_len);
    uint64_t inst_id = 0, shape_hash = 0;
    int64_t signed_at = 0;
    uint32_t vcount = 0;
    if (!ash_rbuf_u64(&r, &inst_id) || !ash_rbuf_i64(&r, &signed_at) ||
        !ash_rbuf_u64(&r, &shape_hash) || !ash_rbuf_u32(&r, &vcount)) {
        free(pl);
        return ASH_ERR_NET;
    }

    AshContract* c = (AshContract*)calloc(1, sizeof(AshContract));
    if (!c) {
        free(pl);
        return ASH_ERR_OOM;
    }
    if (mutex_init_recursive(&c->mu) != 0) {
        free(c);
        free(pl);
        return ASH_ERR_OOM;
    }
    c->rt = rt;
    c->conn = conn;
    c->remote_id = inst_id;
    c->shape_hash = shape_hash;
    c->signed_at = signed_at;
    c->state = ASH_SIGNED;

    st = ASH_OK;
    if (vcount) {
        c->rvow_names = (char**)ash_bytes(c, vcount * sizeof(char*));
        c->rvow_vals = (AshValue*)ash_bytes(c, vcount * sizeof(AshValue));
        if (!c->rvow_names || !c->rvow_vals) {
            st = ASH_ERR_OOM;
        } else {
            memset(c->rvow_names, 0, vcount * sizeof(char*));
            memset(c->rvow_vals, 0, vcount * sizeof(AshValue));
        }
    }
    for (uint32_t i = 0; st == ASH_OK && i < vcount; i++) {
        const char* nm;
        uint32_t nlen;
        if (!ash_rbuf_str(&r, &nm, &nlen)) {
            st = ASH_ERR_NET;
            break;
        }
        char* nmc = (char*)ash_bytes(c, (uint64_t)nlen + 1);
        if (!nmc) {
            st = ASH_ERR_OOM;
            break;
        }
        memcpy(nmc, nm, nlen);
        nmc[nlen] = '\0';
        c->rvow_names[i] = nmc;
        AshValue v;
        size_t consumed = 0;
        if (ash_wire_decode_value(c, r.p, r.left, &v, &consumed) != ASH_OK) {
            st = ASH_ERR_NET;
            break;
        }
        ash_rbuf_skip(&r, consumed);
        c->rvow_vals[i] = v;
        c->remote_nvows = i + 1;
    }
    free(pl);
    if (st != ASH_OK) {
        contract_free_owned(c);
        pthread_mutex_destroy(&c->mu);
        free(c);
        return st;
    }

    pthread_mutex_lock(&rt->lock);
    int room = rt->ninstances < ASH_MAX_INSTANCES;
    if (room) rt->instances[rt->ninstances++] = c;
    pthread_mutex_unlock(&rt->lock);
    if (!room) {
        contract_free_owned(c);
        pthread_mutex_destroy(&c->mu);
        free(c);
        return ASH_ERR_OOM;
    }

    pthread_mutex_lock(&conn->pmu);
    int dead = conn->dead;
    if (!dead) {
        c->proxy_next = conn->proxies;
        conn->proxies = c;
    }
    pthread_mutex_unlock(&conn->pmu);
    if (dead) {
        pthread_mutex_lock(&c->mu);
        c->state = ASH_BROKEN;
        pthread_mutex_unlock(&c->mu);
    }
    *out = c;
    return ASH_OK;
}

static AshFuture* remote_fulfill(AshContract* c, const char* pledge_name,
                                 const AshValue* args, size_t nargs,
                                 const AshRef* refs, size_t nrefs) {
    AshConn* conn = c->conn;
    struct AshFuture* f = future_new(c);
    if (!f) return NULL;

    /* Every failure short of the future itself arrives through the wait, the
     * same delivery point a local fulfillment's errors keep. Refs never cross
     * the wire in v1, so a ref bearing fulfill is refused loudly here. */
    AshStatus err = ASH_OK;
    if ((nargs > 0 && !args) || (nrefs > 0 && !refs)) {
        err = ASH_ERR_TYPE;
    } else if (nrefs > 0) {
        err = ASH_ERR_TYPE;
    } else {
        pthread_mutex_lock(&c->mu);
        if (c->state == ASH_BROKEN) err = ASH_ERR_STATE;
        pthread_mutex_unlock(&c->mu);
    }
    if (err != ASH_OK) {
        future_finish(f, err, NULL);
        return f;
    }

    AshWBuf w;
    ash_wbuf_init(&w);
    ash_wbuf_u64(&w, c->remote_id);
    ash_wbuf_str(&w, pledge_name, strlen(pledge_name));
    ash_wbuf_u32(&w, (uint32_t)nargs);
    for (size_t i = 0; i < nargs; i++) ash_wbuf_value(&w, &args[i]);
    if (w.err) {
        ash_wbuf_free(&w);
        future_finish(f, ASH_ERR_TYPE, NULL);
        return f;
    }

    pthread_mutex_lock(&conn->pmu);
    if (conn->dead) {
        pthread_mutex_unlock(&conn->pmu);
        ash_wbuf_free(&w);
        future_finish(f, ASH_ERR_NET, NULL);
        return f;
    }
    uint64_t req_id = conn->next_req++;
    f->req_id = req_id;
    f->qnext = conn->pending_futures;
    conn->pending_futures = f;
    pthread_mutex_unlock(&conn->pmu);

    if (conn_send(conn, ASH_WIRE_FULFILL, req_id, w.data, (uint32_t)w.len) != 0) {
        conn_shutdown(conn); /* finishes f with ASH_ERR_NET */
    }
    ash_wbuf_free(&w);
    return f;
}

static AshStatus remote_break(AshContract* c) {
    AshConn* conn = c->conn;
    pthread_mutex_lock(&c->mu);
    if (c->state == ASH_UNSIGNED) {
        pthread_mutex_unlock(&c->mu);
        return ASH_ERR_STATE;
    }
    int already = (c->state == ASH_BROKEN);
    for (struct AshFuture* f = c->futures; f; f = f->next) future_forfeit(f);
    /* An explicit break reclaims the proxy's own heap the way a local break
     * reclaims an instance's, under the lock so no reader is mid write into it.
     * The stored vows and every decoded result die here, the wait before break
     * rule the same across the wire; the vow pointers are cleared so a later
     * ash_vow_ref reads no vows rather than freed memory, exactly as the local
     * path leaves vow_vals NULL. */
    contract_free_owned(c);
    c->rvow_names = NULL;
    c->rvow_vals = NULL;
    c->remote_nvows = 0;
    c->state = ASH_BROKEN;
    pthread_mutex_unlock(&c->mu);
    if (already) return ASH_OK; /* the connection died; nothing to send */

    AshWBuf w;
    ash_wbuf_init(&w);
    ash_wbuf_u64(&w, c->remote_id);
    AshWireFrame fr;
    uint8_t* pl = NULL;
    AshStatus st = conn_call(conn, ASH_WIRE_BREAK, &w, &fr, &pl);
    ash_wbuf_free(&w);
    if (st != ASH_OK) return ASH_OK; /* proxy already latched Broken locally */
    free(pl);
    return ASH_OK;
}

/* One PARTIAL_QUERY round trip, the shared front of every partial read. On
 * ASH_OK the caller owns *out_pl and reads a PARTIAL frame from it. */
static AshStatus remote_partial_query(AshContract* c, AshWireFrame* out_fr,
                                      uint8_t** out_pl) {
    *out_pl = NULL;
    AshWBuf w;
    ash_wbuf_init(&w);
    ash_wbuf_u64(&w, c->remote_id);
    AshStatus st = conn_call(c->conn, ASH_WIRE_PARTIAL_QUERY, &w, out_fr, out_pl);
    ash_wbuf_free(&w);
    if (st != ASH_OK) return st;
    if (out_fr->kind == ASH_WIRE_ERROR) {
        AshStatus e = error_status(out_fr, *out_pl);
        free(*out_pl);
        *out_pl = NULL;
        return e;
    }
    if (out_fr->kind != ASH_WIRE_PARTIAL) {
        free(*out_pl);
        *out_pl = NULL;
        return ASH_ERR_NET;
    }
    return ASH_OK;
}

/* Whether the proxy has latched Broken, an explicit break or a disconnect. A
 * broken proxy answers every partial read locally and empty, both because its
 * heap may already be reclaimed and because a broken instance has nothing to
 * report, so no read after a break touches a heap the break is freeing. */
static int remote_broken(AshContract* c) {
    pthread_mutex_lock(&c->mu);
    int b = (c->state == ASH_BROKEN);
    pthread_mutex_unlock(&c->mu);
    return b;
}

static AshContractState remote_state(AshContract* c) {
    if (remote_broken(c)) return ASH_BROKEN;
    AshWireFrame fr;
    uint8_t* pl = NULL;
    if (remote_partial_query(c, &fr, &pl) != ASH_OK) return ASH_BROKEN;
    AshRBuf r;
    ash_rbuf_init(&r, pl, fr.payload_len);
    uint32_t state = ASH_BROKEN;
    ash_rbuf_u32(&r, &state);
    free(pl);
    return (AshContractState)state;
}

/* Walks the item block of a PARTIAL frame, calling visit for each item until it
 * returns nonzero or the items run out. Returns the item count read. */
static size_t partial_walk_items(AshRBuf* r, uint32_t nitems,
                                 int (*visit)(const char*, uint32_t, uint32_t,
                                              void*),
                                 void* ctx) {
    for (uint32_t i = 0; i < nitems; i++) {
        const char* nm;
        uint32_t nl, is;
        if (!ash_rbuf_str(r, &nm, &nl) || !ash_rbuf_u32(r, &is)) return i;
        if (visit && visit(nm, nl, is, ctx)) return i + 1;
    }
    return nitems;
}

typedef struct CountCtx { AshItemState k; size_t n; } CountCtx;
static int count_visit(const char* nm, uint32_t nl, uint32_t is, void* v) {
    (void)nm; (void)nl;
    CountCtx* cx = (CountCtx*)v;
    if ((AshItemState)is == cx->k) cx->n++;
    return 0;
}

static size_t remote_partial_count(AshContract* c, AshItemState k) {
    if (remote_broken(c)) return 0;
    AshWireFrame fr;
    uint8_t* pl = NULL;
    if (remote_partial_query(c, &fr, &pl) != ASH_OK) return 0;
    AshRBuf r;
    ash_rbuf_init(&r, pl, fr.payload_len);
    uint32_t state, nitems;
    CountCtx cx = { k, 0 };
    if (ash_rbuf_u32(&r, &state) && ash_rbuf_u32(&r, &nitems)) {
        partial_walk_items(&r, nitems, count_visit, &cx);
    }
    free(pl);
    return cx.n;
}

typedef struct NameCtx {
    AshItemState k;
    size_t       want;
    size_t       seen;
    const char*  nm;
    uint32_t     nl;
} NameCtx;
static int name_visit(const char* nm, uint32_t nl, uint32_t is, void* v) {
    NameCtx* cx = (NameCtx*)v;
    if ((AshItemState)is != cx->k) return 0;
    if (cx->seen == cx->want) {
        cx->nm = nm;
        cx->nl = nl;
        return 1;
    }
    cx->seen++;
    return 0;
}

static const char* remote_partial_name(AshContract* c, AshItemState k,
                                       size_t i) {
    if (remote_broken(c)) return NULL;
    AshWireFrame fr;
    uint8_t* pl = NULL;
    if (remote_partial_query(c, &fr, &pl) != ASH_OK) return NULL;
    AshRBuf r;
    ash_rbuf_init(&r, pl, fr.payload_len);
    uint32_t state, nitems;
    NameCtx cx = { k, i, 0, NULL, 0 };
    const char* result = NULL;
    if (ash_rbuf_u32(&r, &state) && ash_rbuf_u32(&r, &nitems)) {
        partial_walk_items(&r, nitems, name_visit, &cx);
        if (cx.nm) {
            /* The name is copied onto the proxy, so the allocation and the write
             * both run under the instance lock, the discipline every c owned
             * write keeps: a concurrent break that frees this heap is serialized
             * against the copy, and a break that already landed leaves the proxy
             * Broken so the copy is skipped rather than aimed at freed memory.
             * The local surface returns descriptor memory; a proxy returns an
             * instance owned copy, alive until the proxy breaks, one rule. */
            pthread_mutex_lock(&c->mu);
            if (c->state != ASH_BROKEN) {
                char* copy = (char*)ash_bytes(c, (uint64_t)cx.nl + 1);
                if (copy) {
                    memcpy(copy, cx.nm, cx.nl);
                    copy[cx.nl] = '\0';
                    result = copy;
                }
            }
            pthread_mutex_unlock(&c->mu);
        }
    }
    free(pl);
    return result;
}

/* Skips the item block whole, leaving the cursor at the error count. */
static int partial_skip_items(AshRBuf* r, uint32_t nitems) {
    for (uint32_t i = 0; i < nitems; i++) {
        const char* nm;
        uint32_t nl, is;
        if (!ash_rbuf_str(r, &nm, &nl) || !ash_rbuf_u32(r, &is)) return 0;
    }
    return 1;
}

static size_t remote_partial_nerrors(AshContract* c) {
    if (remote_broken(c)) return 0;
    AshWireFrame fr;
    uint8_t* pl = NULL;
    if (remote_partial_query(c, &fr, &pl) != ASH_OK) return 0;
    AshRBuf r;
    ash_rbuf_init(&r, pl, fr.payload_len);
    uint32_t state, nitems, nerr = 0;
    if (ash_rbuf_u32(&r, &state) && ash_rbuf_u32(&r, &nitems) &&
        partial_skip_items(&r, nitems)) {
        ash_rbuf_u32(&r, &nerr);
    }
    free(pl);
    return nerr;
}

static AshStatus remote_partial_error(AshContract* c, size_t idx,
                                      const char** pledge_name,
                                      const AshValue** err) {
    if (remote_broken(c)) return ASH_ERR_NAME;
    AshWireFrame fr;
    uint8_t* pl = NULL;
    if (remote_partial_query(c, &fr, &pl) != ASH_OK) return ASH_ERR_NAME;
    AshRBuf r;
    ash_rbuf_init(&r, pl, fr.payload_len);
    uint32_t state, nitems, nerr;
    AshStatus st = ASH_ERR_NAME;
    /* The Err payload is decoded onto the proxy and its name copied there, so
     * the whole walk runs under the instance lock, no c owned allocation or
     * write ever outside it: a break that races is serialized, and a break that
     * already latched Broken skips the walk rather than decoding onto a heap it
     * is reclaiming. The network round trip already finished above, so the lock
     * never covers a wire call. */
    pthread_mutex_lock(&c->mu);
    if (c->state == ASH_BROKEN) {
        pthread_mutex_unlock(&c->mu);
        free(pl);
        return ASH_ERR_NAME;
    }
    if (ash_rbuf_u32(&r, &state) && ash_rbuf_u32(&r, &nitems) &&
        partial_skip_items(&r, nitems) && ash_rbuf_u32(&r, &nerr)) {
        for (uint32_t i = 0; i < nerr; i++) {
            const char* nm;
            uint32_t nl;
            if (!ash_rbuf_str(&r, &nm, &nl)) break;
            AshValue v;
            size_t consumed = 0;
            if (ash_wire_decode_value(c, r.p, r.left, &v, &consumed) != ASH_OK)
                break;
            ash_rbuf_skip(&r, consumed);
            if (i != idx) continue;
            st = ASH_OK;
            if (pledge_name) {
                char* copy = (char*)ash_bytes(c, (uint64_t)nl + 1);
                if (!copy) {
                    st = ASH_ERR_OOM;
                    break;
                }
                memcpy(copy, nm, nl);
                copy[nl] = '\0';
                *pledge_name = copy;
            }
            if (err) {
                AshValue* box = (AshValue*)ash_bytes(c, sizeof(AshValue));
                if (!box) {
                    st = ASH_ERR_OOM;
                    break;
                }
                *box = v;
                *err = box;
            }
            break;
        }
    }
    pthread_mutex_unlock(&c->mu);
    free(pl);
    return st;
}

AshStatus ash_runtime_connect(AshRuntime* rt, const char* addr,
                              const char* token) {
    if (!rt || !addr) return ASH_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    int frozen = rt->frozen;
    uint32_t hs = rt->handshake_ms ? rt->handshake_ms
                                   : ASH_HANDSHAKE_MS_DEFAULT;
    pthread_mutex_unlock(&rt->lock);
    if (frozen) return ASH_ERR_STATE;

    /* The connect itself is on the handshake clock, so a peer that drops its
     * SYNs cannot park the caller forever; the reads and writes of the HELLO
     * round trip carry the same bound, the whole handshake finishing inside hs
     * or failing ASH_ERR_NET. */
    int fd = ash_net_dial_timeout(addr, hs);
    if (fd < 0) return ASH_ERR_NET;
    ash_net_set_rcvtimeo(fd, hs);
    ash_net_set_sndtimeo(fd, hs);

    /* HELLO: the protocol version, then the token as a length prefixed string,
     * empty when the client has none. */
    size_t tlen = token ? strlen(token) : 0;
    uint32_t plen = (uint32_t)(8 + tlen);
    uint8_t* hp = (uint8_t*)malloc(plen);
    if (!hp) {
        close(fd);
        return ASH_ERR_OOM;
    }
    ash_net_put_u32(hp, 1);
    ash_net_put_u32(hp + 4, (uint32_t)tlen);
    if (tlen) memcpy(hp + 8, token, tlen);
    int wr = ash_net_send_frame(fd, ASH_WIRE_HELLO, 1, hp, plen);
    free(hp);
    if (wr != 0) {
        close(fd);
        return ASH_ERR_NET;
    }

    AshWireFrame fr;
    uint8_t* pl = NULL;
    if (ash_net_recv_frame(fd, &fr, &pl) != 0) {
        close(fd);
        return ASH_ERR_NET;
    }
    if (fr.kind == ASH_WIRE_ERROR) {
        AshStatus st = error_status(&fr, pl);
        free(pl);
        close(fd);
        return st;
    }
    if (fr.kind != ASH_WIRE_HELLO_OK || fr.payload_len < 12 || !pl) {
        free(pl);
        close(fd);
        return ASH_ERR_NET;
    }
    uint32_t accepted = ash_net_get_u32(pl);
    uint64_t table_hash = ash_net_get_u64(pl + 4);
    free(pl);
    pl = NULL;
    if (accepted != 1) {
        close(fd);
        return ASH_ERR_VERSION;
    }

    /* INAME_SYNC fetches the whole discovery table in one INAME_TABLE reply. */
    if (ash_net_send_frame(fd, ASH_WIRE_INAME_SYNC, 2, NULL, 0) != 0) {
        close(fd);
        return ASH_ERR_NET;
    }
    if (ash_net_recv_frame(fd, &fr, &pl) != 0) {
        close(fd);
        return ASH_ERR_NET;
    }
    if (fr.kind == ASH_WIRE_ERROR) {
        AshStatus st = error_status(&fr, pl);
        free(pl);
        close(fd);
        return st;
    }
    if (fr.kind != ASH_WIRE_INAME_TABLE || fr.payload_len < 4 || !pl) {
        free(pl);
        close(fd);
        return ASH_ERR_NET;
    }
    uint32_t dlen = ash_net_get_u32(pl);
    if ((uint64_t)dlen + 4 > fr.payload_len) {
        free(pl);
        close(fd);
        return ASH_ERR_NET;
    }
    const uint8_t* dbytes = pl + 4;
    /* The HELLO_OK hash is the daemon's claim about the table it is about to
     * send; a mismatch means the two disagree and the connection is not to be
     * trusted. */
    if (ash_net_fnv1a64(dbytes, dlen) != table_hash) {
        free(pl);
        close(fd);
        return ASH_ERR_NET;
    }

    /* The handshake is done; the connection now stays open under a reader
     * thread that owns every later read. Both clocks come off so a fulfillment
     * blocks as long as its pledge runs, exactly as a local wait does, and a
     * RESULT the size of the wire's cap is never a timed out write. */
    ash_net_set_rcvtimeo(fd, 0);
    ash_net_set_sndtimeo(fd, 0);
    AshConn* conn = conn_new(rt, fd);
    if (!conn) {
        free(pl);
        close(fd);
        return ASH_ERR_OOM;
    }
    if (pthread_create(&conn->reader, NULL, conn_reader, conn) != 0) {
        free(pl);
        conn_free(conn); /* reader not started, just closes and destroys */
        return ASH_ERR_OOM;
    }
    conn->reader_started = 1;

    AshStatus st = iname_merge_remote(rt, conn, dbytes, dlen);
    free(pl);
    if (st != ASH_OK) {
        conn_shutdown(conn);
        conn_free(conn);
        return st;
    }

    pthread_mutex_lock(&rt->lock);
    conn->next = rt->conns;
    rt->conns = conn;
    pthread_mutex_unlock(&rt->lock);
    return ASH_OK;
}
