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

4. **Revocation**: A provisioner sends a revoke event (kind 28010). Signet zeros the cache
   entry, adds the pubkey to the deny list, and refuses all further requests for that agent.

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

A live `signetd` + private D-Bus session is not required for this harness; it intentionally tests the service+transport JSON boundary (`signet_fido_*_json`) used by both transports while avoiding daemon identity, UID→agent mapping, and fleet policy provisioning in unit CI. Full daemon smoke remains a separate build/integration concern.

## NIP-46 Methods

| Method              | Description                                    |
|---------------------|------------------------------------------------|
| `connect`           | Establish session; validate auth secret        |
| `get_public_key`    | Return agent's public key (hex)                |
| `sign_event`        | Policy check then sign from hot cache          |
| `nip04_encrypt`     | NIP-04 encrypt plaintext for a peer            |
| `nip04_decrypt`     | NIP-04 decrypt ciphertext from a peer          |
| `webauthn_get_info` | Return Signet authenticator info JSON          |
| `webauthn_make_credential` | Create a passkey credential JSON response |
| `webauthn_get_assertion` | Sign an assertion JSON response            |
| `webauthn_export`   | Export a credential portability container      |
| `webauthn_import`   | Import a credential portability container      |
| `ping`              | Keepalive                                      |

## Management Protocol

All management traffic flows as NIP-44 v2 encrypted, signed Nostr events. Authorization requires the event's pubkey to be in the `provisioner_pubkeys` list.

| Kind  | Operation         | Description                              |
|-------|-------------------|------------------------------------------|
| 28000 | `provision_agent` | Generate keypair, return bunker:// URI   |
| 28010 | `revoke_agent`    | Wipe key, add to deny list               |
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
4. Signet returns a `nostrconnect://` handoff URI
5. Agent calls `GET /challenge` and `POST /auth` to establish a session
6. Agent connects via NIP-46 using the handoff URI

## Health Endpoint

```bash
curl http://localhost:8080/health
```

Returns 200 if healthy (SQLCipher open, relays connected), 503 otherwise.
