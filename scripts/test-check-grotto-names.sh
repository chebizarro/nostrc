#!/usr/bin/env bash
# Self-test of scripts/check-grotto-names.sh in throwaway git repos: it must
# still fail on stale signer names, and pass only on the marked migration fixture.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
# Assembled here so this file itself stays clean under the guard it tests.
stale="gnostr""-signer"
old_dbus="org.gnostr"".Signer"
marker="grotto-names: allow legacy-fixture"
n=0
check() { # name, wanted status, path, line
    n=$((n + 1)); local repo="$tmp/r$n" got=0
    mkdir -p "$repo/scripts" "$repo/$(dirname "$3")"
    cp "$here/check-grotto-names.sh" "$repo/scripts/"
    printf '%s\n' "$4" > "$repo/$3"
    git -C "$repo" init -q && git -C "$repo" add -A
    (cd "$repo" && bash scripts/check-grotto-names.sh >/dev/null 2>&1) || got=$?
    if [ "$got" -ne "$2" ]; then echo "FAIL: $1 (exit $got, wanted $2)" >&2; exit 1; fi
    echo "ok: $1"
}
fixture="apps/grotto/tests/test-version.py"
check "clean file passes"                           0 apps/grotto/src/a.c "int x; /* Grotto */"
check "stale name in source fails"                  1 apps/grotto/src/a.c "config = \"$stale\""
check "stale name in a test without marker fails"   1 "$fixture" "legacy = config / '$stale'"
check "marked fixture line passes"                  0 "$fixture" "legacy = config / '$stale'  # $marker"
check "marker outside apps/grotto/tests is ignored" 1 apps/grotto/src/a.c "config = \"$stale\"  /* $marker */"
check "marker in another tests dir is ignored"      1 apps/other/tests/t.py "x = '$stale'  # $marker"
check "marker does not cover another line"          1 "$fixture" "$(printf "ok = 1  # %s\nbad = '%s'" "$marker" "$stale")"
check "other old names still fail next to marker"   1 "$fixture" "x = '$old_dbus'  # $marker"
echo "check-grotto-names self-test passed"
