/* test_mesh_serve.c: the B0 mesh gate. It proves ash_runtime_serve stands a
 * server up from a plain C host, not the ashd binary, and that a client drives
 * it exactly as it drives a daemon. One process holds both halves: a server
 * runtime loads libhello, binds Greeter's abstract shout, and serves the frozen
 * table over loopback through the library call; a separate client runtime then
 * connects to that address, resolves the remote greet through the merged iname
 * table, signs Greeter under the discovered hash, fulfills greet, and demands
 * Ok("hello, world"), the same greeting the local host gate pins. The two
 * runtimes are distinct, so the server and the client are two nodes that happen
 * to share a process, and the serve loop under test is the one an embedded node
 * runs. It exits zero only when every check held, run under ASan so the accept
 * loop, the connection threads, and the waiters are watched for a leak. */

#include <ash/ash.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one loopback address the gate uses, distinct from the ports the ashd net
 * gates bind so a parallel make does not collide. */
#define MESH_ADDR  "127.0.0.1:8475"
#define MESH_TOKEN "mesh-serve-token"

/* The names the walking skeleton module registers, the same the net gate pins;
 * the client resolves greet through the merged table but the constants let it
 * check the entry carries the facts a sign would need. */
#define GREETER_NAME  "Greeter"
#define GREET_MANGLED "__ash_ash_Greeter_greet_17cef80f14421b9b_v1"
#define GREETER_SHAPE 0x80464bf23398ab38ULL

static int fail(const char* what) {
    fprintf(stderr, "[test-mesh-serve] FAIL: %s\n", what);
    return 1;
}

/* Greeter.shout, the abstract pledge the server must bind before it can sign
 * Greeter for a peer. It uppercases its name argument the way the host gate's
 * does; the gate never fulfills it, but a bound body is what makes the sign the
 * client asks for legal. */
static AshStatus host_shout(void* ctx, const AshValue* args, size_t nargs,
                            AshValue* out) {
    AshContract* c = (AshContract*)ctx;
    if (nargs != 1 || args[0].ty != ASH_TY_STRING) return ASH_ERR_TYPE;
    AshValue up = ash_string_copy(c, args[0].as.s.ptr, args[0].as.s.len);
    if (args[0].as.s.len && !up.as.s.ptr) return ASH_ERR_OOM;
    for (uint64_t i = 0; i < up.as.s.len; i++) {
        uint8_t ch = up.as.s.ptr[i];
        if (ch >= 'a' && ch <= 'z') up.as.s.ptr[i] = (uint8_t)(ch - 32);
    }
    AshValue* box = ash_box(c);
    if (!box) return ASH_ERR_OOM;
    *box = up;
    memset(out, 0, sizeof(*out));
    out->ty = ASH_TY_RESULT;
    out->tag = 0;
    out->as.box = box;
    return ASH_OK;
}

static AshValue str_arg(const char* s) {
    AshValue v;
    memset(&v, 0, sizeof(v));
    v.ty = ASH_TY_STRING;
    v.as.s.ptr = (uint8_t*)s;
    v.as.s.len = strlen(s);
    return v;
}

static int check_ok_string(const AshValue* out, const char* want) {
    if (out->ty != ASH_TY_RESULT || out->tag != 0) return 0;
    const AshValue* inner = (const AshValue*)out->as.box;
    if (!inner || inner->ty != ASH_TY_STRING) return 0;
    if (inner->as.s.len != strlen(want)) return 0;
    return memcmp(inner->as.s.ptr, want, inner->as.s.len) == 0;
}

int main(int argc, char** argv) {
    const char* module = argc > 1 ? argv[1] : "target/ashc-out/libhello.ash.so";

    /* A serving host owns the SIGPIPE policy the library leaves to it, so a
     * write to a peer that left is a failed write, not a killed process. */
    signal(SIGPIPE, SIG_IGN);

    /* ---- the server node: load, bind, serve ---- */

    AshRuntime* srv_rt = NULL;
    if (ash_runtime_init(NULL, &srv_rt) != ASH_OK) return fail("server init");
    if (ash_module_load(srv_rt, module) != ASH_OK) {
        ash_runtime_shutdown(srv_rt);
        return fail("load libhello into the server");
    }
    if (ash_pledge_bind(srv_rt, "Greeter.shout", host_shout) != ASH_OK) {
        ash_runtime_shutdown(srv_rt);
        return fail("bind Greeter.shout on the server");
    }

    /* serve freezes the runtime, snapshots the dump, binds the address, and
     * starts the accept loop behind the call, returning at once. */
    AshServer* server = NULL;
    if (ash_runtime_serve(srv_rt, MESH_ADDR, MESH_TOKEN, &server) != ASH_OK) {
        ash_runtime_shutdown(srv_rt);
        return fail("ash_runtime_serve");
    }

    /* ---- the client node: connect and drive ---- */

    AshRuntime* cli_rt = NULL;
    if (ash_runtime_init(NULL, &cli_rt) != ASH_OK) {
        ash_server_stop(server);
        ash_runtime_shutdown(srv_rt);
        return fail("client init");
    }
    if (ash_runtime_connect(cli_rt, MESH_ADDR, MESH_TOKEN) != ASH_OK) {
        ash_runtime_shutdown(cli_rt);
        ash_server_stop(server);
        ash_runtime_shutdown(srv_rt);
        return fail("client connect to the served node");
    }

    /* The remote greet resolves out of the merged table carrying the contract's
     * shape hash, proving the served entry landed; a sign then goes by the plain
     * contract name to the connection's origin, the way a client signs a remote
     * contract it knows by name, the merged pledge row not carrying the human
     * name the dump never sent. */
    AshInameEntry ent;
    if (ash_iname_lookup(cli_rt, GREET_MANGLED, &ent) != ASH_OK ||
        ent.kind != ASH_INAME_PLEDGE || ent.shape_hash != GREETER_SHAPE) {
        ash_runtime_shutdown(cli_rt);
        ash_server_stop(server);
        ash_runtime_shutdown(srv_rt);
        return fail("resolve the remote greet entry");
    }

    int rc = 0;
    AshContract* c = NULL;
    if (ash_contract_sign(cli_rt, GREETER_NAME, NULL, 0, GREETER_SHAPE, &c) !=
        ASH_OK) {
        rc = fail("sign Greeter across the wire");
    } else {
        AshValue name = str_arg("world");
        AshValue out;
        memset(&out, 0, sizeof out);
        if (ash_pledge_fulfill_sync(c, "greet", &name, 1, NULL, 0, &out) !=
            ASH_OK) {
            rc = fail("fulfill greet across the wire");
        } else if (!check_ok_string(&out, "hello, world")) {
            rc = fail("remote greeting mismatch");
        }
        if (ash_contract_break(c) != ASH_OK && rc == 0) {
            rc = fail("break the remote instance");
        }
    }

    /* Teardown is the client first, then the server: drop the edge, stop the
     * server so its accept loop and connection threads join and every instance
     * they signed breaks, then the runtimes. */
    ash_runtime_shutdown(cli_rt);
    ash_server_stop(server);
    ash_runtime_shutdown(srv_rt);

    if (rc == 0) fprintf(stderr, "[test-mesh-serve] client ok\n");
    return rc;
}
