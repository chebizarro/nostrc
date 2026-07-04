# Signet Integration Guide

## Agent Integration

Agents connect to Signet via the NIP-46 protocol over Nostr relays, or locally via D-Bus, NIP-5L Unix socket, or SSH agent protocol.

### NIP-46 (Relay-Based)

1. **Provisioning**: An authorized provisioner sends a signed management event (kind 28000)
   to Signet via a Nostr relay. Signet generates a keypair and returns a `nostrconnect://` URI.

2. **Agent Boot**: The agent receives the `nostrconnect://` URI as an environment variable.
   It uses NIP-46 to establish a session with Signet over the relay.

3. **Signing**: The agent sends `sign_event` requests via NIP-46. Signet evaluates per-agent
   policy, signs from the hot key cache, and returns the signed event.

4. **Revocation**: A provisioner sends a revoke event (kind 28010). Signet resolves the
   agent pubkey, adds it to the deny list, burns active leases, wipes the key from both
   cache and store, audits the action, and refuses all further requests for that agent.

### D-Bus (Local / LAN)

Agents on the same machine or LAN can use the `net.signet.Signer` D-Bus interface:

- **Unix socket** (system bus): Auth via `SO_PEERCRED` UID → agent_id mapping. Enable with `dbus_unix = true`.
- **TCP**: For LAN agents. Enable with `dbus_tcp = true` and `dbus_tcp_port = 47472`.

### NIP-5L (Unix Socket)

Line-delimited NIP-46 JSON framing over `/run/signet/nip5l.sock`. Auth via Nostr challenge-response (kind 28100). Supports systemd socket activation. Enable with `nip5l = true`.

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
| `connect`           | auth secret / session data          | Establish session; validate auth secret        |
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

All management traffic flows as NIP-44 v2 encrypted, signed Nostr events. Authorization requires the event's pubkey to be in the `provisioner_pubkeys` list. ACKs fail closed on encryption errors; Signet does not fall back to plaintext management ACKs.

| Kind  | Operation         | Description                              |
|-------|-------------------|------------------------------------------|
| 28000 | `provision_agent` | Generate keypair, return bunker:// URI   |
| 28010 | `revoke_agent`    | Resolve pubkey, deny, burn leases, wipe cache/store, audit |
| 28020 | `set_policy`      | Parse and apply policy JSON for agent    |
| 28030 | `get_status`      | Query daemon health                      |
| 28040 | `list_agents`     | Enumerate managed agents                 |
| 28050 | `rotate_key`      | Rotate agent keypair                     |
| 28090 | `ack`             | Response to any management command       |

## Bootstrap Flow

For automated fleet provisioning:

1. Fleet Commander provisions agent via management event (kind 28000)
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
- **Complete revocation**: `revoke_agent` denies the resolved pubkey, burns active leases, wipes cache and persistent key material, and audits the operation.
