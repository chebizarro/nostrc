# Signet Integration Guide

## Agent Integration

Agents connect to Signet via the NIP-46 protocol over Nostr relays, or locally via D-Bus, NIP-5L Unix socket, or SSH agent protocol.

### NIP-46 (Relay-Based)

1. **Provisioning**: An authorized provisioner sends a Cascadia ContextVM `25910`
   `agent/provision` intent to Signet via a Nostr relay, typically gift-wrapped as kind `1059`.
   Signet generates a keypair and returns a `nostrconnect://` URI.

2. **Agent Boot / Pairing**: The agent receives the `nostrconnect://`/`bunker://` URI as an
   environment variable. Its first NIP-46 `connect` presents the one-time `connect_secret`,
   which Signet consumes while durably recording the client pairing (one transaction).

3. **Signing**: The agent sends `sign_event` requests via NIP-46. Signet resolves the
   client to its agent via the persistent binding, evaluates per-agent policy, signs
   under the custody transaction, and returns the signed event. An identity
   fenced by `agent/writer-acquire` signs only for its assigned owner client;
   see [writer fence](docs/WRITER_EPOCH_CUTOVER.md).

4. **Restarts**: Bound clients reconnect autonomously — no fresh secret, no operator.
   Agent restarts, daemon restarts, or both: the persistent pairing survives (see
   "NIP-46 pairing model" below). A fresh secret (`agent/reissue-connect`) is only needed
   to pair a NEW client key (host rebuild, key compromise, revoked binding).

5. **Revocation**: A provisioner sends a `25910` `agent/revoke` intent. Signet resolves the
   agent pubkey, adds it to the deny list, burns active leases, revokes the agent's client
   bindings, wipes the key from both cache and store, audits the action, and refuses all
   further requests for that agent.

### D-Bus (Local / LAN)

Agents on the same machine or LAN can use the `net.signet.Signer` D-Bus interface:

- **Unix socket** (system bus): Auth via `SO_PEERCRED` UID → agent_id mapping. Enable with `dbus_unix = true`.
- **TCP**: For LAN agents. Enable with `dbus_tcp = true` and `dbus_tcp_port = 47472`.

### NIP-5L (Unix Socket)

Line-delimited NIP-46 JSON framing over `/run/signet/nip5l.sock`. Auth uses a signed ContextVM challenge event. Supports systemd socket activation. Enable with `nip5l = true`.

### SSH Agent

OpenSSH agent protocol on `/run/signet/ssh-agent.sock`. Auth via `SO_PEERCRED`. Enable with `ssh_agent = true`.

### Passkeys / WebAuthn (Programmatic API)

When `[passkeys] enabled = true`, Signet exposes a software FIDO2 authenticator via:

- D-Bus `net.signet.Passkeys`: `GetInfo()`, `MakeCredential(request_json)`, `GetAssertion(request_json)`, `ExportCredential(request_json)`, `ImportCredential(request_json)`
- NIP-46: `webauthn_get_info`, `webauthn_make_credential`, `webauthn_get_assertion`, `webauthn_export`, `webauthn_import`

`MakeCredential` request JSON fields: `rpId`, base64 `clientDataHash` (32 bytes), base64 `userHandle`, optional `userName`, `userDisplayName`, `discoverable`, `userVerification`, `pubKeyCredParams` (must include `-7`), and `excludeCredentials` (base64 IDs or objects with `id`).

`GetAssertion` request JSON fields: `rpId`, base64 `clientDataHash` (32 bytes), optional `userVerification`, and optional `allowCredentials` (base64 IDs or objects with `id`). Responses return base64 WebAuthn blobs (`authData`, `attestationObject` or `signature`) plus `credentialId` and `signCount: 0`.

`ExportCredential` / `webauthn_export` request JSON fields: base64 `credentialId`. Response returns a versioned base64 `container` (`format: "signet-passkey-export"`, `formatVersion: 1`) carrying credential metadata and the fleet-PSK-wrapped payload.

`ImportCredential` / `webauthn_import` request JSON fields: base64 `container`. Import validates the container decrypts under this instance's `[passkeys] sync_key`; containers from another fleet PSK are rejected.

Headless user-present (`UP`) is asserted after Signet policy approval. `userVerification: "required"` is denied unless `[passkeys] allow_headless_uv = true`.

#### Phase 3 interop/conformance harness

The passkey interop gate is wired behind `SIGNET_ENABLE_PASSKEYS` / `signet_enable_passkeys` as `fido_interop_external`. It drives a register→authenticate ceremony through the same `SignetFidoService` JSON entrypoints called by D-Bus `net.signet.Passkeys` and NIP-46 `webauthn_*`, writes WebAuthn artifacts, and reuses `tests/phase0/verify_external.py` with python-fido2 to independently CBOR-decode the attestation object and verify the ES256 assertion.

```bash
cmake -S . -B build -DSIGNET_ENABLE_PASSKEYS=ON
cmake --build build --target test_fido_interop_json
ctest --test-dir build/signet/tests -R fido_interop_external --output-on-failure
```

A live `signetd` + private D-Bus session is not required for this harness; it intentionally tests the service+transport JSON boundary (`signet_fido_*_json`) used by both transports while avoiding daemon identity, UID→agent mapping, and fleet policy provisioning in unit CI.

#### Manual live-daemon D-Bus interop harness

`signet/tests/interop/run_live_dbus_interop.sh` closes the transport last mile manually: it starts a private D-Bus session bus, runs a real passkey-enabled `signetd` against throwaway stores, calls `net.signet.Passkeys.GetInfo`, `MakeCredential`, `GetAssertion`, `ExportCredential`, and `ImportCredential` with `gdbus`, then verifies the live daemon's attestation object and imported-key ES256 assertion using `signet/tests/phase0/verify_external.py` with python-fido2 2.2.1.

It is registered as disabled/manual CTest `live_dbus_interop_manual` and is not part of the default suite:

```bash
cmake -S . -B build -DSIGNET_ENABLE_PASSKEYS=ON
cmake --build build --target signetd
bash signet/tests/interop/run_live_dbus_interop.sh
```

The harness builds/runs a separate `build-signet-testhooks/` daemon with `SIGNET_ENABLE_TEST_HOOKS=ON`; the normal `build/` production-style daemon keeps those hooks compiled out.

Prerequisites: `dbus-daemon` and `gdbus` (on macOS, usually from Homebrew `dbus`/GLib), `python3`, and CMake. The harness uses explicitly named `SIGNET_DBUS_TEST_*` environment switches to map the current UID to a disposable test agent and grant passkey capabilities only for the spawned test-hooks daemon process.

#### Phase 4 Linux virtual CTAP-HID validation

`[passkeys] virtual_ctap = true` creates a Linux-only virtual FIDO2 HID authenticator through `/dev/uhid`. It is disabled by default and is compiled as a clean no-op/stub on non-Linux hosts. The CTAP-HID layer advertises `rk=true`, `up=true`, `uv=false`, and `clientPin=false`; PIN/UV commands and `uv=true` requests return CTAP errors rather than fabricated verification.

Prerequisites on a Linux host:

```bash
cmake -S . -B build -DSIGNET_ENABLE_PASSKEYS=ON
cmake --build build --target signetd test_fido_ctaphid
ctest --test-dir build/signet/tests -R fido_ctaphid --output-on-failure
sudo modprobe uhid
# grant the signetd user access to /dev/uhid, or run the daemon with a narrowly-scoped test privilege
```

Configure passkeys and the virtual device:

```ini
[passkeys]
enabled = true
virtual_ctap = true
backend = software-openssl
aaguid = 80c64041-9927-4901-957f-e0032db96bee
attestation = none
allow_headless_uv = false
sync_key_file = /run/secrets/signet-passkey-sync-key
```

Run `signetd` with normal `SIGNET_DB_KEY`, `SIGNET_BUNKER_NSEC_FILE`, and relay/policy configuration. The virtual CTAP adapter currently uses the daemon `[nostr] identity` value as the `SignetFidoService` `agent_id` for credentials created through the HID path, so provision policy/capability state for that identity before testing.

Functional smoke with libfido2 tools:

```bash
fido2-token -L
# Expect a "Signet virtual FIDO2 authenticator" HID device.

# Exercise register/authenticate with libfido2 tools or a small libfido2 program:
# 1. call authenticatorGetInfo and confirm FIDO_2_0, ES256, rk/up true, uv/clientPin false
# 2. makeCredential with ES256, uv=false, and an rp.id such as example.com
# 3. getAssertion for the returned credential id and the same rp.id
# 4. verify authData flags: UP, BE, BS set; UV clear; signCount == 0
# 5. verify the ES256 signature over authData || clientDataHash using the returned COSE public key
```

Browsers and `fido2-token` must be tested on Linux because macOS does not provide `/dev/uhid`; a macOS build can only prove the stubbed module compiles/links and the frame parser unit test passes.

## NIP-46 Methods

| Method              | Params                              | Description                                    |
|---------------------|-------------------------------------|------------------------------------------------|
| `connect`           | `[remote_signer_pubkey, connect_secret?]` | Pair (one-time secret, consumed atomically with the durable client binding) or reconnect (bound clients need no secret; the client's own stale secret is also accepted) |
| `get_public_key`    | `[]`                                | Return agent's public key (hex)                |
| `sign_event`        | `[event_json]`                      | Policy check then sign under custody           |
| `nip04_encrypt`     | `[peer_pubkey_hex, plaintext]`      | NIP-04 encrypt plaintext for a peer            |
| `nip04_decrypt`     | `[peer_pubkey_hex, ciphertext]`     | NIP-04 decrypt ciphertext from a peer          |
| `nip44_encrypt`     | `[peer_pubkey_hex, plaintext]`      | NIP-44 v2 encrypt plaintext for a peer         |
| `nip44_decrypt`     | `[peer_pubkey_hex, ciphertext]`     | NIP-44 v2 decrypt ciphertext from a peer       |
| `nip44_encrypt_b64` | `[peer_pubkey_hex, base64(plaintext)]` | NIP-44 v2 encrypt of raw (non-UTF-8) bytes  |
| `nip44_decrypt_b64` | `[peer_pubkey_hex, ciphertext]`     | NIP-44 v2 decrypt returning base64(plaintext)  |
| `get_relays`        | `[]`                                | Return relay map JSON                          |
| `webauthn_get_info` | `[]`                                | Return Signet authenticator info JSON          |
| `webauthn_make_credential` | request JSON                 | Create a passkey credential JSON response |
| `webauthn_get_assertion` | request JSON                     | Sign an assertion JSON response            |
| `webauthn_export`   | request JSON                        | Export a credential portability container      |
| `webauthn_import`   | request JSON                        | Import a credential portability container      |
| `ping`              | `[]`                                | Keepalive                                      |

## Management Protocol

All management traffic uses Cascadia ContextVM kind `25910` signed Nostr intents. Relay transport should gift-wrap the intent with NIP-59/NIP-17 kind `1059`; authorization requires the inner sender pubkey (the seal author) to be in the `provisioner_pubkeys` list, with two exceptions: `agent/reissue-connect` also accepts the target agent itself, and `credential/inspect`, `credential/list` and `credential/deliver` also accept a requester holding the owner's exact delivery grant (see [Credential methods](#credential-methods)). Responses are correlated, gift-wrapped ContextVM results. Each management **event id** executes at most once: relay redelivery, republishing of the same serialized event, and history replay are silently dropped instead of re-running non-idempotent commands such as `agent/rotate-key` or `agent/reissue-connect`. The request's own `created_at` — the rumor's for a gift wrap (NIP-59 randomizes only the seal and wrap timestamps; the seal signature authenticates the rumor), the event's for a plain 25910 — must be within `[replay] skew_seconds` (default 120, never 0 for management) of Signet's clock, and management ids are kept twice that long (`[replay] ttl_seconds` applies to NIP-46 only), so a captured request can never run again. Provisioners, self-service agents and delivery-grant requesters each have their own replay cache (16384 live ids), so one class cannot fill another's; a non-provisioner sender may hold 512 live ids (more than its rate limit admits in that time), provisioners are not limited per sender, and a full sender or cache is refused rather than evicting anyone's ids. Many colluding authorized senders of one class can still fill that class's cache and have its requests refused (`replay_quota_exceeded`) until their ids expire; they cannot affect the other classes or replay anything. Note this keys on the delivered Nostr event id — a client retry that re-signs the same request produces a new event id and executes again; recover from a lost reply by sending a new intent. Retired custom management event kinds are not subscribed, parsed, or emitted.

| Kind  | Method | Description |
|-------|--------|-------------|
| 25910 | `agent/provision` | Generate keypair, return bunker:// URI |
| 25910 | `agent/revoke` | Resolve pubkey, deny, burn leases, wipe cache/store, audit |
| 25910 | `agent/set-policy` | Parse and apply policy JSON for agent |
| 25910 | `agent/get-status` | Query daemon health |
| 25910 | `agent/list` | Enumerate managed agents |
| 25910 | `agent/rotate-key` | Rotate agent keypair |
| 25910 | `agent/adopt-existing` | Register an externally supplied (BYO) keypair as an agent |
| 25910 | `agent/reissue-connect` | Mint a fresh one-time `connect_secret` for an existing agent |
| 25910 | `agent/list-clients` | List an agent's persistent NIP-46 client bindings |
| 25910 | `agent/revoke-client` | Soft-revoke a persistent NIP-46 client binding (forces re-pairing) |
| 25910 | `agent/writer-acquire` | Assign an identity's single-writer NIP-46 client |
| 25910 | `agent/writer-revoke` | Clear an identity's single-writer assignment |
| 25910 | `config/grant-provisioner` | Persist and activate a provisioner pubkey |
| 25910 | `config/revoke-provisioner` | Persist and activate a provisioner revocation |
| 25910 | `credential/create` | Store a new credential for an agent |
| 25910 | `credential/import` | Store an existing credential for an agent (provenance `imported`) |
| 25910 | `credential/list` | Payload-free metadata of an agent's credentials (provisioner, or delivery grant) |
| 25910 | `credential/inspect` | Payload-free metadata of one credential (provisioner, or delivery grant) |
| 25910 | `credential/deliver` | Release a credential payload under a one-use lease (provisioner or delivery grant; always needs the grant) |
| 25910 | `credential/rotate` | Replace the payload; the prior version is archived |
| 25910 | `credential/revoke` | Revoke a credential (no further delivery) |
| 25910 | `credential/delete` | Delete a revoked credential and its history |

### Requests, replies and refusals

A request is a JSON-RPC 2.0 object, `{"jsonrpc":"2.0","id":<string|integer>,"method":"<method>","params":{...}}`,
carried as the content of a kind-14 rumor sealed (kind 13) by the sender and
gift-wrapped (kind 1059) to Signet's bunker pubkey. Every reply is gift-wrapped
back the same way and echoes the request's `id` verbatim (a string of at most
256 bytes, or an integer; any other id is answered with `id: null`):

- success: `{"jsonrpc":"2.0","result":{...},"id":<id>}`
- failure: `{"jsonrpc":"2.0","error":{"code":<int>,"message":"<text>","data":"<reason>"},"id":<id>}`

`error.data` is a stable reason code; `error.message` is human text and never
carries secret material. Codes `-32002` (`data: "not_provisioner"`: the method
is provisioner-only and the sender is not a provisioner) and `-32601`
(`data: "unknown_method"`) are sent before the request is read. Every other
failure is `-32000` with the method's reason code in `data`; a malformed
request is `parse_error`. A refusal tells the sender nothing beyond the
refusal itself. Refusals that any method can get:

| `error.data` | Meaning |
|--------------|---------|
| `request_time_skew` | the request's `created_at` is outside Signet's clock window (send it again with a correct clock) |
| `replay_quota_exceeded` | this sender, or its class's replay cache, holds too many requests inside the replay window |
| `rate_limited` | a non-provisioner sender is over its rate (60 requests burst, refilled at 60 per minute). Checked before anything else, including the provisioner-only refusal; further requests are dropped without a reply, and the sender is told at most once a minute. Not audited |

Refusals of non-provisioner senders are audited within a global budget (100,
then one per second); refusals over it are counted, and the next audited
refusal is preceded by one `mgmt_refusals_suppressed` entry carrying the
count, so no number of keys can turn refusals into unbounded audit rows.
| `replay_invalid` | the event has no id |

NIP-59 randomizes each gift wrap's `created_at` up to **48 hours** into the
past. A client subscribing for replies (`kinds:[1059]`, `#p:[<client pubkey>]`)
must therefore use a `since` at least 48 hours back (or none) and match replies
by `id`, not by arrival time.

### Credential methods

Credentials are generic secrets owned by an agent (`agent_id`). Their id is
`cred_` plus 64 hex characters, derived from owner, type and label. Payloads
travel only inside the encrypted request or reply, base64-encoded
(`payload_b64`, at most 1 MiB decoded), and never reach logs or audit records.

**Metadata** (the result of create, import, inspect, rotate, revoke, and each
entry of list) is payload-free:

```json
{"credential_id":"cred_…","agent_id":"vault","secret_type":"credential",
 "label":"db/primary","policy_id":null,"provenance":"imported",
 "created_by":"<provisioner pubkey>","created_at":1760000000,
 "rotated_at":null,"expires_at":null,"revoked_at":null,
 "version":1,"active_version":1,"status":"active"}
```

`status` is `active`, `expired` or `revoked`. `version` starts at 1 and each
rotation increments it by one; versions are never reused.

| Method | Params | Result | Failure codes (`error.data`) |
|--------|--------|--------|------------------------------|
| `credential/create`, `credential/import` | `agent_id`, `secret_type` (`api_token`\|`credential`), `label`, `payload_b64`; optional `policy_id`, `expires_at` | metadata | `credential_exists` (never overwritten), `agent_not_found`, `invalid_credential`, `no_store` |
| `credential/list` | `agent_id` (required for a delivery-grant requester; a provisioner may omit it to list every owner) | `{"count":n,"credentials":[metadata…]}` | `no_capability`, `deny_listed`, `credential_store_failed`, `no_store` |
| `credential/inspect` | `credential_id` | metadata | `credential_not_found` (provisioner), `no_capability`, `deny_listed`, `credential_store_failed`, `no_store` |
| `credential/deliver` | `agent_id` (the owner), `credential_id`; optional `expected_version` (positive integer; rejected on every other method) | `{"credential_id","agent_id","version","payload_b64","lease_id","lease_expires_at"}` | `no_capability`, `deny_listed`, `not_owner` and `not_found` (provisioner only; a delivery-grant requester gets `no_capability`), `revoked`, `expired`, `version_mismatch`, `type_denied`, `rate_limited`, `credential_access_unavailable`, `internal_error` |
| `credential/rotate` | `credential_id`, `payload_b64`; optional `expires_at` (required to revive an expired credential) | metadata | `credential_not_found`, `credential_revoked`, `credential_expired`, `invalid_credential`, `credential_store_failed` |
| `credential/revoke` | `credential_id` | metadata | `credential_not_found`, `credential_store_failed` |
| `credential/delete` | `credential_id` | `{"credential_id","agent_id","deleted":true}` | `credential_not_found`, `credential_not_revoked`, `credential_store_failed` |

**Delivery.** `credential/deliver` goes through Signet's single credential
access path, in order: deny list (owner or requester deny-listed →
`deny_listed`), the exact delivery grant (`no_capability`), rate limit,
ownership and status on metadata before any decryption (`not_owner` before
`revoked` before `expired` before `version_mismatch`), type rules, then a
60-second lease that is minted and burned before the payload is decrypted. The
result's `version` is the version of the released payload, read in the same
statement as the payload. With `expected_version`, a different active version
fails with `version_mismatch` and that version's payload is never released.
The check runs on metadata before the lease, so normally no lease is minted;
if a rotation lands between that check and the decryption, the already-burned
lease is recorded in the audit and the payload is wiped unreleased. A client
that pins a version therefore resolves in one round trip:

```json
{"jsonrpc":"2.0","id":"7f3a…","method":"credential/deliver",
 "params":{"agent_id":"vault","credential_id":"cred_…","expected_version":2}}
```

Every delivery outcome — allow, deny or error — appends a redacted entry to the
hash-chained audit log, naming the requester and, for the management entry, the
authorization path (`"auth":"provisioner"` or `"auth":"delivery_grant"`) and
the real reason (also when the requester was only told `no_capability`). A
refused delivery-grant request records the targeted credential id and owner.

**The delivery grant.** Delivery is released only to a requester the owner's
policy names exactly:

```json
{"default":"deny","allow_clients":["<requester pubkey hex>"],"allow_methods":["credential.deliver"]}
```

set with `agent/set-policy` (or `signetctl set-policy <owner> '<json>'`).
The policy must be exactly this (an optional `ttl_seconds` aside): wildcards,
default-allow, more clients, mixing `credential.deliver` with NIP-46 methods,
any `allow_kinds`/`deny_kinds`, and any `deny_clients`/`deny_methods` entry
all void it, and the grant gives no NIP-46 signing authority. `set-policy`
rejects a client or method entry it cannot read (a client must be `*`, an npub
or 64-hex); a policy file carrying one still loads, but never as a grant. (In
ordinary NIP-46 evaluation an unreadable entry is kept as a placeholder that
never matches, so its list still counts: an `allow_clients` list of nothing
but unreadable entries allows no client.) A
provisioner needs the grant too: provisioner status authorizes the request,
not the release.

**Requesters without provisioner rights.** A requester that is not a
provisioner may call exactly three methods, and only within its grants:

- `credential/deliver` for an owner whose policy grants it;
- `credential/inspect` for a credential whose owner grants it;
- `credential/list` with the `agent_id` of an owner that grants it.

Anything else — another owner's credential, a missing credential, an unscoped
list, every other management method — is refused. A credential the requester
is not granted and a credential that does not exist get byte-identical
`no_capability` refusals — for inspect, and for deliver even under a grant the
requester does hold (another owner's credential) — so the refusal reveals
nothing about which credentials exist. Such a requester is rate-limited per
sender before its inner request is decrypted or anything is read (the gift
wrap is opened first, to authenticate it). Its key carries no management
authority: it
cannot create, rotate, revoke or delete credentials, change policies, or grant
itself anything. Keep it separate from provisioner keys.

### `agent/adopt-existing` (BYO-key)

Binds an `agent_id` to an already-canonical identity instead of minting a fresh
keypair. Same authorization as other management ops (inner sender must be a
configured provisioner). The secret arrives only inside the encrypted
ContextVM/gift-wrap management path; it is never echoed in results, logs, or
audit detail, and secret buffers are zeroized after use.

**Params:**

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `agent_id` | string | yes | Identifier to bind |
| `agent_nsec` | string | yes | `nsec1...` bech32 **or** 64-char hex secret |
| `expected_pubkey` | 64-hex | no | If present, derived pubkey must match exactly |
| `connect_secret` | string | no | Fixed bunker connect secret (else random) |
| `deliver` | bool | no | Gift-wrap the bunker URI to `bootstrap_pubkey` |
| `bootstrap_pubkey` | 64-hex | no | Recipient for delivery |
| `delivery_ttl` | int | no | Delivery expiry seconds (capped at 900) |

**Result:** `{ "agent_id", "pubkey", "adopted": true, "bunker_uri" }`.

**Semantics:** decode `agent_nsec` → derive pubkey → (optional) require
`expected_pubkey` match → fail if `agent_id` exists → fail if the pubkey is
already bound to another agent → fail if the pubkey is deny-listed → store the
secret in the same encrypted key store as provisioned agents (provenance
`adopted`) → return the normal `bunker_uri`. An adopted agent is
indistinguishable from a provisioned one at runtime except that its provenance
is `adopted`.

**Failure codes:** `invalid_secret`, `pubkey_mismatch`, `agent_exists`,
`pubkey_exists`, `deny_listed`, `adopt_failed`.

**Audit:** op `adopt_existing`, fields `agent_id`, `pubkey`, `status`,
`provisioner_pubkey` — never secret material.

**CLI:**

```
signetctl adopt-existing <agent_id> --sec <nsec-or-hex> \
    [--expected-pubkey <hex>] [--deliver <bootstrap_pubkey>] [--ttl <sec>]
```

The CLI prints only `agent_id`, `pubkey`, and `bunker_uri`.

### `agent/reissue-connect` (forced re-pairing)

Mints and persists a fresh one-time `connect_secret` for an **existing** agent,
replacing whatever secret was there before (consumed or not). Routine restarts
do NOT need this — bound clients reconnect via their persistent pairing (see
"NIP-46 pairing model" below). Reissue is the **forced re-pairing** tool: a
new or rebuilt host, a lost or compromised client key, or a binding revoked
via `agent/revoke-client`. The `connect_secret` stays single-use — the next
NIP-46 `connect` consumes the fresh one atomically with the new pairing.

Authorization accepts **two** sender identities (unlike every other management
op, which is provisioner-only):

- a configured provisioner (fleet control — can reissue for any agent), or
- **the target agent itself**: the signed sender pubkey (gift-wrap seal
  author) equals the agent's identity pubkey. Self-service lets a restarted
  headless agent recover its own connect path using its own key — it grants no
  power over other agents or other methods. An unknown `agent_id` and a
  sender/agent mismatch both return `unauthorized`, so agent existence cannot
  be probed. The audit event records the path (`ok_provisioner` / `ok_self`).

**Applicability of the self path:** signing the request requires possession of
the agent's identity private key, so self-service applies to **adopted
(BYO-key) agents** whose host retains its own nsec. Agents **provisioned
inside Signet** never receive their identity key — they only hold a NIP-46
client keypair, which has no verified binding to the identity outside an
established session and is therefore deliberately NOT accepted as an auth
basis — so they must recover via a provisioner (or the bootstrap-token flow).
This is intentional: "self" is proven by an unforgeable signature under the
identity key, never by a claimed `client_pubkey` or request params.

The fresh secret is returned only in the NIP-44-encrypted ContextVM reply to
the requesting sender.

**Params:**

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `agent_id` | string | yes | Existing agent to reissue for |
| `client_pubkey` | 64-hex | no | Accepted for audit/context only — never an auth basis; binding to a new client key happens at the next `connect` |
| `reason` | string | no | Free-form context, e.g. `restart_recovery` (accepted; not enforced) |

**Result:**

```json
{
  "agent_id": "stew",
  "bunker_pubkey": "<signet bunker pubkey>",
  "user_pubkey": "<agent identity pubkey>",
  "connect_secret": "<fresh one-time secret>",
  "bunker_uri": "bunker://<bunker_pubkey>?relay=<...>&secret=<fresh secret>",
  "issued_at": 1752380000
}
```

**Semantics:** agent must exist → deny-list gate (below) → generate a fresh random 32-byte hex secret →
replace the agent's `connect_secret` in a single UPDATE (old or already-consumed
secrets become/stay invalid) → return the fresh secret and bunker URI in the
encrypted reply only. The agent's keypair is untouched (unlike
`agent/rotate-key`).

**Suspension/revocation:** a deny-listed agent cannot obtain a fresh secret on
EITHER auth path — a suspended agent cannot self-service around its
suspension, and a provisioner must lift the deny entry before reissuing
(`deny_listed`, mirroring the adopt-existing gate; keyed by pubkey). A fully
revoked agent's key is wiped, so self-service fails `unauthorized` and
provisioner reissue fails `not_found`.

**Failure codes:** `unauthorized`, `deny_listed`, `not_found`, `reissue_failed`.

**Audit:** op `reissue_connect`, fields `agent_id`, `status` — never secret
material.

**CLI:**

```
signetctl reissue-connect <agent_id> [--out <path>] [--show-secret]
```

The CLI prints `agent_id`, `user_pubkey`, and `bunker_pubkey`. The fresh secret
is written atomically (0600) to `--out <path>` for consumption by e.g. an
OpenClaw `nip46ConnectSecret` file backend, or printed only with
`--show-secret`.

### NIP-46 pairing model (persistent client bindings)

The one-time `connect_secret` is a **pairing bootstrap**, not a session
credential. On the first successful `connect`, Signet consumes the secret and
durably records the binding `client_pubkey → agent_id` (table
`agent_clients`). From then on:

- **Pairing is atomic.** The secret consumption and the durable binding
  commit in one store transaction — a crash or write failure can never burn
  the one-time credential without recording the pairing (or vice versa).
- **Reconnects need no secret.** A `connect` from a bound client succeeds
  with no secret, or with the client's OWN stale/consumed pairing secret
  (what a restarted client typically re-sends — verified against a SHA-256
  recorded at pairing). An arbitrary wrong secret is rejected with
  `auth_failed` so misconfiguration and probing surface. Requests
  (`sign_event`, …) from a bound client resolve directly — no `connect`
  required at all. The binding survives agent AND daemon restarts, so
  headless agents recover autonomously.
- **Authenticity:** every request is NIP-44-encrypted under the bound client
  key, so a reconnect proves possession of the key that was authorized during
  the secret-verified pairing. No new trust is granted.
- **Identity pinning:** bindings record the agent identity pubkey at pairing
  time and resolve only while it matches the agent's CURRENT identity —
  `agent/rotate-key`, revocation + reprovisioning, or re-adoption under a new
  key all invalidate old bindings (no resurrection of prior authority).
  Custody checks the original binding again inside the key-store lock. For
  persistent keys, sign/encrypt/decrypt also hold a SQLite writer transaction;
  get_public_key and WebAuthn hold the SQLite connection mutex instead. A
  revoke/re-adopt between request lookup and execution cannot use the
  replacement identity.
- **Spent-secret uniqueness:** a new pending secret may not match another
  pending secret or any spent pairing-secret hash. Spent hashes remain as
  tombstones after revocation or re-pairing; old bunker URIs cannot become
  fresh pairing credentials for a replacement agent. Tombstones do not expire;
  indexed hash lookups keep issuance cost independent of their count.
- **Suspension precedence:** the daemon's live deny list is checked on every
  path (pairing, reconnect, per-request) before policy evaluation — a
  suspended agent's bound clients are refused immediately.
- **Revocation:** `agent/revoke-client` soft-revokes one binding
  (`revoked_at`); the persistent table is authoritative per-request, so
  revocation takes effect immediately. Full `agent/revoke` revokes all of the
  agent's bindings. A revoked client must re-pair with a fresh one-time secret
  (`agent/reissue-connect`), which re-binds and clears the revocation.
- **Inspection:** `agent/list-clients` returns all bindings (active and
  revoked) with `bound_at` / `last_used` timestamps.
- Policy evaluation and deny-list precedence are unchanged and apply on top
  of binding resolution.

CLI: `signetctl list-clients <agent_id>`, `signetctl revoke-client <client_pubkey>`.

#### Cache-only mode (no persistent store)

Without `SIGNET_DB_KEY` (or with an empty `[store] db_path`) the key store
runs cache-only: agents, their pending connect secrets and client bindings
live in the `signetd` process and nothing is written to disk. NIP-46 pairing
follows the same rules as above for the life of that process:

- `agent/provision` and `agent/adopt-existing` keep only the SHA-256 of the
  agent's one-time `connect_secret` in memory; the first `connect` that
  presents it consumes it and binds the client key in one critical section.
- A bound client reconnects with no secret or its own spent secret; another
  client's secret, an arbitrary wrong secret and unknown clients get
  `auth_failed`, and requests from unbound clients get `not_connected`.
- Bindings are pinned to the exact in-memory identity they were made
  against: `agent/revoke`, `agent/rotate-key`, a key restore, or revoking
  and re-adopting the same `agent_id` (even with the same key) all
  invalidate them and drop them at once.
- Spent secret hashes remain in memory until process exit even when a binding
  is dropped, preventing a later adopt/reissue from reusing an old URI.
- `agent/rotate-key` leaves no pending secret, as in persistent mode;
  `agent/reissue-connect` (provisioner or self-service) mints a fresh one,
  returned in a bunker URI, and the earlier pending secret stops working. A
  restore keeps a still-pending secret.
- **Nothing survives a restart.** A restarted cache-only signer holds no
  agents, secrets or bindings; agents must be provisioned or adopted again
  and every client must pair with the new secret. A reconnect without one
  fails `auth_failed` with a message naming the cache-only signer.
  `agent/list-clients`, `agent/revoke-client`, the deny list and writer
  fences need the persistent store and are unavailable.

Cache-only mode is for tests and development; deploy with the persistent
store.

## Bootstrap Flow

For automated fleet provisioning:

1. Fleet Commander provisions agent via a `25910` `agent/provision` management intent
2. Fleet Commander sends bootstrap token to agent via NIP-17 gift-wrapped DM
3. Agent decrypts token, calls `POST /bootstrap` to verify
4. Signet atomically consumes the single-use bootstrap token; replay attempts return 403
5. Signet returns a `nostrconnect://` handoff URI
6. Agent calls `GET /challenge` and `POST /auth` to establish a session
7. Agent connects via NIP-46 using the handoff URI

## Health Endpoint

```bash
curl http://localhost:8080/health
```

Returns 200 if healthy (SQLCipher open, relays connected), 503 otherwise.

## Security Guarantees

- **Fail-closed crypto paths**: management ACK encryption failures return errors instead of plaintext; relay publish with zero connected relays reports failure.
- **Per-agent credential isolation**: credential payloads use per-agent envelope keys derived via BLAKE2b from the store DEK and agent pubkey; D-Bus `GetToken` only returns credentials owned by the requesting agent.
- **Transactional rotation**: credential rotation archives the old secret and writes the new one in a single SQLite transaction.
- **SQLCipher startup verification**: Signet verifies SQLCipher with `PRAGMA cipher_version` plus a keyed read at startup and refuses to start when `SIGNET_REQUIRE_ENCRYPTED_DB=true` and encryption is unavailable.
- **Complete revocation**: `revoke_agent` denies the resolved pubkey, burns active leases, revokes the agent's persistent client bindings, wipes cache and persistent key material, and audits the operation.
- **Atomic pairing**: NIP-46 `connect` consumes the one-time secret and records the durable client binding in one SQLite transaction — the credential cannot be burned without the pairing (or vice versa).
- **Identity-pinned bindings**: client bindings resolve only while the agent's current identity pubkey matches the one pinned at pairing time — rotation or reprovisioning cannot resurrect prior authority.
- **Management replay protection**: each delivered management event id executes at most once: the request's authenticated `created_at` must be within the clock window, ids outlive the window, each sender class has its own cache, and non-provisioner ids are charged to their sender's quota with nothing unexpired ever evicted, so no sender can flood another's ids out of a cache.
- **Bounded refusals**: non-provisioner senders are rate-limited before any refusal or audit, and their refusals are audited within a global budget (aggregated beyond it).
- **Least-privilege credential delivery**: a delivery requester needs no provisioner rights; the owner's exact `credential.deliver` grant authorizes it to inspect, list and receive that owner's credentials and nothing else, and deny entries override the grant.
- **DB-level identity uniqueness**: a partial unique index on `agents.pubkey` (with upsert semantics that error instead of destructively replacing) prevents one custody key from ever being bound to two agents.
