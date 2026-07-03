#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Phase 0 spike: independent WebAuthn verification with python-fido2.
# Builds the artifact emitter, emits a real attestation object + assertion,
# and verifies them with an independent implementation (python-fido2).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
inc="$here/../../include"
src="$here/../../src"
out="$here/_build"; mkdir -p "$out"
CC="${CC:-cc}"

pc() { pkg-config "$@" 2>/dev/null; }
ossl_cflags="$(pc --cflags openssl || echo "-I/opt/homebrew/opt/openssl@3/include")"
ossl_libs="$(pc --libs openssl || echo "-L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto")"
cbor_cflags="$(pc --cflags libcbor || true)"
cbor_libs="$(pc --libs libcbor || echo "-lcbor")"

echo "== building emitter =="
$CC -std=c11 -Wall -Wextra -O1 -I"$inc" $ossl_cflags $cbor_cflags \
    "$here/emit_artifacts.c" "$src/fido_crypto_openssl.c" "$src/fido_cbor.c" \
    $ossl_libs $cbor_libs -o "$out/emit_artifacts"

"$out/emit_artifacts" > "$out/artifacts.json"
echo "== emitted $out/artifacts.json =="

# Provision an isolated venv with python-fido2 (only installs once).
venv="$out/venv"
if [ ! -x "$venv/bin/python" ]; then
    python3 -m venv "$venv"
fi
if ! "$venv/bin/python" -c "import fido2" 2>/dev/null; then
    "$venv/bin/pip" install --quiet --disable-pip-version-check fido2
fi

echo "== independent verification (python-fido2) =="
"$venv/bin/python" "$here/verify_external.py" "$out/artifacts.json"
