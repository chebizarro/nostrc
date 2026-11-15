#!/usr/bin/env bash
# Apply a test-only bridge to a disposable checkout, never the source checkout.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd -P)"
source_repo="${1:-/Users/bizarro/Documents/Dev/fips}"
out="${2:-$root/libfips-core/tests/vectors/rekey-legacy}"
if [[ $# -gt 2 || "$source_repo" != /* || "$out" != "$root"/* ]]; then
  echo "usage: $0 [absolute-upstream-repo [absolute-output-dir-under-$root]]" >&2
  exit 2
fi
out="$(python3 - "$root" "$out" <<'PATHCHECK'
from pathlib import Path
import sys
root, out = (Path(p).resolve() for p in sys.argv[1:])
assert out != root and out.is_relative_to(root), 'output must stay within this worktree'
print(out)
PATHCHECK
)"
if [[ ${FIPS_NODE_FAULTS+x} || ${FIPS_REKEY_FAULTS+x} ]]; then
  echo 'unset FIPS_NODE_FAULTS and FIPS_REKEY_FAULTS before capture' >&2
  exit 1
fi
# Derive the pin/toolchain from the owning files, not a second moving baseline.
read_config() {
  python3 - "$root" <<'CONFIG'
from pathlib import Path
import re, sys, tomllib
root = Path(sys.argv[1])
crate = tomllib.loads((root / 'tools/fips-vectors/Cargo.toml').read_text())
pin = crate['dependencies']['fips-core']['rev']
assert re.fullmatch('[0-9a-f]{40}', pin)
assert f'Pinned commit: `{pin}`' in (root / 'libfips-core/UPSTREAM.md').read_text()
toolchain = tomllib.loads((root / 'tools/fips-vectors/rust-toolchain.toml').read_text())
print(pin, toolchain['toolchain']['channel'])
CONFIG
}
read -r pin toolchain < <(read_config)
patch="$root/tools/fips-vectors/upstream-patches/$pin-legacy-rekey-export.patch"
[[ -f "$patch" ]]
mkdir -p "$root/build-legacy-capture"
work="$(mktemp -d "$root/build-legacy-capture/capture.XXXXXX")"
trap 'rm -rf "$work"' EXIT HUP INT TERM
# A shared clone reads source objects but owns its checkout/index/refs. No fetch,
# checkout, worktree registration or patch is ever performed in source_repo.
git -C "$root" clone --shared --no-checkout "$source_repo" "$work/upstream"
git -C "$work/upstream" checkout --detach "$pin"
[[ "$(git -C "$work/upstream" rev-parse HEAD)" == "$pin" ]]
git -C "$work/upstream" apply --check --unidiff-zero "$patch"
git -C "$work/upstream" apply --unidiff-zero "$patch"
for run in a b; do
  CARGO_TARGET_DIR="$work/target" FIPS_LEGACY_EXPORT_DIR="$work/$run" \
    cargo "+$toolchain" test --manifest-path "$work/upstream/Cargo.toml" --locked \
      -p fips-core --features slow-tests,test-hooks,ring \
      proto::rekey::tests::legacy::capture::export_legacy_rekey_vectors -- --ignored --exact
done
python3 - "$work/a" "$work/b" "$out" "$pin" <<'VERIFY'
from pathlib import Path
import hashlib, json, sys
first, second, out = map(Path, sys.argv[1:4])
pin = sys.argv[4]
a = {p.name: p.read_bytes() for p in first.iterdir()}
b = {p.name: p.read_bytes() for p in second.iterdir()}
assert a == b, 'two fresh test processes did not produce byte-identical traces/sidecars'
expected = {f'{v}-{p}-{r}.json' for v in ('v050-v051', 'v052', 'v053')
            for p in ('fmp', 'fsp') for r in ('current-initiates', 'legacy-initiates')}
assert {n for n in a if n.endswith('.json')} == expected
refs = set()
def check(v):
    if isinstance(v, list):
        for child in v: check(child)
    elif isinstance(v, dict):
        if 'file' in v:
            name = v['file']
            assert Path(name).name == name and name.endswith('.bin')
            raw = a[name]
            assert v['pin'] == pin and v['generator_version'] == '0.1.0'
            assert v['length'] == len(raw) and v['hex'] == raw.hex()
            assert v['sha256'] == hashlib.sha256(raw).hexdigest()
            refs.add(name)
        for child in v.values(): check(child)
for name in expected:
    d = json.loads(a[name])
    assert d['pin'] == pin and d['schema_version'] == 1
    assert d['family'] == 'rekey-legacy' and d['generator'] == 'fips-vectors'
    assert d['generator_version'] == '0.1.0' and d['cases']['events']
    check(d)
assert refs == {n for n in a if n.endswith('.bin')}, 'orphan/missing binary sidecar'
assert len(a) == len(expected) + len(refs), 'unexpected generated file'
# Refuse stale names rather than silently keeping an obsolete compatibility trace.
if out.exists():
    assert out.is_dir() and all(p.is_file() and p.name in a for p in out.iterdir()), 'unexpected existing output'
out.mkdir(parents=True, exist_ok=True)
for name, raw in a.items(): (out / name).write_bytes(raw)
print(f'Legacy capture: {len(expected)} traces (4 per release model), {len(refs)} sidecars; two runs byte-identical; all descriptors verified')
VERIFY
