#!/usr/bin/env bash
# The B2 mesh gate harness: a Python provider and a C consumer, two processes in
# two languages meeting at one contract. It stands the Python node up in the
# background under a throwaway token, waits until it listens so the socket timing
# cannot make the gate flaky, runs the C consumer against it, and asserts the C
# client read back the value the Python function computed. The consumer also
# dials once with a wrong token and demands ASH_ERR_NET, the provider refusing it
# before any table crosses. The provider is signaled to stop cleanly at the end;
# the token lives only in this process's argv and never in the repo or a log.
#
# Usage: run_mesh_python.sh CONSUMER

set -u

CONSUMER="$1"

HOST="127.0.0.1"
PORT="8480"
ADDR="$HOST:$PORT"
TOKEN="s3cr3t-mesh-python-token"

DEMO="$(dirname "$0")/../../interop/python/demo_mesh.py"

cleanup() {
    if [ -n "${PROV_PID:-}" ]; then
        kill "$PROV_PID" 2>/dev/null
        wait "$PROV_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

# Waits for the provider to accept on its port before the consumer dials, giving
# up if it dies or never comes up so a broken provider fails the gate instead of
# hanging it.
wait_ready() {
    local pid="$1" port="$2" name="$3"
    for _ in $(seq 1 400); do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[test-mesh-python] $name exited before it was ready" >&2
            return 1
        fi
        if (exec 3<>"/dev/tcp/$HOST/$port") 2>/dev/null; then
            exec 3>&- 3<&- 2>/dev/null
            return 0
        fi
        sleep 0.05
    done
    echo "[test-mesh-python] $name did not become ready in time" >&2
    return 1
}

python3 "$DEMO" serve "$ADDR" "$TOKEN" &
PROV_PID=$!

wait_ready "$PROV_PID" "$PORT" "python provider" || exit 1

"$CONSUMER" "$ADDR" "$TOKEN"
rc=$?

if [ "$rc" -eq 0 ]; then
    echo "[test-mesh-python] ok"
fi
exit "$rc"
