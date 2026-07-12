#!/usr/bin/env bash
# The N3 resilience gate harness. It stands up ashd serving the payment module
# on a fixed loopback port under a throwaway token, waits until the daemon
# accepts before letting the client dial, and hands the client the daemon's pid
# so the kill storm phase can SIGKILL it mid flight. The client bounds itself
# with an internal alarm, and this harness bounds the whole run again from the
# outside, so a hang is a failure the gate reports rather than a parked CI.
#
# Usage: run_stress.sh ASHD CLIENT MODULE

set -u

ASHD="$1"
CLIENT="$2"
MODULE="$3"

HOST="127.0.0.1"
PORT="8473"
ADDR="$HOST:$PORT"

TOKEN="s3cr3t-stress-token"
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
        echo "[test-net-stress] ashd exited before it was ready" >&2
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
    echo "[test-net-stress] ashd did not become ready in time" >&2
    exit 1
fi

"$CLIENT" "$ADDR" "$TOKEN" "$MODULE" "$PID"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "[test-net-stress] ok"
fi
exit "$rc"
