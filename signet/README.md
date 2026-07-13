# Signet — NIP-46 Nostr Bunker for Agent Fleets

Signet is a NIP-46 compliant Nostr bunker server built for managing cryptographic identities in autonomous agent fleets. It is not a general-purpose key manager for humans — it is infrastructure for systems where many agents each require their own Nostr identity, and none of those identities should ever be exposed as a raw private key.

## Features

### Core Signing

- **NIP-46 Remote Signing** — Signs Nostr events on behalf of registered agents over relay-based NIP-46 sessions
- **Persistent Client Pairing** — The one-time `connect_secret` is a pairing bootstrap: it is consumed atomically with a durable `client_pubkey → agent` binding, so bound clients reconnect across agent AND daemon restarts with no secret and no operator (identity-pinned; suspension/revocation take effect immediately)
- **NIP-04 / NIP-44 Encryption** — Encrypt and decrypt messages for agents using standard Nostr encryption protocols, including NIP-44 v2 `nip44_encrypt` / `nip44_decrypt` over NIP-46
- **Hot Key Cache** — `sodium_malloc`-backed, `mlock`'d GHashTable for zero-latency signing (no disk read on the sign path)
- **SQLCipher Persistence** — AES-256 encrypted SQLite database for agent records, key material, credentials, leases, and audit logs, verified at startup with `PRAGMA cipher_version` plus a keyed read
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

All provisioning, revocation, and policy management is done through Cascadia ContextVM signed Nostr intents — there is no REST management API. The canonical management plane is kind `25910`, usually gift-wrapped with NIP-59/NIP-17 kind `1059` for relay transport.

| Kind  | Method | Description |
|-------|--------|-------------|
| 25910 | `agent/provision` | Generate keypair, return bunker:// URI |
| 25910 | `agent/revoke` | Resolve pubkey, deny, burn leases, wipe key from cache/store, audit |
| 25910 | `agent/set-policy` | Parse and apply policy JSON |
| 25910 | `agent/get-status` | Query daemon health |
| 25910 | `agent/list` | Enumerate managed agents |
| 25910 | `agent/rotate-key` | Rotate agent keypair |
| 25910 | `agent/adopt-existing` | Register an externally supplied (BYO) keypair as an agent |
| 25910 | `agent/reissue-connect` | Mint a fresh one-time `connect_secret` for an existing agent (forced re-pairing) |
| 25910 | `agent/list-clients` | List an agent's persistent NIP-46 client bindings |
| 25910 | `agent/revoke-client` | Soft-revoke a NIP-46 client binding (client must re-pair) |

Management intents are encrypted between provisioner and bunker when transported through gift-wrap. Responses are correlated ContextVM results; legacy `28090` ACKs are only for the disabled-by-default `legacy_28000` compatibility path. Authorization requires the event pubkey to be in the `provisioner_pubkeys` list, with one exception: `agent/reissue-connect` is also authorized when the signed sender IS the target agent (self-service re-pairing; grants no power over other agents or methods). Each delivered management event id executes at most once per replay-cache TTL, so relay redelivery and history replay cannot re-run non-idempotent commands; self-service events are isolated in their own replay domain.

### Bootstrap & Onboarding

- **NIP-17 Bootstrap Delivery** — Fleet Commander sends bootstrap tokens as gift-wrapped NIP-17 DMs to the agent's throwaway bootstrap pubkey
- **Single-Use Bootstrap Tokens** — Time-limited, attempt-capped, SHA256-hashed tokens bound to agent_id and bootstrap_pubkey; `POST /bootstrap` consumes the token atomically and replays return 403
- **Challenge-Response Auth** — Shared Nostr-signed challenge validator (kind 28100) used across all transports; 30-second TTL, single-use challenges
- **Session Leases** — Time-bound session tokens (24h TTL) issued after successful authentication

### Credential Management

- **Extended Secret Store** — Stores multiple credential types (Nostr nsec, SSH keys, API tokens, certificates) with envelope encryption at rest
- **Per-Agent Envelope Keys** — Each agent gets a distinct credential-encryption key derived via BLAKE2b from the store DEK and agent pubkey
- **Credential Leasing** — Time-bound leases for credential access; agents request sessions via D-Bus or NIP-5L, Signet brokers the credential exchange
- **Version-Tracked Rotation** — Credential rotation archives the previous version; old versions remain in the `secret_versions` table
- **Transactional Rotation** — `signet_store_rotate_secret` archives the old value and updates the current value in one SQLite transaction
- **Session Brokering** — Agents call `GetSession(credential_id)` to exchange a stored credential for a service session token with automatic lease tracking

### Audit & Observability

- **Hash-Chained Audit Log** — Every signing, management, and credential operation is written to the SQLCipher audit store with `entry_hash = SHA256(ts || agent_id || op || detail || prev_hash)`. Chain integrity is verifiable offline via `signetctl verify-audit`. (Hash-chaining applies to the database audit store; the stdout/file JSON stream below is a separate, non-chained operational log.)
- **Structured JSON Logging** — 12-factor compliant; operational audit output to stdout or file (not hash-chained; use the SQLCipher store for tamper-evident history)
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
Admin → ContextVM 25910 intent → Relay → signetd → provision/revoke/policy → Relay → Admin
Fleet Cmdr → NIP-17 bootstrap → Relay → Agent → POST /bootstrap → signetd → bunker:// URI
```

### Components

| Component            | Description                                                    |
|----------------------|----------------------------------------------------------------|
| **signetd**          | Main daemon; wires all subsystems with config-gated activation |
| **signetctl**        | CLI for remote management and local store introspection        |
| **NIP-46 Server**    | Handles `sign_event`, `get_public_key`, `connect` (pairing + binding reconnect), NIP-04/NIP-44 encryption, `get_relays`, `ping`; resolves clients via persistent, identity-pinned bindings |
| **Key Store**        | `mlock`'d GHashTable + SQLCipher persistence                   |
| **Policy Store**     | File-backed (GKeyFile) with SIGHUP reload and TTL              |
| **Policy Engine**    | Evaluates requests against per-agent policy and rate limits    |
| **Capability Engine**| Capability-based authorization with rate limiting              |
| **Management Handler**| Parses and executes ContextVM `25910` management intents; legacy `28000`-series events are compatibility-only when explicitly enabled |
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

For real AES-256 encryption at rest, build against SQLCipher (otherwise the DB
is plain SQLite and only per-record envelope encryption protects secret keys):

```bash
meson setup builddir -Dsignet_use_sqlcipher=true
```

At startup Signet verifies SQLCipher is active (`PRAGMA cipher_version` + a
keyed read). If verification fails and `SIGNET_REQUIRE_ENCRYPTED_DB=true`, the
daemon refuses to start; otherwise it logs a prominent warning. Set
`SIGNET_REQUIRE_ENCRYPTED_DB=true` in production so accidental plain SQLite
builds fail closed.

#### Migrating a legacy plaintext database

When a SQLCipher build opens a `db_path` that points at a **legacy plaintext
SQLite database** (created by an older build linked against plain SQLite), it
refuses to use it as-is and transparently migrates it to a new SQLCipher-
encrypted database keyed by `SIGNET_DB_KEY`. The original is preserved as
`<db_path>.plaintext-backup`, all rows are copied via SQLCipher's
`sqlcipher_export`, and stale WAL/`-shm` sidecars are removed. Detection is by
the on-disk `SQLite format 3\0` magic header, so already-encrypted databases
are never touched.

Auto-migration on startup is the default. To disable it (and instead refuse to
open a legacy database), set `SIGNET_MIGRATE_PLAINTEXT_DB=false`. You can also
migrate explicitly without starting the daemon:

```bash
SIGNET_DB_KEY=... signetctl -c signet.conf migrate-db
```

Migration requires a SQLCipher-linked build (`-Dsignet_use_sqlcipher=true` for
meson, `-DSIGNET_USE_SQLCIPHER=ON` for CMake); plain-SQLite builds keep their
prior behavior.

### Run

```bash
export SIGNET_DB_KEY="<base64-encoded-32-byte-key>"
export SIGNET_BUNKER_NSEC="nsec1..."
./builddir/signetd -c signet.conf
```

Both `SIGNET_DB_KEY` and `SIGNET_BUNKER_NSEC` are **required**. Signet refuses to start without them. In production, also set `SIGNET_REQUIRE_ENCRYPTED_DB=true` so startup fails unless SQLCipher verification succeeds.

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
| `SIGNET_DB_KEY`                | SQLCipher master key (required; hex, base64, or passphrase) |
| `SIGNET_REQUIRE_ENCRYPTED_DB`  | Refuse to start unless the DB is SQLCipher-encrypted |
| `SIGNET_MIGRATE_PLAINTEXT_DB`  | Auto-migrate a legacy plaintext DB to SQLCipher on open (default true; set `false` to refuse instead) |
| `SIGNET_BUNKER_NSEC`          | Bunker identity nsec (required)          |
| `SIGNET_PROVISIONER_NSEC`     | Provisioner nsec for signetctl           |
| `SIGNET_BUNKER_PUBKEY`        | Bunker pubkey (npub/hex) for signetctl to address the bunker |
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

# Adopt an existing (BYO) keypair as an agent
signetctl adopt-existing my-agent --sec nsec1... --expected-pubkey <hex>

# Mint a fresh one-time connect_secret (forced re-pairing);
# --out writes it atomically (0600) for a file-backed client config
signetctl reissue-connect my-agent --out /run/agent/connect-secret

# List / revoke an agent's persistent NIP-46 client bindings
signetctl list-clients my-agent
signetctl revoke-client <client_pubkey>

# Rotate an agent identity key
signetctl rotate my-agent

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

# Migrate a legacy plaintext SQLite DB to SQLCipher (requires a SQLCipher build)
SIGNET_DB_KEY=... signetctl migrate-db
```

Remote commands emit **Cascadia ContextVM** JSON-RPC 2.0 intents (kind 25910)
wrapped in **NIP-59 gift-wraps** (kind 1059) addressed to the bunker, and consume
the daemon's gift-wrapped ContextVM reply (validated by the bunker's pubkey and
correlated by the JSON-RPC `id`). This matches the daemon's default protocol; the
deprecated 28000-series is no longer used by signetctl.

Requirements:
- `SIGNET_PROVISIONER_NSEC` — the provisioner key that signs/authorizes intents.
- The bunker's pubkey, so signetctl can address it: set `[nostr] bunker_pubkey`
  (npub or hex) in the config, or `SIGNET_BUNKER_PUBKEY` in the environment.

## Policy Configuration

See `policies.toml.example` for detailed examples. Policies are defined per-identity in GKeyFile format:

```ini
[identity.my-agent]
default = deny
allow_clients = ["*"]
allow_methods = ["sign_event", "get_public_key", "nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt"]
allow_kinds = [1, 4, 7]
deny_kinds = [30078]
ttl_seconds = 3600
```

Policies can be updated at runtime via the SET_POLICY management command or by editing the policy file and sending SIGHUP.

## NIP-46 Methods

| Method            | Params                                      | Description                         |
|-------------------|---------------------------------------------|-------------------------------------|
| `connect`         | `[signer_pubkey, connect_secret?]`          | Pair (one-time secret) or reconnect (bound clients need no secret) |
| `get_public_key`  | `[]`                                        | Return agent public key hex         |
| `sign_event`      | `[event_json]`                              | Policy check then sign from cache   |
| `nip04_encrypt`   | `[peer_pubkey_hex, plaintext]`              | NIP-04 encrypt for a peer           |
| `nip04_decrypt`   | `[peer_pubkey_hex, ciphertext]`             | NIP-04 decrypt from a peer          |
| `nip44_encrypt`   | `[peer_pubkey_hex, plaintext]`              | NIP-44 v2 encrypt for a peer        |
| `nip44_decrypt`   | `[peer_pubkey_hex, ciphertext]`             | NIP-44 v2 decrypt from a peer       |
| `get_relays`      | `[]`                                        | Return relay map JSON               |
| `ping`            | `[]`                                        | Keepalive                           |

## D-Bus Interface

Signet exposes two D-Bus interfaces (see `dbus/net.signet.Signer.xml`):

**net.signet.Signer** — Nostr signing operations:
- `GetPublicKey() → pubkey_hex`
- `SignEvent(event_json) → signed_event_json`
- `Encrypt(plaintext, peer_pubkey, algorithm) → ciphertext`
- `Decrypt(ciphertext, peer_pubkey, algorithm) → plaintext`

**net.signet.Credentials** — Credential lease operations:
- `GetSession(service_url) → (session_token, expires_at)` — refuses to mint tokens when no credential store is configured
- `GetToken(credential_type) → (token, expires_at)` — enforces per-agent ownership; agents can only retrieve their own credentials
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

## Testing & CI

Signet has a real hardening test suite: 17 Meson tests / 18 CMake ctests (plus passkeys-ON entries) cover real crypto paths, rate-limit enforcement, signed-challenge authentication verification, audit hash-chain tamper detection, management replay protection, connect-secret reissue and self-service authorization, persistent client-binding pairing/reconnect/revocation, and pubkey backfill/uniqueness. `.github/workflows/signet-ci.yml` runs the Signet CI matrix, including a passkeys-ON entry.

## Security Model

- Agent nsecs never leave Signet's process — agents only receive `nostrconnect://` URIs
- Key material in the hot cache is `sodium_malloc`'d + `mlock`'d (never swapped) and `secure_wipe`'d on revocation/shutdown
- SQLCipher provides AES-256 encryption at rest and is verified at startup with `PRAGMA cipher_version` plus a keyed read; with `SIGNET_REQUIRE_ENCRYPTED_DB=true`, verification failure stops the daemon
- Credential payloads are envelope-encrypted with per-agent libsodium secretbox keys derived via BLAKE2b from the store DEK and agent pubkey
- Core dumps disabled via `prctl(PR_SET_DUMPABLE, 0)`
- Management protocol uses NIP-44 v2 encryption between provisioner and bunker, and ACKs fail closed instead of falling back to plaintext on encryption errors
- Relay publish reports an error when zero relays are connected instead of pretending success
- Bootstrap tokens are single-use, time-limited, attempt-capped, and stored as SHA256 hashes; `POST /bootstrap` consumes them atomically and replay returns 403
- Revocation resolves the agent pubkey, adds it to the deny list, burns leases, revokes the agent's persistent client bindings, wipes the key from cache and store, and records an audit entry
- NIP-46 pairing is atomic (secret consumption + durable client binding in one transaction); bindings are pinned to the agent identity pubkey at pairing time, so rotation or reprovisioning never resurrects old client authority
- The daemon's live deny list is enforced on every NIP-46 path (pairing, reconnect, per-request) before policy evaluation — suspension takes effect immediately
- Management intents are replay-protected per delivered event id (dedicated caches; self-service traffic cannot evict provisioner entries), and one custody pubkey can never be bound to two agents (partial unique index + non-destructive upsert)
- Credential rotation archives and updates secrets in a single SQLite transaction
- Hash-chained audit log provides tamper-evident operation history
- Runs as non-root `signet` user in production

## License

MIT
