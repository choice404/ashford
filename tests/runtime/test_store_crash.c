/* test_store_crash.c: the S3 durability gate, the rollback on crash proof the
 * whole store layer rests on. A transaction is all or nothing across a clean
 * break, which test_store_txn already pins; this gate proves it across an
 * unclean one, a process killed dead in the middle of an open transaction with
 * no chance to roll anything back itself. It forks a child that signs a fresh
 * Ledger against the shared file, runs a debit that opens the transaction and
 * buffers its write, tells the parent over a pipe that the write is buffered
 * and uncommitted, and then blocks forever. The parent SIGKILLs it there, so
 * the child dies with the transaction open and the debit unwritten to the file,
 * exactly the crash the layer must survive. The parent reopens the file in a
 * fresh contract and reads the account back: the buffered debit is absent, the
 * backend rolled its hot journal back on the next open, and the balance is byte
 * for byte the pre debit value. No uncommitted write survived the kill.
 *
 * The sign kill loop drives that crash again and again against one file, a
 * child killed mid transaction every pass, and after each the parent reopens,
 * reconciles the schema against the live table, and reads the account: the file
 * is consistent every time, no half written debit and no shape the reconcile
 * refuses, so a run of crashes cannot corrupt the store or drift its balance.
 * The child is SIGKILLed and never runs an atexit, so its own allocations are
 * the kernel's to reclaim and no sanitizer watches them; the parent inits and
 * shuts down cleanly, so ASan and LSan watch every allocation the surviving
 * process makes. The child opens its own runtime rather than the one it
 * inherited across the fork, because the parent's pool threads do not cross a
 * fork and only a fresh runtime has workers to fulfill against. */

#include <ash/ash.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static int g_fail = 0;

#define CHECK(cond, what)                                           \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "[test_store_crash] FAIL: %s\n", what); \
            g_fail = 1;                                             \
        }                                                           \
    } while (0)

static AshValue int_val(int64_t i) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_INT;
    v.as.i = i;
    return v;
}

static AshValue float_val(double f) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_FLOAT;
    v.as.f = f;
    return v;
}

static AshValue str_val(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static const char* g_module = "target/ashc-out/libledger.ash.so";

/* Signs a fresh Ledger against the dsn on the given runtime. */
static AshContract* sign_on(AshRuntime* rt, const char* dsn) {
    AshVowBinding ovr = { "dsn", str_val(dsn) };
    AshContract* c = NULL;
    ash_contract_sign(rt, "Ledger", &ovr, 1, 0, &c);
    return c;
}

/* Reads one account's balance on the parent's runtime, a fresh instance so the
 * committed file is the witness. Reconciles the schema on the way in, so a read
 * that succeeds is also proof the shape still validates after a crash. */
static double read_balance(AshRuntime* rt, const char* dsn, int64_t id,
                           int* ok) {
    *ok = 0;
    AshContract* c = sign_on(rt, dsn);
    if (!c || ash_contract_state(c) != ASH_SIGNED) return -1.0;
    AshValue key[1] = { int_val(id) };
    AshValue out;
    memset(&out, 0, sizeof(out));
    AshStatus st = ash_pledge_fulfill_sync(c, "balance", key, 1, NULL, 0, &out);
    double bal = -1.0;
    if (st == ASH_OK && out.ty == ASH_TY_RESULT && out.tag == 0 && out.as.box) {
        const AshValue* p = (const AshValue*)out.as.box;
        if (p->ty == ASH_TY_FLOAT) {
            bal = p->as.f;
            *ok = 1;
        }
    }
    ash_contract_break(c);
    return bal;
}

/* Forks a child that opens a transaction and dies in it. The child signs a
 * fresh Ledger on its own runtime, debits the account so the transaction opens
 * and its write buffers, writes one byte to signal the parent that the debit is
 * buffered and uncommitted, and then blocks. The parent reads the byte, SIGKILLs
 * the child there, and reaps it. Returns 1 when the child was signaled dead
 * mid transaction, the crash the assertion needs. */
static int crash_mid_txn(const char* dsn, int64_t id, double amount) {
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return 0;
    }
    if (pid == 0) {
        /* the child: a fresh runtime, a fresh sign, a debit that opens the
         * transaction, then a signal and a block until the kill. */
        close(pfd[0]);
        AshRuntime* crt = NULL;
        if (ash_runtime_init(NULL, &crt) != ASH_OK || !crt) _exit(2);
        if (ash_module_load(crt, g_module) != ASH_OK) _exit(2);
        AshContract* c = sign_on(crt, dsn);
        if (!c || ash_contract_state(c) != ASH_SIGNED) _exit(2);
        AshValue d[2] = { int_val(id), float_val(amount) };
        AshValue out;
        memset(&out, 0, sizeof(out));
        AshStatus st = ash_pledge_fulfill_sync(c, "debit", d, 2, NULL, 0, &out);
        /* a debit that would not open the transaction is a bug in the setup,
         * not the crash under test; leave without signaling so the parent's
         * read on the pipe fails and the pass is caught. */
        if (st != ASH_OK) _exit(2);
        char one = 1;
        ssize_t w = write(pfd[1], &one, 1);
        (void)w;
        for (;;) pause();
    }
    /* the parent: wait for the buffered signal, then kill the child dead in the
     * open transaction and reap it. */
    close(pfd[1]);
    char b = 0;
    ssize_t r = read(pfd[0], &b, 1);
    close(pfd[0]);
    if (r != 1 || b != 1) {
        /* the child never buffered the debit; kill and fail. */
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 0;
    }
    kill(pid, SIGKILL);
    int wst = 0;
    waitpid(pid, &wst, 0);
    return WIFSIGNALED(wst) && WTERMSIG(wst) == SIGKILL;
}

int main(void) {
    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK || !rt) {
        fprintf(stderr, "[test_store_crash] FAIL: runtime init\n");
        return 1;
    }
    CHECK(ash_module_load(rt, g_module) == ASH_OK, "load ledger module");

    char db_path[] = "target/ashcrash_XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) {
        fprintf(stderr, "[test_store_crash] FAIL: mkstemp\n");
        return 1;
    }
    close(fd);
    char dsn[96];
    snprintf(dsn, sizeof(dsn), "file:%s", db_path);

    /* ---- seed one account the parent owns the truth of ---- */

    AshContract* seed = sign_on(rt, dsn);
    if (seed) {
        AshValue a[3] = { int_val(1), str_val("alice"), float_val(100.0) };
        AshValue out;
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(seed, "open", a, 3, NULL, 0, &out) ==
                  ASH_OK,
              "seed account 1");
        ash_contract_break(seed);
    }

    /* ---- crash mid transaction: a child killed with the debit open, and the
     * reopened file shows the debit never landed ---- */

    CHECK(crash_mid_txn(dsn, 1, 40.0),
          "a child was killed dead in the open transaction");
    int ok = 0;
    double bal = read_balance(rt, dsn, 1, &ok);
    CHECK(ok, "the reopened file reconciles and reads after the crash");
    CHECK(bal == 100.0,
          "the buffered debit did not survive the crash, 1 still 100");

    /* ---- the sign kill loop: crash again and again, the file consistent every
     * pass, its balance never drifting and its shape never diverging ---- */

    const int rounds = 12;
    for (int i = 0; i < rounds; i++) {
        CHECK(crash_mid_txn(dsn, 1, 25.0),
              "a loop child was killed in its open transaction");
        ok = 0;
        bal = read_balance(rt, dsn, 1, &ok);
        CHECK(ok, "the file reconciles and reads after a loop crash");
        CHECK(bal == 100.0, "the loop crash left 1 at 100, no drift");
    }

    /* the file is still a working store after the run of crashes: a plain write
     * lands and reads back, so the reconcile and the surface both survived. */
    AshContract* fin = sign_on(rt, dsn);
    if (fin && ash_contract_state(fin) == ASH_SIGNED) {
        AshValue set[3] = { int_val(1), str_val("alice"), float_val(250.0) };
        AshValue out;
        memset(&out, 0, sizeof(out));
        CHECK(ash_pledge_fulfill_sync(fin, "set_balance", set, 3, NULL, 0,
                                      &out) == ASH_OK,
              "the store still writes after the crash loop");
        ash_contract_break(fin);
    } else {
        CHECK(0, "the store still signs after the crash loop");
    }
    ok = 0;
    bal = read_balance(rt, dsn, 1, &ok);
    CHECK(ok && bal == 250.0, "the post loop write read back, the store intact");

    ash_runtime_shutdown(rt);
    unlink(db_path);

    if (g_fail) {
        fprintf(stderr, "[test_store_crash] failures\n");
        return 1;
    }
    fprintf(stderr, "[test_store_crash] ok\n");
    return 0;
}
