#!/usr/bin/env bash
#
# check-macos-keychain.sh — refuse to go on when the developer's macOS keychain
# configuration is not the login keychain (nostrc-2hmd).
#
# A test or an ad-hoc command once left a developer's *default keychain* and
# *search list* pointing at a throwaway keychain under /tmp. Nothing failed
# until a reboot removed the file; in between, everything apps saved went to
# the throwaway. This check is read-only: it never changes the configuration.
#
# Fails when, for the user domain,
#   - the default keychain is not the login keychain, or
#   - the search list names a keychain that does not exist, or one under a
#     temporary directory.
# No-op on other systems. scripts/pre-push runs it before and after the tests.
#
# Environment (tests): NOSTRC_SECURITY_CMD (the `security` binary to call),
# NOSTRC_KEYCHAIN_EXPECT (the expected default; ~/Library/Keychains/login.keychain-db),
# NOSTRC_KEYCHAIN_GUARD_UNAME (stands in for `uname -s`).
set -euo pipefail

[ "${NOSTRC_KEYCHAIN_GUARD_UNAME:-$(uname -s)}" = Darwin ] || exit 0

security_cmd="${NOSTRC_SECURITY_CMD:-security}"
expected="${NOSTRC_KEYCHAIN_EXPECT:-$HOME/Library/Keychains/login.keychain-db}"

# `security` prints one quoted path per line, indented.
paths() { sed -E 's/^[[:space:]]*"(.*)"[[:space:]]*$/\1/' | sed '/^[[:space:]]*$/d'; }

fail=0
default="$("$security_cmd" default-keychain -d user 2>/dev/null | paths | head -1 || true)"
if [ "$default" != "$expected" ]; then
    echo "keychain guard: the default keychain is '${default:-<none>}', not '$expected'" >&2
    fail=1
fi

while IFS= read -r keychain; do
    case "$keychain" in
        /tmp/*|/private/tmp/*|/var/folders/*|/private/var/folders/*|"${TMPDIR:-/nonexistent}"*)
            echo "keychain guard: the search list names a temporary keychain: $keychain" >&2
            fail=1 ;;
    esac
    if [ ! -e "$keychain" ]; then
        echo "keychain guard: the search list names a keychain that does not exist: $keychain" >&2
        fail=1
    fi
done < <("$security_cmd" list-keychains -d user 2>/dev/null | paths)

if [ "$fail" -ne 0 ]; then
    cat >&2 <<'MSG'
keychain guard: this machine's keychain configuration was changed (nostrc-2hmd).
Do NOT choose "Reset To Defaults" in a macOS keychain dialog. Restore it with:
  security list-keychains -d user -s ~/Library/Keychains/login.keychain-db
  security default-keychain -d user -s ~/Library/Keychains/login.keychain-db
(only the owner of the machine runs these; agents never do).
MSG
    exit 1
fi
