# Signet Secret Store — Threat Model

Status: current as of 2026-08-04 (fp-signet-secret-store-completion Item 4).
Scope: the encrypted secret store (`store.c`, `store_secrets.c`), its key
material, backup/restore, and the memory-hygiene guarantees around secret
payloads. Transport/authorization threat surfaces (ContextVM management,
capability/lease enforcement, audit chain) are summarized where they interact
with the store; their detailed enforcement lives in `credential_access.c` and
`mgmt_protocol.c`.

## 1. Assets

| Asset | Where it lives |
|---|---|
| Agent signing keys (nostr nsec) | `agents.secret_key` (envelope-encrypted blob) |
| Generic credentials (ssh_key, api_token, credential, certificate) | `secrets.payload` + archived `secret_versions.payload` (envelope-encrypted blobs) |
| Master key (`SIGNET_DB_KEY`) | Operator environment / config; never persisted by Signet |
| Derived DEK | `SignetStore.dek`, mlock'd process memory only |
| Backup key (`SIGNET_BACKUP_KEY`) | Operator environment; never persisted |
| Metadata (ids, labels, owners, policies, expiry, provenance) | Cleartext *inside* the SQLCipher envelope; payload-free by design |
| Audit log | `audit_log`, hash-chained, never contains payloads |

## 2. Adversary model

In scope:

- **A1 — Offline disk theft**: attacker obtains the database file (and/or WAL
  sidecars) without any key.
- **A2 — Backup exfiltration**: attacker obtains a backup file, possibly with
  the backup key (backups are the artifact most likely to leave the host).
- **A3 — Tampering**: attacker modifies the database file or individual
  ciphertext blobs and hopes Signet serves altered or garbage secrets.
- **A4 — Network adversary on the management plane**: submits plaintext,
  replayed, or unauthorized mutation requests over Nostr/ContextVM.
- **A5 — Wrong-key / downgrade operation**: operator or attacker opens the
  store with a wrong key or tries to run it unencrypted.
- **A6 — Post-exit memory disclosure**: swap, core dumps, freed-memory reuse.

Out of scope (accepted):

- Root or same-UID attacker on the live host while the daemon runs: such an
  attacker can read `/proc/<pid>/environ`, ptrace the process, or simply use
  the daemon's own D-Bus surface. No at-rest design defends against this.
- Hardware/side-channel attacks; malicious SQLCipher/libsodium builds.

## 3. Encryption layers and key hierarchy

```
SIGNET_DB_KEY (master key: hex-64 / base64 / raw; >= 32 bytes key material)
  ├─(passed as passphrase)─► SQLCipher layer: whole-file AES-256-CBC, per-page
  │                      HMAC. `PRAGMA key = '<text>'` treats the string as a
  │                      passphrase run through SQLCipher's internal PBKDF2 —
  │                      not as a raw key blob — while the envelope DEK
  │                      canonical-decodes hex/base64 first. The two layers
  │                      therefore consume the master string differently by
  │                      construction.
  └─(BLAKE2b, domain "signet-agent-nsec-v1")──► DEK (32 B, mlock'd)
        └─(BLAKE2b keyed by DEK, msg = agent_pubkey)──► per-agent key
              └─ XSalsa20-Poly1305 secretbox per record, fresh 24-byte
                 random nonce per encryption (agents.secret_key,
                 secrets.payload, secret_versions.payload)

SIGNET_BACKUP_KEY (independent; >= 32 bytes key material, weak keys refused)
  └──► SQLCipher layer of the backup file ONLY (signet_store_backup)
```

### 3.1 Both layers derive from the same master secret — justification

The SQLCipher key is the master key itself; the envelope DEK is a one-way,
domain-separated derivation of the same master. This is deliberate, not an
oversight:

1. **The two keys sit at the same trust boundary.** Both would be supplied to
   the same process, from the same environment/config, unlocked at the same
   time. Splitting them into two independent operator-managed secrets does not
   move that boundary: any attacker who can read one env var can read two.
   It would only double the key-custody burden and the ways to lose data.
2. **Domain separation already guarantees distinct key bits.** The DEK is
   `BLAKE2b(key = master material, msg = "signet-agent-nsec-v1")`; knowledge
   of DEK bits does not reveal the master (one-way), and no protocol ever uses
   the same bits in both layers.
3. **The envelope layer's purpose is not key independence — it is
   defense-in-depth against SQLCipher *bypass*.** Precisely: it protects
   payloads in failure modes that disclose database pages **without
   disclosing the master secret itself** — the build links plain SQLite
   (SQLCipher absent), plaintext WAL/journal sidecars leak, a legacy
   `.plaintext-backup` file from migration lingers, page-level data is
   disclosed (backup tooling, FS snapshots), or an
   authorized-but-metadata-only reader gets SQL access. In every one of those
   cases the master key was never disclosed, so the derived DEK holds. It
   deliberately does NOT protect against disclosures of the master secret
   (SQLCipher key-schedule memory extraction, process compromise, env-var
   leak): those defeat both layers, and no derived-key scheme could prevent
   that.
4. **Re-keying cost.** Separating the layers now would re-key every deployed
   database (both layers), a high-risk migration with no boundary gain.

**Where key separation genuinely buys something, Signet has it**: the backup
key is independent, because backups are the artifact designed to leave the
host (§5). Rotation of the master key is intentionally a manual, offline
procedure (export/re-import) — it cannot silently diverge from the DEK.

Consequence, stated plainly: **compromise of `SIGNET_DB_KEY` compromises both
layers.** That is inherent to a single-root-secret design and would equally be
true of two keys stored side by side.

### 3.2 Wrong key / downgrade behavior (A5)

- Wrong master key → the first keyed read fails and `signet_store_open`
  refuses (no second divergent DB is ever created). Covered by
  `test_backup_restore.c:test_wrong_keys_fail_closed`.
- Plain-SQLite build opening an encrypted DB → keyed read fails, refused.
- SQLCipher build finding a legacy plaintext DB → refuses to serve it as-is;
  migrates it to SQLCipher (crash-safe swap) unless
  `SIGNET_MIGRATE_PLAINTEXT_DB=false`.
- Operators can hard-fail unencrypted operation with
  `SIGNET_REQUIRE_ENCRYPTED_DB=true`; otherwise a plaintext-at-rest store is
  loudly warned about and the envelope layer remains the only at-rest
  protection.
- Keys never appear on argv: `SIGNET_DB_KEY`/`SIGNET_BACKUP_KEY` are
  environment-only; payload input is stdin/protected-file only.

### 3.3 Tamper behavior (A3)

- File-level tamper: SQLCipher per-page HMAC makes the keyed read fail; the
  store refuses to open (`test_file_tamper_rejected`).
- Record-level tamper (an attacker with SQL-level write access flips
  ciphertext bytes): secretbox Poly1305 authentication fails and retrieval
  returns an error, never attacker-controlled plaintext
  (`test_envelope_tamper_rejected`).
- Audit history tamper: `audit_log` is hash-chained; `signetctl verify-audit`
  / `signet_audit_verify_chain` detect edits, and chain appends are atomic
  (connection mutex + `BEGIN IMMEDIATE`) so concurrent writers cannot fork it.

### 3.4 Management-plane interactions (A4)

Mutations reach the store only through the ContextVM management handler:

- **Plaintext rejection**: request content MUST decrypt as NIP-44 v2 from the
  authenticated sender; plaintext fallback is rejected before parsing
  (`test_plaintext_mutation_rejected`).
- **Replay**: with the replay cache attached, a delivered event id executes at
  most once; duplicates are dropped, missing ids fail closed
  (`test_credential_mutation_replay_rejected`, `test_mgmt_replay.c`).
- **Authorization**: non-provisioner senders are dropped (and chain-audited);
  retrieval goes through the single `signet_credential_access_acquire` path
  (deny-list → capability → rate limit → owner/status before decrypt → type
  rules → lease burn → mandatory audit).

## 4. Memory hygiene (mlock / zeroization inventory)

Verified against the current code:

| Material | Protection |
|---|---|
| DEK (`SignetStore.dek`) | `sodium_mlock` at open; `sodium_memzero` + `munlock` at close and on every early-exit path in `signet_store_open` |
| Key-derivation intermediates (`ikm`, base64 scratch in `signet_derive_dek`; entropy probes) | `sodium_memzero` on all paths |
| Per-agent envelope keys (`akey`) | stack; `sodium_memzero` after every encrypt/decrypt, including error paths |
| Decrypted payloads (`SignetSecretRecord.payload`, agent secret keys) | `sodium_malloc` guarded allocations (mlock'd, canary, auto-wipe on `sodium_free`); `signet_secret_record_clear` wipes |
| SQL strings embedding keys (`PRAGMA key`, `ATTACH ... KEY`) | `sodium_memzero` before `sqlite3_free` in open, migrate, backup, restore, and verification helpers |
| Envelope sample buffers in restore's master-key check | wiped after authentication (plaintext, per-agent key, derived DEK probe) |
| NIP-44 plaintexts in the mgmt handler (may hold nsec/payloads) | `secure_wipe` before `free` on every exit path; sender/bunker `sk` buffers memzero'd |
| Ciphertext + nonce scratch in `store_secrets.c` | memzero'd after insert |

Known residual gaps (accepted, documented):

- **The master key string itself** is read from the process environment.
  `getenv` memory cannot be reliably wiped and remains readable at
  `/proc/<pid>/environ` for same-UID/root observers (out-of-scope adversary).
  Hardening option for the future: file-based / systemd-credential key
  delivery.
- SQLCipher's internal key schedule and page cache are managed by SQLCipher;
  it wipes its own key material on close but this is outside Signet's control.
- No global `madvise(MADV_DONTDUMP)` / rlimit-core suppression; deployments
  should disable core dumps for signetd (systemd `LimitCORE=0`).
- The ≥ 32-byte key rule is a **length floor, not an entropy measurement**: a
  long low-entropy string passes it. Operators must generate
  `SIGNET_DB_KEY`/`SIGNET_BACKUP_KEY` from a CSPRNG (e.g.
  `openssl rand -hex 32`); passphrase-style keys get only SQLCipher's PBKDF2
  stretching on the outer layer.
- Backup/restore create their files with `O_EXCL|O_NOFOLLOW` and mode 0600,
  but do not do full fd-relative (`openat`) path handling; the store/backup
  directories are assumed operator-owned (0700) per the adversary model — a
  same-UID writer in those directories is out of scope.

## 5. Backup & restore (A2)

`signet_store_backup` / `signet_store_restore_backup`
(`signetctl backup-db` / `restore-db`):

- The backup is a complete point-in-time SQLCipher database containing
  **current and archived** secrets (`secret_versions`), agents, leases,
  policies, deny list, and the audit chain — restart-durable by construction
  and proven by `test_backup_restore_roundtrip`.
- It is keyed by an **independent** `SIGNET_BACKUP_KEY` (≥ 32 bytes of key
  material enforced; weak keys refused). The master key never leaves the host
  with the backup.
- **Two keys are required to read payloads from a backup**: the backup key
  opens the SQLCipher envelope of the file, but every payload blob inside is
  still secretbox-encrypted under the master-derived DEK. A stolen backup +
  backup key yields metadata only. Consequently, restore requires the same
  master key that was active when the backup was taken (rotation of the
  master key invalidates old backups' payloads — archive accordingly).
- **Wrong-master restore is detected, not discovered later**: restore
  authenticates one envelope blob from inside the backup (agent custody blob
  under the DEK, or credential blob under the per-agent key) against the
  supplied master key and refuses on mismatch — a wrong-but-strong master key
  can re-key the SQLCipher layer but cannot forge Poly1305 authentication of
  the envelope. Only a backup containing no envelope blobs at all is exempt.
- **Restore cannot race a running daemon or discard WAL state**: when the
  existing database opens with the master key, restore takes an exclusive
  lock (refusing if signetd holds the store) and folds its WAL into the
  `.pre-restore` copy; when it does not (lost-key recovery), any `-wal`
  sidecar is preserved verbatim next to the copy.
- **File hygiene**: backup targets and restore working files are created
  atomically (`O_EXCL|O_NOFOLLOW`, mode 0600) — existing files, symlinks, and
  path aliases of the live database are refused race-free; verification
  includes a full `PRAGMA integrity_check` (every page HMAC-verified);
  `DETACH` and `fsync` failures fail the operation rather than reporting an
  unverified artifact as durable.
- A backup that cannot be produced encrypted (plain-SQLite build) is refused
  — never a silent plaintext copy. The written file is verified (opens with
  the backup key, SQLCipher active, core tables readable, no plaintext SQLite
  header) and fsync'd before success; failures never leave partial files.
- Restore is offline and crash-safe: exported to `<db>.restoring`, verified
  with the master key, fsync'd, previous DB preserved as `<db>.pre-restore`
  (still encrypted), stale `-wal`/`-shm` sidecars removed, then an atomic
  `rename()`. Any failure leaves the original untouched.
- Existing backup targets are never overwritten.

## 6. Test coverage map (Item 4 matrix)

| Property | Test |
|---|---|
| Create/read/list without payload disclosure | `test_credential_lifecycle.c`, `test_signetctl_credentials.sh`, `test_credential_access.c` (redaction) |
| Wrong agent / missing capability / deny precedence / lease burn / rate limit | `test_credential_access.c` |
| Plaintext mutation rejected | `test_credential_lifecycle.c:test_plaintext_mutation_rejected` |
| Replay (mgmt + credential mutations) | `test_mgmt_replay.c`, `test_credential_lifecycle.c:test_credential_mutation_replay_rejected`, `test_replay_cache.c` |
| Duplicate/overwrite refusal | `test_credential_lifecycle.c` (deterministic-id refusal) |
| Rotation + archived versions | `test_credential_lifecycle.c`, `test_backup_restore.c` |
| Expiry / revoke | `test_credential_lifecycle.c`, `test_credential_access.c` |
| Restart durability | `test_backup_restore.c:test_restart_durability` |
| Backup/restore (incl. archived secrets, point-in-time, restore-over-existing) | `test_backup_restore.c:test_backup_restore_roundtrip` |
| Wrong DB key / wrong backup key / wrong-master restore / alias refusal | `test_backup_restore.c:test_wrong_keys_fail_closed` |
| File tamper / envelope tamper | `test_backup_restore.c:test_file_tamper_rejected`, `:test_envelope_tamper_rejected` |
| Backup refusals (weak key, overwrite, read-only handle) + 0600 perms | `test_backup_restore.c:test_backup_refusals`, `:test_backup_restore_roundtrip` |
| Audit chain integrity (incl. concurrent appends) | `test_store_lifecycle.c`, `test_credential_access.c`, `test_backup_restore.c` (post-restore) |
| CLI exit codes | `test_signetctl_credentials.sh` |
| Concurrency (flood bounds, signing + retrieval load) | `test_item3_concurrency.c` |
| Git credential canary | `test_git_credential.c` |
| Plaintext→SQLCipher migration | `test_db_migration.c` |

SQLCipher-dependent cases skip on plain-SQLite builds and run in the
SQLCipher-configured build (`-Dsignet_use_sqlcipher=true`).
