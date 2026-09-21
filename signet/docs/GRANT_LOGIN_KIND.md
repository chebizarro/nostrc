# Signet: grant login-kind for the nostr-homed NIP-46 provider

This walkthrough closes the C7 live-signer proof (`nostrc-ot2c.7`) once the
bunker operator opens a narrow ACL hole: signing the auth-challenge event
(kind `NH_AUTH_CHALLENGE_KIND` = **1**) for the agent bound to nostr-homed's
pre-paired client transport key. Until that grant lands, provider_nip46
completes connect + get_public_key against the live bunker but sign_event
returns `reason_code=policy.default_deny` (see `signet/src/policy_store.c`).

Beads: `nostrc-ot2c.7` (C7 — external pre-login provider full-OK proof),
`nostrc-7t61` (C7-operator ACL follow-up).

## What the grant does

`signetctl set-policy <agent_id> <policy-json>` sends a Cascadia ContextVM
JSON-RPC intent (`agent/set-policy`) gift-wrapped (NIP-59, kind 1059) to the
bunker over the configured relays. The daemon (`signetd`) parses the intent,
calls `signet_policy_store_set_identity_json()` for the target agent, and
persists the update to the file-backed policy store. The reply carries
`policy_set` (persisted) or `policy_set_not_persisted` (in-memory only —
disk write failed).

The policy JSON shape is documented in
`signet/include/signet/policy_store.h` (see
`signet_policy_store_set_identity_json`). The turnkey helper emits the
minimal grant our provider needs:

```json
{
  "default": "deny",
  "allow_clients": ["*"],
  "deny_clients":  [],
  "allow_methods": ["sign_event", "get_public_key"],
  "deny_methods":  [],
  "allow_kinds":   [1],
  "deny_kinds":    []
}
```

## The one operator secret

`signetctl` reads the provisioner nsec **only** from
`SIGNET_PROVISIONER_NSEC_FILE` (single-line `nsec1…` or 64-hex, file mode
0600). The legacy env-inline `SIGNET_PROVISIONER_NSEC` and any argv-inline
secret are rejected by `signetctl_resolve_provisioner_key()` on purpose.

Everything else this helper needs is public: the bunker pubkey and relay
list live in the interlocutor client config's `bunker_uri` (parsed at run
time by the helper — no secret material is copied out).

## Build signetctl

```sh
cmake -S <repo-root> -B /tmp/signet-grant-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSIGNET_ENABLE=ON \
  -DBUILD_APPS=OFF -DBUILD_TESTING=OFF \
  -DENABLE_NOSTR_HOMED=OFF -DBUILD_NOSTR_GTK=OFF \
  -DBUILD_LIBHANAMI=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF -DWITH_NIP77_NOSTRDB=OFF

cmake --build /tmp/signet-grant-build --target signetctl -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
```

The binary lands at `/tmp/signet-grant-build/signet/signetctl`, which is the
default the helper searches (override with `--signetctl <path>`).

## Discover the agent id

`signetctl list` publishes an `agent/list` intent and returns the daemon's
`{"count":N,"agents":[…]}` reply. Run it with the same env the grant will
use:

```sh
export SIGNET_PROVISIONER_NSEC_FILE=/secure/mount/signet-provisioner.nsec
export SIGNET_RELAYS="wss://bunker.sharegap.net,wss://relay.sharegap.net"
export SIGNET_BUNKER_PUBKEY=6c3e45a62048ab5886adc4b85216f7eddd034693bf550b8fcfed15f1f2b0618f
/tmp/signet-grant-build/signet/signetctl list
```

The right agent is the one whose custody pubkey equals the value the live
bunker returned to nostr-homed's `get_public_key` (surfaced by the live
test as `bunker user_pubkey=…`). Local operators with SQLCipher access can
alternatively use `signetctl list-agents` for `AGENT_ID → PUBKEY` mapping.

## Turnkey grant

`signet/tools/grant-login-kind.sh` wires the pieces together. It:

1. Locates `signetctl` (default `/tmp/signet-grant-build/signet/signetctl`,
   overridable via `--signetctl`).
2. Reads `bunker_uri` from `$HOME/.config/interlocutor/config.toml` (or
   `--interlocutor <path>`) and extracts the bunker pubkey + relay list
   (never the `secret=` connect token).
3. Verifies `SIGNET_PROVISIONER_NSEC_FILE` is set and readable.
4. Builds the minimal grant JSON shown above (extend with `--allow-kinds`,
   `--allow-methods`, `--default`).
5. Exports `SIGNET_RELAYS` + `SIGNET_BUNKER_PUBKEY` and execs
   `signetctl set-policy <agent> <policy>`.

Minimal invocation:

```sh
export SIGNET_PROVISIONER_NSEC_FILE=/secure/mount/signet-provisioner.nsec
signet/tools/grant-login-kind.sh --agent <agent_id>
```

Preview the exact JSON without contacting the bunker:

```sh
SIGNET_PROVISIONER_NSEC_FILE=/dev/null \
  signet/tools/grant-login-kind.sh --agent <agent_id> --dry-run
```

Wildcard variants:

```sh
# Full sign_event access (broad; use only for lab agents):
signet/tools/grant-login-kind.sh --agent <agent_id> --allow-kinds '*'

# Login kind + relay list events (kind 10002) as well:
signet/tools/grant-login-kind.sh --agent <agent_id> --allow-kinds '1,10002'
```

## Confirm the grant closed the proof

Re-run the live NIP-46 broker test from a build that includes
`test_broker_login_nip46_live` (see
`gnome/nostr-homed/tests/integration/test_broker_login_nip46_live.c`):

```sh
NH_NIP46_LIVE=1 <build>/gnome/nostr-homed/test_broker_login_nip46_live
```

Expected:

```
live matching-key -> result=ok proof=0
test_broker_login_nip46_live: OK
```

`result=ok` means the bunker signed the challenge event and the provider's
strict `nh_auth_challenge_verify()` (pubkey / id / signature) accepted it.
That is the full positive path for C7. If the run still emits
`invalid_proof` with `policy.default_deny`, the grant hit the wrong
agent — cross-check the agent id via `signetctl list` and the bunker's
`get_public_key` reply in the test log.

## Security notes

- The helper never touches the pre-paired client transport secret
  (`bunker_client_secret_key_file` in the interlocutor config) or the
  URI's `secret=` connect token; both stay client-side.
- `signetctl` gift-wraps the intent, so relay operators only see kind-1059
  events addressed to the bunker pubkey — the policy JSON is not
  observable on the wire.
- The default helper JSON keeps `default: "deny"` and grants only kind 1
  + two NIP-46 methods; nothing else about the agent's policy changes.
- Response events carry ack correlation (JSON-RPC `id`, bunker sender
  pubkey) checked by `signetctl_on_event`, so a stray reply from an
  unrelated command in the relay history cannot poison the exit code.
