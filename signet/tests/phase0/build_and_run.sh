#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Phase 0 spike: build and run the standalone feasibility tests.
# Links only OpenSSL / libsodium / sqlite3 — no full signet build required.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
inc="$here/../../include"
src="$here/../../src"
CC="${CC:-cc}"

pc() { pkg-config "$@" 2>/dev/null; }

ossl_cflags="$(pc --cflags openssl || echo "-I/opt/homebrew/opt/openssl@3/include")"
ossl_libs="$(pc --libs openssl || echo "-L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto")"
sodium_cflags="$(pc --cflags libsodium || echo "-I/opt/homebrew/include")"
sodium_libs="$(pc --libs libsodium || echo "-L/opt/homebrew/lib -lsodium")"
sqlite_cflags="$(pc --cflags sqlite3 || true)"
sqlite_libs="$(pc --libs sqlite3 || echo "-lsqlite3")"

CFLAGS="-std=c11 -Wall -Wextra -O1 -I$inc $ossl_cflags $sodium_cflags $sqlite_cflags"
out="$here/_build"; mkdir -p "$out"

echo "== compiling =="
$CC $CFLAGS "$here/test_fido_crypto.c" "$src/fido_crypto_openssl.c" \
    $ossl_libs -o "$out/test_fido_crypto"
$CC $CFLAGS "$here/test_fido_cbor.c" "$src/fido_crypto_openssl.c" "$src/fido_cbor.c" \
    $ossl_libs -o "$out/test_fido_cbor"
$CC $CFLAGS "$here/test_passkey_store.c" "$src/fido_crypto_openssl.c" \
    $ossl_libs $sodium_libs $sqlite_libs -o "$out/test_passkey_store"

echo "== running =="
"$out/test_fido_crypto"
"$out/test_fido_cbor"
"$out/test_passkey_store"
echo "== all phase0 spike tests passed =="
