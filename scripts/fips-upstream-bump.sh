#!/usr/bin/env bash
# Non-mutating candidate rehearsal. No pin/fixture edits, commits or issue updates.
set -Eeuo pipefail
trap 'echo "fips-upstream-bump: step failed (not a compatibility pass)" >&2; exit 2' ERR
root="$(cd "$(dirname "$0")/.." && pwd -P)"
usage() {
  echo "usage: $0 -C absolute-upstream-repo candidate [--legacy-patch absolute-patch] [--keep]" >&2
  echo "       $0 --check-perf-baseline   # fast, read-only accepted-pin preflight" >&2
  exit 2
}
perf_check_only=0
patch=; keep=0
if [[ $# == 1 && "$1" == --check-perf-baseline ]]; then
  perf_check_only=1
else
  [[ $# -ge 3 && "$1" == -C && "$2" == /* ]] || usage
  repo="$2"; candidate="$3"; shift 3
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --legacy-patch) [[ $# -ge 2 && "$2" == /* ]] || usage; patch="$2"; shift 2 ;;
      --keep) keep=1; shift ;;
      *) usage ;;
    esac
  done
fi
read -r old toolchain perf_reference < <(python3 - "$root" <<'PYBASE'
from pathlib import Path
import json, re, sys, tomllib
r = Path(sys.argv[1])
c = r / 'tools/fips-vectors'
pin = tomllib.loads((c / 'Cargo.toml').read_text())['dependencies']['fips-core']['rev']
assert re.fullmatch(r'[0-9a-f]{40}', pin), 'exporter requires the full upstream pin'
assert f'Pinned commit: `{pin}`' in (r / 'libfips-core/UPSTREAM.md').read_text()
baseline = json.loads((r / 'fipsd/tests/perf_baseline.json').read_text())
if baseline['rust_commit'] != pin:
    raise SystemExit('UPSTREAM.md differs from the immutable Rust performance baseline; '
                     'remeasure the immutable C reference with the accepted Rust pin '
                     '(fipsd/docs/perf.md, Baseline/comparer policy). Do not relabel old timings.')
assert re.fullmatch(r'[0-9a-f]{40}', baseline['reference_commit']), 'invalid C reference commit'
print(pin, tomllib.loads((c / 'rust-toolchain.toml').read_text())['toolchain']['channel'],
      baseline['reference_commit'])
PYBASE
)
[[ -n "$old" && -n "$toolchain" && -n "$perf_reference" ]]
echo "Performance baseline: Rust $old; immutable C reference $perf_reference"
if [[ "$perf_check_only" == 1 ]]; then exit 0; fi
for tool in git python3 cargo rustup diff; do command -v "$tool" >/dev/null; done
if [[ ${FIPS_NODE_FAULTS+x} || ${FIPS_REKEY_FAULTS+x} ]]; then
  echo 'unset FIPS_NODE_FAULTS and FIPS_REKEY_FAULTS before rehearsal' >&2
  exit 2
fi
new="$(git -C "$repo" rev-parse --verify --end-of-options "$candidate^{commit}")"
if [[ "$new" != "$old" ]]; then
  echo "Acceptance requires performance re-baselining with Rust $new and unchanged C $perf_reference."
  echo 'See fipsd/docs/perf.md; keep workload/tolerance fixed, retain raw measurements, then run --check-perf-baseline.'
  echo 'Hosted qualification still requires candidate and immutable reference measured on the same runner.'
fi
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
