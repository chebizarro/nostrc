# Signet — NIP-46 Nostr Bunker for Agent Fleets

Signet is a NIP-46 compliant Nostr bunker server built for managing cryptographic identities in autonomous agent fleets. It is not a general-purpose key manager for humans — it is infrastructure for systems where many agents each require their own Nostr identity, and none of those identities should ever be exposed as a raw private key.

## Features

### Core Signing

- **NIP-46 Remote Signing** — Signs Nostr events on behalf of registered agents over relay-based NIP-46 sessions
- **NIP-04 / NIP-44 Encryption** — Encrypt and decrypt messages for agents using standard Nostr encryption protocols
- **Hot Key Cache** — `sodium_malloc`-backed, `mlock`'d GHashTable for zero-latency signing (no disk read on the sign path)
- **SQLCipher Persistence** — AES-256 encrypted SQLite database for agent records, key material, credentials, leases, and audit logs
- **Per-Agent Key Rotation** — Rotate an agent's keypair without reprovisioning; old keys are wiped from cache and store

### Policy & Authorization

- **File-Backed Policy Store** — Per-identity policies with allow/deny lists for clients, methods, and event kinds; reloads on SIGHUP without restart
- **Runtime Policy Updates** — SET_POLICY management command parses submitted JSON and applies policies in-memory with file persistence
- **Capability Engine** — Fine-grained capability model (`nostr.sign`, `nostr.encrypt`, `credential.get_token`, `ssh.sign`, etc.) with rate limiting via token bucket
- **Fleet Registry** — Pluggable authorization backend (NIP-51 fleet lists, deny lists, internal mint tables) checked on every request
- **Deny List** — SQLCipher-backed pubkey deny list with emergency and normal revocation paths; deny list takes precedence over all other authorization

### Transports

- **NIP-46 over Relays** — Primary transport; encrypted NIP-44 v2 sessions over WebSocket relays with exponential backoff reconnect
- **D-Bus Unix** — `net.signet.Signer` and `net.signet.Credentials` interfaces on the system bus; auth via `SO_PEERCRED` UID-to-agent mapping
- **D-Bus TCP** — Same interfaces over TCP for LAN agents; configurable port (default 47472)
- **NIP-5L Unix Socket** — Line-delimited NIP-46 JSON framing over `/run/signet/nip5l.sock` with Nostr challenge auth; supports systemd socket activation
- **SSH Agent** — OpenSSH agent protocol on `/run/signet/ssh-agent.sock`; `SSH_AGENTC_SIGN_REQUEST` signs from the hot cache; supports systemd socket activation
- **Bootstrap HTTP** — `POST /bootstrap`, `GET /challenge`, `POST /auth` endpoints for agent onboarding and re-authentication

All v2 transports (D-Bus, NIP-5L, SSH agent, bootstrap) are disabled by default and enabled via config or environment variables.

### Management Protocol

All provisioning, revocation, and policy management is done through signed Nostr events — there is no REST management API.

| Kind  | Operation         | Description                           |
|-------|-------------------|---------------------------------------|
| 28000 | `provision_agent` | Generate keypair, return bunker:// URI|
| 28010 | `revoke_agent`    | Wipe key from cache and store         |
| 28020 | `set_policy`      | Parse and apply policy JSON           |
| 28030 | `get_status`      | Query daemon health                   |
| 28040 | `list_agents`     | Enumerate managed agents              |
| 28050 | `rotate_key`      | Rotate agent keypair                  |
| 28090 | `ack`             | Response to any management command    |

Management events are NIP-44 v2 encrypted between provisioner and bunker. Ack responses are encrypted to the requesting provisioner. Authorization requires the event's pubkey to be in the `provisioner_pubkeys` list.

### Bootstrap & Onboarding

- **NIP-17 Bootstrap Delivery** — Fleet Commander sends bootstrap tokens as gift-wrapped NIP-17 DMs to the agent's throwaway bootstrap pubkey
- **Single-Use Bootstrap Tokens** — Time-limited, attempt-capped, SHA256-hashed tokens bound to agent_id and bootstrap_pubkey
- **Challenge-Response Auth** — Shared Nostr-signed challenge validator (kind 28100) used across all transports; 30-second TTL, single-use challenges
- **Session Leases** — Time-bound session tokens (24h TTL) issued after successful authentication

### Credential Management

- **Extended Secret Store** — Stores multiple credential types (Nostr nsec, SSH keys, API tokens, certificates) with envelope encryption at rest
- **Credential Leasing** — Time-bound leases for credential access; agents request sessions via D-Bus or NIP-5L, Signet brokers the credential exchange
- **Version-Tracked Rotation** — Credential rotation archives the previous version; old versions remain in the `secret_versions` table
- **Session Brokering** — Agents call `GetSession(credential_id)` to exchange a stored credential for a service session token with automatic lease tracking

### Audit & Observability

- **Hash-Chained Audit Log** — Every signing, management, and credential operation is logged with `entry_hash = SHA256(ts || agent_id || op || detail || prev_hash)`. Chain integrity is verifiable offline via `signetctl verify-audit`
- **Structured JSON Logging** — 12-factor compliant; audit output to stdout or file
- **Health Endpoint** — `GET /health` via libmicrohttpd with Prometheus-style counters (bootstrap_total, auth_ok/denied/error, sign_total, revoke_total, active sessions/leases)
- **Replay Protection** — In-memory rolling window with configurable TTL and clock skew tolerance

### Process Hardening

- `sodium_malloc` + `mlock` on all key material (never swapped to disk)
- `prctl(PR_SET_DUMPABLE, 0)` disables core dumps
- `secure_wipe` / `explicit_bzero` on all secret buffers at revocation and shutdown
- Non-root `signet` user in production (Docker)
- Agent nsecs never leave Signet's process — agents receive only `nostrconnect://` URIs

## Architecture

The control plane is Nostr. All of it. There is no REST management API. The only HTTP surface is `/health` and the bootstrap endpoints.

```
Agent → NIP-46 request → Relay → signetd → hot cache → sign → Relay → Agent
Agent → D-Bus / NIP-5L / SSH → signetd → hot cache → sign → Agent
Admin → Signed mgmt event → Relay → signetd → provision/revoke/policy → Relay → Admin
Fleet Cmdr → NIP-17 bootstrap → Relay → Agent → POST /bootstrap → signetd → bunker:// URI
```

### Components

| Component            | Description                                                    |
|----------------------|----------------------------------------------------------------|
| **signetd**          | Main daemon; wires all subsystems with config-gated activation |
| **signetctl**        | CLI for remote management and local store introspection        |
| **NIP-46 Server**    | Handles `sign_event`, `get_public_key`, `connect`, `ping`     |
| **Key Store**        | `mlock`'d GHashTable + SQLCipher persistence                   |
| **Policy Store**     | File-backed (GKeyFile) with SIGHUP reload and TTL              |
| **Policy Engine**    | Evaluates requests against per-agent policy and rate limits    |
| **Capability Engine**| Capability-based authorization with rate limiting              |
| **Management Handler**| Parses and executes management events (kinds 28000-28090)     |
| **Relay Pool**       | WebSocket connections with exponential backoff reconnect       |
| **Audit Logger**     | Structured JSON logging to stdout or file                      |
| **Health Server**    | `GET /health` via libmicrohttpd                                |
| **Bootstrap Server** | HTTP endpoints for agent onboarding                            |
| **Session Broker**   | Exchanges stored credentials for session tokens                |
| **Deny List**        | SQLCipher-backed pubkey deny list for revocation               |
| **Challenge Store**  | In-memory challenge management with TTL cleanup                |
| **Replay Cache**     | Rolling window replay protection                               |

## Quick Start

### Prerequisites

- C compiler (C11)
- Meson ≥ 0.59
- GLib 2.56+, GObject, json-glib 1.0+
- libnostr + nostr-gobject (from monorepo)
- SQLCipher
- libsodium
- libmicrohttpd

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
./builddir/signetd -c signet.conf
```

Both `SIGNET_DB_KEY` and `SIGNET_BUNKER_NSEC` are **required**. Signet refuses to start without them.

### Docker

```bash
docker compose up -d
```

## Configuration

Signet uses GKeyFile INI format. See `signet.conf.example` for the full reference.

```ini
[server]
log_level = info
health_port = 8080

[store]
db_path = /data/signet.db

[nostr]
relays = wss://relay.example.com
reconnect_interval_s = 30
provisioner_pubkeys = <hex pubkey>
identity = default

[policy_defaults]
default_decision = deny
policy_file = /etc/signet/policies.conf
allowed_kinds =
rate_limit_rpm = 0

[replay]
max_entries = 50000
ttl_seconds = 600
skew_seconds = 120

[audit]
path =
stdout = true

[bootstrap]
# port = 9487

[dbus]
# unix = true
# tcp = false
# tcp_port = 47472

[nip5l]
# enabled = false
# socket = /run/signet/nip5l.sock

[ssh_agent]
# enabled = false
# socket = /run/signet/ssh-agent.sock
```

### Environment Variable Overrides

All configuration can be overridden with `SIGNET_`-prefixed environment variables:

| Variable                       | Description                              |
|--------------------------------|------------------------------------------|
| `SIGNET_DB_KEY`                | SQLCipher master key (required)          |
| `SIGNET_BUNKER_NSEC`          | Bunker identity nsec (required)          |
| `SIGNET_PROVISIONER_NSEC`     | Provisioner nsec for signetctl           |
| `SIGNET_RELAYS`                | Comma-separated relay URLs               |
| `SIGNET_LOG_LEVEL`            | `debug`, `info`, `warn`, `error`         |
| `SIGNET_DB_PATH`              | SQLCipher database path                  |
| `SIGNET_HEALTH_PORT`          | Health endpoint port (0 to disable)      |
| `SIGNET_AUDIT_PATH`           | Audit log file path (empty = stdout)     |
| `SIGNET_POLICY_PATH`          | Policy file path                         |
| `SIGNET_PROVISIONER_PUBKEYS`  | Comma-separated authorized hex pubkeys   |
| `SIGNET_BOOTSTRAP_PORT`       | Bootstrap HTTP port (0 = disabled)       |
| `SIGNET_DBUS_UNIX`            | Enable D-Bus Unix (`true`/`false`)       |
| `SIGNET_DBUS_TCP`             | Enable D-Bus TCP (`true`/`false`)        |
| `SIGNET_DBUS_TCP_PORT`        | D-Bus TCP port                           |
| `SIGNET_NIP5L`                | Enable NIP-5L (`true`/`false`)           |
| `SIGNET_NIP5L_SOCKET`         | NIP-5L socket path                       |
| `SIGNET_SSH_AGENT`            | Enable SSH agent (`true`/`false`)        |
| `SIGNET_SSH_AGENT_SOCKET`     | SSH agent socket path                    |

## CLI (signetctl)

### Remote Management (via Nostr relay)

```bash
# Provision a new agent identity
signetctl provision my-agent

# Revoke an agent identity
signetctl revoke my-agent

# Query daemon health
signetctl status

# List managed agents
signetctl list
```

### Local Store Introspection

```bash
# List agents with details
signetctl list-agents

# List active sessions
signetctl list-sessions

# List active credential leases
signetctl list-leases

# Verify hash-chained audit log integrity
signetctl verify-audit

# Rotate a credential (archives old version)
signetctl rotate-credential <credential-id>
```

Remote commands require `SIGNET_PROVISIONER_NSEC` to be set. Management events are NIP-44 encrypted and ack responses are validated by sender pubkey and correlated by request_id.

## Policy Configuration

See `policies.toml.example` for detailed examples. Policies are defined per-identity in GKeyFile format:

```ini
[identity.my-agent]
default = deny
allow_clients = ["*"]
allow_methods = ["sign_event", "get_public_key", "nip04_encrypt", "nip04_decrypt"]
allow_kinds = [1, 4, 7]
deny_kinds = [30078]
ttl_seconds = 3600
```

Policies can be updated at runtime via the SET_POLICY management command or by editing the policy file and sending SIGHUP.

## D-Bus Interface

Signet exposes two D-Bus interfaces (see `dbus/net.signet.Signer.xml`):

**net.signet.Signer** — Nostr signing operations:
- `GetPublicKey() → pubkey_hex`
- `SignEvent(event_json) → signed_event_json`
- `Encrypt(plaintext, peer_pubkey, algorithm) → ciphertext`
- `Decrypt(ciphertext, peer_pubkey, algorithm) → plaintext`

**net.signet.Credentials** — Credential lease operations:
- `GetSession(service_url) → (session_token, expires_at)`
- `GetToken(credential_type) → (token, expires_at)`
- `ListCredentials() → credential_types[]`

## Health Endpoint

```bash
curl http://localhost:8080/health
```

```json
{
  "status": "ok",
  "db_open": true,
  "relays_connected": true,
  "agents_active": 14,
  "cache_entries": 14,
  "relay_count": 2,
  "uptime_sec": 86400,
  "active_sessions": 12,
  "active_leases": 8,
  "sign_total": 42891,
  "auth_total_ok": 156,
  "auth_total_denied": 3
}
```

## Security Model

- Agent nsecs never leave Signet's process — agents only receive `nostrconnect://` URIs
- Key material in the hot cache is `sodium_malloc`'d + `mlock`'d (never swapped) and `secure_wipe`'d on revocation/shutdown
- SQLCipher provides AES-256 encryption at rest; credential payloads are additionally envelope-encrypted with per-agent libsodium secretbox
- Core dumps disabled via `prctl(PR_SET_DUMPABLE, 0)`
- Management protocol uses NIP-44 v2 encryption between provisioner and bunker
- Bootstrap tokens are single-use, time-limited, attempt-capped, and stored as SHA256 hashes
- Hash-chained audit log provides tamper-evident operation history
- Runs as non-root `signet` user in production

## License

MIT
