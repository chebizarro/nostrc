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
   client to its agent via the persistent binding, evaluates per-agent policy, signs from
   the hot key cache, and returns the signed event.

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

Run `signetd` with normal `SIGNET_DB_KEY`, `SIGNET_BUNKER_NSEC`, and relay/policy configuration. The virtual CTAP adapter currently uses the daemon `[nostr] identity` value as the `SignetFidoService` `agent_id` for credentials created through the HID path, so provision policy/capability state for that identity before testing.

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
| `sign_event`        | `[event_json]`                      | Policy check then sign from hot cache          |
| `nip04_encrypt`     | `[peer_pubkey_hex, plaintext]`      | NIP-04 encrypt plaintext for a peer            |
| `nip04_decrypt`     | `[peer_pubkey_hex, ciphertext]`     | NIP-04 decrypt ciphertext from a peer          |
| `nip44_encrypt`     | `[peer_pubkey_hex, plaintext]`      | NIP-44 v2 encrypt plaintext for a peer         |
| `nip44_decrypt`     | `[peer_pubkey_hex, ciphertext]`     | NIP-44 v2 decrypt ciphertext from a peer       |
| `get_relays`        | `[]`                                | Return relay map JSON                          |
| `webauthn_get_info` | `[]`                                | Return Signet authenticator info JSON          |
| `webauthn_make_credential` | request JSON                 | Create a passkey credential JSON response |
| `webauthn_get_assertion` | request JSON                     | Sign an assertion JSON response            |
| `webauthn_export`   | request JSON                        | Export a credential portability container      |
| `webauthn_import`   | request JSON                        | Import a credential portability container      |
| `ping`              | `[]`                                | Keepalive                                      |

## Management Protocol

All management traffic uses Cascadia ContextVM kind `25910` signed Nostr intents. Relay transport should gift-wrap the intent with NIP-59/NIP-17 kind `1059`; authorization requires the inner sender pubkey to be in the `provisioner_pubkeys` list (sole exception: `agent/reissue-connect` also accepts the target agent itself — see below). Responses are correlated, gift-wrapped ContextVM results. Each management **event id** executes at most once per replay-cache TTL: relay redelivery, republishing of the same serialized event, and history replay are silently dropped instead of re-running non-idempotent commands such as `agent/rotate-key` or `agent/reissue-connect`. Note this keys on the delivered Nostr event id — a client retry that re-signs the same request produces a new event id and executes again; recover from a lost reply by sending a new intent. Retired custom management event kinds are not subscribed, parsed, or emitted.

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
- **Management replay protection**: each delivered management event id executes at most once per replay-cache TTL, with self-service reissue events isolated in their own replay domain so they cannot evict provisioner entries.
- **DB-level identity uniqueness**: a partial unique index on `agents.pubkey` (with upsert semantics that error instead of destructively replacing) prevents one custody key from ever being bound to two agents.
