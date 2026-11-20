#!/bin/sh
# Compare the FIPS compatibility watch set without changing either repository.
# Exit 0: unchanged; 1: watched changes require review; 2: invalid invocation.
set -eu

usage() {
    echo "Usage: $0 [-C <fips-repo>] [<old-rev>] <new-rev>" >&2
    echo "  -C defaults to the current directory; omitted old-rev uses UPSTREAM.md." >&2
    exit 2
}
fail() {
    printf 'fips-upstream-diff: %s\n' "$*" >&2
    exit 2
}

repo=.
while [ "$#" -gt 0 ]; do
    case "$1" in
        -C) [ "$#" -ge 2 ] || usage; repo=$2; shift 2 ;;
        --) shift; break ;;
        -*) usage ;;
        *) break ;;
    esac
done
case "$#" in
    1)
        script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd) || fail "cannot locate script"
        pin_file=$script_dir/../libfips-core/UPSTREAM.md
        [ -r "$pin_file" ] || fail "cannot read $pin_file"
        old=$(sed -n "s/^Pinned commit: \`\([0-9a-f]\{40\}\)\`$/\1/p" "$pin_file")
        [ -n "$old" ] || fail "missing Pinned commit in $pin_file"
        new=$1
        ;;
    2) old=$1; new=$2 ;;
    *) usage ;;
esac
# Canonicalize before Git calls so relative -C paths work from any directory.
case "$repo" in /*) ;; *) repo=$PWD/$repo ;; esac
repo=$(CDPATH='' cd "$repo" && pwd) || fail "cannot open upstream repository"
old_commit=$(git -C "$repo" rev-parse --verify --end-of-options "$old^{commit}" 2>/dev/null) ||
    fail "invalid old revision: $old"
new_commit=$(git -C "$repo" rev-parse --verify --end-of-options "$new^{commit}" 2>/dev/null) ||
    fail "invalid new revision: $new"

fixed='core/src/noise/mod.rs
core/src/noise/cipher.rs
core/src/proto/link.rs
core/src/proto/framing.rs
core/src/proto/coord.rs
docs/reference/wire-formats.md
docs/reference/nostr-events.md
docs/reference/control-socket.md
docs/reference/native-api.md
docs/reference/configuration.md
CHANGELOG.md'
# Union both trees: additions and removals must be visible, including nested wire.rs.
old_paths=$(git -C "$repo" ls-tree -r --name-only "$old_commit" -- core/src/proto/) ||
    fail "cannot list old tree"
new_paths=$(git -C "$repo" ls-tree -r --name-only "$new_commit" -- core/src/proto/) ||
    fail "cannot list new tree"
wire_paths=$(printf '%s\n%s\n' "$old_paths" "$new_paths" |
    LC_ALL=C awk '/^core\/src\/proto\/.*\/wire\.rs$/')
files=$(printf '%s\n%s\n' "$fixed" "$wire_paths" | LC_ALL=C sort -u)
changed=0
unchanged=0
noise=0
link=0
nostr=0
host=0
formats=0
changelog=0
areas=
printf 'FIPS upstream diff: %s -> %s\nRepository: %s\n\n' "$old_commit" "$new_commit" "$repo"
while IFS= read -r file; do
    [ -n "$file" ] || continue
    if git -C "$repo" diff --no-ext-diff --no-textconv --quiet "$old_commit" "$new_commit" -- "$file"; then
        printf 'unchanged %s\n' "$file"
        unchanged=$((unchanged + 1))
    else
        status=$?
        [ "$status" -eq 1 ] || fail "cannot diff $file (git exit $status)"
        printf 'changed   %s\n' "$file"
        changed=$((changed + 1))
        case "$file" in
            core/src/noise/*) noise=1 ;;
            core/src/proto/*/wire.rs)
                area=${file#core/src/proto/}
                area=${area%%/*}
                case " $areas " in
                    *" $area "*) ;;
                    *) areas="${areas:+$areas }$area" ;;
                esac
                ;;
            core/src/proto/link.rs|core/src/proto/framing.rs|core/src/proto/coord.rs) link=1 ;;
            docs/reference/wire-formats.md) formats=1 ;;
            docs/reference/nostr-events.md) nostr=1 ;;
            docs/reference/control-socket.md|docs/reference/native-api.md|docs/reference/configuration.md) host=1 ;;
            CHANGELOG.md) changelog=1 ;;
        esac
    fi
done <<EOF
$files
EOF

printf '\nCHANGELOG hunk:\n'
if [ "$changelog" -eq 1 ]; then
    git -C "$repo" --no-pager diff --no-ext-diff --no-textconv --no-color \
        --no-renames --unified=3 "$old_commit" "$new_commit" -- CHANGELOG.md ||
        fail "cannot diff CHANGELOG.md"
else
    printf '(unchanged)\n'
fi
printf '\nDifferential-vector regeneration checklist:\n'
if [ "$changed" -eq 0 ]; then
    printf '(none: watch set unchanged)\n'
else
    if [ "$noise" -eq 1 ]; then printf '[ ] noise vectors (IK, XK, sessions, replay)\n'; fi
    # Word splitting is intentional: areas is a deduplicated list of module names.
    for area in $areas; do printf '[ ] %s vectors\n' "$area"; done
    if [ "$link" -eq 1 ]; then printf '[ ] link + framing + coord vectors\n'; fi
    if [ "$nostr" -eq 1 ]; then printf '[ ] nostr fixtures\n'; fi
    if [ "$host" -eq 1 ]; then printf '[ ] fipsd golden snapshots\n'; fi
    if [ "$formats" -eq 1 ]; then
        printf '[ ] full protocol-vector sweep (including rekey and legacy 0.5.x)\n'
    fi
    if [ "$changelog" -eq 1 ]; then
        printf '[ ] review CHANGELOG for additional vector families\n'
    fi
fi
printf '\nSummary: %s changed, %s unchanged watched files.\n' "$changed" "$unchanged"
[ "$changed" -eq 0 ] || exit 1
