#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# grant-login-kind.sh - Turnkey signet ACL grant that unblocks the nostr-homed
# NIP-46 login challenge (kind = NH_AUTH_CHALLENGE_KIND = 1).
#
# Background: nostr-homed's provider_nip46 asks the bunker to sign the auth
# challenge event (kind 1). If the bunker's signet policy store has no
# allow_kinds rule permitting kind 1 for the agent bound to our client
# transport key, sign_event returns reason_code=policy.default_deny (see
# signet/src/policy_store.c). This helper wraps `signetctl set-policy` so
# a bunker operator can grant the kind without hand-rolling any JSON.
#
# Inputs:
#   --interlocutor <path>   interlocutor config.toml (defaults to
#                           $HOME/.config/interlocutor/config.toml). We read
#                           bunker_uri from it and extract the bunker pubkey
#                           + relay list (both live values, no secrets).
#   --bunker-pubkey <hex>   override bunker pubkey (skip the config lookup).
#   --relays <csv>          override relay list (comma-separated URLs).
#   --agent <agent_id>      required. The signet agent whose policy to edit.
#                           Ask the operator, or discover via
#                           `signetctl list` (needs the provisioner nsec).
#   --allow-kinds <csv>     event kinds to append (default: "1"). Use "*"
#                           for wildcard.
#   --allow-methods <csv>   NIP-46 methods to allow (default:
#                           "sign_event,get_public_key").
#   --default <allow|deny>  policy default (default: deny). We keep deny by
#                           default because this helper's only job is to open
#                           a narrow ACL hole, not to relax the whole agent.
#   --dry-run               print the resolved signetctl invocation + JSON
#                           and exit without contacting the bunker.
#   --signetctl <path>      override signetctl binary path (default: search
#                           /tmp/signet-grant-build/signet/signetctl, then
#                           PATH).
#   -h, --help              show this help.
#
# ONE operator-supplied secret is required: the provisioner nsec, mounted
# as a file and pointed at by SIGNET_PROVISIONER_NSEC_FILE. signetctl reads
# it via signetctl_resolve_provisioner_key() (signet/src/signetctl_main.c);
# argv-inline / SIGNET_PROVISIONER_NSEC (env) are rejected on purpose.
#
# We never echo, log, copy, or serialize the provisioner nsec, the bunker
# nsec, the client transport secret, or the URI's `secret=` connect token.
# The interlocutor config's bunker_uri is parsed for its bunker pubkey and
# relay list only.
#
# On success the helper prints the daemon's ack ("policy_set" or
# "policy_set_not_persisted") and exits 0. On error it exits non-zero with
# the underlying signetctl / bunker message.
#
# Beads: nostrc-ot2c.7 (C7 - external pre-login provider full-OK proof),
# nostrc-7t61 (C7-operator ACL follow-up).

set -euo pipefail

die() { printf 'grant-login-kind: %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '3,60p' "$0" | sed 's/^# \{0,1\}//'
}

interlocutor_config="${HOME:-}/.config/interlocutor/config.toml"
bunker_pubkey=""
relays=""
agent_id=""
allow_kinds="1"
allow_methods="sign_event,get_public_key"
default_decision="deny"
dry_run=0
signetctl_bin=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --interlocutor) interlocutor_config="${2:?}"; shift 2 ;;
    --bunker-pubkey) bunker_pubkey="${2:?}"; shift 2 ;;
    --relays) relays="${2:?}"; shift 2 ;;
    --agent) agent_id="${2:?}"; shift 2 ;;
    --allow-kinds) allow_kinds="${2:?}"; shift 2 ;;
    --allow-methods) allow_methods="${2:?}"; shift 2 ;;
    --default) default_decision="${2:?}"; shift 2 ;;
    --signetctl) signetctl_bin="${2:?}"; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

[[ -n "$agent_id" ]] || die "--agent <agent_id> is required. Discover live agent ids with 'signetctl list' (needs the provisioner nsec)."

case "$default_decision" in
  allow|deny) ;;
  *) die "--default must be 'allow' or 'deny'" ;;
esac

# 1. Locate signetctl.
if [[ -z "$signetctl_bin" ]]; then
  for candidate in /tmp/signet-grant-build/signet/signetctl "$(command -v signetctl 2>/dev/null || true)"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      signetctl_bin="$candidate"
      break
    fi
  done
fi
[[ -n "$signetctl_bin" && -x "$signetctl_bin" ]] || die "signetctl binary not found. Build it (see signet/docs/GRANT_LOGIN_KIND.md) or pass --signetctl <path>."

# 2. Resolve bunker pubkey + relay list.
if [[ -z "$bunker_pubkey" || -z "$relays" ]]; then
  [[ -r "$interlocutor_config" ]] || die "interlocutor config '$interlocutor_config' not readable; pass --interlocutor, --bunker-pubkey, --relays."
  # Extract the bunker_uri line ("bunker://<hex>?relay=...&relay=...&secret=...").
  bunker_uri=$(sed -n 's/^[[:space:]]*bunker_uri[[:space:]]*=[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$interlocutor_config" | head -n1)
  [[ -n "$bunker_uri" && "$bunker_uri" == bunker://* ]] || die "no valid bunker_uri in $interlocutor_config"
  if [[ -z "$bunker_pubkey" ]]; then
    # <hex> between "bunker://" and "?"
    bunker_pubkey="${bunker_uri#bunker://}"
    bunker_pubkey="${bunker_pubkey%%\?*}"
  fi
  if [[ -z "$relays" ]]; then
    # All &relay=... query params, url-decoded (%2F -> /, %3A -> :).
    query="${bunker_uri#*\?}"
    relay_list=""
    IFS='&' read -ra parts <<<"$query"
    for p in "${parts[@]}"; do
      case "$p" in
        relay=*)
          v="${p#relay=}"
          # Basic URL-decode for %XX bytes (relay URLs only use %2F/%3A in practice).
          decoded=$(printf '%b' "$(printf '%s' "$v" | sed 's/+/ /g; s/%\([0-9A-Fa-f]\{2\}\)/\\x\1/g')")
          if [[ -z "$relay_list" ]]; then relay_list="$decoded"; else relay_list="$relay_list,$decoded"; fi
          ;;
      esac
    done
    relays="$relay_list"
  fi
fi
[[ "$bunker_pubkey" =~ ^[0-9a-f]{64}$ ]] || die "bunker pubkey must be 64 lowercase hex chars (got: $bunker_pubkey)"
[[ -n "$relays" ]] || die "no relays resolved; pass --relays wss://..."

# 3. Require the provisioner nsec (file-only per signetctl policy).
[[ -n "${SIGNET_PROVISIONER_NSEC_FILE:-}" ]] || die "SIGNET_PROVISIONER_NSEC_FILE not set. Mount the provisioner nsec (nsec1... or 64-hex, single line, mode 0600) and export the path. signetctl rejects SIGNET_PROVISIONER_NSEC (env-inline) and argv-inline secrets on purpose."
if [[ $dry_run -eq 0 ]]; then
  [[ -r "$SIGNET_PROVISIONER_NSEC_FILE" ]] || die "SIGNET_PROVISIONER_NSEC_FILE='$SIGNET_PROVISIONER_NSEC_FILE' is not readable by this user."
fi

# 4. Build the policy JSON (shape documented at
#    signet/include/signet/policy_store.h:168 - signet_policy_store_set_identity_json).
#    We emit a MINIMAL policy: default deny, allow the login-challenge kind(s)
#    and the two NIP-46 methods provider_nip46 actually uses. Extend with
#    --allow-kinds / --allow-methods for other agents.
csv_to_json_array() {
  # Emit a JSON array. If the CSV is exactly "*" produce ["*"]. Otherwise
  # split on commas and quote each token. Numeric tokens (kinds) are emitted
  # unquoted so signet_parse_json_kind_array accepts them as integers.
  local csv="$1"
  local kind_array="${2:-0}"    # 1 = try to parse tokens as integers
  local out="["
  local first=1
  IFS=',' read -ra items <<<"$csv"
  for raw in "${items[@]}"; do
    tok="${raw//[[:space:]]/}"
    [[ -n "$tok" ]] || continue
    if [[ $first -eq 1 ]]; then first=0; else out+=","; fi
    if [[ "$tok" == "*" ]]; then
      out+='"*"'
    elif [[ "$kind_array" -eq 1 && "$tok" =~ ^-?[0-9]+$ ]]; then
      out+="$tok"
    else
      out+="\"$tok\""
    fi
  done
  out+="]"
  printf '%s' "$out"
}

kinds_json=$(csv_to_json_array "$allow_kinds" 1)
methods_json=$(csv_to_json_array "$allow_methods" 0)

policy_json=$(printf '{"default":"%s","allow_clients":["*"],"deny_clients":[],"allow_methods":%s,"deny_methods":[],"allow_kinds":%s,"deny_kinds":[]}' \
  "$default_decision" "$methods_json" "$kinds_json")

# 5. Export the signetctl-side environment (relays + bunker pubkey). The
#    provisioner nsec file path is already in the caller's env.
export SIGNET_RELAYS="$relays"
export SIGNET_BUNKER_PUBKEY="$bunker_pubkey"

printf 'grant-login-kind: signetctl=%s\n' "$signetctl_bin"
printf 'grant-login-kind: bunker_pubkey=%s\n' "$bunker_pubkey"
printf 'grant-login-kind: relays=%s\n' "$relays"
printf 'grant-login-kind: agent_id=%s\n' "$agent_id"
printf 'grant-login-kind: policy=%s\n' "$policy_json"

if [[ $dry_run -eq 1 ]]; then
  printf 'grant-login-kind: dry-run - would run: %s set-policy %s <policy>\n' "$signetctl_bin" "$agent_id"
  exit 0
fi

# 6. Fire the grant. signetctl gift-wraps this (kind 1059) to the bunker
#    over the configured relays and blocks on the ack (SIGNETCTL_TIMEOUT_SEC).
exec "$signetctl_bin" set-policy "$agent_id" "$policy_json"
