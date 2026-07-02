#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Phase 3 interop gate: run the wired SignetFidoService JSON entrypoint emitter
# and independently verify its WebAuthn artifacts with python-fido2.
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 /path/to/test_fido_interop_json [output-dir]" >&2
  exit 2
fi

emitter="$1"
here="$(cd "$(dirname "$0")" && pwd)"
out="${2:-$here/phase0/_build}"
mkdir -p "$out"
artifact="$out/phase3_interop_artifacts.json"

echo "== emitting Phase 3 wired JSON-entrypoint artifacts =="
"$emitter" > "$artifact"
echo "== emitted $artifact =="

venv="$here/phase0/_build/venv"
if [ ! -x "$venv/bin/python" ]; then
  python3 -m venv "$venv"
fi
if ! "$venv/bin/python" -c "import fido2" 2>/dev/null; then
  "$venv/bin/pip" install --quiet --disable-pip-version-check fido2==2.2.1
fi

echo "== independent verification (python-fido2) =="
"$venv/bin/python" "$here/phase0/verify_external.py" "$artifact"
