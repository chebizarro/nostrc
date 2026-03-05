# Signet Integration Guide

## Migration Notice

Signet is being migrated from a HashiCorp Vault backend to SQLCipher + mlock'd hot key cache.
The Vault client has been removed. See the README for the current architecture.

## Agent Integration

Agents connect to Signet via the NIP-46 protocol. The workflow is:

1. **Provisioning**: An authorized provisioner sends a signed management event (kind 28000)
   to Signet via a Nostr relay. Signet generates a keypair and returns a `nostrconnect://` URI.

2. **Agent Boot**: The agent receives the `nostrconnect://` URI as an environment variable.
   It uses NIP-46 to establish a session with Signet over the relay.

3. **Signing**: The agent sends `sign_event` requests via NIP-46. Signet evaluates per-agent
   policy, signs from the hot key cache, and returns the signed event.

4. **Revocation**: A provisioner sends a revoke event (kind 28010). Signet zeros the cache
   entry and refuses all further requests for that agent.

## NIP-46 Methods

- `connect` — Establish session; validate auth secret
- `get_public_key` — Return agent's npub
- `sign_event` — Policy check then sign from cache
- `ping` — Keepalive

## Management Protocol (Nostr Events)

All management traffic flows as signed Nostr events. See `protocol/MANAGEMENT-PROTOCOL.md`
for the wire format specification.

| Kind  | Purpose |
|-------|---------|
| 28000 | Provision request |
| 28001 | Provision response |
| 28010 | Revoke request |
| 28011 | Revoke response |
| 28020 | Policy update request |
| 28021 | Policy update response |
| 28030 | Status request |
| 28031 | Status response |
| 28040 | Audit query request |
| 28041 | Audit query response |
| 28090 | Error response |

## Health Endpoint

```bash
curl http://localhost:8080/health
```

Returns 200 if healthy, 503 if SQLCipher not open or no relay connections.
