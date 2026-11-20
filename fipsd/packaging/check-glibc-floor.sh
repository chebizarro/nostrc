#!/bin/bash
# Fail when a shipped binary needs a newer glibc than the declared floor.
#
# The defect this exists to catch installs cleanly and then cannot run: a
# binary linked against a newer glibc records a symbol-version dependency
# the loader refuses on an older system, while the package manager reports
# success. nostrc builds one .deb per distribution release, so the floor is
# that release's glibc (fipsd/packaging/build-floor.env); a build on a newer
# host than the package claims to be for fails here.
#
# Usage: check-glibc-floor.sh <artifact|binary>...
#   A .deb is unpacked and every ELF executable in it is checked.
#   Anything else is treated as a single ELF binary.
#
# Reads the floor from fipsd/packaging/build-floor.env unless
# FIPSD_GLIBC_FLOOR is set.
#
# Exit 0 = every input was examined and none is above the floor. Exit 1 = a
# binary needs a newer glibc than the floor. Exit 2 = an input could not be
# examined (missing, not an ELF object, a .deb that would not unpack or holds
# no binaries), or the check could not run at all; never treated as a pass,
# and never reported as a binary above the floor.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# A missing or empty floor means the check cannot run, so it exits 2 here
# rather than letting `set -e` or a `:?` expansion end the run with status 1,
# which callers would read as a binary above the floor.
FLOOR_ENV="$SCRIPT_DIR/build-floor.env"
if [ -z "${FIPSD_GLIBC_FLOOR:-}" ]; then
    [ -r "$FLOOR_ENV" ] || {
        echo "check-glibc-floor: cannot read $FLOOR_ENV and FIPSD_GLIBC_FLOOR is not set;" >&2
        echo "                   there is no floor to check against." >&2
        exit 2
    }
    # shellcheck disable=SC1090,SC1091  # build-floor.env beside this script
    . "$FLOOR_ENV"
fi
if [ -z "${FIPSD_GLIBC_FLOOR:-}" ]; then
    echo "check-glibc-floor: $FLOOR_ENV declares no FIPSD_GLIBC_FLOOR;" >&2
    echo "                   there is no floor to check against." >&2
    exit 2
fi
FLOOR="$FIPSD_GLIBC_FLOOR"

for tool in readelf dpkg dpkg-deb; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "check-glibc-floor: $tool is not installed; cannot check anything." >&2
        echo "                   Refusing to report a pass I did not establish." >&2
        exit 2
    }
done

# The maximum glibc version a binary requires, read from the `Version needs`
# section, which is the table the dynamic loader enforces. Not computed from
# `objdump -T | grep GLIBC_ | sort -V | tail -1`: that sorts whole lines and
# the version is not the leading field.
#
# Prints nothing and returns 1 when it finds no GLIBC requirement at all, so an
# unreadable or non-dynamic input cannot be scored as a pass.
max_glibc_need() {
    local bin="$1" found
    found=$(readelf -VW "$bin" 2>/dev/null \
        | awk '/Version needs section/,0' \
        | grep -oE 'GLIBC_[0-9.]+' \
        | sed 's/GLIBC_//' \
        | sort -V \
        | tail -1) || true
    [ -n "$found" ] || return 1
    printf '%s\n' "$found"
    return 0
}

FAILED=0
CHECKED=0
UNCHECKED=0
UNCHECKED_LIST=()

unchecked() {
    echo "  UNCHECKED $1: could not check ($2)" >&2
    UNCHECKED_LIST+=("$1: $2")
    UNCHECKED=$((UNCHECKED + 1))
}

# A regular file that is an ELF executable or shared object (not an archive,
# not a relocatable object), whatever its mode: dh_fixperms strips the
# execute bit from shared libraries.
is_elf() {
    head -c 4 "$1" 2>/dev/null | cmp -s - <(printf '\x7fELF') || return 1
    readelf -hW "$1" 2>/dev/null | grep -qE '^ *Type: *(EXEC|DYN)'
}

check_binary() {
    local bin="$1" label="$2" need
    if ! need=$(max_glibc_need "$bin"); then
        if is_elf "$bin"; then
            echo "  ok    $label (no glibc version requirement)"
            CHECKED=$((CHECKED + 1))
            return
        fi
        unchecked "$label" "not a readable ELF object"
        return
    fi
    CHECKED=$((CHECKED + 1))
    if dpkg --compare-versions "$need" gt "$FLOOR"; then
        echo "  FAIL  $label needs glibc $need, above the declared floor $FLOOR" >&2
        FAILED=$((FAILED + 1))
    else
        echo "  ok    $label needs glibc $need"
    fi
}

check_deb() {
    local deb="$1" tmp
    tmp=$(mktemp -d)
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN
    if ! dpkg-deb -x "$deb" "$tmp"; then
        unchecked "$(basename "$deb")" "dpkg-deb could not unpack it"
        return
    fi
    # A package ships scripts, headers and a static archive beside its
    # binaries, so filter to ELF executables and shared objects by magic.
    # A .deb with no ELF object at all is not a pass: the layout moved and
    # this check examined nothing. The -dev package is the exception by
    # content: headers, a static archive and a symlink.
    local found=0 f
    while IFS= read -r -d '' f; do
        is_elf "$f" || continue
        found=1
        check_binary "$f" "$(basename "$deb"):$(basename "$f")"
    done < <(find "$tmp" -type f -print0)
    if [ "$found" -eq 0 ]; then
        case "$(basename "$deb")" in
            *-dev_*) echo "  ok    $(basename "$deb") (development package, no ELF executables)"; CHECKED=$((CHECKED + 1)) ;;
            *) unchecked "$(basename "$deb")" "contains no ELF executables" ;;
        esac
    fi
}

[ $# -gt 0 ] || {
    echo "usage: check-glibc-floor.sh <artifact|binary>..." >&2
    exit 2
}

echo "=== glibc floor check (declared floor: $FLOOR) ==="
for arg in "$@"; do
    [ -e "$arg" ] || { unchecked "$arg" "does not exist"; continue; }
    case "$arg" in
        *.deb) check_deb "$arg" ;;
        *)     check_binary "$arg" "$(basename "$arg")" ;;
    esac
done

print_unchecked() {
    [ "$UNCHECKED" -gt 0 ] || return 0
    echo "  Not checked:" >&2
    local entry
    for entry in "${UNCHECKED_LIST[@]}"; do
        echo "    $entry" >&2
    done
}

if [ "$FAILED" -ne 0 ]; then
    echo "check-glibc-floor: $FAILED binary(ies) above the floor across $CHECKED checked, $UNCHECKED input(s) not checked." >&2
    echo "  A binary above the floor installs cleanly and then fails to start." >&2
    echo "  Build in the image fipsd/packaging/build-floor.env names" >&2
    echo "  (fipsd/packaging/debian/build-deb-container.sh), or raise the floor with the image." >&2
    print_unchecked
    exit 1
fi

if [ "$UNCHECKED" -ne 0 ]; then
    if [ "$CHECKED" -eq 0 ]; then
        echo "check-glibc-floor: could not check $UNCHECKED input(s) and examined no binaries." >&2
    else
        echo "check-glibc-floor: could not check $UNCHECKED input(s); nothing was found above the floor in the $CHECKED binaries checked." >&2
    fi
    print_unchecked
    exit 2
fi

if [ "$CHECKED" -eq 0 ]; then
    echo "check-glibc-floor: examined no binaries; refusing to report a pass." >&2
    exit 2
fi

echo "=== glibc floor check passed ($CHECKED binaries, all at or below $FLOOR) ==="
