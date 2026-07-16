/* ashd.c: the Ashford network daemon, now the thin case of a mesh node. It is a
 * small main over the runtime and the serve call that libashrt already carries:
 * it brings up an AshRuntime, dlopens the modules named on its command line and
 * lets their registrars fill the iname table, loads a token, and makes one
 * ash_runtime_serve call. The accept loop, the connection threads, and the per
 * connection request dispatch all live in the library now, so the daemon and an
 * embedded server run the exact same code and cannot drift; ashd is a node that
 * only serves and never connects, a main over the one call.
 *
 * The daemon owns the one thing the library deliberately does not: a signal
 * policy. SIGINT and SIGTERM ask for a clean stop, and the library has no say
 * in that, so ashd blocks the two, waits for either, and calls ash_server_stop
 * from its own thread, where joining the connection threads is safe. SIGPIPE is
 * ignored so a write to a peer that already left is a failed write the library
 * handles rather than a killed daemon. */

#include <ash/ash.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The handshake timeout the daemon enforces on a first HELLO, the mirror of the
 * client's connect timeout: a peer that will not finish a HELLO is not a peer,
 * and the thread it holds is reaped rather than parked forever. This is the
 * default; --handshake-ms overrides it. The serve loop reads the clock off the
 * runtime, so the daemon passes it through AshRuntimeConfig at init. */
#define ASHD_HANDSHAKE_MS 0 /* 0 lets the runtime pick its ten second default */

/* Set by the signal handler, waited on by main. The handler does no more than
 * this: ash_server_stop joins threads, which a signal handler must never do, so
 * the stop runs on main's thread once the flag is seen. */
static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* Reads a token file, trimming one trailing newline so an editor's newline does
 * not become part of the secret. The bytes are the daemon's expected token; a
 * NULL path leaves the daemon tokenless, accepting empty tokens. */
static int load_token(const char* path, char** out) {
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

    /* A write to a peer that already hung up must not kill the daemon; it is a
     * failed write the library handles and closes on. */
    signal(SIGPIPE, SIG_IGN);

    /* The handshake clock rides the runtime, both the reap the serve loop
     * enforces and the value connect would give a peer; the daemon only serves,
     * so it sets the one clock here. */
    AshRuntimeConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.handshake_ms = handshake_ms;

    AshRuntime* rt = NULL;
    if (ash_runtime_init(&cfg, &rt) != ASH_OK) {
        fprintf(stderr, "[ashd] runtime init failed\n");
        return 1;
    }
    for (size_t i = 0; i < nmodules; i++) {
        if (ash_module_load(rt, modules[i]) != ASH_OK) {
            fprintf(stderr, "[ashd] failed to load module %s\n", modules[i]);
            ash_runtime_shutdown(rt);
            return 1;
        }
    }

    char* token = NULL;
    if (token_file) {
        if (load_token(token_file, &token) != 0) {
            fprintf(stderr, "[ashd] failed to read token file %s\n", token_file);
            ash_runtime_shutdown(rt);
            return 1;
        }
    }

    /* One serve call: it freezes the runtime, snapshots the dump, binds the
     * address, and starts the accept loop on a background thread, returning at
     * once. The daemon keeps this thread for nothing but waiting on a signal. */
    AshServer* server = NULL;
    AshStatus st = ash_runtime_serve(rt, listen_addr, token, &server);
    free(token); /* the server copied it; the daemon's copy is done */
    if (st != ASH_OK) {
        fprintf(stderr, "[ashd] failed to serve on %s\n", listen_addr);
        ash_runtime_shutdown(rt);
        return 1;
    }

    /* Block SIGINT and SIGTERM, install a handler that only raises the flag, and
     * wait on either with sigsuspend so the arrival cannot be lost to a race
     * between the flag check and the wait. When one lands, the stop runs here on
     * main's thread, where joining the connection threads is legal. */
    sigset_t block, oldmask;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &oldmask);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "[ashd] listening on %s\n", listen_addr);

    while (!g_stop) sigsuspend(&oldmask);
    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    /* Graceful shutdown: stop the server, which shuts the listener down, wakes
     * every connection thread blocked in a read, joins them all, and breaks
     * what they signed, then tear the runtime down with nothing still calling
     * in. */
    ash_server_stop(server);
    ash_runtime_shutdown(rt);
    return 0;
}
