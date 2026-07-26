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
 * and one per pledge; geas_runtime_freeze latches the registration surface
 * shut, after which load, register, and bind report GEAS_ERR_STATE while
 * sign and fulfill continue unchanged. Every iname read takes the runtime
 * lock, the simple discipline TSan can vouch for.
 *
 * Memory follows one rule. Every allocation a pledge makes goes through the
 * instance's block list, vow values and frames included, so
 * geas_contract_break frees the lot in one walk and valgrind stays clean
 * whatever the pledge did. The one exception threading forced: the future
 * struct itself is heap memory the runtime tracks per instance and frees at
 * shutdown, so a wait that races a break lands on a live struct and reports
 * GEAS_ERR_STATE instead of touching freed memory. Everything a future's
 * value points at is still instance owned and still dies at break, which is
 * why the wait-before-break rule keeps mattering to a host that wants the
 * bytes.
 *
 * M5 makes fulfillment concurrent for real. geas_pledge_fulfill validates and
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
 * instance's lock (fulfill). geas_pledge_fulfill_sync detects a pool worker
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

#include <geas/geas.h>
#include <geas/geas_store.h>
#include <geas/geas_wire.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GEAS_MAX_CONTRACT_TYPES 64
#define GEAS_MAX_MODULES        64
#define GEAS_MAX_INSTANCES      256
#define GEAS_MAX_BINDINGS       128

#define GEAS_POOL_DEFAULT_THREADS 4
#define GEAS_POOL_MAX_THREADS     256

/* A block the instance owns. Blocks form a singly linked list headed in the
 * instance; the header rides in front of the caller's bytes. */
typedef struct GeasBlock {
    struct GeasBlock* next;
} GeasBlock;

/* The per-pledge latch values. A pledge latches on its first Ok or first
 * Err and never moves again; the contract state is recomputed from these
 * latches after every fulfillment. */
enum {
    PLEDGE_PENDING   = 0,
    PLEDGE_FULFILLED = 1,
    PLEDGE_BROKEN    = 2
};

/* The per-subcontract transaction state, one slot per descriptor sub for a
 * store-backed contract. A transactional subcontract opens its episode lazily
 * on the first fulfillment of one of its pledges, so TXN_NONE is the fresh
 * state, TXN_OPEN a live transaction on the connection, and TXN_DONE the one
 * commit or rollback that closes it. Because an episode is one outcome and not
 * a latch, a pledge whose sub reads TXN_DONE is refused GEAS_ERR_STATE rather
 * than run again. */
enum {
    TXN_NONE = 0,
    TXN_OPEN = 1,
    TXN_DONE = 2
};

struct GeasContract {
    GeasRuntime*            rt;
    const GeasContractDesc* desc;
    GeasContractState       state;
    GeasBlock*              owned;
    GeasValue*              vow_vals;  /* one per desc vow, instance owned */
    GeasPledgeFn*           fns;       /* dispatch table resolved at sign */
    struct GeasFuture*      futures;   /* every future this instance issued */
    uint64_t               shape_hash;
    int64_t                signed_at;

    /* The latches, one slot per descriptor pledge, and the first Err payload
     * each broken pledge carried. Both arrays are plain heap freed at
     * shutdown, not instance blocks, so the partial surface stays readable
     * after a break; the payload structs point into the instance heap, so an
     * explicit break zeroes them when it frees that heap, while an automatic
     * break leaves both alone on purpose, the errors are what it reports. */
    uint8_t*               pledge_state; /* PLEDGE_*, one per pledge */
    GeasValue*              pledge_err;   /* first Err payload per pledge */

    /* One TXN_* slot per descriptor subcontract, NULL for a contract with no
     * subs or no store. A transactional subcontract's episode is tracked here:
     * TXN_OPEN while a transaction buffers its writes, TXN_DONE once the commit
     * or rollback landed. Plain heap freed at shutdown, like the latches, and
     * touched only under this instance's lock so the connection stays single
     * threaded. */
    uint8_t*               sub_txn;

    /* The store connection, NULL for a contract that declares no schema. A
     * store-backed contract opens it at sign to the bound dsn, reconciles every
     * schema against it in the same call, holds it for the life of the
     * signature, and closes it at break before the heap is reclaimed. It lives
     * under this instance's lock like everything else, so the connection is
     * single-threaded by construction and the runtime adds no store lock. */
    GeasStore*              store;
    pthread_mutex_t        mu;        /* recursive; the instance lock */
};

/* A future is the receipt of one fulfillment. Between the fulfill and the
 * worker it is also the task: the dispatch fn, the prepared frame, and the
 * queue link ride inside it, so the pool queue allocates nothing. The struct
 * is heap memory tracked on the instance's futures list and freed at
 * shutdown, or earlier by the synchronous path once its one wait has
 * delivered; a break forfeits every unwaited future to GEAS_ERR_STATE and
 * clears its pointers into the instance heap before that heap goes away. */
struct GeasFuture {
    struct GeasFuture* next;    /* instance futures list */
    struct GeasFuture* qnext;   /* pool queue link */
    GeasContract*      c;
    GeasPledgeFn       fn;
    uint32_t          pidx;    /* descriptor index of the pledge, for latch */
    GeasValue*         frame;   /* instance owned, one slot per parameter */
    size_t            frame_nargs;
    GeasStatus         status;
    uint32_t          done;
    uint32_t          waited;
    GeasValue          value;
    GeasRef*           refs;      /* instance owned copy of the caller's refs */
    GeasValue*         ref_slots; /* the mutable trailing slots of the frame */
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
typedef struct GeasBinding {
    const GeasPledgeDesc* pd;
    GeasPledgeFn          fn;
} GeasBinding;

struct GeasRuntime {
    const GeasContractDesc* descs[GEAS_MAX_CONTRACT_TYPES];
    size_t                 ndescs;
    void*                  modules[GEAS_MAX_MODULES];
    size_t                 nmodules;
    GeasContract*           instances[GEAS_MAX_INSTANCES];
    size_t                 ninstances;
    GeasBinding             bindings[GEAS_MAX_BINDINGS];
    size_t                 nbindings;

    /* The iname table: one entry per registered contract and one per pledge,
     * kept sorted by mangled name so lookup is a binary search and the dump
     * is byte stable. A contract entry's mangled string is runtime owned
     * heap; a pledge entry borrows its descriptor's string. frozen latches
     * the registration surface shut; sign and fulfill never read it. */
    GeasInameEntry*         inames;
    size_t                 ninames;
    size_t                 iname_cap;
    int                    frozen;
    pthread_mutex_t        lock;      /* guards the tables above */

    /* The pool: nworkers threads draining one unbounded intrusive queue of
     * futures. qmu and qcv are a leaf lock pair, never held with another. */
    pthread_t*             workers;
    uint32_t               nworkers;
    struct GeasFuture*      qhead;
    struct GeasFuture*      qtail;
    int                    qstop;
    pthread_mutex_t        qmu;
    pthread_cond_t         qcv;
};

typedef GeasStatus (*GeasRegisterFn)(GeasRuntime*);

/* The latch readers the partial surface shares with the requirements
 * evaluator further down; both run under the instance lock. */
static int pledge_is_loose(const GeasContractDesc* d, uint32_t i);
static int sub_all(const GeasContract* c, uint32_t s, uint8_t want);

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

static void future_free(struct GeasFuture* f) {
    pthread_mutex_destroy(&f->mu);
    pthread_cond_destroy(&f->cv);
    free(f);
}

/* A fresh future linked onto its instance. NULL on any allocation failure;
 * nothing is left behind in that case. */
static struct GeasFuture* future_new(GeasContract* c) {
    struct GeasFuture* f = calloc(1, sizeof(struct GeasFuture));
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
static void future_unref(struct GeasFuture* f) {
    pthread_mutex_lock(&f->mu);
    uint32_t left = --f->refcnt;
    pthread_mutex_unlock(&f->mu);
    if (left == 0) future_free(f);
}

/* Publishes an outcome exactly once. A future a break already forfeited
 * keeps its GEAS_ERR_STATE; the worker's later completion is a no-op. */
static void future_finish(struct GeasFuture* f, GeasStatus st,
                          const GeasValue* val) {
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
 * to GEAS_ERR_STATE and drops every pointer into the instance heap, because
 * that heap is about to be freed and a late wait must find nothing to touch.
 * A waited future is left alone; its one delivery already happened. The
 * caller holds the instance lock, so no thunk is mid-run on this instance
 * and any waiter mid-write-back holds f->mu and finishes before the mark. */
static void future_forfeit(struct GeasFuture* f) {
    pthread_mutex_lock(&f->mu);
    if (!f->waited) {
        f->status = GEAS_ERR_STATE;
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
static void future_release(struct GeasFuture* f) {
    GeasContract* c = f->c;
    pthread_mutex_lock(&c->mu);
    struct GeasFuture** p = &c->futures;
    while (*p && *p != f) p = &(*p)->next;
    if (*p) *p = f->next;
    pthread_mutex_unlock(&c->mu);
    future_unref(f);
}

/* ---- the pool ---- */

static void run_task(struct GeasFuture* f);

static void pool_enqueue(GeasRuntime* rt, struct GeasFuture* f) {
    pthread_mutex_lock(&rt->qmu);
    f->qnext = NULL;
    if (rt->qtail) rt->qtail->qnext = f;
    else rt->qhead = f;
    rt->qtail = f;
    pthread_cond_signal(&rt->qcv);
    pthread_mutex_unlock(&rt->qmu);
}

/* Raised on pool worker threads and read by geas_pledge_fulfill_sync: a
 * synchronous fulfillment started from inside a thunk runs inline on the
 * worker rather than riding the queue it is draining. */
static __thread int t_pool_worker;

/* Workers drain the queue until shutdown raises qstop, and even then they
 * finish what is queued before exiting, so shutdown never strands a future
 * an outstanding wait is parked on. */
static void* pool_worker(void* arg) {
    GeasRuntime* rt = (GeasRuntime*)arg;
    t_pool_worker = 1;
    for (;;) {
        pthread_mutex_lock(&rt->qmu);
        while (!rt->qhead && !rt->qstop) {
            pthread_cond_wait(&rt->qcv, &rt->qmu);
        }
        struct GeasFuture* f = rt->qhead;
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
static int iname_find(const GeasRuntime* rt, const char* mangled, size_t* pos) {
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
 * table is GEAS_ERR_NAME; growth failure is GEAS_ERR_OOM. Under rt->lock. */
static GeasStatus iname_insert(GeasRuntime* rt, const GeasInameEntry* e) {
    size_t pos;
    if (iname_find(rt, e->mangled, &pos)) return GEAS_ERR_NAME;
    if (rt->ninames == rt->iname_cap) {
        size_t cap = rt->iname_cap ? rt->iname_cap * 2 : 16;
        GeasInameEntry* grown = realloc(rt->inames, cap * sizeof(*grown));
        if (!grown) return GEAS_ERR_OOM;
        rt->inames = grown;
        rt->iname_cap = cap;
    }
    memmove(rt->inames + pos + 1, rt->inames + pos,
            (rt->ninames - pos) * sizeof(GeasInameEntry));
    rt->inames[pos] = *e;
    rt->ninames++;
    return GEAS_OK;
}

/* The contract level mangled name the runtime synthesizes, since the
 * compiler mangles pledges only: __geas_ash_{contract}__{shapehash16}_v{ver},
 * the pledge format with an empty symbol slot and the shape hash where a
 * pledge carries its signature hash. Heap the runtime owns until shutdown. */
static char* iname_contract_mangled(const GeasContractDesc* desc) {
    int n = snprintf(NULL, 0, "__geas_ash_%s__%016llx_v%u", desc->name,
                     (unsigned long long)desc->shape_hash, desc->version);
    if (n < 0) return NULL;
    char* s = malloc((size_t)n + 1);
    if (!s) return NULL;
    snprintf(s, (size_t)n + 1, "__geas_ash_%s__%016llx_v%u", desc->name,
             (unsigned long long)desc->shape_hash, desc->version);
    return s;
}

/* Removes every entry one contract contributed, the rollback when a later
 * insert of the same registration fails. Under rt->lock. */
static void iname_remove_contract(GeasRuntime* rt, const char* contract) {
    size_t w = 0;
    for (size_t r = 0; r < rt->ninames; r++) {
        GeasInameEntry* e = &rt->inames[r];
        if (strcmp(e->contract, contract) == 0) {
            if (e->kind == GEAS_INAME_CONTRACT) free((char*)e->mangled);
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
static GeasStatus iname_register(GeasRuntime* rt, const GeasContractDesc* desc) {
    char* cm = iname_contract_mangled(desc);
    if (!cm) return GEAS_ERR_OOM;
    GeasInameEntry ce;
    memset(&ce, 0, sizeof(ce));
    ce.mangled = cm;
    ce.kind = GEAS_INAME_CONTRACT;
    ce.contract = desc->name;
    ce.symbol = NULL;
    ce.shape_hash = desc->shape_hash;
    ce.version = desc->version;
    GeasStatus st = iname_insert(rt, &ce);
    if (st != GEAS_OK) {
        free(cm);
        return st;
    }
    for (uint32_t i = 0; i < desc->npledges; i++) {
        const GeasPledgeDesc* pd = &desc->pledges[i];
        if (!pd->mangled) continue;
        GeasInameEntry pe;
        memset(&pe, 0, sizeof(pe));
        pe.mangled = pd->mangled;
        pe.kind = GEAS_INAME_PLEDGE;
        pe.contract = desc->name;
        pe.symbol = pd->name;
        pe.shape_hash = desc->shape_hash;
        pe.version = desc->version;
        pe.nargs = pd->nargs;
        st = iname_insert(rt, &pe);
        if (st != GEAS_OK) {
            iname_remove_contract(rt, desc->name);
            return st;
        }
    }
    return GEAS_OK;
}

/* ---- runtime lifecycle ---- */

GeasStatus geas_runtime_init(const GeasRuntimeConfig* cfg, GeasRuntime** out) {
    if (!out) return GEAS_ERR_TYPE;
    uint32_t nworkers = GEAS_POOL_DEFAULT_THREADS;
    if (cfg && cfg->max_threads != 0) {
        if (cfg->max_threads > GEAS_POOL_MAX_THREADS) return GEAS_ERR_TYPE;
        nworkers = cfg->max_threads;
    }
    GeasRuntime* rt = calloc(1, sizeof(GeasRuntime));
    if (!rt) return GEAS_ERR_OOM;
    if (pthread_mutex_init(&rt->lock, NULL) != 0) {
        free(rt);
        return GEAS_ERR_OOM;
    }
    if (pthread_mutex_init(&rt->qmu, NULL) != 0) {
        pthread_mutex_destroy(&rt->lock);
        free(rt);
        return GEAS_ERR_OOM;
    }
    if (pthread_cond_init(&rt->qcv, NULL) != 0) {
        pthread_mutex_destroy(&rt->qmu);
        pthread_mutex_destroy(&rt->lock);
        free(rt);
        return GEAS_ERR_OOM;
    }
    rt->workers = calloc(nworkers, sizeof(pthread_t));
    if (!rt->workers) {
        pthread_cond_destroy(&rt->qcv);
        pthread_mutex_destroy(&rt->qmu);
        pthread_mutex_destroy(&rt->lock);
        free(rt);
        return GEAS_ERR_OOM;
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
            return GEAS_ERR_OOM;
        }
    }
    rt->nworkers = nworkers;
    *out = rt;
    return GEAS_OK;
}

static void contract_free_owned(GeasContract* c) {
    GeasBlock* b = c->owned;
    while (b) {
        GeasBlock* next = b->next;
        free(b);
        b = next;
    }
    c->owned = NULL;
    c->vow_vals = NULL;
    c->fns = NULL;
}

void geas_runtime_shutdown(GeasRuntime* rt) {
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
    for (size_t i = 0; i < rt->ninstances; i++) {
        GeasContract* c = rt->instances[i];
        struct GeasFuture* f = c->futures;
        while (f) {
            struct GeasFuture* next = f->next;
            future_free(f);
            f = next;
        }
        if (c->store) {
            geas_store_close(c->store);
            c->store = NULL;
        }
        contract_free_owned(c);
        free(c->pledge_state);
        free(c->pledge_err);
        free(c->sub_txn);
        pthread_mutex_destroy(&c->mu);
        free(c);
    }
    for (size_t i = 0; i < rt->ninames; i++) {
        /* A contract entry's mangled name is its own heap; a pledge entry
         * borrows its descriptor's string, so only the contract entries free. */
        if (rt->inames[i].kind == GEAS_INAME_CONTRACT) {
            free((char*)rt->inames[i].mangled);
        }
    }
    free(rt->inames);
    for (size_t i = 0; i < rt->nmodules; i++) {
        dlclose(rt->modules[i]);
    }
    pthread_cond_destroy(&rt->qcv);
    pthread_mutex_destroy(&rt->qmu);
    pthread_mutex_destroy(&rt->lock);
    free(rt);
}

GeasStatus geas_module_load(GeasRuntime* rt, const char* so_path) {
    if (!rt || !so_path) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    int frozen = rt->frozen;
    pthread_mutex_unlock(&rt->lock);
    if (frozen) return GEAS_ERR_STATE;
    void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return GEAS_ERR_LOAD;
    GeasRegisterFn reg = (GeasRegisterFn)dlsym(handle, "geas_module_register");
    if (!reg) {
        dlclose(handle);
        return GEAS_ERR_LOAD;
    }
    GeasStatus st = reg(rt);
    if (st != GEAS_OK) {
        dlclose(handle);
        return st;
    }
    pthread_mutex_lock(&rt->lock);
    if (rt->nmodules == GEAS_MAX_MODULES) {
        pthread_mutex_unlock(&rt->lock);
        dlclose(handle);
        return GEAS_ERR_OOM;
    }
    rt->modules[rt->nmodules++] = handle;
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

GeasStatus geas_register_contract(GeasRuntime* rt, const GeasContractDesc* desc) {
    if (!rt || !desc || !desc->name) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    if (rt->frozen) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_STATE;
    }
    if (rt->ndescs == GEAS_MAX_CONTRACT_TYPES) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_OOM;
    }
    for (size_t i = 0; i < rt->ndescs; i++) {
        if (strcmp(rt->descs[i]->name, desc->name) == 0) {
            pthread_mutex_unlock(&rt->lock);
            return GEAS_ERR_NAME;
        }
    }
    /* The iname entries go in before the descriptor commits, so a mangled
     * name collision or an allocation failure leaves the runtime exactly as
     * it was and the registration reports the failure whole. */
    GeasStatus st = iname_register(rt, desc);
    if (st != GEAS_OK) {
        pthread_mutex_unlock(&rt->lock);
        return st;
    }
    rt->descs[rt->ndescs++] = desc;
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

/* ---- the iname surface ---- */

GeasStatus geas_runtime_freeze(GeasRuntime* rt) {
    if (!rt) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    rt->frozen = 1;
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

GeasStatus geas_iname_lookup(GeasRuntime* rt, const char* mangled,
                           GeasInameEntry* out) {
    if (!rt || !mangled || !out) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    size_t pos;
    int hit = iname_find(rt, mangled, &pos);
    if (hit) *out = rt->inames[pos];
    pthread_mutex_unlock(&rt->lock);
    return hit ? GEAS_OK : GEAS_ERR_NAME;
}

size_t geas_iname_count(GeasRuntime* rt) {
    if (!rt) return 0;
    pthread_mutex_lock(&rt->lock);
    size_t n = rt->ninames;
    pthread_mutex_unlock(&rt->lock);
    return n;
}

GeasStatus geas_iname_at(GeasRuntime* rt, size_t i, GeasInameEntry* out) {
    if (!rt || !out) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    if (i >= rt->ninames) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_NAME;
    }
    *out = rt->inames[i];
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

/* One canonical line per entry. The format is pinned by docs/abi.md; a
 * change here is a wire change. */
static int iname_line(const GeasInameEntry* e, char* buf, size_t cap) {
    return snprintf(buf, cap, "%s %s %016llx v%u\n", e->mangled,
                    e->kind == GEAS_INAME_CONTRACT ? "contract" : "pledge",
                    (unsigned long long)e->shape_hash, e->version);
}

GeasStatus geas_iname_dump(GeasRuntime* rt, char* buf, size_t cap, size_t* need) {
    if (!rt || !need) return GEAS_ERR_TYPE;
    if (!buf) cap = 0;
    pthread_mutex_lock(&rt->lock);
    size_t total = 1; /* the terminating NUL */
    for (size_t i = 0; i < rt->ninames; i++) {
        int n = iname_line(&rt->inames[i], NULL, 0);
        if (n < 0) {
            pthread_mutex_unlock(&rt->lock);
            return GEAS_ERR_OOM;
        }
        total += (size_t)n;
    }
    *need = total;
    if (cap < total) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_OOM;
    }
    size_t off = 0;
    for (size_t i = 0; i < rt->ninames; i++) {
        int n = iname_line(&rt->inames[i], buf + off, cap - off);
        if (n < 0 || (size_t)n >= cap - off) {
            pthread_mutex_unlock(&rt->lock);
            return GEAS_ERR_OOM;
        }
        off += (size_t)n;
    }
    buf[off] = '\0';
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

/* ---- contracts ---- */

static const GeasContractDesc* find_desc(const GeasRuntime* rt, const char* name) {
    for (size_t i = 0; i < rt->ndescs; i++) {
        if (strcmp(rt->descs[i]->name, name) == 0) return rt->descs[i];
    }
    return NULL;
}

static const GeasVowDesc* find_vow_desc(const GeasContractDesc* desc,
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
static GeasStatus copy_vow_value(GeasContract* c, const GeasValue* src,
                                GeasValue* dst) {
    if (src->ty == GEAS_TY_INSTANCE) return GEAS_ERR_TYPE;
    return geas_value_deep_copy(c, src, dst);
}

/* Fills the instance's vow storage: defaults first, then the overrides, each
 * override checked by name and by type, and every vow accounted for. */
static GeasStatus bind_vows(GeasContract* c, const GeasVowBinding* vows,
                           size_t nvows) {
    const GeasContractDesc* desc = c->desc;
    if (desc->nvows == 0) {
        return (nvows == 0) ? GEAS_OK : GEAS_ERR_NAME;
    }
    c->vow_vals = (GeasValue*)geas_bytes(c, desc->nvows * sizeof(GeasValue));
    if (!c->vow_vals) return GEAS_ERR_OOM;
    memset(c->vow_vals, 0, desc->nvows * sizeof(GeasValue));

    uint8_t bound[GEAS_MAX_CONTRACT_TYPES] = {0};
    if (desc->nvows > GEAS_MAX_CONTRACT_TYPES) return GEAS_ERR_OOM;

    for (size_t i = 0; i < nvows; i++) {
        if (!vows[i].name) return GEAS_ERR_NAME;
        const GeasVowDesc* vd = find_vow_desc(desc, vows[i].name);
        if (!vd) return GEAS_ERR_NAME;
        if (vows[i].value.ty != vd->ty) return GEAS_ERR_TYPE;
        size_t slot = (size_t)(vd - desc->vows);
        GeasStatus st = copy_vow_value(c, &vows[i].value, &c->vow_vals[slot]);
        if (st != GEAS_OK) return st;
        bound[slot] = 1;
    }
    for (uint32_t j = 0; j < desc->nvows; j++) {
        if (bound[j]) continue;
        if (!desc->vows[j].has_default) return GEAS_ERR_UNBOUND;
        GeasStatus st = copy_vow_value(c, &desc->vows[j].default_value,
                                      &c->vow_vals[j]);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* The host binding over a pledge descriptor, or NULL when nothing bound.
 * Called under the runtime lock. */
static GeasPledgeFn find_binding(const GeasRuntime* rt, const GeasPledgeDesc* pd) {
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
static GeasStatus resolve_dispatch(const GeasRuntime* rt,
                                  const GeasContractDesc* desc,
                                  GeasPledgeFn** fns_out) {
    *fns_out = NULL;
    if (desc->npledges == 0) return GEAS_OK;
    GeasPledgeFn* fns = calloc(desc->npledges, sizeof(GeasPledgeFn));
    if (!fns) return GEAS_ERR_OOM;
    for (uint32_t i = 0; i < desc->npledges; i++) {
        GeasPledgeFn fn = find_binding(rt, &desc->pledges[i]);
        if (!fn) fn = desc->pledges[i].fn;
        if (!fn) {
            free(fns);
            return GEAS_ERR_UNBOUND;
        }
        fns[i] = fn;
    }
    *fns_out = fns;
    return GEAS_OK;
}

/* ---- the store path ---- */

/* Rows and their bytes allocate onto the signing instance through this hook, so
 * the driver stays as pure over an allocator as the wire codec did: S0 handed
 * it a plain arena, S1 hands it the instance, and a row read out of a table
 * lives on the instance heap and dies at its break like every other value. */
static uint8_t* instance_store_bytes(void* ctx, uint64_t n) {
    return geas_bytes((GeasContract*)ctx, n);
}

typedef struct StoreScratchBlock {
    struct StoreScratchBlock* next;
    union {
        long double ld;
        void* ptr;
        int64_t i;
    } align;
} StoreScratchBlock;

typedef struct StoreScratchAlloc {
    StoreScratchBlock* blocks;
} StoreScratchAlloc;

static uint8_t* scratch_store_bytes(void* ctx, uint64_t n) {
    if (n > SIZE_MAX - sizeof(StoreScratchBlock)) return NULL;
    StoreScratchAlloc* scratch = (StoreScratchAlloc*)ctx;
    StoreScratchBlock* block = (StoreScratchBlock*)malloc(sizeof(StoreScratchBlock) + (size_t)n);
    if (!block) return NULL;
    block->next = scratch->blocks;
    scratch->blocks = block;
    return (uint8_t*)(block + 1);
}

static void scratch_store_free(StoreScratchAlloc* scratch) {
    StoreScratchBlock* block = scratch->blocks;
    while (block) {
        StoreScratchBlock* next = block->next;
        free(block);
        block = next;
    }
    scratch->blocks = NULL;
}

/* Opens the store connection at sign and reconciles every schema against the
 * live database in the same call, the whole store side of sign. A store-backed
 * contract, one with at least one schema, binds a database named by its dsn
 * vow: an absent dsn vow, the same gap an unsupplied vow always hit, is
 * GEAS_ERR_UNBOUND; a dsn that will not open is GEAS_ERR_STORE; a schema that
 * will not reconcile is GEAS_ERR_TYPE, and none of them leaves a half open
 * connection on the instance. A contract with no schema takes this path as a no
 * op and signs with no database, exactly as every contract did before the
 * store layer. */
static GeasStatus store_sign_reconcile(GeasContract* c) {
    const GeasContractDesc* desc = c->desc;
    if (desc->nschemas == 0) return GEAS_OK;

    int slot = -1;
    for (uint32_t i = 0; i < desc->nvows; i++) {
        if (strcmp(desc->vows[i].name, "dsn") == 0) { slot = (int)i; break; }
    }
    if (slot < 0 || !c->vow_vals) return GEAS_ERR_UNBOUND;
    const GeasValue* d = &c->vow_vals[slot];
    if (d->ty != GEAS_TY_STRING) return GEAS_ERR_UNBOUND;

    char* dsn = (char*)malloc((size_t)d->as.s.len + 1);
    if (!dsn) return GEAS_ERR_OOM;
    if (d->as.s.len) memcpy(dsn, d->as.s.ptr, (size_t)d->as.s.len);
    dsn[d->as.s.len] = 0;

    GeasStore* s = NULL;
    GeasStatus st = geas_store_open(dsn, &s);
    free(dsn);
    if (st != GEAS_OK) return st;

    for (uint32_t i = 0; i < desc->nschemas; i++) {
        st = geas_store_reconcile(s, &desc->schemas[i]);
        if (st != GEAS_OK) {
            geas_store_close(s);
            return st;
        }
    }
    c->store = s;
    return GEAS_OK;
}

/* The column tags of a schema in one heap array, the col_types the query API
 * decodes each row against. */
static uint32_t* store_col_types(const GeasSchemaDesc* s) {
    uint32_t* ct = (uint32_t*)malloc((size_t)s->ncols * sizeof(uint32_t));
    if (!ct) return NULL;
    for (uint32_t i = 0; i < s->ncols; i++) ct[i] = s->cols[i].ty;
    return ct;
}

/* The byte budget a column name list needs, the shared sizing every builder
 * leans on so no fixed cap can clip a wide schema. */
static size_t store_cols_span(const GeasSchemaDesc* s) {
    size_t n = 0;
    for (uint32_t i = 0; i < s->ncols; i++) n += strlen(s->cols[i].name) + 4;
    return n;
}

/* SELECT c0, c1, ... FROM table WHERE pk=?, the one row lookup by primary key. */
static char* store_sql_select(const GeasSchemaDesc* s) {
    size_t need = 48 + strlen(s->table) + strlen(s->cols[0].name) + store_cols_span(s);
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT ");
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s", i ? ", " : "", s->cols[i].name);
    snprintf(q + n, need - n, " FROM %s WHERE %s=?", s->table, s->cols[0].name);
    return q;
}

/* SELECT c0, c1, ... FROM table WHERE col=?1, the equality lookup by column. */
static char* store_sql_select_eq(const GeasSchemaDesc* s, uint32_t col) {
    size_t need = 48 + strlen(s->table) + strlen(s->cols[col].name) + store_cols_span(s);
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT ");
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s", i ? ", " : "", s->cols[i].name);
    snprintf(q + n, need - n, " FROM %s WHERE %s=?1", s->table, s->cols[col].name);
    return q;
}

/* SELECT c0, c1, ... FROM table WHERE c0 OP ?1 AND ..., every term bound. */
static char* store_sql_select_where(const GeasSchemaDesc* s,
                                    const GeasStoreTerm* terms,
                                    uint32_t nterms) {
    static const char* ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    size_t need = 48 + strlen(s->table) + store_cols_span(s);
    for (uint32_t i = 0; i < nterms; i++) {
        need += strlen(s->cols[terms[i].col].name) + strlen(ops[terms[i].cmp]) + 24;
    }
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT ");
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s", i ? ", " : "", s->cols[i].name);
    n += (size_t)snprintf(q + n, need - n, " FROM %s WHERE ", s->table);
    for (uint32_t i = 0; i < nterms; i++) {
        n += (size_t)snprintf(q + n, need - n, "%s%s %s ?%u",
                              i ? " AND " : "", s->cols[terms[i].col].name,
                              ops[terms[i].cmp], i + 1);
    }
    return q;
}

/* SELECT COUNT(*) FROM table WHERE c0 OP ?1 AND ..., every term bound. */
static char* store_sql_count_where(const GeasSchemaDesc* s,
                                   const GeasStoreTerm* terms,
                                   uint32_t nterms) {
    static const char* ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    size_t need = 48 + strlen(s->table);
    for (uint32_t i = 0; i < nterms; i++) {
        need += strlen(s->cols[terms[i].col].name) + strlen(ops[terms[i].cmp]) + 24;
    }
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT COUNT(*) FROM %s WHERE ", s->table);
    for (uint32_t i = 0; i < nterms; i++) {
        n += (size_t)snprintf(q + n, need - n, "%s%s %s ?%u",
                              i ? " AND " : "", s->cols[terms[i].col].name,
                              ops[terms[i].cmp], i + 1);
    }
    return q;
}

/* SELECT COALESCE(SUM(cN), 0) FROM table WHERE c0 OP ?1 AND ..., every term bound. */
static char* store_sql_sum_where(const GeasSchemaDesc* s,
                                 uint32_t sum_col,
                                 const GeasStoreTerm* terms,
                                 uint32_t nterms) {
    static const char* ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    const char* zero = s->cols[sum_col].ty == GEAS_TY_FLOAT ? "0.0" : "0";
    size_t need = 72 + strlen(s->table) + strlen(s->cols[sum_col].name) + strlen(zero);
    for (uint32_t i = 0; i < nterms; i++) {
        need += strlen(s->cols[terms[i].col].name) + strlen(ops[terms[i].cmp]) + 24;
    }
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT COALESCE(SUM(%s), %s) FROM %s WHERE ",
                          s->cols[sum_col].name, zero, s->table);
    for (uint32_t i = 0; i < nterms; i++) {
        n += (size_t)snprintf(q + n, need - n, "%s%s %s ?%u",
                              i ? " AND " : "", s->cols[terms[i].col].name,
                              ops[terms[i].cmp], i + 1);
    }
    return q;
}

/* SELECT c0, c1, ... FROM table WHERE c0 OP ?1 AND ... ORDER BY cN ASC. */
static char* store_sql_select_ordered(const GeasSchemaDesc* s,
                                      const GeasStoreTerm* terms,
                                      uint32_t nterms,
                                      uint32_t order_col,
                                      uint32_t order_desc) {
    static const char* ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    const char* dir = order_desc ? "DESC" : "ASC";
    size_t need = 64 + strlen(s->table) + store_cols_span(s) +
                  strlen(s->cols[order_col].name) + strlen(dir);
    for (uint32_t i = 0; i < nterms; i++) {
        need += strlen(s->cols[terms[i].col].name) + strlen(ops[terms[i].cmp]) + 24;
    }
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT ");
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s", i ? ", " : "", s->cols[i].name);
    n += (size_t)snprintf(q + n, need - n, " FROM %s WHERE ", s->table);
    for (uint32_t i = 0; i < nterms; i++) {
        n += (size_t)snprintf(q + n, need - n, "%s%s %s ?%u",
                              i ? " AND " : "", s->cols[terms[i].col].name,
                              ops[terms[i].cmp], i + 1);
    }
    snprintf(q + n, need - n, " ORDER BY %s %s", s->cols[order_col].name, dir);
    return q;
}

/* SELECT c0, c1, ... FROM table WHERE c0 OP ?1 AND ... ORDER BY cN ASC LIMIT ?M. */
static char* store_sql_select_page(const GeasSchemaDesc* s,
                                   const GeasStoreTerm* terms,
                                   uint32_t nterms,
                                   uint32_t order_col,
                                   uint32_t order_desc) {
    static const char* ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    const char* dir = order_desc ? "DESC" : "ASC";
    size_t need = 80 + strlen(s->table) + store_cols_span(s) +
                  strlen(s->cols[order_col].name) + strlen(dir);
    for (uint32_t i = 0; i < nterms; i++) {
        need += strlen(s->cols[terms[i].col].name) + strlen(ops[terms[i].cmp]) + 24;
    }
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT ");
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s", i ? ", " : "", s->cols[i].name);
    n += (size_t)snprintf(q + n, need - n, " FROM %s WHERE ", s->table);
    for (uint32_t i = 0; i < nterms; i++) {
        n += (size_t)snprintf(q + n, need - n, "%s%s %s ?%u",
                              i ? " AND " : "", s->cols[terms[i].col].name,
                              ops[terms[i].cmp], i + 1);
    }
    snprintf(q + n, need - n, " ORDER BY %s %s LIMIT ?%u",
             s->cols[order_col].name, dir, nterms + 1);
    return q;
}

static size_t store_sql_any_where_span(const GeasSchemaDesc* s,
                                       const GeasStoreTerm* terms,
                                       uint32_t ngroups,
                                       uint32_t total_terms) {
    static const char* ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    size_t need = (size_t)ngroups * 8;
    for (uint32_t i = 0; i < total_terms; i++) {
        need += strlen(s->cols[terms[i].col].name) + strlen(ops[terms[i].cmp]) + 24;
    }
    return need;
}

static size_t store_sql_emit_any_where(char* q, size_t need, size_t n,
                                       const GeasSchemaDesc* s,
                                       const GeasStoreTerm* terms,
                                       const uint32_t* group_lens,
                                       uint32_t ngroups) {
    static const char* ops[] = { "=", "<>", "<", "<=", ">", ">=" };
    uint32_t term_i = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        n += (size_t)snprintf(q + n, need - n, "%s(", g ? " OR " : "");
        for (uint32_t i = 0; i < group_lens[g]; i++) {
            const GeasStoreTerm* term = &terms[term_i];
            n += (size_t)snprintf(q + n, need - n, "%s%s %s ?%u",
                                  i ? " AND " : "", s->cols[term->col].name,
                                  ops[term->cmp], term_i + 1);
            term_i++;
        }
        n += (size_t)snprintf(q + n, need - n, ")");
    }
    return n;
}

/* SELECT c0, c1, ... FROM table WHERE (g0) OR (g1), optionally ordered and bounded. */
static char* store_sql_select_any(const GeasSchemaDesc* s,
                                  const GeasStoreTerm* terms,
                                  const uint32_t* group_lens,
                                  uint32_t ngroups,
                                  uint32_t total_terms,
                                  uint32_t order_col,
                                  uint32_t order_desc,
                                  uint32_t bounded) {
    const uint32_t ordered = order_col != GEAS_STORE_NO_ORDER;
    const char* dir = order_desc ? "DESC" : "ASC";
    size_t need = 80 + strlen(s->table) + store_cols_span(s) +
                  store_sql_any_where_span(s, terms, ngroups, total_terms);
    if (ordered) need += strlen(s->cols[order_col].name) + strlen(dir) + 16;
    if (bounded) need += 24;
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT ");
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s", i ? ", " : "", s->cols[i].name);
    n += (size_t)snprintf(q + n, need - n, " FROM %s WHERE ", s->table);
    n = store_sql_emit_any_where(q, need, n, s, terms, group_lens, ngroups);
    if (ordered) {
        n += (size_t)snprintf(q + n, need - n, " ORDER BY %s %s",
                              s->cols[order_col].name, dir);
    }
    if (bounded) snprintf(q + n, need - n, " LIMIT ?%u", total_terms + 1);
    return q;
}

/* SELECT COUNT(*) FROM table WHERE (g0) OR (g1), every term bound. */
static char* store_sql_count_any(const GeasSchemaDesc* s,
                                 const GeasStoreTerm* terms,
                                 const uint32_t* group_lens,
                                 uint32_t ngroups,
                                 uint32_t total_terms) {
    size_t need = 48 + strlen(s->table) +
                  store_sql_any_where_span(s, terms, ngroups, total_terms);
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT COUNT(*) FROM %s WHERE ", s->table);
    store_sql_emit_any_where(q, need, n, s, terms, group_lens, ngroups);
    return q;
}

/* SELECT COALESCE(SUM(cN), 0) FROM table WHERE (g0) OR (g1), every term bound. */
static char* store_sql_sum_any(const GeasSchemaDesc* s,
                               uint32_t sum_col,
                               const GeasStoreTerm* terms,
                               const uint32_t* group_lens,
                               uint32_t ngroups,
                               uint32_t total_terms) {
    const char* zero = s->cols[sum_col].ty == GEAS_TY_FLOAT ? "0.0" : "0";
    size_t need = 72 + strlen(s->table) + strlen(s->cols[sum_col].name) + strlen(zero) +
                  store_sql_any_where_span(s, terms, ngroups, total_terms);
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT COALESCE(SUM(%s), %s) FROM %s WHERE ",
                          s->cols[sum_col].name, zero, s->table);
    store_sql_emit_any_where(q, need, n, s, terms, group_lens, ngroups);
    return q;
}

/* SELECT cN FROM table WHERE (g0) OR (g1) ORDER BY cN ASC LIMIT 1. */
static char* store_sql_extreme_any(const GeasSchemaDesc* s,
                                   uint32_t agg_col,
                                   uint32_t desc,
                                   const GeasStoreTerm* terms,
                                   const uint32_t* group_lens,
                                   uint32_t ngroups,
                                   uint32_t total_terms) {
    const char* dir = desc ? "DESC" : "ASC";
    size_t need = 80 + strlen(s->table) + strlen(s->cols[agg_col].name) * 2 +
                  strlen(dir) + store_sql_any_where_span(s, terms, ngroups,
                                                         total_terms);
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "SELECT %s FROM %s WHERE ",
                          s->cols[agg_col].name, s->table);
    n = store_sql_emit_any_where(q, need, n, s, terms, group_lens, ngroups);
    snprintf(q + n, need - n, " ORDER BY %s %s LIMIT 1",
             s->cols[agg_col].name, dir);
    return q;
}

/* SELECT COUNT(*), COALESCE(SUM(cN), 0) FROM table WHERE (g0) OR (g1). */
static char* store_sql_avg_any(const GeasSchemaDesc* s,
                               uint32_t agg_col,
                               const GeasStoreTerm* terms,
                               const uint32_t* group_lens,
                               uint32_t ngroups,
                               uint32_t total_terms) {
    const char* zero = s->cols[agg_col].ty == GEAS_TY_FLOAT ? "0.0" : "0";
    size_t need = 96 + strlen(s->table) + strlen(s->cols[agg_col].name) + strlen(zero) +
                  store_sql_any_where_span(s, terms, ngroups, total_terms);
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n,
                          "SELECT COUNT(*), COALESCE(SUM(%s), %s) FROM %s WHERE ",
                          s->cols[agg_col].name, zero, s->table);
    store_sql_emit_any_where(q, need, n, s, terms, group_lens, ngroups);
    return q;
}

/* INSERT INTO table(c0, ...) VALUES(?, ...), every column bound positionally. */
static char* store_sql_insert(const GeasSchemaDesc* s) {
    size_t need = 48 + strlen(s->table) + store_cols_span(s) + (size_t)s->ncols * 3;
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "INSERT INTO %s(", s->table);
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s", i ? ", " : "", s->cols[i].name);
    n += (size_t)snprintf(q + n, need - n, ") VALUES(");
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s?", i ? ", " : "");
    snprintf(q + n, need - n, ")");
    return q;
}

/* UPDATE table SET c0=?, c1=?, ... WHERE pk=?, the row's columns then the key. */
static char* store_sql_update(const GeasSchemaDesc* s) {
    size_t need = 48 + strlen(s->table) + strlen(s->cols[0].name) + store_cols_span(s) * 2;
    char* q = (char*)malloc(need);
    if (!q) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(q + n, need - n, "UPDATE %s SET ", s->table);
    for (uint32_t i = 0; i < s->ncols; i++)
        n += (size_t)snprintf(q + n, need - n, "%s%s=?", i ? ", " : "", s->cols[i].name);
    snprintf(q + n, need - n, " WHERE %s=?", s->cols[0].name);
    return q;
}

/* Wraps a payload value into Ok(payload), the Result the store surface returns
 * on success; the Err arm is the surface's and a backend failure never reaches
 * it, riding the status instead. */
static GeasStatus store_ok(GeasContract* c, const GeasValue* payload, GeasValue* out) {
    GeasValue* box = geas_box(c);
    if (!box) return GEAS_ERR_OOM;
    *box = *payload;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return GEAS_OK;
}

/* Ok(Unit), the result the write forms report. */
static GeasStatus store_ok_unit(GeasContract* c, GeasValue* out) {
    GeasValue unit;
    memset(&unit, 0, sizeof(unit));
    unit.ty = GEAS_TY_UNIT;
    return store_ok(c, &unit, out);
}

/* The row parameters an insert or update binds: a record's fields are the
 * schema's columns in declaration order, so its field array is the parameter
 * frame as it stands. A value that is not a record of the right width is a
 * codegen bug, refused here as GEAS_ERR_TYPE rather than bound wrong. */
static const GeasValue* store_row_params(const GeasSchemaDesc* s,
                                        const GeasValue* row) {
    if (!row || row->ty != GEAS_TY_RECORD) return NULL;
    if (row->as.list.len != s->ncols) return NULL;
    return (const GeasValue*)row->as.list.data;
}

GeasStatus geas_store_find(GeasContract* c, const GeasSchemaDesc* schema,
                         const GeasValue* key, GeasValue* out) {
    if (!c || !schema || !key || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store && schema->ncols > 0) {
        char* sql = store_sql_select(schema);
        uint32_t* cts = store_col_types(schema);
        if (!sql || !cts) {
            st = GEAS_ERR_OOM;
        } else {
            GeasStoreAlloc alloc = { instance_store_bytes, c };
            GeasValue rows;
            st = geas_store_query(c->store, sql, key, 1, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == GEAS_OK) {
                GeasValue opt;
                memset(&opt, 0, sizeof(opt));
                opt.ty = GEAS_TY_OPTION;
                if (rows.as.list.len > 0) {
                    GeasValue* box = geas_box(c);
                    if (!box) {
                        st = GEAS_ERR_OOM;
                    } else {
                        *box = ((GeasValue*)rows.as.list.data)[0];
                        opt.tag = 1;
                        opt.as.box = box;
                    }
                }
                if (st == GEAS_OK) st = store_ok(c, &opt, out);
            }
        }
        free(sql);
        free(cts);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_query_eq(GeasContract* c, const GeasSchemaDesc* schema,
                             uint32_t col, const GeasValue* value,
                             GeasValue* out) {
    if (!c || !schema || !value || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || col >= schema->ncols) return GEAS_ERR_TYPE;
    if (value->ty != schema->cols[col].ty) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_select_eq(schema, col);
        uint32_t* cts = store_col_types(schema);
        if (!sql || !cts) {
            st = GEAS_ERR_OOM;
        } else {
            GeasStoreAlloc alloc = { instance_store_bytes, c };
            GeasValue rows;
            st = geas_store_query(c->store, sql, value, 1, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == GEAS_OK) st = store_ok(c, &rows, out);
        }
        free(sql);
        free(cts);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_query_where(GeasContract* c, const GeasSchemaDesc* schema,
                                const GeasStoreTerm* terms, uint32_t nterms,
                                GeasValue* out) {
    if (!c || !schema || !terms || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || nterms == 0) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < nterms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_select_where(schema, terms, nterms);
        uint32_t* cts = store_col_types(schema);
        GeasValue* params = (GeasValue*)malloc((size_t)nterms * sizeof(GeasValue));
        if (!sql || !cts || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < nterms; i++) params[i] = *terms[i].value;
            GeasStoreAlloc alloc = { instance_store_bytes, c };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, nterms, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == GEAS_OK) st = store_ok(c, &rows, out);
        }
        free(sql);
        free(cts);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_query_ordered(GeasContract* c, const GeasSchemaDesc* schema,
                                  const GeasStoreTerm* terms, uint32_t nterms,
                                  uint32_t order_col, uint32_t order_desc,
                                  GeasValue* out) {
    if (!c || !schema || !terms || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || nterms == 0) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < nterms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    if (order_col >= schema->ncols) return GEAS_ERR_TYPE;
    if (order_desc > 1) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_select_ordered(schema, terms, nterms, order_col, order_desc);
        uint32_t* cts = store_col_types(schema);
        GeasValue* params = (GeasValue*)malloc((size_t)nterms * sizeof(GeasValue));
        if (!sql || !cts || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < nterms; i++) params[i] = *terms[i].value;
            GeasStoreAlloc alloc = { instance_store_bytes, c };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, nterms, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == GEAS_OK) st = store_ok(c, &rows, out);
        }
        free(sql);
        free(cts);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_query_page(GeasContract* c, const GeasSchemaDesc* schema,
                               const GeasStoreTerm* terms, uint32_t nterms,
                               uint32_t order_col, uint32_t order_desc,
                               const GeasValue* limit, GeasValue* out) {
    if (!c || !schema || !terms || !limit || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || nterms == 0) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < nterms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    if (order_col >= schema->ncols) return GEAS_ERR_TYPE;
    if (order_desc > 1) return GEAS_ERR_TYPE;
    if (limit->ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
    if (limit->as.i < 0) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_select_page(schema, terms, nterms, order_col, order_desc);
        uint32_t* cts = store_col_types(schema);
        GeasValue* params = (GeasValue*)malloc((size_t)(nterms + 1) * sizeof(GeasValue));
        if (!sql || !cts || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < nterms; i++) params[i] = *terms[i].value;
            params[nterms] = *limit;
            GeasStoreAlloc alloc = { instance_store_bytes, c };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, nterms + 1, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == GEAS_OK) st = store_ok(c, &rows, out);
        }
        free(sql);
        free(cts);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_select(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasStoreTerm* terms,
                           const uint32_t* group_lens, uint32_t ngroups,
                           uint32_t order_col, uint32_t order_desc,
                           const GeasValue* limit, GeasValue* out) {
    if (!c || !schema || !terms || !group_lens || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || ngroups == 0) return GEAS_ERR_TYPE;
    uint32_t total_terms = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        if (group_lens[g] == 0) return GEAS_ERR_TYPE;
        if (UINT32_MAX - total_terms < group_lens[g]) return GEAS_ERR_TYPE;
        total_terms += group_lens[g];
    }
    for (uint32_t i = 0; i < total_terms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    uint32_t bounded = limit != NULL;
    if (order_col == GEAS_STORE_NO_ORDER) {
        if (bounded) return GEAS_ERR_TYPE;
    } else {
        if (order_col >= schema->ncols) return GEAS_ERR_TYPE;
        if (order_desc > 1) return GEAS_ERR_TYPE;
    }
    if (bounded) {
        if (limit->ty != GEAS_TY_INT) return GEAS_ERR_TYPE;
        if (limit->as.i < 0) return GEAS_ERR_TYPE;
        if (total_terms == UINT32_MAX) return GEAS_ERR_TYPE;
    }
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_select_any(schema, terms, group_lens, ngroups, total_terms,
                                         order_col, order_desc, bounded);
        uint32_t* cts = store_col_types(schema);
        GeasValue* params = (GeasValue*)malloc((size_t)(total_terms + bounded) * sizeof(GeasValue));
        if (!sql || !cts || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < total_terms; i++) params[i] = *terms[i].value;
            if (bounded) params[total_terms] = *limit;
            GeasStoreAlloc alloc = { instance_store_bytes, c };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, total_terms + bounded, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == GEAS_OK) st = store_ok(c, &rows, out);
        }
        free(sql);
        free(cts);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_count_any(GeasContract* c, const GeasSchemaDesc* schema,
                              const GeasStoreTerm* terms,
                              const uint32_t* group_lens, uint32_t ngroups,
                              GeasValue* out) {
    if (!c || !schema || !terms || !group_lens || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || ngroups == 0) return GEAS_ERR_TYPE;
    uint32_t total_terms = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        if (group_lens[g] == 0) return GEAS_ERR_TYPE;
        if (UINT32_MAX - total_terms < group_lens[g]) return GEAS_ERR_TYPE;
        total_terms += group_lens[g];
    }
    for (uint32_t i = 0; i < total_terms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_count_any(schema, terms, group_lens, ngroups, total_terms);
        GeasValue* params = (GeasValue*)malloc((size_t)total_terms * sizeof(GeasValue));
        if (!sql || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < total_terms; i++) params[i] = *terms[i].value;
            uint32_t count_types[1] = { GEAS_TY_INT };
            StoreScratchAlloc scratch = { NULL };
            GeasStoreAlloc alloc = { scratch_store_bytes, &scratch };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, total_terms, count_types, NULL,
                                 1, &alloc, &rows);
            if (st == GEAS_OK) {
                if (rows.ty != GEAS_TY_LIST || rows.as.list.len != 1 || !rows.as.list.data) {
                    st = GEAS_ERR_STORE;
                } else {
                    GeasValue* rec = (GeasValue*)rows.as.list.data;
                    if (rec[0].ty != GEAS_TY_RECORD || rec[0].as.list.len != 1 ||
                        !rec[0].as.list.data) {
                        st = GEAS_ERR_STORE;
                    } else {
                        GeasValue* field = (GeasValue*)rec[0].as.list.data;
                        if (field[0].ty != GEAS_TY_INT) {
                            st = GEAS_ERR_STORE;
                        } else {
                            GeasValue count;
                            memset(&count, 0, sizeof(count));
                            count.ty = GEAS_TY_INT;
                            count.as.i = field[0].as.i;
                            st = store_ok(c, &count, out);
                        }
                    }
                }
            }
            scratch_store_free(&scratch);
        }
        free(sql);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_sum_any(GeasContract* c, const GeasSchemaDesc* schema,
                            uint32_t sum_col, const GeasStoreTerm* terms,
                            const uint32_t* group_lens, uint32_t ngroups,
                            GeasValue* out) {
    if (!c || !schema || !terms || !group_lens || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || ngroups == 0) return GEAS_ERR_TYPE;
    uint32_t total_terms = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        if (group_lens[g] == 0) return GEAS_ERR_TYPE;
        if (UINT32_MAX - total_terms < group_lens[g]) return GEAS_ERR_TYPE;
        total_terms += group_lens[g];
    }
    for (uint32_t i = 0; i < total_terms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    if (sum_col >= schema->ncols) return GEAS_ERR_TYPE;
    uint32_t sum_ty = schema->cols[sum_col].ty;
    if (sum_ty != GEAS_TY_INT && sum_ty != GEAS_TY_FLOAT) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_sum_any(schema, sum_col, terms, group_lens, ngroups,
                                      total_terms);
        GeasValue* params = (GeasValue*)malloc((size_t)total_terms * sizeof(GeasValue));
        if (!sql || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < total_terms; i++) params[i] = *terms[i].value;
            uint32_t sum_types[1] = { sum_ty };
            StoreScratchAlloc scratch = { NULL };
            GeasStoreAlloc alloc = { scratch_store_bytes, &scratch };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, total_terms, sum_types, NULL,
                                 1, &alloc, &rows);
            if (st == GEAS_OK) {
                if (rows.ty != GEAS_TY_LIST || rows.as.list.len != 1 || !rows.as.list.data) {
                    st = GEAS_ERR_STORE;
                } else {
                    GeasValue* rec = (GeasValue*)rows.as.list.data;
                    if (rec[0].ty != GEAS_TY_RECORD || rec[0].as.list.len != 1 ||
                        !rec[0].as.list.data) {
                        st = GEAS_ERR_STORE;
                    } else {
                        GeasValue* field = (GeasValue*)rec[0].as.list.data;
                        if (field[0].ty != sum_ty) {
                            st = GEAS_ERR_STORE;
                        } else {
                            GeasValue sum;
                            memset(&sum, 0, sizeof(sum));
                            sum.ty = sum_ty;
                            if (sum_ty == GEAS_TY_INT) {
                                sum.as.i = field[0].as.i;
                            } else {
                                sum.as.f = field[0].as.f;
                            }
                            st = store_ok(c, &sum, out);
                        }
                    }
                }
            }
            scratch_store_free(&scratch);
        }
        free(sql);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_agg_any(GeasContract* c, const GeasSchemaDesc* schema,
                            uint32_t agg, uint32_t agg_col,
                            const GeasStoreTerm* terms,
                            const uint32_t* group_lens, uint32_t ngroups,
                            GeasValue* out) {
    if (!c || !schema || !terms || !group_lens || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || ngroups == 0) return GEAS_ERR_TYPE;
    if (agg > GEAS_AGG_AVG) return GEAS_ERR_TYPE;
    uint32_t total_terms = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        if (group_lens[g] == 0) return GEAS_ERR_TYPE;
        if (UINT32_MAX - total_terms < group_lens[g]) return GEAS_ERR_TYPE;
        total_terms += group_lens[g];
    }
    for (uint32_t i = 0; i < total_terms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    if (agg_col >= schema->ncols) return GEAS_ERR_TYPE;
    uint32_t agg_ty = schema->cols[agg_col].ty;
    if (agg == GEAS_AGG_AVG && agg_ty != GEAS_TY_INT && agg_ty != GEAS_TY_FLOAT) {
        return GEAS_ERR_TYPE;
    }
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = NULL;
        uint32_t result_types[2] = { GEAS_TY_INT, agg_ty };
        uint32_t nresult_types = 1;
        if (agg == GEAS_AGG_AVG) {
            sql = store_sql_avg_any(schema, agg_col, terms, group_lens, ngroups,
                                    total_terms);
            nresult_types = 2;
        } else {
            sql = store_sql_extreme_any(schema, agg_col, agg == GEAS_AGG_MAX,
                                        terms, group_lens, ngroups, total_terms);
            result_types[0] = agg_ty;
        }
        GeasValue* params = (GeasValue*)malloc((size_t)total_terms * sizeof(GeasValue));
        if (!sql || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < total_terms; i++) params[i] = *terms[i].value;
            StoreScratchAlloc scratch = { NULL };
            GeasStoreAlloc alloc = { scratch_store_bytes, &scratch };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, total_terms, result_types, NULL,
                                 nresult_types, &alloc, &rows);
            if (st == GEAS_OK) {
                GeasValue opt;
                memset(&opt, 0, sizeof(opt));
                opt.ty = GEAS_TY_OPTION;
                if (agg == GEAS_AGG_AVG) {
                    if (rows.ty != GEAS_TY_LIST || rows.as.list.len != 1 ||
                        !rows.as.list.data) {
                        st = GEAS_ERR_STORE;
                    } else {
                        GeasValue* rec = (GeasValue*)rows.as.list.data;
                        if (rec[0].ty != GEAS_TY_RECORD || rec[0].as.list.len != 2 ||
                            !rec[0].as.list.data) {
                            st = GEAS_ERR_STORE;
                        } else {
                            GeasValue* field = (GeasValue*)rec[0].as.list.data;
                            if (field[0].ty != GEAS_TY_INT || field[1].ty != agg_ty) {
                                st = GEAS_ERR_STORE;
                            } else if (field[0].as.i != 0) {
                                GeasValue* box = geas_box(c);
                                if (!box) {
                                    st = GEAS_ERR_OOM;
                                } else {
                                    memset(box, 0, sizeof(*box));
                                    box->ty = GEAS_TY_FLOAT;
                                    double sum = agg_ty == GEAS_TY_INT
                                                     ? (double)field[1].as.i
                                                     : field[1].as.f;
                                    box->as.f = sum / (double)field[0].as.i;
                                    opt.tag = 1;
                                    opt.as.box = box;
                                }
                            }
                        }
                    }
                } else if (rows.ty != GEAS_TY_LIST || rows.as.list.len > 1 ||
                           (rows.as.list.len && !rows.as.list.data)) {
                    st = GEAS_ERR_STORE;
                } else if (rows.as.list.len > 0) {
                    GeasValue* rec = (GeasValue*)rows.as.list.data;
                    if (rec[0].ty != GEAS_TY_RECORD || rec[0].as.list.len != 1 ||
                        !rec[0].as.list.data) {
                        st = GEAS_ERR_STORE;
                    } else {
                        GeasValue* field = (GeasValue*)rec[0].as.list.data;
                        if (field[0].ty != agg_ty) {
                            st = GEAS_ERR_STORE;
                        } else {
                            GeasValue* box = geas_box(c);
                            if (!box) {
                                st = GEAS_ERR_OOM;
                            } else {
                                st = geas_value_deep_copy(c, &field[0], box);
                                if (st == GEAS_OK) {
                                    opt.tag = 1;
                                    opt.as.box = box;
                                }
                            }
                        }
                    }
                }
                if (st == GEAS_OK) st = store_ok(c, &opt, out);
            }
            scratch_store_free(&scratch);
        }
        free(sql);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_count_where(GeasContract* c, const GeasSchemaDesc* schema,
                                const GeasStoreTerm* terms, uint32_t nterms,
                                GeasValue* out) {
    if (!c || !schema || !terms || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || nterms == 0) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < nterms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_count_where(schema, terms, nterms);
        GeasValue* params = (GeasValue*)malloc((size_t)nterms * sizeof(GeasValue));
        if (!sql || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < nterms; i++) params[i] = *terms[i].value;
            uint32_t count_types[1] = { GEAS_TY_INT };
            StoreScratchAlloc scratch = { NULL };
            GeasStoreAlloc alloc = { scratch_store_bytes, &scratch };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, nterms, count_types, NULL,
                                 1, &alloc, &rows);
            if (st == GEAS_OK) {
                if (rows.ty != GEAS_TY_LIST || rows.as.list.len != 1 || !rows.as.list.data) {
                    st = GEAS_ERR_STORE;
                } else {
                    GeasValue* rec = (GeasValue*)rows.as.list.data;
                    if (rec[0].ty != GEAS_TY_RECORD || rec[0].as.list.len != 1 ||
                        !rec[0].as.list.data) {
                        st = GEAS_ERR_STORE;
                    } else {
                        GeasValue* field = (GeasValue*)rec[0].as.list.data;
                        if (field[0].ty != GEAS_TY_INT) {
                            st = GEAS_ERR_STORE;
                        } else {
                            GeasValue count;
                            memset(&count, 0, sizeof(count));
                            count.ty = GEAS_TY_INT;
                            count.as.i = field[0].as.i;
                            st = store_ok(c, &count, out);
                        }
                    }
                }
            }
            scratch_store_free(&scratch);
        }
        free(sql);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_sum_where(GeasContract* c, const GeasSchemaDesc* schema,
                              uint32_t sum_col, const GeasStoreTerm* terms,
                              uint32_t nterms, GeasValue* out) {
    if (!c || !schema || !terms || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || nterms == 0) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < nterms; i++) {
        if (terms[i].col >= schema->ncols) return GEAS_ERR_TYPE;
        if (terms[i].cmp > GEAS_CMP_GE) return GEAS_ERR_TYPE;
        if (!terms[i].value) return GEAS_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return GEAS_ERR_TYPE;
    }
    if (sum_col >= schema->ncols) return GEAS_ERR_TYPE;
    uint32_t sum_ty = schema->cols[sum_col].ty;
    if (sum_ty != GEAS_TY_INT && sum_ty != GEAS_TY_FLOAT) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_sum_where(schema, sum_col, terms, nterms);
        GeasValue* params = (GeasValue*)malloc((size_t)nterms * sizeof(GeasValue));
        if (!sql || !params) {
            st = GEAS_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < nterms; i++) params[i] = *terms[i].value;
            uint32_t sum_types[1] = { sum_ty };
            StoreScratchAlloc scratch = { NULL };
            GeasStoreAlloc alloc = { scratch_store_bytes, &scratch };
            GeasValue rows;
            st = geas_store_query(c->store, sql, params, nterms, sum_types, NULL,
                                 1, &alloc, &rows);
            if (st == GEAS_OK) {
                if (rows.ty != GEAS_TY_LIST || rows.as.list.len != 1 || !rows.as.list.data) {
                    st = GEAS_ERR_STORE;
                } else {
                    GeasValue* rec = (GeasValue*)rows.as.list.data;
                    if (rec[0].ty != GEAS_TY_RECORD || rec[0].as.list.len != 1 ||
                        !rec[0].as.list.data) {
                        st = GEAS_ERR_STORE;
                    } else {
                        GeasValue* field = (GeasValue*)rec[0].as.list.data;
                        if (field[0].ty != sum_ty) {
                            st = GEAS_ERR_STORE;
                        } else {
                            GeasValue sum;
                            memset(&sum, 0, sizeof(sum));
                            sum.ty = sum_ty;
                            if (sum_ty == GEAS_TY_INT) {
                                sum.as.i = field[0].as.i;
                            } else {
                                sum.as.f = field[0].as.f;
                            }
                            st = store_ok(c, &sum, out);
                        }
                    }
                }
            }
            scratch_store_free(&scratch);
        }
        free(sql);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_insert(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasValue* row, GeasValue* out) {
    if (!c || !schema || !row || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    const GeasValue* params = store_row_params(schema, row);
    if (!params) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_insert(schema);
        if (!sql) {
            st = GEAS_ERR_OOM;
        } else {
            st = geas_store_exec_params(c->store, sql, params, schema->ncols, NULL);
            if (st == GEAS_OK) st = store_ok_unit(c, out);
        }
        free(sql);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_update(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasValue* key, const GeasValue* row,
                           GeasValue* out) {
    if (!c || !schema || !key || !row || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    const GeasValue* rowp = store_row_params(schema, row);
    if (!rowp) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_update(schema);
        GeasValue* params = (GeasValue*)malloc((size_t)(schema->ncols + 1) *
                                             sizeof(GeasValue));
        if (!sql || !params) {
            st = GEAS_ERR_OOM;
        } else {
            memcpy(params, rowp, (size_t)schema->ncols * sizeof(GeasValue));
            params[schema->ncols] = *key;
            st = geas_store_exec_params(c->store, sql, params,
                                       (size_t)schema->ncols + 1, NULL);
            if (st == GEAS_OK) st = store_ok_unit(c, out);
        }
        free(sql);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_store_delete(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasValue* key, GeasValue* out) {
    if (!c || !schema || !key || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_ERR_STORE;
    if (c->store && schema->ncols > 0) {
        size_t need = 48 + strlen(schema->table) + strlen(schema->cols[0].name);
        char* sql = (char*)malloc(need);
        if (!sql) {
            st = GEAS_ERR_OOM;
        } else {
            snprintf(sql, need, "DELETE FROM %s WHERE %s=?", schema->table,
                     schema->cols[0].name);
            st = geas_store_exec_params(c->store, sql, key, 1, NULL);
            if (st == GEAS_OK) st = store_ok_unit(c, out);
        }
        free(sql);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

/* Sign runs in three phases so the runtime lock never sits above an instance
 * mutex. Phase one resolves the descriptor and dispatch table under the
 * runtime lock alone; phase two builds the instance with no locks held,
 * where the allocation helpers take only the fresh instance's own mutex,
 * which nothing else can reach yet; phase three publishes it under the
 * runtime lock, where the capacity check belongs because that is where the
 * slot is taken. A pledge body signing through geas_instance_runtime already
 * holds its own instance lock, and this shape keeps that edge one way:
 * instance above runtime, never the reverse. */
GeasStatus geas_contract_sign(GeasRuntime* rt, const char* contract_name,
                            const GeasVowBinding* vows, size_t nvows,
                            uint64_t expected_hash, GeasContract** out) {
    if (!rt || !contract_name || !out) return GEAS_ERR_TYPE;
    if (nvows > 0 && !vows) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    const GeasContractDesc* desc = find_desc(rt, contract_name);
    if (!desc) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_NAME;
    }
    if (expected_hash != 0 && expected_hash != desc->shape_hash) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_VERSION;
    }
    GeasPledgeFn* fns = NULL;
    GeasStatus st = resolve_dispatch(rt, desc, &fns);
    pthread_mutex_unlock(&rt->lock);
    if (st != GEAS_OK) return st;

    GeasContract* c = calloc(1, sizeof(GeasContract));
    if (!c) {
        free(fns);
        return GEAS_ERR_OOM;
    }
    if (mutex_init_recursive(&c->mu) != 0) {
        free(c);
        free(fns);
        return GEAS_ERR_OOM;
    }
    c->rt = rt;
    c->desc = desc;
    if (desc->npledges > 0) {
        c->fns = (GeasPledgeFn*)geas_bytes(c, desc->npledges * sizeof(GeasPledgeFn));
        if (!c->fns) st = GEAS_ERR_OOM;
        else memcpy(c->fns, fns, desc->npledges * sizeof(GeasPledgeFn));
    }
    free(fns);
    if (st == GEAS_OK) st = bind_vows(c, vows, nvows);
    if (st == GEAS_OK && desc->npledges > 0) {
        c->pledge_state = calloc(desc->npledges, sizeof(uint8_t));
        c->pledge_err = calloc(desc->npledges, sizeof(GeasValue));
        if (!c->pledge_state || !c->pledge_err) st = GEAS_ERR_OOM;
    }
    /* One transaction slot per subcontract, all TXN_NONE, so the first
     * fulfillment of a transactional subcontract opens its episode lazily. A
     * contract with no subcontract carries none. */
    if (st == GEAS_OK && desc->nsubs > 0) {
        c->sub_txn = calloc(desc->nsubs, sizeof(uint8_t));
        if (!c->sub_txn) st = GEAS_ERR_OOM;
    }
    /* The store side of sign: open the bound dsn and reconcile every schema, so
     * a store-backed contract that activates has a live, shape checked
     * connection, and one that fails to open or reconcile never publishes. */
    if (st == GEAS_OK) st = store_sign_reconcile(c);
    if (st == GEAS_OK) {
        c->state = GEAS_SIGNED;
        c->shape_hash = desc->shape_hash;
        c->signed_at = (int64_t)time(NULL);
        pthread_mutex_lock(&rt->lock);
        if (rt->ninstances == GEAS_MAX_INSTANCES) {
            st = GEAS_ERR_OOM;
        } else {
            rt->instances[rt->ninstances++] = c;
        }
        pthread_mutex_unlock(&rt->lock);
    }
    if (st != GEAS_OK) {
        if (c->store) {
            geas_store_close(c->store);
            c->store = NULL;
        }
        contract_free_owned(c);
        free(c->pledge_state);
        free(c->pledge_err);
        free(c->sub_txn);
        pthread_mutex_destroy(&c->mu);
        free(c);
        return st;
    }
    *out = c;
    return GEAS_OK;
}

GeasContractState geas_contract_state(const GeasContract* c) {
    if (!c) return GEAS_UNSIGNED;
    GeasContract* mc = (GeasContract*)c;
    pthread_mutex_lock(&mc->mu);
    GeasContractState s = mc->state;
    pthread_mutex_unlock(&mc->mu);
    return s;
}

uint64_t geas_contract_hash(const GeasContract* c) {
    return c ? c->shape_hash : 0;
}

int64_t geas_contract_signed_at(const GeasContract* c) {
    return c ? c->signed_at : 0;
}

/* The backref a cross-contract sign needs: a compiled thunk reaches the
 * runtime through its own ctx, and a host bound body may do the same. The
 * field is written once under the runtime lock at sign and never moves, so
 * the read needs no lock. */
GeasRuntime* geas_instance_runtime(const GeasContract* c) {
    return c ? c->rt : NULL;
}

/* Break under the instance lock, which is the whole in-flight story: a thunk
 * mid-run on a worker holds this lock, so the break waits it out; a task
 * still queued finds the state already Broken when its worker gets the lock
 * and never touches the freed heap. Every unwaited future is forfeited to
 * GEAS_ERR_STATE before the heap goes, so a late wait delivers a clean error
 * instead of freed memory. A fulfillment racing the break resolves to one of
 * exactly two outcomes: delivered before the break, or GEAS_ERR_STATE. */
GeasStatus geas_contract_break(GeasContract* c) {
    if (!c) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    if (c->state == GEAS_UNSIGNED) {
        pthread_mutex_unlock(&c->mu);
        return GEAS_ERR_STATE;
    }
    for (struct GeasFuture* f = c->futures; f; f = f->next) {
        future_forfeit(f);
    }
    /* The stored Err payloads point into the heap about to be freed, so an
     * explicit break zeroes them; the latches themselves survive so the
     * partial surface still reports which pledges landed and which broke. */
    if (c->pledge_err && c->desc->npledges > 0) {
        memset(c->pledge_err, 0, c->desc->npledges * sizeof(GeasValue));
    }
    /* The store side of break, in the order docs/database.md fixes: roll back
     * any open transaction, then close the connection, then reclaim the heap.
     * S1 is autocommit so a rollback usually finds nothing open, but it runs
     * anyway so no uncommitted write can ever survive a break, and the close
     * lands before the heap the rows live on goes away. */
    if (c->store) {
        geas_store_rollback(c->store);
        geas_store_close(c->store);
        c->store = NULL;
    }
    contract_free_owned(c);
    c->state = GEAS_BROKEN;
    pthread_mutex_unlock(&c->mu);
    return GEAS_OK;
}

/* ---- the partial result ---- */

/* A named subcontract item's state: fulfilled when every pledge inside it
 * latched Ok, broken when every pledge inside it latched Err, pending
 * otherwise. Called under the instance lock. */
static GeasItemState sub_item_state(const GeasContract* c, uint32_t s) {
    if (sub_all(c, s, PLEDGE_FULFILLED)) return GEAS_ITEM_FULFILLED;
    if (sub_all(c, s, PLEDGE_BROKEN)) return GEAS_ITEM_BROKEN;
    return GEAS_ITEM_PENDING;
}

static GeasItemState loose_item_state(const GeasContract* c, uint32_t i) {
    if (!c->pledge_state) return GEAS_ITEM_PENDING;
    if (c->pledge_state[i] == PLEDGE_FULFILLED) return GEAS_ITEM_FULFILLED;
    if (c->pledge_state[i] == PLEDGE_BROKEN) return GEAS_ITEM_BROKEN;
    return GEAS_ITEM_PENDING;
}

/* The one walk both partial item calls share: items in descriptor order,
 * named subcontracts first then loose pledges, counting the ones whose state
 * reads k, and capturing the want-th match's name when the caller asked for
 * one. Anonymous subcontracts group their pledges for the policy but have no
 * name a PartialResult could report, so they are not items. Called under the
 * instance lock. */
static size_t partial_scan(const GeasContract* c, GeasItemState k, size_t want,
                           const char** name_out) {
    const GeasContractDesc* d = c->desc;
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

size_t geas_partial_count(GeasContract* c, GeasItemState k) {
    if (!c) return 0;
    pthread_mutex_lock(&c->mu);
    size_t n = partial_scan(c, k, 0, NULL);
    pthread_mutex_unlock(&c->mu);
    return n;
}

const char* geas_partial_name(GeasContract* c, GeasItemState k, size_t i) {
    if (!c) return NULL;
    const char* name = NULL;
    pthread_mutex_lock(&c->mu);
    partial_scan(c, k, i, &name);
    pthread_mutex_unlock(&c->mu);
    return name;
}

size_t geas_partial_nerrors(GeasContract* c) {
    if (!c) return 0;
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

GeasStatus geas_partial_error(GeasContract* c, size_t i,
                            const char** pledge_name, const GeasValue** err) {
    if (!c) return GEAS_ERR_TYPE;
    GeasStatus st = GEAS_ERR_NAME;
    pthread_mutex_lock(&c->mu);
    if (c->pledge_state) {
        size_t n = 0;
        for (uint32_t p = 0; p < c->desc->npledges; p++) {
            if (c->pledge_state[p] != PLEDGE_BROKEN) continue;
            if (n == i) {
                if (pledge_name) *pledge_name = c->desc->pledges[p].name;
                if (err) *err = &c->pledge_err[p];
                st = GEAS_OK;
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
const GeasValue* geas_vow_ref(GeasContract* c, const char* name) {
    if (!c || !name) return NULL;
    pthread_mutex_lock(&c->mu);
    const GeasValue* v = NULL;
    if (c->vow_vals) {
        const GeasVowDesc* vd = find_vow_desc(c->desc, name);
        if (vd) v = &c->vow_vals[vd - c->desc->vows];
    }
    pthread_mutex_unlock(&c->mu);
    return v;
}

/* ---- pledges ---- */

static const GeasPledgeDesc* find_pledge(const GeasContractDesc* desc,
                                        const char* name) {
    for (uint32_t i = 0; i < desc->npledges; i++) {
        if (strcmp(desc->pledges[i].name, name) == 0) return &desc->pledges[i];
    }
    return NULL;
}

/* Resolves "Contract.pledge" or a mangled symbol to its descriptor entry.
 * Called under the runtime lock. */
static const GeasPledgeDesc* resolve_pledge_name(const GeasRuntime* rt,
                                                const char* name) {
    const char* dot = strchr(name, '.');
    if (dot && dot != name && dot[1] != '\0') {
        size_t clen = (size_t)(dot - name);
        for (size_t i = 0; i < rt->ndescs; i++) {
            const GeasContractDesc* d = rt->descs[i];
            if (strncmp(d->name, name, clen) != 0 || d->name[clen] != '\0')
                continue;
            return find_pledge(d, dot + 1);
        }
        return NULL;
    }
    for (size_t i = 0; i < rt->ndescs; i++) {
        const GeasContractDesc* d = rt->descs[i];
        for (uint32_t j = 0; j < d->npledges; j++) {
            if (d->pledges[j].mangled &&
                strcmp(d->pledges[j].mangled, name) == 0)
                return &d->pledges[j];
        }
    }
    return NULL;
}

GeasStatus geas_pledge_bind(GeasRuntime* rt, const char* pledge_name,
                          GeasPledgeFn fn) {
    if (!rt || !pledge_name || !fn) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&rt->lock);
    if (rt->frozen) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_STATE;
    }
    const GeasPledgeDesc* pd = resolve_pledge_name(rt, pledge_name);
    if (!pd) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_NAME;
    }
    for (size_t i = 0; i < rt->nbindings; i++) {
        if (rt->bindings[i].pd == pd) {
            rt->bindings[i].fn = fn;
            pthread_mutex_unlock(&rt->lock);
            return GEAS_OK;
        }
    }
    if (rt->nbindings == GEAS_MAX_BINDINGS) {
        pthread_mutex_unlock(&rt->lock);
        return GEAS_ERR_OOM;
    }
    rt->bindings[rt->nbindings].pd = pd;
    rt->bindings[rt->nbindings].fn = fn;
    rt->nbindings++;
    pthread_mutex_unlock(&rt->lock);
    return GEAS_OK;
}

/* Copies the host value a ref points at into an instance owned frame slot.
 * v1 passes scalars and strings by reference; the composite types wait for a
 * host repr worth standardizing. */
static GeasStatus ref_copy_in(GeasContract* c, const GeasRef* r, GeasValue* slot) {
    if (!r->host_ptr) return GEAS_ERR_TYPE;
    memset(slot, 0, sizeof(*slot));
    slot->ty = r->ty;
    switch ((GeasTypeTag)r->ty) {
    case GEAS_TY_INT:   slot->as.i  = *(const int64_t*)r->host_ptr;  return GEAS_OK;
    case GEAS_TY_UINT:  slot->as.u  = *(const uint64_t*)r->host_ptr; return GEAS_OK;
    case GEAS_TY_FLOAT: slot->as.f  = *(const double*)r->host_ptr;   return GEAS_OK;
    case GEAS_TY_BOOL:
    case GEAS_TY_BYTE:  slot->as.b  = *(const uint8_t*)r->host_ptr;  return GEAS_OK;
    case GEAS_TY_CHAR:  slot->as.ch = *(const uint32_t*)r->host_ptr; return GEAS_OK;
    case GEAS_TY_STRING: {
        const GeasString* hs = (const GeasString*)r->host_ptr;
        *slot = geas_string_copy(c, hs->ptr, hs->len);
        if (hs->len && !slot->as.s.ptr) return GEAS_ERR_OOM;
        return GEAS_OK;
    }
    default:
        return GEAS_ERR_TYPE;
    }
}

/* Writes one slot's final value back to host memory, the default protocol
 * when the ref carries no callback: scalars in place, strings as a whole
 * GeasString struct whose bytes stay instance owned. Runs on the thread that
 * collects the outcome, while the host is blocked in the geas call. */
static GeasStatus ref_write_back(const GeasRef* r, const GeasValue* slot) {
    if (slot->ty != r->ty) return GEAS_ERR_TYPE;
    if (r->write_back) {
        r->write_back(r->host_ptr, slot, r->user);
        return GEAS_OK;
    }
    switch ((GeasTypeTag)r->ty) {
    case GEAS_TY_INT:   *(int64_t*)r->host_ptr   = slot->as.i;  return GEAS_OK;
    case GEAS_TY_UINT:  *(uint64_t*)r->host_ptr  = slot->as.u;  return GEAS_OK;
    case GEAS_TY_FLOAT: *(double*)r->host_ptr    = slot->as.f;  return GEAS_OK;
    case GEAS_TY_BOOL:
    case GEAS_TY_BYTE:  *(uint8_t*)r->host_ptr   = slot->as.b;  return GEAS_OK;
    case GEAS_TY_CHAR:  *(uint32_t*)r->host_ptr  = slot->as.ch; return GEAS_OK;
    case GEAS_TY_STRING: *(GeasString*)r->host_ptr = slot->as.s; return GEAS_OK;
    default:
        return GEAS_ERR_TYPE;
    }
}

/* Applies every write back of a delivered fulfillment. Slot types are
 * checked first so a pledge that broke the protocol writes nothing at all. */
static GeasStatus write_back_refs(const GeasRef* refs, const GeasValue* slots,
                                 size_t nrefs) {
    for (size_t i = 0; i < nrefs; i++) {
        if (slots[i].ty != refs[i].ty) return GEAS_ERR_TYPE;
    }
    for (size_t i = 0; i < nrefs; i++) {
        GeasStatus st = ref_write_back(&refs[i], &slots[i]);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* Builds the frame a thunk sees: one instance owned slot per declared
 * parameter, the value arguments deep copied first, the refs copied in
 * behind them. Everything happens on the caller's thread, so no host memory
 * is ever read after the fulfill call returns. */
static GeasStatus prepare_frame(GeasContract* c, const GeasValue* args,
                               size_t nargs, const GeasRef* refs, size_t nrefs,
                               GeasValue** frame_out) {
    size_t total = nargs + nrefs;
    *frame_out = NULL;
    if (total == 0) return GEAS_OK;
    GeasValue* frame = (GeasValue*)geas_bytes(c, total * sizeof(GeasValue));
    if (!frame) return GEAS_ERR_OOM;
    for (size_t i = 0; i < nargs; i++) {
        GeasStatus st = geas_value_deep_copy(c, &args[i], &frame[i]);
        if (st != GEAS_OK) return st;
    }
    for (size_t i = 0; i < nrefs; i++) {
        GeasStatus st = ref_copy_in(c, &refs[i], &frame[nargs + i]);
        if (st != GEAS_OK) return st;
    }
    *frame_out = frame;
    return GEAS_OK;
}

/* ---- the requirements evaluator ---- */

/* Whether a pledge sits outside every subcontract. A sub index outside the
 * subs table reads as loose, which is what a zero-filled handwritten
 * descriptor gets. */
static int pledge_is_loose(const GeasContractDesc* d, uint32_t i) {
    int32_t s = d->pledges[i].sub;
    return s < 0 || (uint32_t)s >= d->nsubs;
}

/* Whether every pledge of subcontract s has latched want. An empty
 * subcontract holds no latch to test and reads false for both fulfilled and
 * broken. Called under the instance lock. */
static int sub_all(const GeasContract* c, uint32_t s, uint8_t want) {
    const GeasContractDesc* d = c->desc;
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
static int atom_true(const GeasContract* c, const GeasReqAtom* a) {
    uint8_t want = (a->kind == GEAS_ATOM_BROKEN) ? PLEDGE_BROKEN
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

static int eval_line(const GeasContract* c, const GeasReqOp* ops, uint32_t n) {
    uint8_t st[REQ_STACK_MAX];
    int sp = 0;
    if (n == 0 || !ops) return 0;
    for (uint32_t i = 0; i < n; i++) {
        switch (ops[i].op) {
        case GEAS_REQ_ATOM:
            if (sp >= REQ_STACK_MAX) return 0;
            if (ops[i].atom >= c->desc->natoms) return 0;
            st[sp++] = (uint8_t)atom_true(c, &c->desc->atoms[ops[i].atom]);
            break;
        case GEAS_REQ_NOT:
            if (sp < 1) return 0;
            st[sp - 1] = !st[sp - 1];
            break;
        case GEAS_REQ_AND:
            if (sp < 2) return 0;
            st[sp - 2] = st[sp - 2] && st[sp - 1];
            sp--;
            break;
        case GEAS_REQ_OR:
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
 * least one subcontract is, break when everything is broken. geas compiled
 * modules never land here, the compiler serializes the source block or
 * synthesizes these same defaults as trees. Called under the instance
 * lock. */
static int default_line(const GeasContract* c, uint8_t want) {
    const GeasContractDesc* d = c->desc;
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

static int default_partial(const GeasContract* c) {
    for (uint32_t s = 0; s < c->desc->nsubs; s++) {
        if (sub_all(c, s, PLEDGE_FULFILLED)) return 1;
    }
    return 0;
}

/* Recomputes the contract state from the latches, in the grammar's priority
 * order: break, then fulfill, then partial, the first line that matches
 * setting the state, and Signed when none does. Broken is terminal, every
 * later fulfillment is refused with GEAS_ERR_STATE before a thunk runs.
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
static void eval_policy(GeasContract* c) {
    const GeasContractDesc* d = c->desc;
    if (c->state == GEAS_BROKEN) return;
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
    if (brk) c->state = GEAS_BROKEN;
    else if (ful) c->state = GEAS_FULFILLED;
    else if (par) c->state = GEAS_PARTIAL;
    else c->state = GEAS_SIGNED;
}

/* The per-pledge latch, the grammar's law: fulfilled on the first Ok, broken
 * on an Err that lands before any Ok, and never a change after either. The
 * first Err's payload is kept beside the latch; the box it may point into is
 * instance owned, so the struct copy stays valid exactly as long as the
 * instance heap does. Called under the instance lock. */
static void latch_pledge(GeasContract* c, uint32_t pidx, const GeasValue* out) {
    if (!c->pledge_state || pidx >= c->desc->npledges) return;
    if (c->pledge_state[pidx] != PLEDGE_PENDING) return;
    if (out->ty == GEAS_TY_RESULT && out->tag == 1) {
        c->pledge_state[pidx] = PLEDGE_BROKEN;
        if (out->as.box) c->pledge_err[pidx] = *(const GeasValue*)out->as.box;
    } else {
        c->pledge_state[pidx] = PLEDGE_FULFILLED;
    }
}

/* ---- transactional subcontracts ---- */

/* The subcontract a pledge belongs to when that subcontract is transactional,
 * or -1. A loose pledge, a sub index outside the table, and a subcontract
 * without the flag all read -1, so the store transaction machinery is a no-op
 * for every pledge outside a transactional group and for every contract that
 * carries no sub_flags at all. */
static int32_t pledge_txn_sub(const GeasContractDesc* d, uint32_t pidx) {
    if (pidx >= d->npledges) return -1;
    int32_t s = d->pledges[pidx].sub;
    if (s < 0 || (uint32_t)s >= d->nsubs) return -1;
    if (!d->sub_flags) return -1;
    if ((d->sub_flags[s] & GEAS_SUB_TRANSACTIONAL) == 0) return -1;
    return s;
}

/* Opens the episode before the thunk runs. A pledge outside any transactional
 * subcontract proceeds untouched; a pledge whose episode already resolved is
 * refused GEAS_ERR_STATE, the once-only law a committed transaction earns; the
 * first pledge of a transactional subcontract begins the transaction lazily and
 * marks it open. A begin the backend refuses rides back as GEAS_ERR_STORE and
 * the episode stays unopened. Returns GEAS_OK to proceed, or the status to
 * deliver instead. Called under the instance lock. */
static GeasStatus txn_before(GeasContract* c, uint32_t pidx) {
    int32_t s = pledge_txn_sub(c->desc, pidx);
    if (s < 0 || !c->sub_txn || !c->store) return GEAS_OK;
    if (c->sub_txn[s] == TXN_DONE) return GEAS_ERR_STATE;
    if (c->sub_txn[s] == TXN_NONE) {
        GeasStatus st = geas_store_begin(c->store);
        if (st != GEAS_OK) return st;
        c->sub_txn[s] = TXN_OPEN;
    }
    return GEAS_OK;
}

/* Resolves the episode after one pledge outcome, the whole all-or-nothing rule
 * in one place. A backend or runtime failure mid episode rolls the transaction
 * back and closes it, so no half written episode survives; an Err latched the
 * pledge broken and the buffered writes vanish the instant it lands; an Ok that
 * completes the subcontract commits the whole episode. The completion test is
 * sub_all over PLEDGE_FULFILLED, the same "every pledge of this subcontract
 * latched Ok" predicate the requirements evaluator reads through atom_true and
 * sub_item_state, so the commit point can never drift from the subcontract's
 * own fulfillment. A commit the backend refuses is GEAS_ERR_STORE with the
 * episode rolled back. Returns the status to deliver. Called under the instance
 * lock, after latch_pledge and eval_policy. */
static GeasStatus txn_after(GeasContract* c, uint32_t pidx, GeasStatus st,
                           const GeasValue* out) {
    int32_t s = pledge_txn_sub(c->desc, pidx);
    if (s < 0 || !c->sub_txn || !c->store || c->sub_txn[s] != TXN_OPEN) {
        return st;
    }
    if (st != GEAS_OK) {
        geas_store_rollback(c->store);
        c->sub_txn[s] = TXN_DONE;
        return st;
    }
    if (out && out->ty == GEAS_TY_RESULT && out->tag == 1) {
        geas_store_rollback(c->store);
        c->sub_txn[s] = TXN_DONE;
        return st;
    }
    if (sub_all(c, (uint32_t)s, PLEDGE_FULFILLED)) {
        GeasStatus cs = geas_store_commit(c->store);
        c->sub_txn[s] = TXN_DONE;
        if (cs != GEAS_OK) {
            geas_store_rollback(c->store);
            return GEAS_ERR_STORE;
        }
    }
    return st;
}

/* One fulfillment on a pool worker. The instance lock covers the state
 * check, the thunk run, and the latch, so fulfillments against one instance
 * serialize while distinct instances run truly in parallel, and a break can
 * never free the heap under a running thunk. The completion happens inside
 * the same critical section: the lock handoff to the waiting thread is what
 * makes the thunk's slot writes visible to the write back. */
static void run_task(struct GeasFuture* f) {
    GeasContract* c = f->c;
    pthread_mutex_lock(&c->mu);
    if (c->state != GEAS_SIGNED && c->state != GEAS_PARTIAL &&
        c->state != GEAS_FULFILLED) {
        future_finish(f, GEAS_ERR_STATE, NULL);
    } else {
        GeasStatus gate = txn_before(c, f->pidx);
        if (gate != GEAS_OK) {
            future_finish(f, gate, NULL);
        } else {
            GeasValue out;
            memset(&out, 0, sizeof(out));
            GeasStatus st = f->fn((void*)c, f->frame, f->frame_nargs, &out);
            if (st == GEAS_OK) {
                latch_pledge(c, f->pidx, &out);
                eval_policy(c);
            }
            st = txn_after(c, f->pidx, st, &out);
            future_finish(f, st, &out);
        }
    }
    pthread_mutex_unlock(&c->mu);
}

/* Starts a fulfillment: validate and copy in on the caller's thread, under
 * the instance lock, then hand the future to the pool. Every failure short
 * of allocating the future itself is delivered through the wait, the M4
 * contract that survives concurrency unchanged. */
GeasFuture* geas_pledge_fulfill(GeasContract* c, const char* pledge_name,
                              const GeasValue* args, size_t nargs,
                              const GeasRef* refs, size_t nrefs) {
    if (!c || !pledge_name) return NULL;
    struct GeasFuture* f = future_new(c);
    if (!f) return NULL;
    GeasStatus st = GEAS_OK;
    int ready = 0;
    pthread_mutex_lock(&c->mu);
    do {
        if ((nargs > 0 && !args) || (nrefs > 0 && !refs)) {
            st = GEAS_ERR_TYPE;
            break;
        }
        if (c->state != GEAS_SIGNED && c->state != GEAS_PARTIAL &&
            c->state != GEAS_FULFILLED) {
            st = GEAS_ERR_STATE;
            break;
        }
        const GeasPledgeDesc* p = find_pledge(c->desc, pledge_name);
        if (!p) {
            st = GEAS_ERR_NAME;
            break;
        }
        if (nargs + nrefs != p->nargs) {
            st = GEAS_ERR_TYPE;
            break;
        }
        if (nrefs > 0) {
            f->refs = (GeasRef*)geas_bytes(c, nrefs * sizeof(GeasRef));
            if (!f->refs) {
                st = GEAS_ERR_OOM;
                break;
            }
            memcpy(f->refs, refs, nrefs * sizeof(GeasRef));
            f->nrefs = nrefs;
        }
        GeasValue* frame = NULL;
        st = prepare_frame(c, args, nargs, refs, nrefs, &frame);
        if (st != GEAS_OK) break;
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
GeasStatus geas_future_wait(GeasFuture* f, GeasValue* out) {
    if (!f || !out) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&f->mu);
    while (!f->done) {
        pthread_cond_wait(&f->cv, &f->mu);
    }
    if (f->waited) {
        pthread_mutex_unlock(&f->mu);
        return GEAS_ERR_STATE;
    }
    f->waited = 1;
    GeasStatus st = f->status;
    if (st == GEAS_OK && f->nrefs > 0 && f->ref_slots) {
        GeasStatus wb = write_back_refs(f->refs, f->ref_slots, f->nrefs);
        if (wb != GEAS_OK) {
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
static GeasStatus fulfill_inline(GeasContract* c, const char* pledge_name,
                                const GeasValue* args, size_t nargs,
                                const GeasRef* refs, size_t nrefs,
                                GeasValue* out) {
    GeasStatus st = GEAS_OK;
    pthread_mutex_lock(&c->mu);
    do {
        if ((nargs > 0 && !args) || (nrefs > 0 && !refs)) {
            st = GEAS_ERR_TYPE;
            break;
        }
        if (c->state != GEAS_SIGNED && c->state != GEAS_PARTIAL &&
            c->state != GEAS_FULFILLED) {
            st = GEAS_ERR_STATE;
            break;
        }
        const GeasPledgeDesc* p = find_pledge(c->desc, pledge_name);
        if (!p) {
            st = GEAS_ERR_NAME;
            break;
        }
        if (nargs + nrefs != p->nargs) {
            st = GEAS_ERR_TYPE;
            break;
        }
        GeasValue* frame = NULL;
        st = prepare_frame(c, args, nargs, refs, nrefs, &frame);
        if (st != GEAS_OK) break;
        uint32_t pidx = (uint32_t)(p - c->desc->pledges);
        GeasStatus gate = txn_before(c, pidx);
        if (gate != GEAS_OK) {
            st = gate;
            break;
        }
        GeasValue res;
        memset(&res, 0, sizeof(res));
        GeasPledgeFn fn = c->fns[p - c->desc->pledges];
        st = fn((void*)c, frame, p->nargs, &res);
        if (st == GEAS_OK) {
            latch_pledge(c, pidx, &res);
            eval_policy(c);
        }
        st = txn_after(c, pidx, st, &res);
        if (st != GEAS_OK) break;
        if (nrefs > 0 && frame) {
            GeasStatus wb = write_back_refs(refs, frame + nargs, nrefs);
            if (wb != GEAS_OK) {
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
GeasStatus geas_pledge_fulfill_sync(GeasContract* c, const char* pledge_name,
                                  const GeasValue* args, size_t nargs,
                                  const GeasRef* refs, size_t nrefs,
                                  GeasValue* out) {
    if (!c || !pledge_name || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (t_pool_worker) {
        return fulfill_inline(c, pledge_name, args, nargs, refs, nrefs, out);
    }
    GeasFuture* f = geas_pledge_fulfill(c, pledge_name, args, nargs, refs, nrefs);
    if (!f) return GEAS_ERR_OOM;
    GeasStatus st = geas_future_wait(f, out);
    future_release(f);
    return st;
}

static GeasStatus partial_names_value(GeasContract* c, GeasContract* owner,
                                     GeasItemState k, GeasValue* out) {
    size_t n = partial_scan(c, k, 0, NULL);
    GeasValue* data = NULL;
    if (n > 0) {
        data = (GeasValue*)geas_bytes(owner, (uint64_t)n * sizeof(GeasValue));
        if (!data) return GEAS_ERR_OOM;
        memset(data, 0, n * sizeof(GeasValue));
        for (size_t i = 0; i < n; i++) {
            const char* name = NULL;
            partial_scan(c, k, i, &name);
            if (!name) return GEAS_ERR_NAME;
            GeasValue s = geas_string_copy(owner, (const uint8_t*)name,
                                         strlen(name));
            if (!s.as.s.ptr && s.as.s.len == 0 && name[0]) {
                return GEAS_ERR_OOM;
            }
            data[i] = s;
        }
    }
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_LIST;
    out->as.list.data = data;
    out->as.list.len = n;
    out->as.list.cap = n;
    out->as.list.elem_ty = GEAS_TY_STRING;
    return GEAS_OK;
}

GeasStatus geas_partial_value(GeasContract* c, GeasContract* owner,
                            GeasValue* out) {
    if (!c || !owner || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_OK;
    GeasValue record;
    memset(&record, 0, sizeof(record));
    GeasValue* fields = (GeasValue*)geas_bytes(owner, 4 * sizeof(GeasValue));
    if (!fields) {
        st = GEAS_ERR_OOM;
    } else {
        memset(fields, 0, 4 * sizeof(GeasValue));
        fields[0] = geas_state_value(c);
        st = partial_names_value(c, owner, GEAS_ITEM_FULFILLED, &fields[1]);
        if (st == GEAS_OK) {
            st = partial_names_value(c, owner, GEAS_ITEM_PENDING, &fields[2]);
        }
        if (st == GEAS_OK) {
            st = partial_names_value(c, owner, GEAS_ITEM_BROKEN, &fields[3]);
        }
        if (st == GEAS_OK) {
            record.ty = GEAS_TY_RECORD;
            record.as.list.data = fields;
            record.as.list.len = 4;
            record.as.list.cap = 4;
            record.as.list.elem_ty = 0;
            *out = record;
        }
    }
    if (st != GEAS_OK) memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&c->mu);
    return st;
}

/* ---- the parked instance ---- */

/* The park row's schema, one table the runtime owns in whatever database the
 * caller names. The blobs are the wire codec's canonical encoding, the same
 * bytes the network trusts, so a parked value round trips exactly and the
 * codec's goldens stand guard over the park format for free. */
static const char* PARK_DDL =
    "CREATE TABLE IF NOT EXISTS geas_park ("
    "pkey TEXT PRIMARY KEY, contract TEXT, version INTEGER, "
    "shape_hash INTEGER, state INTEGER, signed_at INTEGER, "
    "vows BLOB, latches BLOB, errs BLOB, subtxn BLOB)";

/* Whether a stored Err payload is present: an explicit break zeroes the
 * struct, and a pledge that never broke never wrote one, so all zero is the
 * absent spelling here exactly as it is on the partial surface. */
static int park_err_present(const GeasValue* v) {
    static const GeasValue zero;
    return memcmp(v, &zero, sizeof(GeasValue)) != 0;
}

/* Encodes n values back to back into one malloc'd buffer, the sequential
 * form both blobs share: the decoder walks by each value's own consumed
 * count, so no frame or count rides in the bytes. */
static GeasStatus park_encode_values(const GeasValue* vals, size_t n,
                                    uint8_t** buf_out, size_t* len_out) {
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        size_t need = 0;
        /* The sizing call: a NULL buffer answers GEAS_ERR_OOM with *need set,
         * the codec's own size protocol, so only another status is a fault. */
        GeasStatus st = geas_wire_encode_value(&vals[i], NULL, 0, &need);
        if (st != GEAS_OK && st != GEAS_ERR_OOM) return st;
        total += need;
    }
    uint8_t* buf = malloc(total ? total : 1);
    if (!buf) return GEAS_ERR_OOM;
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t need = 0;
        GeasStatus st = geas_wire_encode_value(&vals[i], buf + off, total - off,
                                             &need);
        if (st != GEAS_OK) {
            free(buf);
            return st;
        }
        off += need;
    }
    *buf_out = buf;
    *len_out = total;
    return GEAS_OK;
}

/* The Err payload blob: per pledge one presence byte, then the encoded value
 * when present. Sized first, then written, the codec's own two pass shape. */
static GeasStatus park_encode_errs(const GeasContract* c,
                                  uint8_t** buf_out, size_t* len_out) {
    uint32_t n = c->desc->npledges;
    size_t total = n;
    for (uint32_t i = 0; i < n; i++) {
        if (!park_err_present(&c->pledge_err[i])) continue;
        size_t need = 0;
        GeasStatus st = geas_wire_encode_value(&c->pledge_err[i], NULL, 0, &need);
        if (st != GEAS_OK && st != GEAS_ERR_OOM) return st;
        total += need;
    }
    uint8_t* buf = malloc(total ? total : 1);
    if (!buf) return GEAS_ERR_OOM;
    size_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        int present = park_err_present(&c->pledge_err[i]);
        buf[off++] = (uint8_t)(present ? 1 : 0);
        if (!present) continue;
        size_t need = 0;
        GeasStatus st = geas_wire_encode_value(&c->pledge_err[i], buf + off,
                                             total - off, &need);
        if (st != GEAS_OK) {
            free(buf);
            return st;
        }
        off += need;
    }
    *buf_out = buf;
    *len_out = total;
    return GEAS_OK;
}

static GeasValue park_str_param(const uint8_t* p, size_t n) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)(n ? p : (const uint8_t*)"");
    v.as.s.len = n;
    return v;
}

static GeasValue park_int_param(int64_t i) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_INT;
    v.as.i = i;
    return v;
}

GeasStatus geas_instance_park(GeasContract* c, const char* dsn, const char* key) {
    if (!c || !dsn || !key) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    GeasStatus st = GEAS_OK;
    if (c->state == GEAS_UNSIGNED) st = GEAS_ERR_STATE;
    /* An explicit break reclaimed the instance heap the vows and payloads
     * live on; the latches still read, but there is nothing left to write
     * down. An automatic break keeps that heap on purpose and parks fine. */
    if (st == GEAS_OK && c->desc->npledges > 0 && !c->owned) st = GEAS_ERR_STATE;
    /* A park is a state between walks: an unwaited future is a walk still in
     * the air, and an open transactional episode holds buffered writes no row
     * can carry. Both refuse rather than guess. */
    if (st == GEAS_OK) {
        for (struct GeasFuture* f = c->futures; f; f = f->next) {
            if (!f->waited) {
                st = GEAS_ERR_STATE;
                break;
            }
        }
    }
    if (st == GEAS_OK && c->sub_txn) {
        for (uint32_t i = 0; i < c->desc->nsubs; i++) {
            if (c->sub_txn[i] == TXN_OPEN) {
                st = GEAS_ERR_STATE;
                break;
            }
        }
    }
    uint8_t* vows_buf = NULL;
    size_t vows_len = 0;
    uint8_t* errs_buf = NULL;
    size_t errs_len = 0;
    if (st == GEAS_OK && c->desc->nvows > 0) {
        st = park_encode_values(c->vow_vals, c->desc->nvows,
                                &vows_buf, &vows_len);
    }
    if (st == GEAS_OK && c->desc->npledges > 0) {
        st = park_encode_errs(c, &errs_buf, &errs_len);
    }
    GeasStore* s = NULL;
    if (st == GEAS_OK) st = geas_store_open(dsn, &s);
    if (st == GEAS_OK) st = geas_store_exec(s, PARK_DDL);
    if (st == GEAS_OK) {
        GeasValue params[10];
        params[0] = park_str_param((const uint8_t*)key, strlen(key));
        params[1] = park_str_param((const uint8_t*)c->desc->name,
                                   strlen(c->desc->name));
        params[2] = park_int_param((int64_t)c->desc->version);
        params[3] = park_int_param((int64_t)c->desc->shape_hash);
        params[4] = park_int_param((int64_t)c->state);
        params[5] = park_int_param(c->signed_at);
        params[6] = park_str_param(vows_buf, vows_len);
        params[7] = park_str_param(c->pledge_state, c->desc->npledges);
        params[8] = park_str_param(errs_buf, errs_len);
        params[9] = park_str_param(c->sub_txn, c->desc->nsubs);
        st = geas_store_exec_params(s,
            "INSERT OR REPLACE INTO geas_park "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
            params, 10, NULL);
    }
    geas_store_close(s);
    free(vows_buf);
    free(errs_buf);
    pthread_mutex_unlock(&c->mu);
    return st;
}

/* The malloc arena the resume query reads its row into: one chunk per
 * allocation on a list, freed wholesale once every byte the row carried has
 * been decoded onto the new instance. */
typedef struct ParkChunk {
    struct ParkChunk* next;
} ParkChunk;

static uint8_t* park_arena_bytes(void* ctx, uint64_t n) {
    ParkChunk** head = (ParkChunk**)ctx;
    ParkChunk* ch = malloc(sizeof(ParkChunk) + (size_t)n);
    if (!ch) return NULL;
    ch->next = *head;
    *head = ch;
    return (uint8_t*)(ch + 1);
}

static void park_arena_free(ParkChunk* head) {
    while (head) {
        ParkChunk* next = head->next;
        free(head);
        head = next;
    }
}

GeasStatus geas_instance_resume(GeasRuntime* rt, const char* dsn, const char* key,
                              uint64_t expected_hash, GeasContract** out) {
    if (!rt || !dsn || !key || !out) return GEAS_ERR_TYPE;
    *out = NULL;

    GeasStore* s = NULL;
    GeasStatus st = geas_store_open(dsn, &s);
    if (st != GEAS_OK) return st;
    st = geas_store_exec(s, PARK_DDL);
    ParkChunk* arena = NULL;
    GeasValue rows;
    memset(&rows, 0, sizeof(rows));
    if (st == GEAS_OK) {
        GeasValue kv = park_str_param((const uint8_t*)key, strlen(key));
        static const uint32_t cols[9] = {
            GEAS_TY_STRING, GEAS_TY_INT, GEAS_TY_UINT, GEAS_TY_INT, GEAS_TY_INT,
            GEAS_TY_STRING, GEAS_TY_STRING, GEAS_TY_STRING, GEAS_TY_STRING
        };
        GeasStoreAlloc alloc = { park_arena_bytes, &arena };
        st = geas_store_query(s,
            "SELECT contract, version, shape_hash, state, signed_at, "
            "vows, latches, errs, subtxn FROM geas_park WHERE pkey = ?1",
            &kv, 1, cols, NULL, 9, &alloc, &rows);
    }
    geas_store_close(s);
    if (st != GEAS_OK) {
        park_arena_free(arena);
        return st;
    }
    if (rows.as.list.len == 0) {
        park_arena_free(arena);
        return GEAS_ERR_NAME;
    }
    const GeasValue* f = (const GeasValue*)((const GeasValue*)rows.as.list.data)[0]
                            .as.list.data;
    const GeasString rname   = f[0].as.s;
    const int64_t   rver    = f[1].as.i;
    const uint64_t  rshape  = f[2].as.u;
    const int64_t   rstate  = f[3].as.i;
    const int64_t   rsigned = f[4].as.i;
    const GeasString bvows   = f[5].as.s;
    const GeasString blatch  = f[6].as.s;
    const GeasString berrs   = f[7].as.s;
    const GeasString bsubtxn = f[8].as.s;

    if (rstate < (int64_t)GEAS_SIGNED || rstate > (int64_t)GEAS_BROKEN) {
        park_arena_free(arena);
        return GEAS_ERR_TYPE;
    }
    char* name = malloc(rname.len + 1);
    if (!name) {
        park_arena_free(arena);
        return GEAS_ERR_OOM;
    }
    memcpy(name, rname.ptr, rname.len);
    name[rname.len] = 0;

    pthread_mutex_lock(&rt->lock);
    const GeasContractDesc* desc = find_desc(rt, name);
    free(name);
    if (!desc) {
        pthread_mutex_unlock(&rt->lock);
        park_arena_free(arena);
        return GEAS_ERR_NAME;
    }
    /* The row must describe the module this runtime registered, and a caller
     * pinning a hash must agree with both, the same skew rule sign runs. */
    if ((int64_t)desc->version != rver || desc->shape_hash != rshape ||
        (expected_hash != 0 && expected_hash != desc->shape_hash)) {
        pthread_mutex_unlock(&rt->lock);
        park_arena_free(arena);
        return GEAS_ERR_VERSION;
    }
    GeasPledgeFn* fns = NULL;
    st = resolve_dispatch(rt, desc, &fns);
    pthread_mutex_unlock(&rt->lock);
    if (st != GEAS_OK) {
        park_arena_free(arena);
        return st;
    }

    GeasContract* c = calloc(1, sizeof(GeasContract));
    if (!c) {
        free(fns);
        park_arena_free(arena);
        return GEAS_ERR_OOM;
    }
    if (mutex_init_recursive(&c->mu) != 0) {
        free(c);
        free(fns);
        park_arena_free(arena);
        return GEAS_ERR_OOM;
    }
    c->rt = rt;
    c->desc = desc;
    if (desc->npledges > 0) {
        c->fns = (GeasPledgeFn*)geas_bytes(c, desc->npledges * sizeof(GeasPledgeFn));
        if (!c->fns) st = GEAS_ERR_OOM;
        else memcpy(c->fns, fns, desc->npledges * sizeof(GeasPledgeFn));
    }
    free(fns);

    /* The vows decode straight onto the new instance in declaration order,
     * each checked against its declared type, and the blob must hold exactly
     * the declared count, no more and no less. */
    if (st == GEAS_OK && desc->nvows > 0) {
        c->vow_vals = (GeasValue*)geas_bytes(c, desc->nvows * sizeof(GeasValue));
        if (!c->vow_vals) st = GEAS_ERR_OOM;
        else memset(c->vow_vals, 0, desc->nvows * sizeof(GeasValue));
        size_t off = 0;
        for (uint32_t i = 0; st == GEAS_OK && i < desc->nvows; i++) {
            size_t used = 0;
            st = geas_wire_decode_value(c, bvows.ptr + off, bvows.len - off,
                                       &c->vow_vals[i], &used);
            if (st == GEAS_OK && c->vow_vals[i].ty != desc->vows[i].ty) {
                st = GEAS_ERR_TYPE;
            }
            off += used;
        }
        if (st == GEAS_OK && off != bvows.len) st = GEAS_ERR_TYPE;
    } else if (st == GEAS_OK && bvows.len != 0) {
        st = GEAS_ERR_TYPE;
    }

    /* The latches replay byte for byte, then each present Err payload decodes
     * onto the instance heap, the home the partial surface expects. */
    if (st == GEAS_OK && desc->npledges > 0) {
        c->pledge_state = calloc(desc->npledges, sizeof(uint8_t));
        c->pledge_err = calloc(desc->npledges, sizeof(GeasValue));
        if (!c->pledge_state || !c->pledge_err) st = GEAS_ERR_OOM;
        if (st == GEAS_OK && blatch.len != desc->npledges) st = GEAS_ERR_TYPE;
        for (uint32_t i = 0; st == GEAS_OK && i < desc->npledges; i++) {
            if (blatch.ptr[i] > PLEDGE_BROKEN) st = GEAS_ERR_TYPE;
            else c->pledge_state[i] = blatch.ptr[i];
        }
        size_t off = 0;
        for (uint32_t i = 0; st == GEAS_OK && i < desc->npledges; i++) {
            if (off >= berrs.len) {
                st = GEAS_ERR_TYPE;
                break;
            }
            uint8_t present = berrs.ptr[off++];
            if (present > 1) {
                st = GEAS_ERR_TYPE;
            } else if (present == 1) {
                size_t used = 0;
                st = geas_wire_decode_value(c, berrs.ptr + off, berrs.len - off,
                                           &c->pledge_err[i], &used);
                off += used;
            }
        }
        if (st == GEAS_OK && off != berrs.len) st = GEAS_ERR_TYPE;
    }

    /* The transactional fates replay too: TXN_DONE stays done, so a resumed
     * episode can never run twice, and a recorded TXN_OPEN is a row park
     * refused to write, so reading one is corruption, not state. */
    if (st == GEAS_OK && desc->nsubs > 0) {
        c->sub_txn = calloc(desc->nsubs, sizeof(uint8_t));
        if (!c->sub_txn) st = GEAS_ERR_OOM;
        if (st == GEAS_OK && bsubtxn.len != desc->nsubs) st = GEAS_ERR_TYPE;
        for (uint32_t i = 0; st == GEAS_OK && i < desc->nsubs; i++) {
            if (bsubtxn.ptr[i] == TXN_OPEN || bsubtxn.ptr[i] > TXN_DONE) {
                st = GEAS_ERR_TYPE;
            } else {
                c->sub_txn[i] = bsubtxn.ptr[i];
            }
        }
    }

    if (st == GEAS_OK) st = store_sign_reconcile(c);
    if (st == GEAS_OK) {
        c->state = (GeasContractState)rstate;
        c->shape_hash = desc->shape_hash;
        c->signed_at = rsigned;
        pthread_mutex_lock(&rt->lock);
        if (rt->ninstances == GEAS_MAX_INSTANCES) {
            st = GEAS_ERR_OOM;
        } else {
            rt->instances[rt->ninstances++] = c;
        }
        pthread_mutex_unlock(&rt->lock);
    }
    park_arena_free(arena);
    if (st != GEAS_OK) {
        if (c->store) {
            geas_store_close(c->store);
            c->store = NULL;
        }
        contract_free_owned(c);
        free(c->pledge_state);
        free(c->pledge_err);
        free(c->sub_txn);
        pthread_mutex_destroy(&c->mu);
        free(c);
        return st;
    }
    *out = c;
    return GEAS_OK;
}

/* The canonical state spellings, the strings instance.status() answers in
 * the language and any host may print. Static storage, never freed. */
const char* geas_state_name(GeasContractState s) {
    switch (s) {
    case GEAS_UNSIGNED:  return "Unsigned";
    case GEAS_SIGNED:    return "Signed";
    case GEAS_FULFILLED: return "Fulfilled";
    case GEAS_PARTIAL:   return "Partial";
    case GEAS_BROKEN:    return "Broken";
    }
    return "Unknown";
}

GeasValue geas_state_value(const GeasContract* c) {
    const char* n = geas_state_name(geas_contract_state(c));
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    v.as.s.ptr = (uint8_t*)n;
    v.as.s.len = strlen(n);
    return v;
}

/* The NUL terminated copy of a String value the park calls take their dsn
 * and key through when the caller holds values rather than C strings, the
 * compiled thunk's own case. Plain heap, freed by the wrapper. */
static char* park_cstr(const GeasValue* v) {
    if (!v || v->ty != GEAS_TY_STRING) return NULL;
    char* p = malloc((size_t)v->as.s.len + 1);
    if (!p) return NULL;
    if (v->as.s.len) memcpy(p, v->as.s.ptr, (size_t)v->as.s.len);
    p[v->as.s.len] = 0;
    return p;
}

GeasStatus geas_instance_park_v(GeasContract* c, const GeasValue* dsn,
                              const GeasValue* key) {
    if (!c || !dsn || !key || dsn->ty != GEAS_TY_STRING ||
        key->ty != GEAS_TY_STRING) {
        return GEAS_ERR_TYPE;
    }
    char* d = park_cstr(dsn);
    char* k = park_cstr(key);
    GeasStatus st = (d && k) ? geas_instance_park(c, d, k) : GEAS_ERR_OOM;
    free(d);
    free(k);
    return st;
}

GeasStatus geas_instance_resume_v(GeasRuntime* rt, const GeasValue* dsn,
                                const GeasValue* key, uint64_t expected_hash,
                                GeasContract** out) {
    if (!rt || !dsn || !key || !out || dsn->ty != GEAS_TY_STRING ||
        key->ty != GEAS_TY_STRING) {
        return GEAS_ERR_TYPE;
    }
    char* d = park_cstr(dsn);
    char* k = park_cstr(key);
    GeasStatus st = (d && k)
        ? geas_instance_resume(rt, d, k, expected_hash, out)
        : GEAS_ERR_OOM;
    free(d);
    free(k);
    return st;
}

/* ---- allocation helpers ---- */

/* Every helper is safe from a thunk, where the worker already holds the
 * recursive instance lock, and from a host thread outside any fulfillment,
 * where the lock is taken cold right here. Only the block list is guarded;
 * a single GeasValue is not a shared object, and two threads mutating the
 * same list or string value remain a host bug. */
uint8_t* geas_bytes(GeasContract* c, uint64_t n) {
    if (!c) return NULL;
    GeasBlock* b = malloc(sizeof(GeasBlock) + n);
    if (!b) return NULL;
    pthread_mutex_lock(&c->mu);
    b->next = c->owned;
    c->owned = b;
    pthread_mutex_unlock(&c->mu);
    return (uint8_t*)(b + 1);
}

GeasValue* geas_box(GeasContract* c) {
    GeasValue* v = (GeasValue*)geas_bytes(c, sizeof(GeasValue));
    if (v) memset(v, 0, sizeof(*v));
    return v;
}

GeasValue geas_string_copy(GeasContract* c, const uint8_t* utf8, uint64_t len) {
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_STRING;
    uint8_t* dst = geas_bytes(c, len);
    if (dst && len) memcpy(dst, utf8, len);
    v.as.s.ptr = dst;
    v.as.s.len = dst ? len : 0;
    return v;
}

GeasStatus geas_string_concat(GeasContract* c, const GeasValue* a,
                            const GeasValue* b, GeasValue* out) {
    if (!c || !a || !b || !out) return GEAS_ERR_TYPE;
    if (a->ty != GEAS_TY_STRING || b->ty != GEAS_TY_STRING) return GEAS_ERR_TYPE;
    uint64_t n = a->as.s.len + b->as.s.len;
    uint8_t* buf = geas_bytes(c, n);
    if (!buf) return GEAS_ERR_OOM;
    if (a->as.s.len) memcpy(buf, a->as.s.ptr, a->as.s.len);
    if (b->as.s.len) memcpy(buf + a->as.s.len, b->as.s.ptr, b->as.s.len);
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_STRING;
    out->as.s.ptr = buf;
    out->as.s.len = n;
    return GEAS_OK;
}

int geas_string_eq(const GeasValue* a, const GeasValue* b) {
    if (!a || !b) return 0;
    if (a->ty != GEAS_TY_STRING || b->ty != GEAS_TY_STRING) return 0;
    if (a->as.s.len != b->as.s.len) return 0;
    if (a->as.s.len == 0) return 1;
    return memcmp(a->as.s.ptr, b->as.s.ptr, a->as.s.len) == 0;
}

/* ---- deep values ---- */

GeasStatus geas_list_new(GeasContract* c, uint32_t elem_ty, uint64_t cap,
                       GeasValue* out) {
    if (!c || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_LIST;
    out->as.list.elem_ty = elem_ty;
    if (cap == 0) return GEAS_OK;
    GeasValue* data = (GeasValue*)geas_bytes(c, cap * sizeof(GeasValue));
    if (!data) return GEAS_ERR_OOM;
    memset(data, 0, cap * sizeof(GeasValue));
    out->as.list.data = data;
    out->as.list.cap = cap;
    return GEAS_OK;
}

GeasStatus geas_list_push(GeasContract* c, GeasValue* list, const GeasValue* elem) {
    if (!c || !list || !elem) return GEAS_ERR_TYPE;
    if (list->ty != GEAS_TY_LIST) return GEAS_ERR_TYPE;
    if (elem->ty != list->as.list.elem_ty) return GEAS_ERR_TYPE;
    if (list->as.list.len == list->as.list.cap) {
        uint64_t cap = list->as.list.cap ? list->as.list.cap * 2 : 4;
        GeasValue* data = (GeasValue*)geas_bytes(c, cap * sizeof(GeasValue));
        if (!data) return GEAS_ERR_OOM;
        if (list->as.list.len) {
            memcpy(data, list->as.list.data,
                   list->as.list.len * sizeof(GeasValue));
        }
        list->as.list.data = data;
        list->as.list.cap = cap;
    }
    ((GeasValue*)list->as.list.data)[list->as.list.len++] = *elem;
    return GEAS_OK;
}

const GeasValue* geas_list_get(const GeasValue* v, uint64_t idx) {
    if (!v) return NULL;
    if (v->ty != GEAS_TY_LIST && v->ty != GEAS_TY_TUPLE) return NULL;
    if (idx >= v->as.list.len) return NULL;
    return (const GeasValue*)v->as.list.data + idx;
}

/* Overwrites one live slot in place. No allocation happens here, so no
 * contract rides the call; the element must already be instance owned, the
 * same rule geas_list_push states. Out of range is GEAS_ERR_TYPE, the status a
 * compiled index assignment returns from its pledge on a bad index. */
GeasStatus geas_list_set(GeasValue* list, uint64_t idx, const GeasValue* elem) {
    if (!list || !elem) return GEAS_ERR_TYPE;
    if (list->ty != GEAS_TY_LIST) return GEAS_ERR_TYPE;
    if (elem->ty != list->as.list.elem_ty) return GEAS_ERR_TYPE;
    if (idx >= list->as.list.len) return GEAS_ERR_TYPE;
    ((GeasValue*)list->as.list.data)[idx] = *elem;
    return GEAS_OK;
}

/* ---- maps ---- */

/* A map rides the list arm: the data pointer holds interleaved key, value
 * pairs, entries[2i] the key and entries[2i+1] its value, len counts slots so
 * it is always twice the pair count, and elem_ty is the key's tag alone; the
 * value type is the checker's knowledge, not the runtime's. Pairs stay in
 * insertion order, which is the order any serialization sees. Lookup and
 * insert are a linear scan over the keys through geas_value_eq, O(n) in the
 * pair count, the v1 tradeoff that keeps the repr one arm deep. */

GeasValue geas_map_new(GeasContract* c, uint32_t key_ty) {
    (void)c; /* an empty map allocates nothing; the ctx rides for symmetry */
    GeasValue v;
    memset(&v, 0, sizeof(v));
    v.ty = GEAS_TY_MAP;
    v.as.list.elem_ty = key_ty;
    return v;
}

/* The slot index of k's value inside m, or -1 for a miss. Assumes m is a map
 * and k matches its key tag; the public entry points check first. */
static int64_t map_find(const GeasValue* m, const GeasValue* k) {
    const GeasValue* e = (const GeasValue*)m->as.list.data;
    for (uint64_t i = 0; i + 1 < m->as.list.len; i += 2) {
        if (geas_value_eq(&e[i], k)) return (int64_t)(i + 1);
    }
    return -1;
}

GeasStatus geas_map_set(GeasContract* c, GeasValue* m, const GeasValue* k,
                      const GeasValue* v) {
    if (!c || !m || !k || !v) return GEAS_ERR_TYPE;
    if (m->ty != GEAS_TY_MAP) return GEAS_ERR_TYPE;
    if (k->ty != m->as.list.elem_ty) return GEAS_ERR_TYPE;
    /* Both halves are deep copied before anything is committed, so an OOM
     * mid-copy leaves the map exactly as it was. */
    GeasValue vc;
    GeasStatus st = geas_value_deep_copy(c, v, &vc);
    if (st != GEAS_OK) return st;
    int64_t hit = map_find(m, k);
    if (hit >= 0) {
        ((GeasValue*)m->as.list.data)[hit] = vc;
        return GEAS_OK;
    }
    GeasValue kc;
    st = geas_value_deep_copy(c, k, &kc);
    if (st != GEAS_OK) return st;
    if (m->as.list.len + 2 > m->as.list.cap) {
        uint64_t cap = m->as.list.cap ? m->as.list.cap * 2 : 8;
        GeasValue* data = (GeasValue*)geas_bytes(c, cap * sizeof(GeasValue));
        if (!data) return GEAS_ERR_OOM;
        if (m->as.list.len) {
            memcpy(data, m->as.list.data, m->as.list.len * sizeof(GeasValue));
        }
        m->as.list.data = data;
        m->as.list.cap = cap;
    }
    GeasValue* e = (GeasValue*)m->as.list.data;
    e[m->as.list.len] = kc;
    e[m->as.list.len + 1] = vc;
    m->as.list.len += 2;
    return GEAS_OK;
}

int geas_map_get(const GeasValue* m, const GeasValue* k, const GeasValue** out) {
    if (!m || !k || !out) return 0;
    if (m->ty != GEAS_TY_MAP) return 0;
    if (k->ty != m->as.list.elem_ty) return 0;
    int64_t hit = map_find(m, k);
    if (hit < 0) return 0;
    *out = (const GeasValue*)m->as.list.data + hit;
    return 1;
}

/* Structural equality, the recursion mirroring geas_value_deep_copy: what the
 * copy can reach, the compare can test. A tag mismatch reads unequal. A map
 * compares pair by pair in insertion order, keys and values both, the same
 * order semantics serialization promises, so the answer is deterministic. */
int geas_value_eq(const GeasValue* a, const GeasValue* b) {
    if (!a || !b) return 0;
    if (a->ty != b->ty) return 0;
    switch ((GeasTypeTag)a->ty) {
    case GEAS_TY_UNIT:  return 1;
    case GEAS_TY_INT:   return a->as.i == b->as.i;
    case GEAS_TY_UINT:  return a->as.u == b->as.u;
    case GEAS_TY_FLOAT: return a->as.f == b->as.f;
    case GEAS_TY_BOOL:
    case GEAS_TY_BYTE:  return a->as.b == b->as.b;
    case GEAS_TY_CHAR:  return a->as.ch == b->as.ch;
    case GEAS_TY_STRING:
        return geas_string_eq(a, b);
    case GEAS_TY_LIST:
    case GEAS_TY_MAP:
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
    case GEAS_TY_SUM: {
        if (a->tag != b->tag) return 0;
        if (a->as.list.len != b->as.list.len) return 0;
        const GeasValue* xa = (const GeasValue*)a->as.list.data;
        const GeasValue* xb = (const GeasValue*)b->as.list.data;
        for (uint64_t i = 0; i < a->as.list.len; i++) {
            if (!geas_value_eq(&xa[i], &xb[i])) return 0;
        }
        return 1;
    }
    case GEAS_TY_OPTION:
    case GEAS_TY_RESULT: {
        if (a->tag != b->tag) return 0;
        if (!a->as.box && !b->as.box) return 1;
        if (!a->as.box || !b->as.box) return 0;
        return geas_value_eq((const GeasValue*)a->as.box,
                            (const GeasValue*)b->as.box);
    }
    case GEAS_TY_INSTANCE:
        /* An instance value is a reference handle; equality is identity. */
        return a->as.box == b->as.box;
    case GEAS_TY_PLEDGE_REF:
    default:
        return 0;
    }
}

GeasStatus geas_tuple_new(GeasContract* c, uint64_t count, GeasValue* out) {
    if (!c || !out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_TUPLE;
    if (count == 0) return GEAS_OK;
    GeasValue* data = (GeasValue*)geas_bytes(c, count * sizeof(GeasValue));
    if (!data) return GEAS_ERR_OOM;
    memset(data, 0, count * sizeof(GeasValue));
    out->as.list.data = data;
    out->as.list.len = count;
    out->as.list.cap = count;
    return GEAS_OK;
}

/* The recursive workhorse behind copy-in. Scalars are the struct copy, a
 * string copies its bytes, list, map, tuple, record, and sum payloads copy
 * element by element on the shared list arm, a map's interleaved keys and
 * values riding along like any other elements, and Option and Result rebox
 * their payload. */
GeasStatus geas_value_deep_copy(GeasContract* c, const GeasValue* src,
                              GeasValue* dst) {
    if (!c || !src || !dst) return GEAS_ERR_TYPE;
    switch ((GeasTypeTag)src->ty) {
    case GEAS_TY_UNIT:
    case GEAS_TY_INT:
    case GEAS_TY_UINT:
    case GEAS_TY_FLOAT:
    case GEAS_TY_BOOL:
    case GEAS_TY_BYTE:
    case GEAS_TY_CHAR:
    case GEAS_TY_PLEDGE_REF:
    case GEAS_TY_INSTANCE:
        /* An instance value is a reference handle, the one deliberate value
         * semantics exception: the copy shares the instance. Internal only;
         * the ABI never carries this tag across the boundary. */
        *dst = *src;
        return GEAS_OK;
    case GEAS_TY_STRING:
        *dst = geas_string_copy(c, src->as.s.ptr, src->as.s.len);
        if (src->as.s.len && !dst->as.s.ptr) return GEAS_ERR_OOM;
        return GEAS_OK;
    case GEAS_TY_LIST:
    case GEAS_TY_MAP:
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
    case GEAS_TY_SUM: {
        uint64_t n = src->as.list.len;
        memset(dst, 0, sizeof(*dst));
        dst->ty = src->ty;
        dst->tag = src->tag;
        dst->as.list.elem_ty = src->as.list.elem_ty;
        dst->as.list.len = n;
        dst->as.list.cap = n;
        if (n == 0) return GEAS_OK;
        GeasValue* data = (GeasValue*)geas_bytes(c, n * sizeof(GeasValue));
        if (!data) return GEAS_ERR_OOM;
        const GeasValue* from = (const GeasValue*)src->as.list.data;
        for (uint64_t i = 0; i < n; i++) {
            GeasStatus st = geas_value_deep_copy(c, &from[i], &data[i]);
            if (st != GEAS_OK) return st;
        }
        dst->as.list.data = data;
        return GEAS_OK;
    }
    case GEAS_TY_OPTION:
    case GEAS_TY_RESULT: {
        memset(dst, 0, sizeof(*dst));
        dst->ty = src->ty;
        dst->tag = src->tag;
        if (!src->as.box) return GEAS_OK;
        GeasValue* boxed = geas_box(c);
        if (!boxed) return GEAS_ERR_OOM;
        GeasStatus st = geas_value_deep_copy(c, (const GeasValue*)src->as.box,
                                           boxed);
        if (st != GEAS_OK) return st;
        dst->as.box = boxed;
        return GEAS_OK;
    }
    default:
        return GEAS_ERR_TYPE;
    }
}

/* ---- the debug renderer ---- */

/* A counting sink: pos advances for every byte the render wants, and bytes
 * land in buf only while they fit. The render runs once with a NULL buf to
 * size the text and once more to write it, so a too-small cap writes
 * nothing, the same promise geas_iname_dump makes. */
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
static void render_string(RenderSink* s, const GeasString* str) {
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

#define GEAS_RENDER_DEPTH 8

static void render_value(RenderSink* s, const GeasValue* v, unsigned depth);

static void render_elems(RenderSink* s, const GeasValue* v, unsigned depth,
                         const char* open, const char* close) {
    sink_str(s, open);
    const GeasValue* xs = (const GeasValue*)v->as.list.data;
    for (uint64_t i = 0; i < v->as.list.len; i++) {
        if (i) sink_str(s, ", ");
        render_value(s, &xs[i], depth + 1);
    }
    sink_str(s, close);
}

/* A map in its canonical spelling: {k: v, ...} pairs in insertion order,
 * which is the only order a map has. */
static void render_map(RenderSink* s, const GeasValue* v, unsigned depth) {
    sink_put(s, "{", 1);
    const GeasValue* xs = (const GeasValue*)v->as.list.data;
    for (uint64_t i = 0; i + 1 < v->as.list.len; i += 2) {
        if (i) sink_str(s, ", ");
        render_value(s, &xs[i], depth + 1);
        sink_str(s, ": ");
        render_value(s, &xs[i + 1], depth + 1);
    }
    sink_put(s, "}", 1);
}

static void render_box(RenderSink* s, const GeasValue* v, unsigned depth,
                       const char* name) {
    sink_str(s, name);
    sink_put(s, "(", 1);
    if (v->as.box) {
        render_value(s, (const GeasValue*)v->as.box, depth + 1);
    } else {
        sink_put(s, "?", 1);
    }
    sink_put(s, ")", 1);
}

static void render_value(RenderSink* s, const GeasValue* v, unsigned depth) {
    if (depth > GEAS_RENDER_DEPTH) {
        sink_str(s, "...");
        return;
    }
    switch ((GeasTypeTag)v->ty) {
    case GEAS_TY_UNIT:   sink_str(s, "()"); return;
    case GEAS_TY_INT:    sink_fmt(s, "%lld", (long long)v->as.i); return;
    case GEAS_TY_UINT:   sink_fmt(s, "%llu", (unsigned long long)v->as.u); return;
    case GEAS_TY_FLOAT:  sink_fmt(s, "%g", v->as.f); return;
    case GEAS_TY_BOOL:   sink_str(s, v->as.b ? "true" : "false"); return;
    case GEAS_TY_BYTE:   sink_fmt(s, "%u", (unsigned)v->as.b); return;
    case GEAS_TY_CHAR:   sink_fmt(s, "U+%04X", (unsigned)v->as.ch); return;
    case GEAS_TY_STRING: render_string(s, &v->as.s); return;
    case GEAS_TY_LIST:   render_elems(s, v, depth, "[", "]"); return;
    case GEAS_TY_TUPLE:  render_elems(s, v, depth, "(", ")"); return;
    case GEAS_TY_RECORD: render_elems(s, v, depth, "{", "}"); return;
    case GEAS_TY_SUM:
        sink_fmt(s, "#%u", (unsigned)v->tag);
        if (v->as.list.len) render_elems(s, v, depth, "(", ")");
        return;
    case GEAS_TY_OPTION:
        if (v->tag == 0) { sink_str(s, "None"); return; }
        render_box(s, v, depth, "Some");
        return;
    case GEAS_TY_RESULT:
        render_box(s, v, depth, v->tag == 0 ? "Ok" : "Err");
        return;
    case GEAS_TY_MAP:        render_map(s, v, depth); return;
    case GEAS_TY_PLEDGE_REF: sink_str(s, "<pledge>"); return;
    case GEAS_TY_INSTANCE:   sink_str(s, "<instance>"); return;
    default:                sink_str(s, "<?>"); return;
    }
}

/* Renders a value in its canonical debug spelling, the text the emitted
 * standalone wrapper prints for a Main.run Err. The size protocol is
 * geas_iname_dump's: *need receives the full size including the NUL, a cap at
 * least that writes the text, anything smaller writes nothing and reports
 * GEAS_ERR_OOM, so a NULL buf with cap 0 sizes the buffer. */
GeasStatus geas_value_render(const GeasValue* v, char* buf, size_t cap,
                           size_t* need) {
    if (!v || !need) return GEAS_ERR_TYPE;
    if (!buf) cap = 0;
    RenderSink size_pass = { NULL, 0, 0 };
    render_value(&size_pass, v, 0);
    *need = size_pass.pos + 1;
    if (cap < *need) return GEAS_ERR_OOM;
    RenderSink write_pass = { buf, cap, 0 };
    render_value(&write_pass, v, 0);
    buf[write_pass.pos] = '\0';
    return GEAS_OK;
}
