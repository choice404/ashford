#!/usr/bin/env bash
# The network gate harness. It stands up ashd on a fixed loopback port under a
# throwaway token, waits until the daemon actually accepts a connection before
# letting the client dial, runs the client scenario, and kills the daemon
# afterward. The readiness wait is what keeps the gate from racing the daemon's
# listen call, the one timing that could make a socket test flaky.
#
# Usage: run_net.sh ASHD CLIENT MODULE

set -u

ASHD="$1"
CLIENT="$2"
MODULE="$3"

HOST="127.0.0.1"
PORT="8471"
ADDR="$HOST:$PORT"

TOKEN="s3cr3t-net-token"
TOKENFILE="$(mktemp)"
printf '%s\n' "$TOKEN" > "$TOKENFILE"

cleanup() {
    if [ -n "${PID:-}" ]; then
        kill "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    rm -f "$TOKENFILE"
}
trap cleanup EXIT

"$ASHD" --listen "$ADDR" --token-file "$TOKENFILE" --module "$MODULE" &
PID=$!

# Wait for the daemon to accept before dialing. Give up if it dies or never
# comes up, so a broken daemon fails the gate instead of hanging it.
ready=0
for _ in $(seq 1 400); do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "[test-net] ashd exited before it was ready" >&2
        exit 1
    fi
    if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
        exec 3>&- 3<&- 2>/dev/null
        ready=1
        break
    fi
    sleep 0.05
done
if [ "$ready" -ne 1 ]; then
    echo "[test-net] ashd did not become ready in time" >&2
    exit 1
fi

"$CLIENT" "$ADDR" "$TOKEN"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "[test-net] ok"
fi
exit "$rc"
