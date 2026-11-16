#!/usr/bin/env bash
# Non-mutating candidate rehearsal. No pin/fixture edits, commits or issue updates.
set -Eeuo pipefail
trap 'echo "fips-upstream-bump: step failed (not a compatibility pass)" >&2; exit 2' ERR
root="$(cd "$(dirname "$0")/.." && pwd -P)"
usage() {
  echo "usage: $0 -C absolute-upstream-repo candidate [--legacy-patch absolute-patch] [--keep]" >&2
  exit 2
}
[[ $# -ge 3 && "$1" == -C && "$2" == /* ]] || usage
repo="$2"; candidate="$3"; shift 3
patch=; keep=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --legacy-patch) [[ $# -ge 2 && "$2" == /* ]] || usage; patch="$2"; shift 2 ;;
    --keep) keep=1; shift ;;
    *) usage ;;
  esac
done
for tool in git python3 cargo rustup diff; do command -v "$tool" >/dev/null; done
if [[ ${FIPS_NODE_FAULTS+x} || ${FIPS_REKEY_FAULTS+x} ]]; then
  echo 'unset FIPS_NODE_FAULTS and FIPS_REKEY_FAULTS before rehearsal' >&2
  exit 2
fi
new="$(git -C "$repo" rev-parse --verify --end-of-options "$candidate^{commit}")"
read -r old toolchain < <(python3 - "$root" <<'PY'
from pathlib import Path
import sys, tomllib
r = Path(sys.argv[1])
c = r / 'tools/fips-vectors'
pin = tomllib.loads((c / 'Cargo.toml').read_text())['dependencies']['fips-core']['rev']
assert f'Pinned commit: `{pin}`' in (r / 'libfips-core/UPSTREAM.md').read_text()
print(pin, tomllib.loads((c / 'rust-toolchain.toml').read_text())['toolchain']['channel'])
PY
)
[[ -n "$old" && -n "$toolchain" ]]
rustup run "$toolchain" rustc --version
patch="${patch:-$root/tools/fips-vectors/upstream-patches/$new-legacy-rekey-export.patch}"
mkdir -p "$root/build-fips-upstream-bump"
work="$(mktemp -d "$root/build-fips-upstream-bump/run.XXXXXX")"
cleanup() {
  if [[ "$keep" == 1 ]]; then
    echo "Retained rehearsal (remove after review): $work"
  else
    rm -rf "$work"
  fi
}
trap cleanup EXIT
export CARGO_HOME="$work/cargo-home" CARGO_TARGET_DIR="$work/target"
status=0
"$root/scripts/fips-upstream-diff.sh" -C "$repo" "$old" "$new" > "$work/watch.txt" || status=$?
cat "$work/watch.txt"
[[ "$status" -le 1 ]] || exit 2
git -C "$repo" --no-pager diff --stat "$old" "$new"
[[ -f "$patch" ]] || {
  echo "Missing candidate legacy patch: $patch; review/rebase it first (see UPSTREAM.md)." >&2
  exit 2
}
mkdir -p "$work/tools/fips-vectors"
cp -R "$root/tools/fips-vectors/"{src,fixtures} "$work/tools/fips-vectors/"
cp "$root/tools/fips-vectors/"{Cargo.toml,Cargo.lock,rust-toolchain.toml} "$work/tools/fips-vectors/"
# Only the scratch crate gets candidate provenance. The committed baseline stays put.
python3 - "$work/tools/fips-vectors" "$old" "$new" <<'PY'
from pathlib import Path
import sys
crate, old, new = Path(sys.argv[1]), *sys.argv[2:]
for rel in ('Cargo.toml', 'Cargo.lock', 'src/main.rs'):
    p = crate / rel
    p.write_text(p.read_text().replace(old, new))
PY
# Explicit +toolchain: --manifest-path does NOT select a crate's rust-toolchain.toml.
if [[ "$new" != "$old" ]]; then
  cargo "+$toolchain" update --manifest-path "$work/tools/fips-vectors/Cargo.toml" -p fips-core
fi
cargo "+$toolchain" build --manifest-path "$work/tools/fips-vectors/Cargo.toml" --locked
for run in a b; do
  "$CARGO_TARGET_DIR/debug/fips-vectors" export --out "$work/$run"
done
diff -ru "$work/a" "$work/b"
"$root/tools/fips-vectors/scripts/capture-legacy-rekey.sh" "$repo" "$work/a/rekey-legacy" "$new" "$patch"
comparison=0
diff -ru "$root/libfips-core/tests/vectors" "$work/a" > "$work/fixtures.diff" || comparison=$?
[[ "$comparison" -le 1 ]] || exit 2
if [[ "$comparison" == 0 ]]; then
  echo 'All eight families: byte-identical to checked-in fixtures (including provenance).'
else
  diff -rq "$root/libfips-core/tests/vectors" "$work/a" || [[ $? == 1 ]]
  echo 'Candidate fixtures differ: review wire bytes AND provenance; do not automatically bump.'
fi
echo "Candidate: $new; baseline: $old; watch exit: $status; fixture comparison exit: $comparison"
echo "Generated vector root: $work/a"
echo 'Run the ASan/UBSan C differential sweep separately; this export is not host interop proof.'
if [[ "$status" != 0 || "$comparison" != 0 ]]; then exit 1; fi
