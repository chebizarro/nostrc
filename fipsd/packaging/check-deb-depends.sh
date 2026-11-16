#!/bin/bash
# Fail when a package's declared Depends does not match the libraries its
# binaries actually need.
#
# debhelper fills ${shlibs:Depends} from dpkg-shlibdeps, so a mismatch here
# means a hand-written entry in fipsd/packaging/debian/control, a binary
# dpkg-shlibdeps skipped with a warning, or a package whose layout moved.
# This runs dpkg-shlibdeps once over every ELF object the package contains
# and compares the result with the Depends the package ships:
#
#   - every derived entry must be shipped;
#   - for a derived "name (>= v)", the highest ">=" floor shipped for that
#     name must EQUAL v. Below it, the package installs where the binaries
#     cannot run. Above it, a hand-written floor no longer tracks what the
#     binaries need;
#   - shipped entries that nothing derives (systemd) are listed, not failed:
#     they are the ones written by hand in debian/control.
#
# It also compares the package's Recommends with the Recommends
# debian/control declares for it, entry for entry and in any order, so a
# packaging change that dropped or altered it cannot ship unnoticed.
#
# Run it on the distribution the package was built on, so the symbols files
# and the C library it reads are the ones dpkg-shlibdeps read at build time.
#
# Usage: check-deb-depends.sh <package.deb>...
# debian/control is read from the checkout this script sits in.
# Exit: 0 every package matches, 1 a mismatch, 2 could not establish a result.

set -euo pipefail

for tool in dpkg dpkg-deb dpkg-query dpkg-shlibdeps readelf; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "check-deb-depends: $tool is not installed; cannot check anything." >&2
        echo "                   Refusing to report a pass I did not establish." >&2
        exit 2
    }
done

[ $# -gt 0 ] || {
    echo "usage: check-deb-depends.sh <package.deb>..." >&2
    exit 2
}

CONTROL="$(cd "$(dirname "$0")" && pwd)/debian/control"
[ -r "$CONTROL" ] || {
    echo "check-deb-depends: cannot read $CONTROL." >&2
    exit 2
}

# The Recommends line debian/control declares for a binary package, or
# empty when it declares none. Only one-line values are supported; the
# control file is ours to keep that way.
declared_recommends() {
    awk -v pkg="$1" '
        /^Package:/ { inpkg = ($2 == pkg); next }
        inpkg && /^Recommends:/ { sub(/^Recommends:[[:space:]]*/, ""); print; exit }
    ' "$CONTROL"
}

FAILED=0
UNKNOWN=0
CHECKED=0

# A relation list entry of the simple forms dpkg-shlibdeps prints: a bare name
# or "name (>= version)". Anything else is compared word for word.
SIMPLE_RE='^([a-z0-9][a-z0-9+.-]*)( \(>= ([^)]+)\))?$'

# A regular file that is an ELF executable or shared object (not an archive,
# not a relocatable object), whatever its mode.
is_elf_object() {
    head -c 4 "$1" 2>/dev/null | cmp -s - <(printf '\x7fELF') || return 1
    readelf -hW "$1" 2>/dev/null | grep -qE '^ *Type: *(EXEC|DYN)'
}

# Split a Depends value on commas into one trimmed entry per line.
split_deps() {
    printf '%s\n' "$1" | tr ',' '\n' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' | sed '/^$/d'
    return 0
}

check_deb() {
    local deb="$1" label tmp arch shipped derived out package
    label=$(basename "$deb")
    tmp=$(mktemp -d)
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN

    if ! dpkg-deb -x "$deb" "$tmp/root"; then
        echo "  ERROR $label could not be unpacked" >&2
        UNKNOWN=$((UNKNOWN + 1))
        return
    fi
    package=$(dpkg-deb -f "$deb" Package)
    arch=$(dpkg-deb -f "$deb" Architecture)
    shipped=$(dpkg-deb -f "$deb" Depends)
    if [ -z "$arch" ] || [ -z "$package" ]; then
        echo "  ERROR $label has no Architecture or Package field" >&2
        UNKNOWN=$((UNKNOWN + 1))
        return
    fi

    # Every ELF executable or shared object, found by magic rather than by
    # mode: dh_fixperms strips the execute bit from shared libraries, and a
    # static archive or an object file is ELF inside but not something
    # dpkg-shlibdeps reads. The same filter as check-glibc-floor.sh.
    local bins=() f
    while IFS= read -r -d '' f; do
        is_elf_object "$f" || continue
        bins+=("$f")
    done < <(find "$tmp/root" -type f -print0 | sort -z)
    if [ "${#bins[@]}" -eq 0 ]; then
        # The -dev package ships headers, a static archive and a symlink:
        # nothing dpkg-shlibdeps can read, so its Depends are all written by
        # hand (libfipsnative0 (= version)) and are listed, not compared.
        case "$package" in
            *-dev)
                echo "  $label ($arch): no ELF objects; nothing to derive"
                while IFS= read -r e; do echo "    hand-declared $e"; done < <(split_deps "$shipped")
                CHECKED=$((CHECKED + 1))
                return
                ;;
        esac
        echo "  ERROR $label contains no ELF objects; the layout moved and nothing was checked" >&2
        UNKNOWN=$((UNKNOWN + 1))
        return
    fi

    # dpkg-shlibdeps needs a debian/control in its working directory, and an
    # empty one is enough. DEB_HOST_ARCH selects the symbols files, so a
    # foreign-architecture package is resolved against that architecture's.
    #
    # A library the package ships privately (libnostr-json under
    # usr/lib/<triplet>/fipsd, reached through an RPATH) is found through -l
    # and has no dependency information, which is right: it is in the same
    # package. --ignore-missing-info is needed for that one case, so the
    # warnings it turns the errors into are read back, and any library
    # outside the unpacked package that lacks information is an error here.
    mkdir -p "$tmp/work/debian"
    : > "$tmp/work/debian/control"
    local privdirs=() d
    while IFS= read -r -d '' d; do privdirs+=("-l$d"); done \
        < <(find "$tmp/root/usr/lib" -mindepth 2 -maxdepth 2 -type d -path '*-linux-*' -print0 2>/dev/null)
    if ! out=$(cd "$tmp/work" && DEB_HOST_ARCH="$arch" dpkg-shlibdeps -O --ignore-missing-info "${privdirs[@]}" "${bins[@]}" 2>"$tmp/shlibdeps.err"); then
        echo "  ERROR $label: dpkg-shlibdeps failed:" >&2
        sed 's/^/        /' "$tmp/shlibdeps.err" >&2
        UNKNOWN=$((UNKNOWN + 1))
        return
    fi
    if grep 'no dependency information found for' "$tmp/shlibdeps.err" | grep -qv "for $tmp/root/"; then
        echo "  ERROR $label: dpkg-shlibdeps found a library it has no dependency information for:" >&2
        grep 'no dependency information found for' "$tmp/shlibdeps.err" | sed 's/^/        /' >&2
        UNKNOWN=$((UNKNOWN + 1))
        return
    fi
    derived=$(printf '%s\n' "$out" | sed -n 's/^shlibs:Depends=//p' | head -n 1)
    if [ -z "$derived" ]; then
        echo "  ERROR $label: dpkg-shlibdeps derived no dependencies for ${#bins[@]} ELF object(s)" >&2
        UNKNOWN=$((UNKNOWN + 1))
        return
    fi

    echo "  $label ($arch, ${#bins[@]} ELF objects)"
    echo "    derived: $derived"
    echo "    shipped: $shipped"

    # Index the shipped list: the highest ">=" floor per name, the names
    # present at all, and every entry verbatim.
    declare -A floor=() present=() verbatim=() derivednames=()
    local e name ver
    while IFS= read -r e; do
        verbatim["$e"]=1
        [[ "$e" =~ $SIMPLE_RE ]] || continue
        name="${BASH_REMATCH[1]}"
        ver="${BASH_REMATCH[3]}"
        present["$name"]=1
        [ -n "$ver" ] || continue
        if [ -z "${floor[$name]:-}" ] || dpkg --compare-versions "$ver" gt "${floor[$name]}"; then
            floor["$name"]="$ver"
        fi
    done < <(split_deps "$shipped")

    local bad=0 have
    while IFS= read -r e; do
        if ! [[ "$e" =~ $SIMPLE_RE ]]; then
            if [ -n "${verbatim[$e]:-}" ]; then
                echo "    ok    $e"
            else
                echo "  FAIL $label: derived '$e' not matched by shipped Depends (missing: $shipped)" >&2
                bad=1
            fi
            continue
        fi
        name="${BASH_REMATCH[1]}"
        ver="${BASH_REMATCH[3]}"
        derivednames["$name"]=1
        if [ -z "${present[$name]:-}" ]; then
            echo "  FAIL $label: derived '$e' not matched by shipped Depends (missing: $shipped)" >&2
            bad=1
            continue
        fi
        if [ -z "$ver" ]; then
            echo "    ok    $e"
            continue
        fi
        have="${floor[$name]:-}"
        if [ -z "$have" ]; then
            echo "  FAIL $label: derived '$e' not matched by shipped Depends (below: $name with no version floor)" >&2
            bad=1
        elif dpkg --compare-versions "$have" lt "$ver"; then
            echo "  FAIL $label: derived '$e' not matched by shipped Depends (below: $name (>= $have))" >&2
            bad=1
        elif dpkg --compare-versions "$have" gt "$ver"; then
            echo "  FAIL $label: derived '$e' not matched by shipped Depends (above: $name (>= $have))" >&2
            bad=1
        else
            echo "    ok    $e"
        fi
    done < <(split_deps "$derived")

    while IFS= read -r e; do
        if [[ "$e" =~ $SIMPLE_RE ]] && [ -n "${derivednames[${BASH_REMATCH[1]}]:-}" ]; then
            continue
        fi
        echo "    hand-declared $e"
    done < <(split_deps "$shipped")

    # Recommends: the same entries as debian/control declares, order aside.
    local recommends declared want got
    recommends=$(dpkg-deb -f "$deb" Recommends)
    declared=$(declared_recommends "$package")
    echo "    recommends shipped: ${recommends:-(none)}; declared: ${declared:-(none)}"
    want=$(split_deps "$declared" | sort)
    got=$(split_deps "$recommends" | sort)
    if [ "$want" != "$got" ]; then
        echo "  FAIL $label: Recommends '${recommends}' differs from debian/control's '${declared}'" >&2
        bad=1
    fi

    CHECKED=$((CHECKED + 1))
    [ "$bad" -eq 0 ] || FAILED=$((FAILED + 1))
}

echo "=== Depends check (shipped Depends against dpkg-shlibdeps, Recommends against debian/control) ==="
for arg in "$@"; do
    if [ ! -f "$arg" ]; then
        echo "  ERROR $arg does not exist" >&2
        UNKNOWN=$((UNKNOWN + 1))
        continue
    fi
    check_deb "$arg"
done

if [ "$FAILED" -ne 0 ]; then
    echo "check-deb-depends: $FAILED of $CHECKED package(s) declare Depends that differ from what their binaries need," >&2
    echo "  or Recommends that differ from what debian/control declares." >&2
    echo "  A missing or low Depends entry installs where the binaries cannot run; a high one" >&2
    echo "  is a hand-written floor that no longer tracks them. Fix fipsd/packaging/debian/control." >&2
    exit 1
fi

# Anything not established, or nothing examined at all, is not a pass.
if [ "$UNKNOWN" -ne 0 ] || [ "$CHECKED" -eq 0 ]; then
    echo "check-deb-depends: could not establish a result ($UNKNOWN error(s), $CHECKED package(s) checked); refusing to report a pass." >&2
    exit 2
fi

echo "=== Depends check passed ($CHECKED package(s)) ==="
