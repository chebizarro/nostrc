# Signet - NIP-46 Nostr Bunker Server

Signet is a NIP-46 compliant Nostr bunker server purpose-built for managing cryptographic identities in agent fleets. It is not a general-purpose key manager for humans — it is infrastructure for autonomous agent systems where many agents each require their own Nostr identity, none of which should ever be exposed as a raw private key.

## Features

- **NIP-46 Remote Signing**: Signs Nostr events on behalf of registered agents
- **SQLCipher Persistence**: AES-256 encrypted SQLite database for agent records and key material
- **Hot Key Cache**: `mlock`'d in-process GHashTable for zero-latency signing (no disk read on sign path)
- **Policy-Gated Signing**: Per-agent allowed event kinds, rate limits, tag restrictions
- **Nostr-Native Management**: All provisioning, revocation, and policy updates via signed Nostr events (no REST API)
- **Replay Protection**: Rolling window prevents replayed management and signing events
- **Audit Logging**: Structured JSON logging of every signing and management operation
- **Process Hardening**: `mlock`, `prctl(PR_SET_DUMPABLE, 0)`, `explicit_bzero` on all key material

## Architecture

The control plane is Nostr. All of it. There is no REST management API. The only HTTP surface is `/health`.

```
Agent → NIP-46 request → Relay → Signet → hot cache lookup → sign → Relay → Agent
Admin → Signed mgmt event → Relay → Signet → provision/revoke/policy → Relay → Admin
```

### Core Components

1. **NIP-46 Server** - Handles signing requests via NIP-46 protocol
2. **Key Cache** - `mlock`'d GHashTable: agent_id → nsec_bytes[32]
3. **SQLCipher Store** - Encrypted persistence for agents, policies, audit log
4. **Policy Engine** - Evaluate proposed events against per-agent policy
5. **Management Protocol** - Nostr event kinds 28000-28090 for fleet management
6. **Audit Logger** - Structured JSON to stdout (12-factor)
7. **Health Server** - GET /health via libmicrohttpd
8. **Relay Pool** - WebSocket connections with exponential backoff reconnect

## Quick Start

### Prerequisites

- C compiler (C11)
- Meson >= 0.59
- GLib 2.56+, GObject
- libnostr + nostr-gobject (from monorepo)
- SQLCipher
- libsodium
- libmicrohttpd
- json-glib 1.0+

### Build

```bash
cd signet
meson setup builddir
meson compile -C builddir
```

### Run

```bash
export SIGNET_DB_KEY="<base64-encoded-32-byte-key>"
export SIGNET_BUNKER_NSEC="nsec1..."
./builddir/signetd -c signet.toml
```

Both `SIGNET_DB_KEY` and `SIGNET_BUNKER_NSEC` are **required**. Signet refuses to start without them.

### Docker

```bash
docker compose up -d
```

## Configuration

See `signet.toml.example` for the full configuration reference.

```toml
[server]
log_level = "info"
health_port = 8080

[store]
db_path = "/data/signet.db"

[nostr]
relays = ["wss://relay.example.com"]
reconnect_interval_s = 30
provisioner_pubkeys = ["<hex pubkey>"]

[policy_defaults]
allowed_kinds = []
rate_limit_rpm = 0
```

## CLI

```bash
signet-cli provision --label "agent-01" --allow-kinds 0,1,4 --rate-limit 60
signet-cli revoke --agent-id <uuid>
signet-cli policy --agent-id <uuid> --allow-kinds 0,1
signet-cli status --agent-id <uuid>
signet-cli audit --agent-id <uuid> --since 2026-01-01
signet-cli health
```

## Health Endpoint

```bash
curl http://localhost:8080/health
```

```json
{
  "status": "ok",
  "db": "open",
  "relays": 2,
  "agents_active": 14,
  "cache_entries": 14,
  "uptime_s": 86400
}
```

## Security

- Agent nsecs never leave Signet's process — agents only receive `nostrconnect://` URIs
- Key material in the hot cache is `mlock`'d (never swapped) and `explicit_bzero`'d on revocation/shutdown
- SQLCipher provides AES-256 encryption at rest; nsec column is additionally encrypted with per-agent libsodium secretbox
- Core dumps disabled via `prctl(PR_SET_DUMPABLE, 0)`
- Runs as non-root `signet` user in production

## Migration Status

This codebase is being migrated from a HashiCorp Vault backend to SQLCipher + hot key cache.
See beads issues for tracking.

## License

MIT
