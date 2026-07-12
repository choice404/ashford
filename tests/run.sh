#!/usr/bin/env bash
# The golden test runner for the ashford front end. Each suite directory pairs
# with an ashc command: tests/lex with `ashc lex`, tests/parse with
# `ashc parse`, and tests/check and tests/typeck with `ashc check`. Every NAME.ash with a
# NAME.expect beside it must be accepted and its dump must match the golden
# byte for byte; check writes nothing to stdout, so its goldens are a single
# blank line guarding that silence. Every NAME.ash with a
# NAME.err beside it must be rejected: a nonzero exit and every line of the
# .err file appearing somewhere in stderr as a substring, so a diagnostic can
# gain context without breaking the suite. tests/check/mfa holds the
# multi-file programs: only files with a golden beside them are roots, the
# rest are modules the roots import. The ashc binary is rebuilt when
# missing; set ASHC to test another build, or DUSK to build with another
# toolchain.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DUSK="${DUSK:-dusk}"
ASHC="${ASHC:-$ROOT/target/dusk-out/ashc}"

if [ ! -x "$ASHC" ]; then
    echo "[tests] building ashc with $DUSK" >&2
    "$DUSK" build compiler/ashc.dusk || exit 1
fi

pass=0
fail=0

fail_case() {
    echo "FAIL $1: $2"
    fail=$((fail + 1))
}

run_suite() {
    local dir="$1"
    local cmd="$2"
    local src name base out code err missing want
    for src in tests/"$dir"/*.ash; do
        name="${src%.ash}"
        base="$dir/$(basename "$name")"

        if [ -f "$name.expect" ]; then
            out="$("$ASHC" "$cmd" "$src" 2>/dev/null)"
            code=$?
            if [ "$code" -ne 0 ]; then
                fail_case "$base" "expected exit 0, got $code"
                continue
            fi
            if ! diff <(printf '%s\n' "$out") "$name.expect" >/dev/null; then
                fail_case "$base" "dump differs from golden"
                diff <(printf '%s\n' "$out") "$name.expect" | head -20
                continue
            fi
            pass=$((pass + 1))
        elif [ -f "$name.err" ]; then
            err="$("$ASHC" "$cmd" "$src" 2>&1 >/dev/null)"
            code=$?
            if [ "$code" -eq 0 ]; then
                fail_case "$base" "expected rejection, got exit 0"
                continue
            fi
            missing=""
            while IFS= read -r want; do
                [ -z "$want" ] && continue
                if ! printf '%s' "$err" | grep -qF -- "$want"; then
                    missing="$want"
                    break
                fi
            done < "$name.err"
            if [ -n "$missing" ]; then
                fail_case "$base" "stderr missing: $missing"
                printf '%s\n' "$err" | head -10
                continue
            fi
            pass=$((pass + 1))
        else
            fail_case "$base" "no .expect or .err beside it"
        fi
    done
}

# The multi-file suite. Every NAME.ash in the directory with a NAME.expect
# or NAME.err beside it is a program root and runs exactly like a check
# case; a file with neither is a helper another root imports and is skipped
# rather than failed, since it is not a test by itself.
run_multi_suite() {
    local dir="$1"
    local src name base out code err missing want
    for src in tests/"$dir"/*.ash; do
        name="${src%.ash}"
        base="$dir/$(basename "$name")"

        if [ -f "$name.expect" ]; then
            out="$("$ASHC" check "$src" 2>/dev/null)"
            code=$?
            if [ "$code" -ne 0 ]; then
                fail_case "$base" "expected exit 0, got $code"
                continue
            fi
            if ! diff <(printf '%s\n' "$out") "$name.expect" >/dev/null; then
                fail_case "$base" "dump differs from golden"
                diff <(printf '%s\n' "$out") "$name.expect" | head -20
                continue
            fi
            pass=$((pass + 1))
        elif [ -f "$name.err" ]; then
            err="$("$ASHC" check "$src" 2>&1 >/dev/null)"
            code=$?
            if [ "$code" -eq 0 ]; then
                fail_case "$base" "expected rejection, got exit 0"
                continue
            fi
            missing=""
            while IFS= read -r want; do
                [ -z "$want" ] && continue
                if ! printf '%s' "$err" | grep -qF -- "$want"; then
                    missing="$want"
                    break
                fi
            done < "$name.err"
            if [ -n "$missing" ]; then
                fail_case "$base" "stderr missing: $missing"
                printf '%s\n' "$err" | head -10
                continue
            fi
            pass=$((pass + 1))
        fi
    done
}

run_suite lex lex
run_suite parse parse
run_suite check check
run_suite typeck check
run_multi_suite check/mfa

echo "[tests] $pass passed, $fail failed"
[ "$fail" -eq 0 ]
