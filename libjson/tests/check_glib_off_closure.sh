#!/usr/bin/env bash
# Verify direct and transitive ELF dependencies for NOSTR_WITH_GLIB=OFF builds.
set -euo pipefail
if [[ $# -eq 0 ]]; then
    echo "usage: $0 ELF-file [...]" >&2
    exit 2
fi
command -v readelf >/dev/null
command -v ldd >/dev/null
for binary in "$@"; do
    direct=$(readelf -d "$binary")
    closure=$(ldd "$binary")
    printf '%s\n%s\n%s\n' "$binary" "$direct" "$closure"
    if grep -Eqi 'lib(glib|gobject|gio)-2\.0' <<<"$direct $closure"; then
        echo "FAIL: GLib dependency in NOSTR_WITH_GLIB=OFF build: $binary" >&2
        exit 1
    fi
    if grep -q 'not found' <<<"$closure"; then
        echo "FAIL: incomplete dependency closure: $binary" >&2
        exit 1
    fi
done
