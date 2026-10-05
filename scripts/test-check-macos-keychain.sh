#!/usr/bin/env bash
# Self-test of scripts/check-macos-keychain.sh with a stub `security`.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Not under a temporary directory: the guard rejects keychains there, and the
# stand-in login keychain must look like a real one.
base="${XDG_CACHE_HOME:-$HOME/.cache}"; mkdir -p "$base"
tmp="$(mktemp -d "$base/nostrc-keychain-guard-selftest.XXXXXX")"; trap 'rm -rf "$tmp"' EXIT
login="$tmp/home/Library/Keychains/login.keychain-db"
mkdir -p "$(dirname "$login")"; : > "$login"
stub="$tmp/security"
cat > "$stub" <<'STUB'
#!/usr/bin/env bash
case "$1" in
    default-keychain) printf '    "%s"\n' "$STUB_DEFAULT" ;;
    list-keychains) printf '%s\n' "$STUB_LIST" | while IFS= read -r l; do [ -n "$l" ] && printf '    "%s"\n' "$l"; done ;;
esac
STUB
chmod +x "$stub"
run() { # default, list -> exit status
    STUB_DEFAULT="$1" STUB_LIST="$2" NOSTRC_SECURITY_CMD="$stub" NOSTRC_KEYCHAIN_EXPECT="$login" \
        NOSTRC_KEYCHAIN_GUARD_UNAME=Darwin TMPDIR="$tmp/tmpdir/" bash "$here/check-macos-keychain.sh" 2>/dev/null
}
expect() { # name, wanted status, default, list
    local got=0; run "$3" "$4" || got=$?
    if [ "$got" -ne "$2" ]; then echo "FAIL: $1 (exit $got, wanted $2)" >&2; exit 1; fi
    echo "ok: $1"
}
other="$tmp/home/Library/Keychains/work.keychain-db"; : > "$other"
expect "login keychain is default and listed" 0 "$login" "$login"
expect "an extra existing keychain is fine" 0 "$login" "$login"$'\n'"$other"
expect "default is a throwaway keychain" 1 "/private/tmp/nostrc-test-keychain-1.keychain" "$login"
expect "search list names a missing keychain" 1 "$login" "$tmp/gone.keychain-db"
expect "search list names a temporary keychain" 1 "$login" "/private/tmp/x.keychain"
expect "no default keychain" 1 "" "$login"
NOSTRC_KEYCHAIN_GUARD_UNAME=Linux NOSTRC_SECURITY_CMD=/nonexistent bash "$here/check-macos-keychain.sh" && echo "ok: no-op off macOS"
echo "check-macos-keychain self-test passed"
