#!/usr/bin/env bash
# The N2 transparency gate harness. It stands up ashd serving the payment module
# on a fixed loopback port under a throwaway token, waits until the daemon
# accepts before letting the client dial, and hands the client the daemon's pid
# so the client can kill it mid flight for the disconnect check. The client runs
# the same sequence locally and remotely and then severs the daemon itself, so
# the harness's own kill on exit is only a backstop.
#
# Usage: run_net2.sh ASHD CLIENT MODULE

set -u

ASHD="$1"
CLIENT="$2"
MODULE="$3"

HOST="127.0.0.1"
PORT="8472"
ADDR="$HOST:$PORT"

TOKEN="s3cr3t-net2-token"
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

# Wait for the daemon to accept before dialing, so the socket timing cannot make
# the gate flaky. Give up if it dies or never comes up.
ready=0
for _ in $(seq 1 400); do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "[test-net2] ashd exited before it was ready" >&2
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
    echo "[test-net2] ashd did not become ready in time" >&2
    exit 1
fi

"$CLIENT" "$ADDR" "$TOKEN" "$MODULE" "$PID"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "[test-net2] ok"
fi
exit "$rc"
