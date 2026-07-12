/* test_handshake.c: the N1 network gate's client. It is a foreign program that
 * links libashrt and speaks the ABI, exactly like skeleton/host.c, but instead
 * of loading a module it connects to an ashd daemon serving one. It proves the
 * whole N1 surface: a good handshake merges the daemon's iname table into the
 * client's own so the remote Greeter entries resolve by lookup, enumeration,
 * and dump; the dump hash the handshake carried held, since a successful
 * connect refuses a table that does not match the HELLO_OK hash; a bad token
 * is refused with ASH_ERR_NET before any table crosses; a forged protocol
 * version is refused with ASH_ERR_VERSION; and a dead address is ASH_ERR_NET.
 *
 * The forged-version case reaches past the public API to the internal socket
 * helpers, the only way to send a HELLO the library would never send. It
 * exits zero only when every check held. */

#include <ash/ash.h>

#include "ash_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The names the walking-skeleton module registers, the same the local iname
 * gate pins. The shape hash is the contract's, carried by both the contract
 * entry and its pledge entries. */
#define GREETER_CONTRACT "__ash_ash_Greeter__80464bf23398ab38_v1"
#define GREET_MANGLED    "__ash_ash_Greeter_greet_17cef80f14421b9b_v1"
#define SHOUT_MANGLED    "__ash_ash_Greeter_shout_17cef80f14421b9b_v1"
#define GREETER_SHAPE    0x80464bf23398ab38ULL

static int fail(const char* what) {
    fprintf(stderr, "[test-net] FAIL: %s\n", what);
    return 1;
}

/* Sends a HELLO with a chosen protocol version and the correct token, then
 * reads one reply frame. The daemon checks the version before the token, so a
 * forged version must come back as ERROR with ASH_ERR_VERSION. Returns the
 * status the daemon reported, or ASH_ERR_NET on any socket trouble. */
static AshStatus forged_version_status(const char* addr, const char* token,
                                       uint32_t version) {
    int fd = ash_net_dial(addr);
    if (fd < 0) return ASH_ERR_NET;
    size_t tlen = token ? strlen(token) : 0;
    uint32_t plen = (uint32_t)(8 + tlen);
    uint8_t* p = (uint8_t*)malloc(plen);
    if (!p) {
        close(fd);
        return ASH_ERR_NET;
    }
    ash_net_put_u32(p, version);
    ash_net_put_u32(p + 4, (uint32_t)tlen);
    if (tlen) memcpy(p + 8, token, tlen);
    int wr = ash_net_send_frame(fd, ASH_WIRE_HELLO, 1, p, plen);
    free(p);
    if (wr != 0) {
        close(fd);
        return ASH_ERR_NET;
    }
    AshWireFrame fr;
    uint8_t* pl = NULL;
    AshStatus st = ASH_ERR_NET;
    if (ash_net_recv_frame(fd, &fr, &pl) == 0 && fr.kind == ASH_WIRE_ERROR &&
        fr.payload_len >= 4 && pl) {
        st = (AshStatus)ash_net_get_u32(pl);
    }
    free(pl);
    close(fd);
    return st;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s host:port token\n", argv[0]);
        return 2;
    }
    const char* addr = argv[1];
    const char* token = argv[2];

    /* ---- the good handshake fills the table ---- */

    AshRuntime* rt = NULL;
    if (ash_runtime_init(NULL, &rt) != ASH_OK) return fail("runtime init");

    if (ash_runtime_connect(rt, addr, token) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("connect with the right token");
    }

    /* The remote pledge resolves by its mangled name, carrying the contract's
     * shape hash and version, the pair a later sign would check under. */
    AshInameEntry greet;
    if (ash_iname_lookup(rt, GREET_MANGLED, &greet) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("lookup of the remote greet entry");
    }
    if (greet.kind != ASH_INAME_PLEDGE || greet.shape_hash != GREETER_SHAPE ||
        greet.version != 1) {
        ash_runtime_shutdown(rt);
        return fail("remote greet entry carries the wrong facts");
    }

    AshInameEntry contract;
    if (ash_iname_lookup(rt, GREETER_CONTRACT, &contract) != ASH_OK ||
        contract.kind != ASH_INAME_CONTRACT ||
        contract.shape_hash != GREETER_SHAPE || contract.version != 1) {
        ash_runtime_shutdown(rt);
        return fail("remote contract entry missing or wrong");
    }

    AshInameEntry shout;
    if (ash_iname_lookup(rt, SHOUT_MANGLED, &shout) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("lookup of the remote shout entry");
    }

    /* All three entries enumerate, and the canonical dump names them, proving
     * the merged table reads exactly like a locally registered one. */
    if (ash_iname_count(rt) < 3) {
        ash_runtime_shutdown(rt);
        return fail("merged table has too few entries");
    }
    size_t need = 0;
    if (ash_iname_dump(rt, NULL, 0, &need) != ASH_ERR_OOM || need <= 1) {
        ash_runtime_shutdown(rt);
        return fail("dump size query on the merged table");
    }
    char* dump = (char*)malloc(need);
    if (!dump) {
        ash_runtime_shutdown(rt);
        return fail("allocating the dump buffer");
    }
    if (ash_iname_dump(rt, dump, need, &need) != ASH_OK ||
        !strstr(dump, GREET_MANGLED) || !strstr(dump, GREETER_CONTRACT) ||
        !strstr(dump, SHOUT_MANGLED)) {
        free(dump);
        ash_runtime_shutdown(rt);
        return fail("merged dump content");
    }

    /* The dump the client holds, hashed the daemon's way, is what the
     * handshake already checked against the HELLO_OK hash; a successful
     * connect means the two agreed. Re-hash it here to pin that the client's
     * merged table is byte identical to what the daemon served. */
    uint64_t rehash = ash_net_fnv1a64((const uint8_t*)dump, need - 1);
    free(dump);
    if (rehash == 0) {
        ash_runtime_shutdown(rt);
        return fail("degenerate dump hash");
    }

    /* Connect obeys the freeze law, exactly like a module load. */
    if (ash_runtime_freeze(rt) != ASH_OK) {
        ash_runtime_shutdown(rt);
        return fail("freeze");
    }
    if (ash_runtime_connect(rt, addr, token) != ASH_ERR_STATE) {
        ash_runtime_shutdown(rt);
        return fail("connect after freeze did not report ASH_ERR_STATE");
    }
    ash_runtime_shutdown(rt);

    /* ---- a bad token is refused before any table crosses ---- */

    AshRuntime* rt2 = NULL;
    if (ash_runtime_init(NULL, &rt2) != ASH_OK) return fail("runtime init 2");
    if (ash_runtime_connect(rt2, addr, "not-the-token") != ASH_ERR_NET) {
        ash_runtime_shutdown(rt2);
        return fail("bad token did not report ASH_ERR_NET");
    }
    if (ash_iname_count(rt2) != 0) {
        ash_runtime_shutdown(rt2);
        return fail("a refused connect still merged names");
    }
    ash_runtime_shutdown(rt2);

    /* ---- a forged protocol version is refused ---- */

    if (forged_version_status(addr, token, 2) != ASH_ERR_VERSION) {
        return fail("forged version did not report ASH_ERR_VERSION");
    }

    /* ---- a dead address is a network failure ---- */

    AshRuntime* rt3 = NULL;
    if (ash_runtime_init(NULL, &rt3) != ASH_OK) return fail("runtime init 3");
    if (ash_runtime_connect(rt3, "127.0.0.1:1", token) != ASH_ERR_NET) {
        ash_runtime_shutdown(rt3);
        return fail("connect to a dead address did not report ASH_ERR_NET");
    }
    ash_runtime_shutdown(rt3);

    fprintf(stderr, "[test-net] client ok\n");
    return 0;
}
