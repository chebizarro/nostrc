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

## NIP-46 Methods

| Method              | Description                                    |
|---------------------|------------------------------------------------|
| `connect`           | Establish session; validate auth secret        |
| `get_public_key`    | Return agent's public key (hex)                |
| `sign_event`        | Policy check then sign from hot cache          |
| `nip04_encrypt`     | NIP-04 encrypt plaintext for a peer            |
| `nip04_decrypt`     | NIP-04 decrypt ciphertext from a peer          |
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
