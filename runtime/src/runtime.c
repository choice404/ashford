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
#include <ash/ash_store.h>
#include <ash/ash_wire.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* The per-subcontract transaction state, one slot per descriptor sub for a
 * store-backed contract. A transactional subcontract opens its episode lazily
 * on the first fulfillment of one of its pledges, so TXN_NONE is the fresh
 * state, TXN_OPEN a live transaction on the connection, and TXN_DONE the one
 * commit or rollback that closes it. Because an episode is one outcome and not
 * a latch, a pledge whose sub reads TXN_DONE is refused ASH_ERR_STATE rather
 * than run again. */
enum {
    TXN_NONE = 0,
    TXN_OPEN = 1,
    TXN_DONE = 2
};

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

    /* The latches, one slot per descriptor pledge, and the first Err payload
     * each broken pledge carried. Both arrays are plain heap freed at
     * shutdown, not instance blocks, so the partial surface stays readable
     * after a break; the payload structs point into the instance heap, so an
     * explicit break zeroes them when it frees that heap, while an automatic
     * break leaves both alone on purpose, the errors are what it reports. */
    uint8_t*               pledge_state; /* PLEDGE_*, one per pledge */
    AshValue*              pledge_err;   /* first Err payload per pledge */

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
    AshStore*              store;
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
    for (size_t i = 0; i < rt->ninstances; i++) {
        AshContract* c = rt->instances[i];
        struct AshFuture* f = c->futures;
        while (f) {
            struct AshFuture* next = f->next;
            future_free(f);
            f = next;
        }
        if (c->store) {
            ash_store_close(c->store);
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
        if (rt->inames[i].kind == ASH_INAME_CONTRACT) {
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

/* ---- the store path ---- */

/* Rows and their bytes allocate onto the signing instance through this hook, so
 * the driver stays as pure over an allocator as the wire codec did: S0 handed
 * it a plain arena, S1 hands it the instance, and a row read out of a table
 * lives on the instance heap and dies at its break like every other value. */
static uint8_t* instance_store_bytes(void* ctx, uint64_t n) {
    return ash_bytes((AshContract*)ctx, n);
}

/* Opens the store connection at sign and reconciles every schema against the
 * live database in the same call, the whole store side of sign. A store-backed
 * contract, one with at least one schema, binds a database named by its dsn
 * vow: an absent dsn vow, the same gap an unsupplied vow always hit, is
 * ASH_ERR_UNBOUND; a dsn that will not open is ASH_ERR_STORE; a schema that
 * will not reconcile is ASH_ERR_TYPE, and none of them leaves a half open
 * connection on the instance. A contract with no schema takes this path as a no
 * op and signs with no database, exactly as every contract did before the
 * store layer. */
static AshStatus store_sign_reconcile(AshContract* c) {
    const AshContractDesc* desc = c->desc;
    if (desc->nschemas == 0) return ASH_OK;

    int slot = -1;
    for (uint32_t i = 0; i < desc->nvows; i++) {
        if (strcmp(desc->vows[i].name, "dsn") == 0) { slot = (int)i; break; }
    }
    if (slot < 0 || !c->vow_vals) return ASH_ERR_UNBOUND;
    const AshValue* d = &c->vow_vals[slot];
    if (d->ty != ASH_TY_STRING) return ASH_ERR_UNBOUND;

    char* dsn = (char*)malloc((size_t)d->as.s.len + 1);
    if (!dsn) return ASH_ERR_OOM;
    if (d->as.s.len) memcpy(dsn, d->as.s.ptr, (size_t)d->as.s.len);
    dsn[d->as.s.len] = 0;

    AshStore* s = NULL;
    AshStatus st = ash_store_open(dsn, &s);
    free(dsn);
    if (st != ASH_OK) return st;

    for (uint32_t i = 0; i < desc->nschemas; i++) {
        st = ash_store_reconcile(s, &desc->schemas[i]);
        if (st != ASH_OK) {
            ash_store_close(s);
            return st;
        }
    }
    c->store = s;
    return ASH_OK;
}

/* The column tags of a schema in one heap array, the col_types the query API
 * decodes each row against. */
static uint32_t* store_col_types(const AshSchemaDesc* s) {
    uint32_t* ct = (uint32_t*)malloc((size_t)s->ncols * sizeof(uint32_t));
    if (!ct) return NULL;
    for (uint32_t i = 0; i < s->ncols; i++) ct[i] = s->cols[i].ty;
    return ct;
}

/* The byte budget a column name list needs, the shared sizing every builder
 * leans on so no fixed cap can clip a wide schema. */
static size_t store_cols_span(const AshSchemaDesc* s) {
    size_t n = 0;
    for (uint32_t i = 0; i < s->ncols; i++) n += strlen(s->cols[i].name) + 4;
    return n;
}

/* SELECT c0, c1, ... FROM table WHERE pk=?, the one row lookup by primary key. */
static char* store_sql_select(const AshSchemaDesc* s) {
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
static char* store_sql_select_eq(const AshSchemaDesc* s, uint32_t col) {
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
static char* store_sql_select_where(const AshSchemaDesc* s,
                                    const AshStoreTerm* terms,
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

/* INSERT INTO table(c0, ...) VALUES(?, ...), every column bound positionally. */
static char* store_sql_insert(const AshSchemaDesc* s) {
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
static char* store_sql_update(const AshSchemaDesc* s) {
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
static AshStatus store_ok(AshContract* c, const AshValue* payload, AshValue* out) {
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    *box = *payload;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

/* Ok(Unit), the result the write forms report. */
static AshStatus store_ok_unit(AshContract* c, AshValue* out) {
    AshValue unit;
    memset(&unit, 0, sizeof(unit));
    unit.ty = ASH_TY_UNIT;
    return store_ok(c, &unit, out);
}

/* The row parameters an insert or update binds: a record's fields are the
 * schema's columns in declaration order, so its field array is the parameter
 * frame as it stands. A value that is not a record of the right width is a
 * codegen bug, refused here as ASH_ERR_TYPE rather than bound wrong. */
static const AshValue* store_row_params(const AshSchemaDesc* s,
                                        const AshValue* row) {
    if (!row || row->ty != ASH_TY_RECORD) return NULL;
    if (row->as.list.len != s->ncols) return NULL;
    return (const AshValue*)row->as.list.data;
}

AshStatus ash_store_find(AshContract* c, const AshSchemaDesc* schema,
                         const AshValue* key, AshValue* out) {
    if (!c || !schema || !key || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_ERR_STORE;
    if (c->store && schema->ncols > 0) {
        char* sql = store_sql_select(schema);
        uint32_t* cts = store_col_types(schema);
        if (!sql || !cts) {
            st = ASH_ERR_OOM;
        } else {
            AshStoreAlloc alloc = { instance_store_bytes, c };
            AshValue rows;
            st = ash_store_query(c->store, sql, key, 1, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == ASH_OK) {
                AshValue opt;
                memset(&opt, 0, sizeof(opt));
                opt.ty = ASH_TY_OPTION;
                if (rows.as.list.len > 0) {
                    AshValue* box = ash_box(c);
                    if (!box) {
                        st = ASH_ERR_OOM;
                    } else {
                        *box = ((AshValue*)rows.as.list.data)[0];
                        opt.tag = 1;
                        opt.as.box = box;
                    }
                }
                if (st == ASH_OK) st = store_ok(c, &opt, out);
            }
        }
        free(sql);
        free(cts);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

AshStatus ash_store_query_eq(AshContract* c, const AshSchemaDesc* schema,
                             uint32_t col, const AshValue* value,
                             AshValue* out) {
    if (!c || !schema || !value || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || col >= schema->ncols) return ASH_ERR_TYPE;
    if (value->ty != schema->cols[col].ty) return ASH_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_select_eq(schema, col);
        uint32_t* cts = store_col_types(schema);
        if (!sql || !cts) {
            st = ASH_ERR_OOM;
        } else {
            AshStoreAlloc alloc = { instance_store_bytes, c };
            AshValue rows;
            st = ash_store_query(c->store, sql, value, 1, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == ASH_OK) st = store_ok(c, &rows, out);
        }
        free(sql);
        free(cts);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

AshStatus ash_store_query_where(AshContract* c, const AshSchemaDesc* schema,
                                const AshStoreTerm* terms, uint32_t nterms,
                                AshValue* out) {
    if (!c || !schema || !terms || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (schema->ncols == 0 || nterms == 0) return ASH_ERR_TYPE;
    for (uint32_t i = 0; i < nterms; i++) {
        if (terms[i].col >= schema->ncols) return ASH_ERR_TYPE;
        if (terms[i].cmp > ASH_CMP_GE) return ASH_ERR_TYPE;
        if (!terms[i].value) return ASH_ERR_TYPE;
        if (terms[i].value->ty != schema->cols[terms[i].col].ty) return ASH_ERR_TYPE;
    }
    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_select_where(schema, terms, nterms);
        uint32_t* cts = store_col_types(schema);
        AshValue* params = (AshValue*)malloc((size_t)nterms * sizeof(AshValue));
        if (!sql || !cts || !params) {
            st = ASH_ERR_OOM;
        } else {
            for (uint32_t i = 0; i < nterms; i++) params[i] = *terms[i].value;
            AshStoreAlloc alloc = { instance_store_bytes, c };
            AshValue rows;
            st = ash_store_query(c->store, sql, params, nterms, cts, NULL,
                                 schema->ncols, &alloc, &rows);
            if (st == ASH_OK) st = store_ok(c, &rows, out);
        }
        free(sql);
        free(cts);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

AshStatus ash_store_insert(AshContract* c, const AshSchemaDesc* schema,
                           const AshValue* row, AshValue* out) {
    if (!c || !schema || !row || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    const AshValue* params = store_row_params(schema, row);
    if (!params) return ASH_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_insert(schema);
        if (!sql) {
            st = ASH_ERR_OOM;
        } else {
            st = ash_store_exec_params(c->store, sql, params, schema->ncols, NULL);
            if (st == ASH_OK) st = store_ok_unit(c, out);
        }
        free(sql);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

AshStatus ash_store_update(AshContract* c, const AshSchemaDesc* schema,
                           const AshValue* key, const AshValue* row,
                           AshValue* out) {
    if (!c || !schema || !key || !row || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    const AshValue* rowp = store_row_params(schema, row);
    if (!rowp) return ASH_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_ERR_STORE;
    if (c->store) {
        char* sql = store_sql_update(schema);
        AshValue* params = (AshValue*)malloc((size_t)(schema->ncols + 1) *
                                             sizeof(AshValue));
        if (!sql || !params) {
            st = ASH_ERR_OOM;
        } else {
            memcpy(params, rowp, (size_t)schema->ncols * sizeof(AshValue));
            params[schema->ncols] = *key;
            st = ash_store_exec_params(c->store, sql, params,
                                       (size_t)schema->ncols + 1, NULL);
            if (st == ASH_OK) st = store_ok_unit(c, out);
        }
        free(sql);
        free(params);
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

AshStatus ash_store_delete(AshContract* c, const AshSchemaDesc* schema,
                           const AshValue* key, AshValue* out) {
    if (!c || !schema || !key || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_ERR_STORE;
    if (c->store && schema->ncols > 0) {
        size_t need = 48 + strlen(schema->table) + strlen(schema->cols[0].name);
        char* sql = (char*)malloc(need);
        if (!sql) {
            st = ASH_ERR_OOM;
        } else {
            snprintf(sql, need, "DELETE FROM %s WHERE %s=?", schema->table,
                     schema->cols[0].name);
            st = ash_store_exec_params(c->store, sql, key, 1, NULL);
            if (st == ASH_OK) st = store_ok_unit(c, out);
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
        pthread_mutex_unlock(&rt->lock);
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
    /* One transaction slot per subcontract, all TXN_NONE, so the first
     * fulfillment of a transactional subcontract opens its episode lazily. A
     * contract with no subcontract carries none. */
    if (st == ASH_OK && desc->nsubs > 0) {
        c->sub_txn = calloc(desc->nsubs, sizeof(uint8_t));
        if (!c->sub_txn) st = ASH_ERR_OOM;
    }
    /* The store side of sign: open the bound dsn and reconcile every schema, so
     * a store-backed contract that activates has a live, shape checked
     * connection, and one that fails to open or reconcile never publishes. */
    if (st == ASH_OK) st = store_sign_reconcile(c);
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
        if (c->store) {
            ash_store_close(c->store);
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
    return ASH_OK;
}

AshContractState ash_contract_state(const AshContract* c) {
    if (!c) return ASH_UNSIGNED;
    AshContract* mc = (AshContract*)c;
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
    /* The store side of break, in the order docs/database.md fixes: roll back
     * any open transaction, then close the connection, then reclaim the heap.
     * S1 is autocommit so a rollback usually finds nothing open, but it runs
     * anyway so no uncommitted write can ever survive a break, and the close
     * lands before the heap the rows live on goes away. */
    if (c->store) {
        ash_store_rollback(c->store);
        ash_store_close(c->store);
        c->store = NULL;
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
    pthread_mutex_lock(&c->mu);
    size_t n = partial_scan(c, k, 0, NULL);
    pthread_mutex_unlock(&c->mu);
    return n;
}

const char* ash_partial_name(AshContract* c, AshItemState k, size_t i) {
    if (!c) return NULL;
    const char* name = NULL;
    pthread_mutex_lock(&c->mu);
    partial_scan(c, k, i, &name);
    pthread_mutex_unlock(&c->mu);
    return name;
}

size_t ash_partial_nerrors(AshContract* c) {
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

AshStatus ash_partial_error(AshContract* c, size_t i,
                            const char** pledge_name, const AshValue** err) {
    if (!c) return ASH_ERR_TYPE;
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

/* ---- transactional subcontracts ---- */

/* The subcontract a pledge belongs to when that subcontract is transactional,
 * or -1. A loose pledge, a sub index outside the table, and a subcontract
 * without the flag all read -1, so the store transaction machinery is a no-op
 * for every pledge outside a transactional group and for every contract that
 * carries no sub_flags at all. */
static int32_t pledge_txn_sub(const AshContractDesc* d, uint32_t pidx) {
    if (pidx >= d->npledges) return -1;
    int32_t s = d->pledges[pidx].sub;
    if (s < 0 || (uint32_t)s >= d->nsubs) return -1;
    if (!d->sub_flags) return -1;
    if ((d->sub_flags[s] & ASH_SUB_TRANSACTIONAL) == 0) return -1;
    return s;
}

/* Opens the episode before the thunk runs. A pledge outside any transactional
 * subcontract proceeds untouched; a pledge whose episode already resolved is
 * refused ASH_ERR_STATE, the once-only law a committed transaction earns; the
 * first pledge of a transactional subcontract begins the transaction lazily and
 * marks it open. A begin the backend refuses rides back as ASH_ERR_STORE and
 * the episode stays unopened. Returns ASH_OK to proceed, or the status to
 * deliver instead. Called under the instance lock. */
static AshStatus txn_before(AshContract* c, uint32_t pidx) {
    int32_t s = pledge_txn_sub(c->desc, pidx);
    if (s < 0 || !c->sub_txn || !c->store) return ASH_OK;
    if (c->sub_txn[s] == TXN_DONE) return ASH_ERR_STATE;
    if (c->sub_txn[s] == TXN_NONE) {
        AshStatus st = ash_store_begin(c->store);
        if (st != ASH_OK) return st;
        c->sub_txn[s] = TXN_OPEN;
    }
    return ASH_OK;
}

/* Resolves the episode after one pledge outcome, the whole all-or-nothing rule
 * in one place. A backend or runtime failure mid episode rolls the transaction
 * back and closes it, so no half written episode survives; an Err latched the
 * pledge broken and the buffered writes vanish the instant it lands; an Ok that
 * completes the subcontract commits the whole episode. The completion test is
 * sub_all over PLEDGE_FULFILLED, the same "every pledge of this subcontract
 * latched Ok" predicate the requirements evaluator reads through atom_true and
 * sub_item_state, so the commit point can never drift from the subcontract's
 * own fulfillment. A commit the backend refuses is ASH_ERR_STORE with the
 * episode rolled back. Returns the status to deliver. Called under the instance
 * lock, after latch_pledge and eval_policy. */
static AshStatus txn_after(AshContract* c, uint32_t pidx, AshStatus st,
                           const AshValue* out) {
    int32_t s = pledge_txn_sub(c->desc, pidx);
    if (s < 0 || !c->sub_txn || !c->store || c->sub_txn[s] != TXN_OPEN) {
        return st;
    }
    if (st != ASH_OK) {
        ash_store_rollback(c->store);
        c->sub_txn[s] = TXN_DONE;
        return st;
    }
    if (out && out->ty == ASH_TY_RESULT && out->tag == 1) {
        ash_store_rollback(c->store);
        c->sub_txn[s] = TXN_DONE;
        return st;
    }
    if (sub_all(c, (uint32_t)s, PLEDGE_FULFILLED)) {
        AshStatus cs = ash_store_commit(c->store);
        c->sub_txn[s] = TXN_DONE;
        if (cs != ASH_OK) {
            ash_store_rollback(c->store);
            return ASH_ERR_STORE;
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
static void run_task(struct AshFuture* f) {
    AshContract* c = f->c;
    pthread_mutex_lock(&c->mu);
    if (c->state != ASH_SIGNED && c->state != ASH_PARTIAL &&
        c->state != ASH_FULFILLED) {
        future_finish(f, ASH_ERR_STATE, NULL);
    } else {
        AshStatus gate = txn_before(c, f->pidx);
        if (gate != ASH_OK) {
            future_finish(f, gate, NULL);
        } else {
            AshValue out;
            memset(&out, 0, sizeof(out));
            AshStatus st = f->fn((void*)c, f->frame, f->frame_nargs, &out);
            if (st == ASH_OK) {
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
AshFuture* ash_pledge_fulfill(AshContract* c, const char* pledge_name,
                              const AshValue* args, size_t nargs,
                              const AshRef* refs, size_t nrefs) {
    if (!c || !pledge_name) return NULL;
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
        uint32_t pidx = (uint32_t)(p - c->desc->pledges);
        AshStatus gate = txn_before(c, pidx);
        if (gate != ASH_OK) {
            st = gate;
            break;
        }
        AshValue res;
        memset(&res, 0, sizeof(res));
        AshPledgeFn fn = c->fns[p - c->desc->pledges];
        st = fn((void*)c, frame, p->nargs, &res);
        if (st == ASH_OK) {
            latch_pledge(c, pidx, &res);
            eval_policy(c);
        }
        st = txn_after(c, pidx, st, &res);
        if (st != ASH_OK) break;
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

static AshStatus partial_names_value(AshContract* c, AshContract* owner,
                                     AshItemState k, AshValue* out) {
    size_t n = partial_scan(c, k, 0, NULL);
    AshValue* data = NULL;
    if (n > 0) {
        data = (AshValue*)ash_bytes(owner, (uint64_t)n * sizeof(AshValue));
        if (!data) return ASH_ERR_OOM;
        memset(data, 0, n * sizeof(AshValue));
        for (size_t i = 0; i < n; i++) {
            const char* name = NULL;
            partial_scan(c, k, i, &name);
            if (!name) return ASH_ERR_NAME;
            AshValue s = ash_string_copy(owner, (const uint8_t*)name,
                                         strlen(name));
            if (!s.as.s.ptr && s.as.s.len == 0 && name[0]) {
                return ASH_ERR_OOM;
            }
            data[i] = s;
        }
    }
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_LIST;
    out->as.list.data = data;
    out->as.list.len = n;
    out->as.list.cap = n;
    out->as.list.elem_ty = ASH_TY_STRING;
    return ASH_OK;
}

AshStatus ash_partial_value(AshContract* c, AshContract* owner,
                            AshValue* out) {
    if (!c || !owner || !out) return ASH_ERR_TYPE;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_OK;
    AshValue record;
    memset(&record, 0, sizeof(record));
    AshValue* fields = (AshValue*)ash_bytes(owner, 4 * sizeof(AshValue));
    if (!fields) {
        st = ASH_ERR_OOM;
    } else {
        memset(fields, 0, 4 * sizeof(AshValue));
        fields[0] = ash_state_value(c);
        st = partial_names_value(c, owner, ASH_ITEM_FULFILLED, &fields[1]);
        if (st == ASH_OK) {
            st = partial_names_value(c, owner, ASH_ITEM_PENDING, &fields[2]);
        }
        if (st == ASH_OK) {
            st = partial_names_value(c, owner, ASH_ITEM_BROKEN, &fields[3]);
        }
        if (st == ASH_OK) {
            record.ty = ASH_TY_RECORD;
            record.as.list.data = fields;
            record.as.list.len = 4;
            record.as.list.cap = 4;
            record.as.list.elem_ty = 0;
            *out = record;
        }
    }
    if (st != ASH_OK) memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&c->mu);
    return st;
}

/* ---- the parked instance ---- */

/* The park row's schema, one table the runtime owns in whatever database the
 * caller names. The blobs are the wire codec's canonical encoding, the same
 * bytes the network trusts, so a parked value round trips exactly and the
 * codec's goldens stand guard over the park format for free. */
static const char* PARK_DDL =
    "CREATE TABLE IF NOT EXISTS ash_park ("
    "pkey TEXT PRIMARY KEY, contract TEXT, version INTEGER, "
    "shape_hash INTEGER, state INTEGER, signed_at INTEGER, "
    "vows BLOB, latches BLOB, errs BLOB, subtxn BLOB)";

/* Whether a stored Err payload is present: an explicit break zeroes the
 * struct, and a pledge that never broke never wrote one, so all zero is the
 * absent spelling here exactly as it is on the partial surface. */
static int park_err_present(const AshValue* v) {
    static const AshValue zero;
    return memcmp(v, &zero, sizeof(AshValue)) != 0;
}

/* Encodes n values back to back into one malloc'd buffer, the sequential
 * form both blobs share: the decoder walks by each value's own consumed
 * count, so no frame or count rides in the bytes. */
static AshStatus park_encode_values(const AshValue* vals, size_t n,
                                    uint8_t** buf_out, size_t* len_out) {
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        size_t need = 0;
        /* The sizing call: a NULL buffer answers ASH_ERR_OOM with *need set,
         * the codec's own size protocol, so only another status is a fault. */
        AshStatus st = ash_wire_encode_value(&vals[i], NULL, 0, &need);
        if (st != ASH_OK && st != ASH_ERR_OOM) return st;
        total += need;
    }
    uint8_t* buf = malloc(total ? total : 1);
    if (!buf) return ASH_ERR_OOM;
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t need = 0;
        AshStatus st = ash_wire_encode_value(&vals[i], buf + off, total - off,
                                             &need);
        if (st != ASH_OK) {
            free(buf);
            return st;
        }
        off += need;
    }
    *buf_out = buf;
    *len_out = total;
    return ASH_OK;
}

/* The Err payload blob: per pledge one presence byte, then the encoded value
 * when present. Sized first, then written, the codec's own two pass shape. */
static AshStatus park_encode_errs(const AshContract* c,
                                  uint8_t** buf_out, size_t* len_out) {
    uint32_t n = c->desc->npledges;
    size_t total = n;
    for (uint32_t i = 0; i < n; i++) {
        if (!park_err_present(&c->pledge_err[i])) continue;
        size_t need = 0;
        AshStatus st = ash_wire_encode_value(&c->pledge_err[i], NULL, 0, &need);
        if (st != ASH_OK && st != ASH_ERR_OOM) return st;
        total += need;
    }
    uint8_t* buf = malloc(total ? total : 1);
    if (!buf) return ASH_ERR_OOM;
    size_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        int present = park_err_present(&c->pledge_err[i]);
        buf[off++] = (uint8_t)(present ? 1 : 0);
        if (!present) continue;
        size_t need = 0;
        AshStatus st = ash_wire_encode_value(&c->pledge_err[i], buf + off,
                                             total - off, &need);
        if (st != ASH_OK) {
            free(buf);
            return st;
        }
        off += need;
    }
    *buf_out = buf;
    *len_out = total;
    return ASH_OK;
}

static AshValue park_str_param(const uint8_t* p, size_t n) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)(n ? p : (const uint8_t*)"");
    v.as.s.len = n;
    return v;
}

static AshValue park_int_param(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

AshStatus ash_instance_park(AshContract* c, const char* dsn, const char* key) {
    if (!c || !dsn || !key) return ASH_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    AshStatus st = ASH_OK;
    if (c->state == ASH_UNSIGNED) st = ASH_ERR_STATE;
    /* An explicit break reclaimed the instance heap the vows and payloads
     * live on; the latches still read, but there is nothing left to write
     * down. An automatic break keeps that heap on purpose and parks fine. */
    if (st == ASH_OK && c->desc->npledges > 0 && !c->owned) st = ASH_ERR_STATE;
    /* A park is a state between walks: an unwaited future is a walk still in
     * the air, and an open transactional episode holds buffered writes no row
     * can carry. Both refuse rather than guess. */
    if (st == ASH_OK) {
        for (struct AshFuture* f = c->futures; f; f = f->next) {
            if (!f->waited) {
                st = ASH_ERR_STATE;
                break;
            }
        }
    }
    if (st == ASH_OK && c->sub_txn) {
        for (uint32_t i = 0; i < c->desc->nsubs; i++) {
            if (c->sub_txn[i] == TXN_OPEN) {
                st = ASH_ERR_STATE;
                break;
            }
        }
    }
    uint8_t* vows_buf = NULL;
    size_t vows_len = 0;
    uint8_t* errs_buf = NULL;
    size_t errs_len = 0;
    if (st == ASH_OK && c->desc->nvows > 0) {
        st = park_encode_values(c->vow_vals, c->desc->nvows,
                                &vows_buf, &vows_len);
    }
    if (st == ASH_OK && c->desc->npledges > 0) {
        st = park_encode_errs(c, &errs_buf, &errs_len);
    }
    AshStore* s = NULL;
    if (st == ASH_OK) st = ash_store_open(dsn, &s);
    if (st == ASH_OK) st = ash_store_exec(s, PARK_DDL);
    if (st == ASH_OK) {
        AshValue params[10];
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
        st = ash_store_exec_params(s,
            "INSERT OR REPLACE INTO ash_park "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
            params, 10, NULL);
    }
    ash_store_close(s);
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

AshStatus ash_instance_resume(AshRuntime* rt, const char* dsn, const char* key,
                              uint64_t expected_hash, AshContract** out) {
    if (!rt || !dsn || !key || !out) return ASH_ERR_TYPE;
    *out = NULL;

    AshStore* s = NULL;
    AshStatus st = ash_store_open(dsn, &s);
    if (st != ASH_OK) return st;
    st = ash_store_exec(s, PARK_DDL);
    ParkChunk* arena = NULL;
    AshValue rows;
    memset(&rows, 0, sizeof(rows));
    if (st == ASH_OK) {
        AshValue kv = park_str_param((const uint8_t*)key, strlen(key));
        static const uint32_t cols[9] = {
            ASH_TY_STRING, ASH_TY_INT, ASH_TY_UINT, ASH_TY_INT, ASH_TY_INT,
            ASH_TY_STRING, ASH_TY_STRING, ASH_TY_STRING, ASH_TY_STRING
        };
        AshStoreAlloc alloc = { park_arena_bytes, &arena };
        st = ash_store_query(s,
            "SELECT contract, version, shape_hash, state, signed_at, "
            "vows, latches, errs, subtxn FROM ash_park WHERE pkey = ?1",
            &kv, 1, cols, NULL, 9, &alloc, &rows);
    }
    ash_store_close(s);
    if (st != ASH_OK) {
        park_arena_free(arena);
        return st;
    }
    if (rows.as.list.len == 0) {
        park_arena_free(arena);
        return ASH_ERR_NAME;
    }
    const AshValue* f = (const AshValue*)((const AshValue*)rows.as.list.data)[0]
                            .as.list.data;
    const AshString rname   = f[0].as.s;
    const int64_t   rver    = f[1].as.i;
    const uint64_t  rshape  = f[2].as.u;
    const int64_t   rstate  = f[3].as.i;
    const int64_t   rsigned = f[4].as.i;
    const AshString bvows   = f[5].as.s;
    const AshString blatch  = f[6].as.s;
    const AshString berrs   = f[7].as.s;
    const AshString bsubtxn = f[8].as.s;

    if (rstate < (int64_t)ASH_SIGNED || rstate > (int64_t)ASH_BROKEN) {
        park_arena_free(arena);
        return ASH_ERR_TYPE;
    }
    char* name = malloc(rname.len + 1);
    if (!name) {
        park_arena_free(arena);
        return ASH_ERR_OOM;
    }
    memcpy(name, rname.ptr, rname.len);
    name[rname.len] = 0;

    pthread_mutex_lock(&rt->lock);
    const AshContractDesc* desc = find_desc(rt, name);
    free(name);
    if (!desc) {
        pthread_mutex_unlock(&rt->lock);
        park_arena_free(arena);
        return ASH_ERR_NAME;
    }
    /* The row must describe the module this runtime registered, and a caller
     * pinning a hash must agree with both, the same skew rule sign runs. */
    if ((int64_t)desc->version != rver || desc->shape_hash != rshape ||
        (expected_hash != 0 && expected_hash != desc->shape_hash)) {
        pthread_mutex_unlock(&rt->lock);
        park_arena_free(arena);
        return ASH_ERR_VERSION;
    }
    AshPledgeFn* fns = NULL;
    st = resolve_dispatch(rt, desc, &fns);
    pthread_mutex_unlock(&rt->lock);
    if (st != ASH_OK) {
        park_arena_free(arena);
        return st;
    }

    AshContract* c = calloc(1, sizeof(AshContract));
    if (!c) {
        free(fns);
        park_arena_free(arena);
        return ASH_ERR_OOM;
    }
    if (mutex_init_recursive(&c->mu) != 0) {
        free(c);
        free(fns);
        park_arena_free(arena);
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

    /* The vows decode straight onto the new instance in declaration order,
     * each checked against its declared type, and the blob must hold exactly
     * the declared count, no more and no less. */
    if (st == ASH_OK && desc->nvows > 0) {
        c->vow_vals = (AshValue*)ash_bytes(c, desc->nvows * sizeof(AshValue));
        if (!c->vow_vals) st = ASH_ERR_OOM;
        else memset(c->vow_vals, 0, desc->nvows * sizeof(AshValue));
        size_t off = 0;
        for (uint32_t i = 0; st == ASH_OK && i < desc->nvows; i++) {
            size_t used = 0;
            st = ash_wire_decode_value(c, bvows.ptr + off, bvows.len - off,
                                       &c->vow_vals[i], &used);
            if (st == ASH_OK && c->vow_vals[i].ty != desc->vows[i].ty) {
                st = ASH_ERR_TYPE;
            }
            off += used;
        }
        if (st == ASH_OK && off != bvows.len) st = ASH_ERR_TYPE;
    } else if (st == ASH_OK && bvows.len != 0) {
        st = ASH_ERR_TYPE;
    }

    /* The latches replay byte for byte, then each present Err payload decodes
     * onto the instance heap, the home the partial surface expects. */
    if (st == ASH_OK && desc->npledges > 0) {
        c->pledge_state = calloc(desc->npledges, sizeof(uint8_t));
        c->pledge_err = calloc(desc->npledges, sizeof(AshValue));
        if (!c->pledge_state || !c->pledge_err) st = ASH_ERR_OOM;
        if (st == ASH_OK && blatch.len != desc->npledges) st = ASH_ERR_TYPE;
        for (uint32_t i = 0; st == ASH_OK && i < desc->npledges; i++) {
            if (blatch.ptr[i] > PLEDGE_BROKEN) st = ASH_ERR_TYPE;
            else c->pledge_state[i] = blatch.ptr[i];
        }
        size_t off = 0;
        for (uint32_t i = 0; st == ASH_OK && i < desc->npledges; i++) {
            if (off >= berrs.len) {
                st = ASH_ERR_TYPE;
                break;
            }
            uint8_t present = berrs.ptr[off++];
            if (present > 1) {
                st = ASH_ERR_TYPE;
            } else if (present == 1) {
                size_t used = 0;
                st = ash_wire_decode_value(c, berrs.ptr + off, berrs.len - off,
                                           &c->pledge_err[i], &used);
                off += used;
            }
        }
        if (st == ASH_OK && off != berrs.len) st = ASH_ERR_TYPE;
    }

    /* The transactional fates replay too: TXN_DONE stays done, so a resumed
     * episode can never run twice, and a recorded TXN_OPEN is a row park
     * refused to write, so reading one is corruption, not state. */
    if (st == ASH_OK && desc->nsubs > 0) {
        c->sub_txn = calloc(desc->nsubs, sizeof(uint8_t));
        if (!c->sub_txn) st = ASH_ERR_OOM;
        if (st == ASH_OK && bsubtxn.len != desc->nsubs) st = ASH_ERR_TYPE;
        for (uint32_t i = 0; st == ASH_OK && i < desc->nsubs; i++) {
            if (bsubtxn.ptr[i] == TXN_OPEN || bsubtxn.ptr[i] > TXN_DONE) {
                st = ASH_ERR_TYPE;
            } else {
                c->sub_txn[i] = bsubtxn.ptr[i];
            }
        }
    }

    if (st == ASH_OK) st = store_sign_reconcile(c);
    if (st == ASH_OK) {
        c->state = (AshContractState)rstate;
        c->shape_hash = desc->shape_hash;
        c->signed_at = rsigned;
        pthread_mutex_lock(&rt->lock);
        if (rt->ninstances == ASH_MAX_INSTANCES) {
            st = ASH_ERR_OOM;
        } else {
            rt->instances[rt->ninstances++] = c;
        }
        pthread_mutex_unlock(&rt->lock);
    }
    park_arena_free(arena);
    if (st != ASH_OK) {
        if (c->store) {
            ash_store_close(c->store);
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
    return ASH_OK;
}

/* The canonical state spellings, the strings instance.status() answers in
 * the language and any host may print. Static storage, never freed. */
const char* ash_state_name(AshContractState s) {
    switch (s) {
    case ASH_UNSIGNED:  return "Unsigned";
    case ASH_SIGNED:    return "Signed";
    case ASH_FULFILLED: return "Fulfilled";
    case ASH_PARTIAL:   return "Partial";
    case ASH_BROKEN:    return "Broken";
    }
    return "Unknown";
}

AshValue ash_state_value(const AshContract* c) {
    const char* n = ash_state_name(ash_contract_state(c));
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)n;
    v.as.s.len = strlen(n);
    return v;
}

/* The NUL terminated copy of a String value the park calls take their dsn
 * and key through when the caller holds values rather than C strings, the
 * compiled thunk's own case. Plain heap, freed by the wrapper. */
static char* park_cstr(const AshValue* v) {
    if (!v || v->ty != ASH_TY_STRING) return NULL;
    char* p = malloc((size_t)v->as.s.len + 1);
    if (!p) return NULL;
    if (v->as.s.len) memcpy(p, v->as.s.ptr, (size_t)v->as.s.len);
    p[v->as.s.len] = 0;
    return p;
}

AshStatus ash_instance_park_v(AshContract* c, const AshValue* dsn,
                              const AshValue* key) {
    if (!c || !dsn || !key || dsn->ty != ASH_TY_STRING ||
        key->ty != ASH_TY_STRING) {
        return ASH_ERR_TYPE;
    }
    char* d = park_cstr(dsn);
    char* k = park_cstr(key);
    AshStatus st = (d && k) ? ash_instance_park(c, d, k) : ASH_ERR_OOM;
    free(d);
    free(k);
    return st;
}

AshStatus ash_instance_resume_v(AshRuntime* rt, const AshValue* dsn,
                                const AshValue* key, uint64_t expected_hash,
                                AshContract** out) {
    if (!rt || !dsn || !key || !out || dsn->ty != ASH_TY_STRING ||
        key->ty != ASH_TY_STRING) {
        return ASH_ERR_TYPE;
    }
    char* d = park_cstr(dsn);
    char* k = park_cstr(key);
    AshStatus st = (d && k)
        ? ash_instance_resume(rt, d, k, expected_hash, out)
        : ASH_ERR_OOM;
    free(d);
    free(k);
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
