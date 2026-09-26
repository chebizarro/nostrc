#!/usr/bin/env bash
# check-authd-dep-purity.sh — dep-purity gate for nostr-authd + pam_nostr.so.
#
# Execution-Index #21a (docs/plans/gnome-integration-and-samba-server-2026-09-25.md
# §5.1). Baseline scaffold: pins today's known-good dependency closure for the
# two pre-login artifacts and fails loudly the moment either one gains an edge
# to the hanami / porthome / signer-client (nip55l) / FUSE namespaces, or any
# OTHER shared-library dependency not already on the pinned allowlist below.
#
# Item #21b (final integrated check, out of scope here) reruns this same
# script after every track lands; this file is the tripwire it depends on.
#
# Usage:
#   check-authd-dep-purity.sh <path-to-nostr-authd> [path-to-pam_nostr.so]
#
# Design (see scripts/README.md for the full rationale):
#   1. `nm -a` (dynamic + static-archive-derived symbols — NOT just undefined
#      refs, since the forbidden libraries are statically linked archives in
#      this tree today; catching only undefined refs would miss a future
#      static link-in) on each artifact, grepped against FORBIDDEN_SYMBOL_REGEX.
#   2. `ldd` on each artifact: any DT_NEEDED entry matching FORBIDDEN_SONAME_REGEX
#      fails immediately.
#   3. Every OTHER `ldd` entry must appear on that artifact's pinned
#      *_ALLOWED_SONAMES list. Anything new fails. Adding a legitimate new
#      dependency requires a human to edit this file and explain why in the
#      commit message — that manual step is the tripwire.
#
# Exit codes: 0 = clean, 1 = violation found, 2 = usage error,
#             77 = skipped (nm/ldd unavailable — e.g. running on macOS).
#
# SPDX-License-Identifier: MIT
set -euo pipefail

# ---------------------------------------------------------------------------
# PINNED ALLOWLIST — see scripts/README.md before editing.
# ---------------------------------------------------------------------------

# Forbidden symbol-name namespaces. Anchored at the START of the symbol name
# so we do not false-positive on the auth runtime's own boundary glue in
# src/auth/auth_porthome.c (nh_auth_porthome_*, nh_auth_broker_porthome_*),
# which is always compiled into nostr_auth_runtime and is NOT the same thing
# as the real portable-home library's nh_porthome_* prefix.
#   - nh_porthome_    : libnostr_porthome (portable-home crypto/manifest/Blossom)
#   - nh_syncd_       : nostr-home-syncd's libnostr_syncd_core
#   - nh_fuse_        : nostr-home-fuse status writer
#   - nh_provision_   : nostr_provision_cli (links libnostr_porthome)
#   - fuse_           : libfuse3's own C API
#   - hanami_         : libhanami (Blossom/libgit2 backend)
#   - nostr_nip55l_   : nips/nip55l signer CLIENT API (nostr-authd/pam_nostr
#                       must never become nip55l *consumers* — see plan §1.2 D6)
FORBIDDEN_SYMBOL_REGEX='^(nh_porthome_|nh_syncd_|nh_fuse_|nh_provision_|fuse_|hanami_|nostr_nip55l_)'

# Forbidden shared-object name fragments — matched against ldd's SONAME
# column regardless of version suffix.
FORBIDDEN_SONAME_REGEX='libfuse3|libfuse\.so|libhanami|lib(nostr[_-]?)?porthome|libnip55l|libnostr[_-]?nip55l'

# Positive allowlist for nostr-authd. Captured from a clean Ubuntu 24.04
# (aarch64) build of this tree on 2026-09-25 with the same CMake flags
# scripts/vm-linux-ci.sh uses on the lab VM:
#   -DNOSTR_HOMED_ENABLE_AUTH_CORE=ON -DNOSTR_HOMED_ENABLE_IDENTITY_CORE=ON
#   -DNOSTR_HOMED_ENABLE_DOMAIN_CONFIG=ON -DNOSTR_HOMED_ENABLE_NSS=ON
#   -DNOSTR_HOMED_ENABLE_AUTH_RUNTIME=ON -DNOSTR_HOMED_ENABLE_PAM=ON
#   -DNOSTR_HOMED_ENABLE_SMB=ON
# Matched by basename with the trailing ".so.N[.N...]" version stripped down
# to ".so", so routine version bumps of an already-approved library do not
# need an edit here — only a wholly new dependency does.
# shellcheck disable=SC2034  # read via check_ldd's `local -n` nameref
AUTHD_ALLOWED_SONAMES=(
  linux-vdso.so
  ld-linux
  libc.so
  libnostr-json.so
  libjansson.so
  libwebsockets.so
  libnsync.so
  libssl.so
  libcrypto.so
  libsecp256k1.so
  libsqlite3.so
  libcap.so    # transitive: libwebsockets (TLS capability drop)
  libz.so      # transitive: libwebsockets (compression extensions)
  libm.so      # transitive: libsqlite3
)

# Positive allowlist for pam_nostr.so, captured the same way. Notably
# smaller than nostr-authd's: the PAM module does not pull in the
# broker-only nostr_auth_core (transaction/challenge/vault/worker/providers),
# only the shared nostr_auth_runtime bits it actually calls into, so no
# libssl/libcrypto/libsqlite3/libsecp256k1/libwebsockets/libnsync edge shows
# up in its ldd closure today. libaudit/libcap-ng arrive transitively via
# libpam itself on Debian/Ubuntu, not via anything nostr-authd-specific.
# shellcheck disable=SC2034  # read via check_ldd's `local -n` nameref
PAM_NOSTR_ALLOWED_SONAMES=(
  linux-vdso.so
  ld-linux
  libc.so
  libpam.so
  libjansson.so
  libaudit.so
  libcap-ng.so
)

# ---------------------------------------------------------------------------

usage() {
  echo "usage: $0 <path-to-nostr-authd> [path-to-pam_nostr.so]" >&2
  exit 2
}

is_allowed() {
  local name="$1"; shift
  local allowed
  for allowed in "$@"; do
    if [[ "$allowed" == "ld-linux" && "$name" == ld-linux* ]]; then
      return 0
    fi
    if [[ "$name" == "$allowed" ]]; then
      return 0
    fi
  done
  return 1
}

# Strip a path down to basename, then collapse ".so.1.2.3" -> ".so".
normalize_soname() {
  local n="$1"
  n="${n##*/}"
  n="$(printf '%s\n' "$n" | sed -E 's/\.so\..*/.so/')"
  printf '%s\n' "$n"
}

check_symbols() {
  local bin="$1" label="$2"
  local hits
  # -a: dynamic + local/static-archive-derived symbols. -D alone would miss
  # symbols pulled in from a statically linked archive (the current shape of
  # every forbidden library in this tree).
  if hits="$(nm -a "$bin" 2>/dev/null | awk '{print $NF}' | grep -E "$FORBIDDEN_SYMBOL_REGEX" || true)"; then
    if [[ -n "$hits" ]]; then
      echo "FAIL: $label ($bin) contains forbidden-namespace symbols:" >&2
      echo "$hits" >&2
      return 1
    fi
  fi
  return 0
}

check_ldd() {
  local bin="$1" label="$2" allowed_array_name="$3"
  local -n allowed_ref="$allowed_array_name"
  local ldd_out
  ldd_out="$(ldd "$bin" 2>/dev/null || true)"
  if [[ -z "$ldd_out" ]]; then
    # Static binary, or ldd genuinely produced nothing — nothing to audit.
    return 0
  fi

  local forbidden_hits
  forbidden_hits="$(printf '%s\n' "$ldd_out" | grep -E "$FORBIDDEN_SONAME_REGEX" || true)"
  if [[ -n "$forbidden_hits" ]]; then
    echo "FAIL: $label ($bin) links a forbidden shared library:" >&2
    echo "$forbidden_hits" >&2
    return 1
  fi

  local rc=0
  local line raw name
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    raw="$(awk '{print $1}' <<<"$line")"
    name="$(normalize_soname "$raw")"
    if ! is_allowed "$name" "${allowed_ref[@]}"; then
      echo "FAIL: $label ($bin) links '$raw' (normalized: $name)," \
           "which is not on the pinned allowlist ($allowed_array_name)." >&2
      echo "      If this is an intentional, reviewed new dependency, add it" >&2
      echo "      to $allowed_array_name in $0 and explain why in the commit" >&2
      echo "      message. That edit IS the dep-purity tripwire." >&2
      rc=1
    fi
  done <<<"$ldd_out"
  return "$rc"
}

check_artifact() {
  local bin="$1" label="$2" allowed_array_name="$3"
  local rc=0
  check_symbols "$bin" "$label" || rc=1
  check_ldd "$bin" "$label" "$allowed_array_name" || rc=1
  return "$rc"
}

main() {
  local authd_bin="${1:-}"
  local pam_bin="${2:-}"

  [[ -n "$authd_bin" ]] || usage
  [[ -e "$authd_bin" ]] || { echo "error: no such file: $authd_bin" >&2; exit 2; }

  if ! command -v nm >/dev/null 2>&1 || ! command -v ldd >/dev/null 2>&1; then
    echo "SKIP: nm and/or ldd not available on this host (expected on e.g. macOS)" >&2
    exit 77
  fi

  local rc=0
  check_artifact "$authd_bin" "nostr-authd" AUTHD_ALLOWED_SONAMES || rc=1

  if [[ -n "$pam_bin" ]]; then
    [[ -e "$pam_bin" ]] || { echo "error: no such file: $pam_bin" >&2; exit 2; }
    check_artifact "$pam_bin" "pam_nostr.so" PAM_NOSTR_ALLOWED_SONAMES || rc=1
  fi

  if [[ "$rc" -eq 0 ]]; then
    echo "ok: dep-purity gate clean"
  fi
  return "$rc"
}

main "$@"
