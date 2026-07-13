#!/usr/bin/env bash
# The Python network gate harness. It stands up two ashd daemons serving the
# payment module on loopback, one under a throwaway token and one with no token
# at all, waits until each accepts before letting the client dial, and runs the
# Python remote demo against both. The demo drives the same payment sequence
# locally and over the wire and proves they agree, walks the token matrix, and
# kills the tokened daemon mid fulfillment to prove ASH_ERR_NET reaches an in
# flight wait. The readiness wait is what keeps the socket timing from making the
# gate flaky; the token file lives in a tmp path and never in the repo.
#
# Usage: run_net_python.sh ASHD MODULE

set -u

ASHD="$1"
MODULE="$2"

HOST="127.0.0.1"
TOK_PORT="8473"
NOTOK_PORT="8474"
TOK_ADDR="$HOST:$TOK_PORT"
NOTOK_ADDR="$HOST:$NOTOK_PORT"

TOKEN="s3cr3t-python-token"
TOKENFILE="$(mktemp)"
printf '%s\n' "$TOKEN" > "$TOKENFILE"

cleanup() {
    if [ -n "${TOK_PID:-}" ]; then
        kill "$TOK_PID" 2>/dev/null
        wait "$TOK_PID" 2>/dev/null
    fi
    if [ -n "${NOTOK_PID:-}" ]; then
        kill "$NOTOK_PID" 2>/dev/null
        wait "$NOTOK_PID" 2>/dev/null
    fi
    rm -f "$TOKENFILE"
}
trap cleanup EXIT

# Waits for a daemon to accept on a port before the client dials, giving up if it
# dies or never comes up so a broken daemon fails the gate instead of hanging it.
wait_ready() {
    local pid="$1" port="$2" name="$3"
    for _ in $(seq 1 400); do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[test-net-python] $name exited before it was ready" >&2
            return 1
        fi
        if (exec 3<>"/dev/tcp/$HOST/$port") 2>/dev/null; then
            exec 3>&- 3<&- 2>/dev/null
            return 0
        fi
        sleep 0.05
    done
    echo "[test-net-python] $name did not become ready in time" >&2
    return 1
}

"$ASHD" --listen "$TOK_ADDR" --token-file "$TOKENFILE" --module "$MODULE" &
TOK_PID=$!
"$ASHD" --listen "$NOTOK_ADDR" --module "$MODULE" &
NOTOK_PID=$!

wait_ready "$TOK_PID" "$TOK_PORT" "tokened ashd" || exit 1
wait_ready "$NOTOK_PID" "$NOTOK_PORT" "tokenless ashd" || exit 1

python3 "$(dirname "$0")/../../interop/python/demo_remote.py" \
    "$TOK_ADDR" "$TOKEN" "$NOTOK_ADDR" "$TOK_PID"
rc=$?
# The demo kills the tokened daemon itself in its disconnect phase, so the
# cleanup kill of that pid is only a backstop.
if [ "$rc" -eq 0 ]; then
    echo "[test-net-python] ok"
fi
exit "$rc"
