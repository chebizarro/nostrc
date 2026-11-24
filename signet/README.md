# Signet — NIP-46 Nostr Bunker for Agent Fleets

Signet is a NIP-46 compliant Nostr bunker server built for managing cryptographic identities in autonomous agent fleets. It is not a general-purpose key manager for humans — it is infrastructure for systems where many agents each require their own Nostr identity, and none of those identities should ever be exposed as a raw private key.

## Launch scope (0.8.0 candidate)

The supported deployment is one Linux/amd64 daemon with a persistent SQLCipher
store, outbound NIP-46 signing and gift-wrapped management, operator-assisted
enrollment, and on-box loopback health/metrics. See the
[deployment runbook](deploy/README.md) and the
[implemented-vs-promised assessment](../docs/plans/signet-production-deployment-2026-11-06.md#11a-assessment-table-promised--observed--status--launch-action).
The assessment is a source-inspection baseline, not proof that the candidate
has passed deployment acceptance.

HTTP bootstrap is packaged behind an optional Caddy profile but disabled at
launch: there is no shipped HTTP session-token consumer. Programmatic passkeys
require the optional `passkeys` image, runtime configuration, a persistent store
and a mounted single-instance fleet PSK; cross-instance sync is not supported
at launch. Virtual CTAP-HID is experimental Linux-host functionality, not a
remote browser authenticator or a Droplet requirement. D-Bus, NIP-5L, SSH agent
and `signet-git-credential` are outside the container launch baseline.
The Go `signet-mcp` wrapper is a stub (fixed `tools/call` response), **not built
or shipped in this release**; a future native sidecar is separate work.

## Features

### Core Signing

- **NIP-46 Remote Signing** — Signs Nostr events on behalf of registered agents over relay-based NIP-46 sessions
- **Persistent Client Pairing** — The one-time `connect_secret` is a pairing bootstrap: it is consumed atomically with a durable `client_pubkey → agent` binding, so bound clients reconnect across agent AND daemon restarts with no secret and no operator (identity-pinned; suspension/revocation take effect immediately)
- **NIP-04 / NIP-44 Encryption** — Encrypt and decrypt messages for agents using standard Nostr encryption protocols, including NIP-44 v2 `nip44_encrypt` / `nip44_decrypt` over NIP-46
- **Hot Key Cache** — `sodium_malloc`-backed GHashTable (memory locking is best-effort) for legacy non-signing key uses; signing now reads custody under a durable SQLite writer transaction
- **Single-Writer Fence** — A provisioner can assign a service identity one authorized NIP-46 client (a single-use client key); every other client, any former owner, and non-NIP-46 transports are refused at custody. Standard NIP-46 request shapes are unchanged. See [cutover contract](docs/WRITER_EPOCH_CUTOVER.md).
- **SQLCipher Persistence** — When built with SQLCipher and opened with a master key, AES-256 encrypted SQLite database for agent records, key material, credentials, leases, and audit logs, verified at startup with `PRAGMA cipher_version` plus a keyed read
- **Per-Agent Key Rotation** — Rotate an agent's keypair without reprovisioning; old keys are wiped from cache and store

### Policy & Authorization

- **File-Backed Policy Store** — Per-identity policies with allow/deny lists for clients, methods, and event kinds; reloads on SIGHUP without restart
- **Runtime Policy Updates** — SET_POLICY management command parses submitted JSON and applies policies in-memory with file persistence
- **Capability Engine** — Fine-grained capability model (`nostr.sign`, `nostr.encrypt`, `credential.get_token`, `ssh.sign`, etc.) with rate limiting via token bucket
- **Fleet Registry** — Pluggable authorization backend (NIP-51 fleet lists, deny lists, internal mint tables) checked on every request
- **Deny List** — SQLCipher-backed pubkey deny list updated by `agent/revoke`; deny-list checks take precedence over other authorization.

### Transports

- **NIP-46 over Relays** — Primary transport; encrypted NIP-44 v2 sessions over WebSocket relays with exponential backoff reconnect
- **D-Bus Unix** — `net.signet.Signer` and `net.signet.Credentials` interfaces on the system bus; auth via `SO_PEERCRED` UID-to-agent mapping
- **D-Bus TCP** — Optional plaintext transport, bound to `127.0.0.1:47472` by default when enabled; LAN access requires an explicit bind host
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

Management intents are encrypted between provisioner and bunker when transported through gift-wrap. Responses are correlated, gift-wrapped ContextVM results. Authorization requires the event pubkey to be in the `provisioner_pubkeys` list, with two exceptions: `agent/reissue-connect` is also authorized when the signed sender IS the target agent (self-service re-pairing; grants no power over other agents or methods), and `credential/inspect`, `credential/list` and `credential/deliver` are also authorized for a requester that the credential owner's policy grants exactly `credential.deliver` (a secret-reading identity with no management authority; see [INTEGRATION.md](INTEGRATION.md#credential-methods)). Replies echo the request's JSON-RPC id, refusals included. Replay protection is RAM-only and keyed by delivered event id: authenticated request time must be within `[replay] skew_seconds`, and ids are retained for twice that window without evicting unexpired entries. It is not durable exactly-once execution: restart, rewrapping and newly signed retries can repeat a logical mutation. After a timeout, inspect state before deciding whether to retry; a lost reply does not mean the mutation failed. Any deliberate stop must keep the daemon down for at least 270 seconds before restart (twice the maximum 120-second replay skew plus 30 seconds of margin); abrupt restarts retain a replay risk. Direct kind-25910 request content is readable by relays: use gift-wrap for confidentiality. Non-provisioner senders are rate-limited per sender before any refusal or audit, and their refusals are audited within a global budget.

### Bootstrap & Onboarding

- **Launch enrollment** — Operator runs `signetctl provision`; the client pairs once with the returned secret-bearing bunker URI, then reconnects with its persistent client key. See [launch enrollment](INTEGRATION.md#launch-enrollment).
- **Optional NIP-17 URI handoff** — Provisioning can send a `signet/bunker-handoff` DM. Enqueue or relay acceptance is not recipient delivery; advertise only after a real receiver completes the round trip.
- **Deferred HTTP workflow** — `/bootstrap`, `/challenge` and `/auth` exist, with token/challenge/lease helpers, but token insertion and receive/ACK helpers have no production callers. These do not constitute automated fleet enrollment; the HTTP profile stays disabled.

#### Disabled HTTP bootstrap contract

The HTTP profile remains disabled; this contract does not add token enrollment or
an automated recipient/ACK workflow. If a provisioner inserts a bootstrap token,
it must bind the token hash to an already-provisioned `agent_id` and the client's
`bootstrap_pubkey` (which can differ from the agent's signer key).

1. Fetch `GET /challenge?agent_id=<id>` over the configured TLS proxy. IDs must be
   provisioned, nonempty, and at most 128 bytes. Challenges expire after 30 seconds.
   The store replaces the oldest pending proof above four per agent or 1,024
   globally; consumed proofs do not occupy those budgets. Issuance and a five-second main-loop
   cleanup source prune expired entries, even during idle periods.
2. Sign a `SIGNET_AUTH_KIND` event with the bootstrap private key, empty content,
   and tags `[["challenge", "<hex>"], ["agent", "<id>"],
   ["purpose", "signet-bootstrap"], ["token", "<SHA256(raw token) hex>"]]`.
   Submit `POST /bootstrap` with `token`, `agent_id`, `bootstrap_pubkey`, and that
   event **object** in `auth_event`. The proof is single-use and cannot serve as
   `/auth`'s `signet-auth` proof; missing, expired, mismatched or replayed proofs
   receive no handoff URI.
3. On success the existing identity receives a fresh random one-time pairing
   secret in the returned bunker URI, replacing any pending pairing secret. The
   token is consumed before rotation so a replay cannot invalidate the winner.
   Failures after consumption burn the token; obtain a new token rather than
   retrying the old one. A lost response is not evidence that nothing changed.

The bootstrap token alone is **not** authority to retrieve the URI: possession of
its bound private key is also required. The returned URI, however, **is a bearer
capability** until first NIP-46 pairing: anyone holding it can use its one-time
secret. Deliver it confidentially, never log it, and do not confuse the response's
agent `pubkey` with the bootstrap client's key. First pairing atomically consumes
the secret and binds the client's public key; established bindings are preserved
by secret rotation. Only one pending secret exists per agent, so a later valid
handoff supersedes an earlier unpaired URI.

### Credential Management

- **Extended Secret Store** — Stores multiple credential types (Nostr nsec, SSH keys, API tokens, certificates) with envelope encryption at rest
- **Per-Agent Envelope Keys** — Each agent gets a distinct credential-encryption key derived via BLAKE2b from the store DEK and agent pubkey
- **Credential Leasing** — Time-bound leases for credential access; agents request sessions via D-Bus or NIP-5L, Signet brokers the credential exchange
- **Version-Tracked Rotation** — Credential rotation archives the previous version; old versions remain in the `secret_versions` table
- **Transactional Rotation** — `signet_store_rotate_secret` archives the old value and updates the current value in one SQLite transaction
- **Session Brokering** — Agents call `GetSession(service_url)` to mint a service session token with automatic lease tracking

### Audit & Observability

- **Database Audit Chain** — Credential-operation audit records use the database hash chain, verifiable with `signetctl verify-audit`. This is not a claim that every signing/management operation is chained. The JSON operational stream is separate and unchained; without an independently retained checkpoint the chain cannot detect wholesale history replacement.
- **Structured JSON Logging** — Operational audit output to stdout or file, not hash-chained. Launch persists `/data/audit.jsonl` and includes it in the backup bundle.
- **Observability** — libmicrohttpd serves `/health` liveness, `/ready` readiness and `/metrics` Prometheus counters; loopback-only in the launch layout
- **Replay Protection** — In-memory rolling window with configurable TTL and clock skew tolerance

### Process Hardening

- Guarded `sodium_malloc` allocations and explicit wiping reduce exposure; locking is best-effort, not a guarantee against swap or all transient copies.
- `SIGNET_MLOCK_MODE=best-effort` is the daemon default; `required` refuses startup unless current and future process pages can be locked with an unlimited soft memlock limit. `off` disables process-wide locking only. See [Memory-locking policy](#memory-locking-policy).
- Linux `prctl(PR_SET_DUMPABLE, 0)` failures warn and continue independently of the locking policy. Enforce a zero core limit in deployment; verify actual process limits.
- Non-root container user; live root/same-UID compromise is outside the at-rest protection boundary.
- Normal remote signing does not export agent nsecs. BYO import, operator-held keys, encrypted backups and credential delivery have separate custody boundaries; bunker URIs are secrets.

## Architecture

The control plane is Nostr. All of it. There is no REST management API. HTTP provides `/health`, `/ready`, `/metrics` and the optional, disabled bootstrap endpoints.

```
Agent → NIP-46 request → Relay → signetd → persistent custody transaction → Relay → Agent
Admin → gift-wrapped ContextVM intent → Relay → signetd → mutation → encrypted reply
Operator → provision → secret-bearing bunker URI → client pairing (no HTTP session)
```

### Components

| Component            | Description                                                    |
|----------------------|----------------------------------------------------------------|
| **signetd**          | Main daemon; wires all subsystems with config-gated activation |
| **signetctl**        | CLI for remote management and local store introspection        |
| **NIP-46 Server**    | Handles `sign_event`, `get_public_key`, `connect` (pairing + binding reconnect), NIP-04/NIP-44 encryption, `get_relays`, `ping`; resolves clients via persistent, identity-pinned bindings |
| **Key Store**        | Guarded cache + persistent custody transactions                   |
| **Policy Store**     | File-backed (GKeyFile) with SIGHUP reload and TTL              |
| **Policy Engine**    | Evaluates requests against per-agent policy and rate limits    |
| **Capability Engine**| Capability-based authorization with rate limiting              |
| **Management Handler**| Parses and executes ContextVM `25910` management intents and returns gift-wrapped JSON-RPC results |
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
SIGNET_DB_KEY_FILE=/run/secrets/signet_db_key signetctl -c signet.conf migrate-db
```

Migration requires a SQLCipher-linked build (`-Dsignet_use_sqlcipher=true` for
meson, `-DSIGNET_USE_SQLCIPHER=ON` for CMake); plain-SQLite builds keep their
prior behavior.

### Run

```bash
export SIGNET_DB_KEY_FILE="/run/secrets/signet_db_key"
export SIGNET_REQUIRE_ENCRYPTED_DB=true
export SIGNET_BUNKER_NSEC_FILE="/run/secrets/signet-bunker-nsec"
./builddir/signetd -c signet.conf
```

Production requires a master-key file, bunker identity file, persistent DB path
and `SIGNET_REQUIRE_ENCRYPTED_DB=true`. The 0.8.0 runtime contract refuses an
explicit invalid config or required-encryption startup without usable persistent
storage. Cache-only mode remains an explicit development option when encryption
is not required. Install protected files before starting; do not generate a new
identity when a file is absent. See the [runbook](deploy/README.md) for custody,
permissions, online backup and the ≥270-second deliberate-stop rule.

### Docker

Use the [deployment runbook](deploy/README.md), following
[image/Compose packaging](../docs/plans/signet-production-deployment-2026-11-06.md#9-w3--images-compose-release-packaging)
and [provisioning/operations](../docs/plans/signet-production-deployment-2026-11-06.md#10-w4--digitalocean-provisioning-and-operations).
The launch layout uses public, digest-pinned GHCR images (owner-approved
2026-11-07); Droplet pulls are anonymous and need no package credential.
It uses systemd supervision, host networking with loopback observability,
rw `/etc/signet` for D2 write-back,
separately mounted secrets and a verified state volume. IPv6 and bootstrap are
disabled; Caddy is only for the optional bootstrap profile. Do not use the old
`signet.toml.example` quick start (that file does not exist).

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
provisioner_pubkeys = <hex pubkey>
identity = default

[policy_defaults]
default_decision = deny
policy_file = /data/policies.conf
allowed_kinds =
rate_limit_rpm = 0

[replay]
max_entries = 50000
ttl_seconds = 600
skew_seconds = 120

[audit]
path = /data/audit.jsonl
stdout = false

[bootstrap]
port = 0
require_tls = true
trusted_proxy_cidrs = 127.0.0.0/8;::1/128;

[dbus]
unix_enabled = false
tcp_enabled = false
# tcp_host = 127.0.0.1
# tcp_port = 47472
# tcp_allow_credentials = false

[nip5l]
# enabled = false
# socket_path = /run/signet/nip5l.sock

[ssh_agent]
# enabled = false
# socket_path = /run/signet/ssh-agent.sock
```

### Environment Variable Overrides

Supported overrides follow (not every INI field has an environment override).
New 0.8.0 file-key and bootstrap-trust inputs are the release contract in
[plan §7.B/§7.D](../docs/plans/signet-production-deployment-2026-11-06.md#7-w1--runtime-safety-and-interface-changes):

| Variable                       | Description                              |
|--------------------------------|------------------------------------------|
| `SIGNET_DB_KEY_FILE` | Protected master-key file; preferred production input |
| `SIGNET_DB_KEY` | Legacy value-bearing master key; do not put in production env files |
| `SIGNET_BACKUP_KEY_FILE` | Protected independent backup-key file for local backup/restore |
| `SIGNET_BACKUP_KEY` | Legacy value-bearing backup key; compatibility only |
| `SIGNET_REQUIRE_ENCRYPTED_DB`  | Refuse to start unless the DB is SQLCipher-encrypted |
| `SIGNET_MIGRATE_PLAINTEXT_DB`  | Auto-migrate a legacy plaintext DB to SQLCipher on open (default true; set `false` to refuse instead) |
| `SIGNET_BUNKER_NSEC_FILE`     | File containing bunker identity nsec (required) |
| `SIGNET_PROVISIONER_NSEC_FILE` | File containing provisioner nsec for signetctl |
| `SIGNET_BUNKER_PUBKEY`        | Bunker pubkey (npub/hex) for signetctl to address the bunker |
| `SIGNET_RELAYS`                | Comma-separated relay URLs               |
| `SIGNET_LOG_LEVEL`            | `debug`, `info`, `warn`, `error`         |
| `SIGNET_MLOCK_MODE`           | Startup-only: `off`, `best-effort` (daemon default), `required` (production Compose default) |
| `SIGNET_DB_PATH`              | SQLCipher database path                  |
| `SIGNET_HEALTH_PORT`          | Health endpoint port (0 to disable)      |
| `SIGNET_AUDIT_PATH`           | Audit log file path (empty = stdout)     |
| `SIGNET_POLICY_PATH`          | Policy file path                         |
| `SIGNET_PROVISIONER_PUBKEYS`  | Overrides the D2 desired-state config list; never set in production (see below) |
| `SIGNET_BOOTSTRAP_PORT`       | Bootstrap HTTP port (default 0 = disabled) |
| `SIGNET_BOOTSTRAP_REQUIRE_TLS` | `[bootstrap] require_tls`, default true |
| `SIGNET_BOOTSTRAP_TRUSTED_PROXY_CIDRS` | CSV override of `[bootstrap] trusted_proxy_cidrs`; default loopback v4/v6; invalid CIDRs fail validation |
| `SIGNET_DBUS_UNIX`            | Enable D-Bus Unix (`true`/`false`)       |
| `SIGNET_DBUS_TCP`             | Enable D-Bus TCP (`true`/`false`)        |
| `SIGNET_DBUS_TCP_PORT`        | D-Bus TCP port                           |
| `SIGNET_DBUS_TCP_HOST`        | D-Bus TCP bind host (default `127.0.0.1`) |
| `SIGNET_NIP5L`                | Enable NIP-5L (`true`/`false`)           |
| `SIGNET_NIP5L_SOCKET`         | NIP-5L socket path                       |
| `SIGNET_SSH_AGENT`            | Enable SSH agent (`true`/`false`)        |
| `SIGNET_SSH_AGENT_SOCKET`     | SSH agent socket path                    |
| `SIGNET_PASSKEYS_ENABLED` | Enable programmatic passkeys (default false) |
| `SIGNET_PASSKEYS_BACKEND` | Backend (`software-openssl`) |
| `SIGNET_PASSKEYS_AAGUID` | Authenticator AAGUID |
| `SIGNET_PASSKEYS_ATTESTATION` | Attestation mode (`none`) |
| `SIGNET_PASSKEYS_ALLOW_HEADLESS_UV` | Opt in to headless UV (default false) |
| `SIGNET_PASSKEYS_VIRTUAL_CTAP` | Experimental Linux UHID adapter (default false) |
| `SIGNET_PASSKEYS_SYNC_KEY` | Value-bearing fleet PSK; compatibility only, prefer file |
| `SIGNET_PASSKEYS_SYNC_KEY_FILE` | Mounted single-instance PSK file (64 hex chars) |

Master/backup file inputs require a regular file owned by the effective UID,
no group/world permission bits, no symlink, at most 4 KiB, and a nonempty
single-line value (one trailing LF or CRLF is stripped). A nonempty legacy
value plus nonempty file path is ambiguous and refused; an empty env value does
not conflict. Run local store commands as the file owner via the ops service.
Bunker/provisioner key decoding is separate. Never put secrets in argv.

## CLI (signetctl)

### Remote Management (via Nostr relay)

```bash
# Provision a new agent identity
signetctl provision my-agent

# Adopt an existing (BYO) keypair as an agent
signetctl adopt-existing my-agent --sec - --expected-pubkey <hex> < /secure/agent.nsec

# Restore custody for an existing agent (stdin, or --sec-from-store)
signetctl restore-existing my-agent --sec - < /secure/agent.nsec

# Assign a new, never-used writer client key; policy is JSON, not a key
signetctl writer-acquire my-agent <client-pubkey-hex>
signetctl set-policy my-agent '{"default":"deny","allow_methods":["sign_event"],"allow_kinds":[1]}'

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

# Persist and immediately activate provisioner authorization changes
signetctl grant-provisioner <pubkey>
signetctl revoke-provisioner <pubkey>

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

# Migrate a legacy plaintext SQLite DB to SQLCipher (requires a SQLCipher build)
SIGNET_DB_KEY_FILE=/run/secrets/signet_db_key signetctl migrate-db
```

Remote commands emit **Cascadia ContextVM** JSON-RPC 2.0 intents (kind 25910)
wrapped in **NIP-59 gift-wraps** (kind 1059) addressed to the bunker, and consume
the daemon's gift-wrapped ContextVM reply (validated by the bunker's pubkey and
correlated by the JSON-RPC `id`). The retired custom management-event transport
is neither emitted nor accepted.

Requirements:
- `SIGNET_PROVISIONER_NSEC_FILE` — a mounted file containing the provisioner key that signs/authorizes intents.
- The bunker's pubkey, so signetctl can address it: set `[nostr] bunker_pubkey`
  (npub or hex) in the config, or `SIGNET_BUNKER_PUBKEY` in the environment.

## Secret-store operator workflow

Credential payloads must be read from stdin or a regular owner-only file; never
put them on the command line. Lifecycle mutations use the encrypted ContextVM
management channel.

```bash
# Create or import an API token/credential, optionally with a Unix expiry time
signetctl create-credential <agent-id> --type api_token --label <label> [--policy-id <id>] [--expires-at <unix>] --stdin
signetctl import-credential <agent-id> --type credential --label <label> [--policy-id <id>] [--expires-at <unix>] --file /secure/owner-only-file

# List or inspect payload-free metadata
signetctl list-credentials [agent-id]
signetctl inspect-credential <credential-id>

# Deliver one credential through the same owner/capability/revocation/expiry
# checks used by D-Bus. The encrypted ContextVM reply is decoded directly into
# an atomic owner-only file; the payload is never printed.
signetctl deliver-credential <agent-id> <credential-id> --out /run/consumer/credential

# Replace the payload atomically; the prior version is archived
signetctl rotate-credential <credential-id> [--expires-at <unix>] --stdin

# Soft-revoke first; permanent deletion requires explicit confirmation
signetctl revoke-credential <credential-id>
signetctl delete-credential <credential-id> --confirm
```

Configure the Git helper globally, then select a credential by its stable id or
let the helper derive `git:<host>` from `SIGNET_AGENT_ID`. It serves HTTPS only;
`SIGNET_GIT_USERNAME` optionally overrides the username.

```bash
git config --global credential.helper '!signet-git-credential'
export SIGNET_GIT_CREDENTIAL_ID=<credential-id>
# Alternatively: export SIGNET_AGENT_ID=<agent-id>
```

Encrypted backup and offline restore use protected key files (legacy value env
inputs remain compatible), never argv. The launch route is online backup via
the ops service, gated on concurrent-writer consistency verification; see the
[runbook](deploy/README.md). Stop `signetd` before restore and wait ≥270 seconds
before restart; the previous database is retained as `<db>.pre-restore`.

```bash
export SIGNET_DB_KEY_FILE=/run/secrets/signet_db_key
export SIGNET_BACKUP_KEY_FILE=/run/secrets/signet_backup_key
signetctl backup-db /secure/signet-backup.db
signetctl restore-db /secure/signet-backup.db --confirm
```

## Policy Configuration

See [policies.conf.example](policies.conf.example) for the canonical example; `policies.toml.example` is deprecated. Policies are defined per-identity in GKeyFile format:

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

Provisioner authority follows **D2: config is desired state**. On first boot the
list seeds the store; on SIGHUP listed keys are granted and omitted keys revoked.
Runtime `grant-provisioner` / `revoke-provisioner` commits to the store before
visibility, then writes the effective list back to the INI. Mount the entire
`/etc/signet` directory rw, owned by UID/GID 10001 (0750), so temp-file + rename
write-back works. Edit the config list and reload, or use signed runtime
commands; do not mix changes without reloading.

A tombstone blocks a listed key only while the config mtime is no later than
the revocation time: editing a stale config after revocation can re-grant that
key if write-back had failed. `SIGNET_PROVISIONER_PUBKEYS` shadows the file,
cannot be written back and is treated as older than any revocation. Never set
it in production. Write-back failure is drift, not loss of current store
authority; the 0.8.0 contract exposes a critical log, metric and `/ready` detail
flag without forcing readiness false. See [plan §7.E](../docs/plans/signet-production-deployment-2026-11-06.md#7e-provisioner-authority-keep-d2-fix-adoption-make-write-back-a-deployment-invariant).

## NIP-46 Methods

| Method            | Params                                      | Description                         |
|-------------------|---------------------------------------------|-------------------------------------|
| `connect`         | `[signer_pubkey, connect_secret?]`          | Pair (one-time secret) or reconnect (bound clients need no secret) |
| `get_public_key`  | `[]`                                        | Return agent public key hex         |
| `sign_event`      | `[event_json]`                              | Policy check then sign under custody |
| `nip04_encrypt`   | `[peer_pubkey_hex, plaintext]`              | NIP-04 encrypt for a peer           |
| `nip04_decrypt`   | `[peer_pubkey_hex, ciphertext]`             | NIP-04 decrypt from a peer          |
| `nip44_encrypt`   | `[peer_pubkey_hex, plaintext]`              | NIP-44 v2 encrypt for a peer        |
| `nip44_decrypt`   | `[peer_pubkey_hex, ciphertext]`             | NIP-44 v2 decrypt from a peer       |
| `nip44_encrypt_b64` | `[peer_pubkey_hex, base64(plaintext)]`    | NIP-44 v2 encrypt of raw bytes      |
| `nip44_decrypt_b64` | `[peer_pubkey_hex, ciphertext]`           | NIP-44 v2 decrypt returning base64(plaintext) |
| `get_relays`      | `[]`                                        | Return relay map JSON               |
| `ping`            | `[]`                                        | Keepalive                           |

NIP-46 carries params as JSON strings, so a plaintext that is not valid UTF-8
cannot round-trip `nip44_encrypt` / `nip44_decrypt`: an encoder either rejects
it or substitutes U+FFFD. The `_b64` variants move the plaintext side over
standard base64 (the ciphertext stays an ordinary NIP-44 v2 payload either
way), so a caller can encrypt or recover raw binary without the key leaving
the bunker. They are needed by protocols whose payload width is itself a
format signal — Concord CORD-06 rekey blobs, for instance, are 72, 104, or 136
bytes and any other width is dropped as malformed. Both map to the
`nostr.encrypt` capability, so a policy already granting NIP-44 grants them;
policies pinning `allow_methods` explicitly must list them to enable the
binary-safe path. `nip44_decrypt` rejects a plaintext that is not NUL-free
UTF-8 with `invalid_plaintext` rather than truncating it.

`switch_relays` replies with the relays Signet is configured to listen on (the
relay set is operator configuration, so it never returns `null`). `logout`
replies `"ack"` and then removes the client's binding; the client must
`connect` with a fresh connect secret to pair again. Both need an existing
binding but not a method grant, so a policy that pins `allow_methods` does
not have to list them.

For an identity fenced by `agent/writer-acquire`, `sign_event` and the four
NIP-44 methods succeed only for the assigned owner client, and NIP-04 is
refused; see [docs/WRITER_EPOCH_CUTOVER.md](docs/WRITER_EPOCH_CUTOVER.md).

## D-Bus Interface

Signet exposes two D-Bus interfaces (see `dbus/net.signet.Signer.xml`):

**net.signet.Signer** — Nostr signing operations:
- `GetPublicKey() → pubkey_hex`
- `SignEvent(event_json) → signed_event_json`
- `Encrypt(plaintext, peer_pubkey, algorithm) → ciphertext`
- `Decrypt(ciphertext, peer_pubkey, algorithm) → plaintext`

**net.signet.Credentials** — Credential lease operations:
- `GetSession(service_url) → (session_token, expires_at)` — refuses to mint tokens when no credential store is configured
- `GetToken(credential_id) → (token, expires_at)` — enforces per-agent ownership; agents can only retrieve their own credentials
- `ListCredentials() → credential_ids[]`

Both calls require their normal capabilities. Plaintext D-Bus TCP also refuses
these two calls with `net.signet.Error.CredentialsOverTcpDisabled` by default.
`[dbus] tcp_allow_credentials = true` lifts only the transport restriction and
logs a startup warning; it does not grant either capability. Unix D-Bus is
unchanged. Authentication and signing remain available over TCP under their
existing rules. Use Unix D-Bus where possible; keep TCP on loopback or protect
it with an encrypted tunnel. To bind directly to a LAN interface, set `[dbus]
tcp_host` (or `SIGNET_DBUS_TCP_HOST`) explicitly. Both new settings require a
restart; SIGHUP does not change them.

For Unix D-Bus, enable `[dbus] unix_enabled` and map the client's OS UID to
its provisioned agent ID in `[uid_map]`. As a provisioner, add only the
credential methods that agent needs to its policy's `allow_methods`:

```bash
signetctl set-policy my-agent '{"default":"deny","allow_methods":["credential.get_token","credential.get_session"]}'
```

`set-policy` replaces the identity policy: preserve any other intended rules.
A wildcard or default-allow alone does not grant either operation. To revoke a
grant, remove its literal entry and submit the updated policy. The running
daemon applies management updates directly; an out-of-process update to the
policy file takes effect on `SIGHUP`. See [the integration guide](INTEGRATION.md#d-bus-credential-grants).

**net.signet.Passkeys** — Programmatic WebAuthn (requires a passkey-enabled
build and `[passkeys] enabled = true`). Map the Unix caller to its provisioned
agent as above, then grant only the methods it needs, for example:

```bash
signetctl set-policy my-agent '{"default":"deny","allow_methods":["passkey.get_info","passkey.make_credential","passkey.get_assertion"]}'
```

Export and import are separate opt-in grants (`passkey.export` and
`passkey.import`); don't give them to an agent that only performs WebAuthn
ceremonies. Wildcard/default-allow policies don't grant passkey methods, and the
deny list takes precedence. To revoke a policy-store grant, remove it from the
replacement policy while keeping the other intended rules. See [Passkeys /
WebAuthn](INTEGRATION.md#passkeys--webauthn-programmatic-api) for the full mapping.

## Health Endpoint

```bash
curl --fail http://127.0.0.1:8080/health
curl --fail http://127.0.0.1:8080/ready
curl --fail http://127.0.0.1:8080/metrics
```

`/health` is process liveness with a JSON component snapshot (which can be
degraded); a relay outage must not cause container recreation. `/ready` returns
200 only when DB, key store, policy store, relay and fleet catch-up state are
ready, otherwise 503. `/metrics` is unauthenticated Prometheus text. These are
on-box loopback endpoints, never public proxy routes; inspect readiness rather
than treating HTTP liveness as enrollment or signing proof.

## Memory-locking policy

`SIGNET_MLOCK_MODE` is read before configuration/secret loading, durable boot
admission or any listener starts. Values are case-insensitive; unset/empty means
`best-effort`. Unknown values (including the unmerged prototype's `future` and
`current`) refuse startup. SIGHUP does not change this process-wide policy.

| Mode | Startup decision |
|---|---|
| `off` | Do not call process-wide locking/rlimit syscalls. Guarded sodium allocations still attempt their own locks. |
| `best-effort` | With an unlimited soft memlock limit, try `MCL_CURRENT\|MCL_FUTURE`, falling back to `MCL_CURRENT`. With a finite or unreadable limit, try only `MCL_CURRENT`. Log degraded coverage or failure and continue. |
| `required` | Require an unlimited soft `RLIMIT_MEMLOCK` and successful `MCL_CURRENT\|MCL_FUTURE`. Otherwise exit 1 before secrets load; no current-only fallback. |

The daemon may raise the soft limit to the **existing hard ceiling**, never raise
that ceiling or add capabilities. Required mode deliberately refuses finite
limits even when `CAP_IPC_LOCK` could bypass them. There is no safe fixed byte
budget for future mappings: with `MCL_FUTURE`, later allocations and stack growth
can fail after a finite limit is exhausted ([mlockall(2)](https://man7.org/linux/man-pages/man2/mlockall.2.html)).
Best-effort current-only locking is a startup snapshot, not protection for later
secrets; new mappings and transient copies may be swappable. Sodium locking is
also best-effort. None of these modes protects against live root/same-UID attacks,
host hibernation, or guarantees that every transient secret copy is never swapped.
On macOS, unsupported/denied `mlockall` calls degrade in best-effort and refuse
startup in required; allocator tuning is a no-op.

Production Compose explicitly selects `required` and already sets unlimited
memlock with all capabilities dropped. Docker's small default memlock allowance
is insufficient: use `--ulimit memlock=-1:-1`, not a new `IPC_LOCK` capability.
A native systemd service can use this drop-in (the shipped Compose supervisor is
not the daemon; Docker's container limits must still be set in Compose):

```ini
[Service]
Environment=SIGNET_MLOCK_MODE=required
LimitMEMLOCK=infinity
LimitCORE=0
```

Verify the effective container/process limits and Linux `VmLck` after startup.
An unlimited **locking** limit is not an unlimited cgroup memory allowance:
the existing 512 MiB Compose cap still applies and exhaustion can cause OOM kills.
On glibc/Linux we cap allocator arenas at two early in startup to reduce retained
mapped memory. This is advisory, not a memory quota or proof of security. We do
not force periodic `malloc_trim()` on signing requests, tune other allocators,
or enlarge the container budget to hide high-water memory usage.

## Testing & CI

Discover and execute the tests registered by your actual build, rather than
relying on fixed test counts:

```bash
ctest --test-dir build-signet-prod/signet/tests -N
ctest --test-dir build-signet-prod/signet/tests --output-on-failure
meson test -C build-signet-meson-sqlcipher --list
meson test -C build-signet-meson-sqlcipher --print-errorlogs
```

See [TESTING](../docs/TESTING.md#signet-packaging-and-live-acceptance) for build,
packaging and live entry points. SQLCipher skips in a plain-SQLite build are not
encrypted-store proof; helper tests are not deployed enrollment acceptance.

## Security Model

The authority for these guarantees and exclusions is [THREAT_MODEL.md](docs/THREAT_MODEL.md).

- Normal remote signing does not export agent nsecs; BYO custody, backup recovery and explicit credential delivery are distinct boundaries. Bunker URIs contain secrets.
- Guarded allocations and wiping reduce memory exposure. Process locking is configurable: best-effort by default, fail-closed at startup with `SIGNET_MLOCK_MODE=required` (production Compose default); neither is a blanket never-swapped guarantee.
- SQLCipher provides AES-256 encryption at rest and is verified at startup with `PRAGMA cipher_version` plus a keyed read; with `SIGNET_REQUIRE_ENCRYPTED_DB=true`, verification failure stops the daemon
- Credential payloads are envelope-encrypted with per-agent libsodium secretbox keys derived via BLAKE2b from the store DEK and agent pubkey
- Linux dumpability hardening is warn-only; enforce and verify deployment core limits.
- Gift-wrapped management encrypts request/reply content; direct 25910 ingress is not confidential. Encryption failure does not authorize plaintext reply fallback.
- Relay publish reports an error when zero relays are connected instead of pretending success
- Bootstrap tokens are single-use, time-limited, attempt-capped, and stored as SHA256 hashes; `POST /bootstrap` consumes them atomically and replay returns 403
- Revocation resolves the agent pubkey, adds it to the deny list, burns leases, revokes the agent's persistent client bindings, wipes the key from cache and store, and records an audit entry
- NIP-46 pairing is atomic (secret consumption + durable client binding in one transaction); bindings are pinned to the agent identity pubkey at pairing time, so rotation or reprovisioning never resurrects old client authority
- The daemon's live deny list is enforced on every NIP-46 path (pairing, reconnect, per-request) before policy evaluation — suspension takes effect immediately
- Management replay caches protect delivered ids during one process lifetime, not logical mutations across restarts/new wraps/new signatures. Observe the ≥270-second deliberate-stop rule and do not blindly retry timed-out mutations.
- One custody pubkey cannot be bound to two agents (partial unique index + non-destructive upsert).
- Credential rotation archives and updates secrets in a single SQLite transaction
- The database audit chain detects covered-record edits, not whole-history replacement without an external checkpoint; the generic logger has no field allowlist, so redaction remains a call-site responsibility.
- Runs as non-root `signet` user in production

## License

MIT
