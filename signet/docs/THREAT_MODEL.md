# Signet — Threat Model

Status: deployment scope reconciled 2026-11-06 for the 0.8.0 candidate; this is
not a completed deployment audit or release attestation. The original store
analysis (2026-08-04) and key hierarchy remain applicable.
Scope: encrypted custody (`store.c`, `store_secrets.c`), backup/restore,
NIP-46 and ContextVM relay transport, authorization, process/host custody and
optional HTTP/passkey surfaces. The launch layout is one Linux/amd64 daemon,
one SQLCipher database, outbound relays and loopback observability. HTTP
bootstrap ships packaged but disabled; MCP is a stub, not shipped or in this
release's audit surface. See the [deployment plan](../../docs/plans/signet-production-deployment-2026-11-06.md#8-w2--relay-correctness-and-security-audit)
and [runbook](../deploy/README.md). W1/W2 corrections specified there require
entrypoint and deployed-image verification; references to their 0.8.0 contract
below are not evidence that those checks ran.

## 1. Assets

| Asset | Where it lives |
|---|---|
| Agent signing keys (nostr nsec) | `agents.secret_key` (envelope-encrypted blob) |
| Generic credentials (ssh_key, api_token, credential, certificate) | `secrets.payload` + archived `secret_versions.payload` (envelope-encrypted blobs) |
| Master key (`SIGNET_DB_KEY_FILE`, legacy `SIGNET_DB_KEY`) | Protected operator file / process memory; legacy environment is not wipeable |
| Derived DEK | `SignetStore.dek`, mlock'd process memory only |
| Backup key (`SIGNET_BACKUP_KEY_FILE`, legacy `SIGNET_BACKUP_KEY`) | Independent protected operator file; escrow separate from backup objects |
| Metadata (ids, labels, owners, policies, expiry, provenance) | Cleartext *inside* the SQLCipher envelope; payload-free by design |
| Audit log | Database `audit_log` chain for covered operations; separate unchained JSON operational stream |
| Bunker identity / provisioner nsec | Separate protected files; provisioner stays on operator workstation |
| Pending connect secrets / bindings | SQLCipher-only protection for `connect_secret` TEXT; durable identity-pinned bindings |
| Passkey fleet PSK | Mounted single-instance file; wraps portable credential containers |
| Policies / release bundle | Mutable `/data/policies.conf`, rw `/etc/signet` desired authority; image digests and config identity |

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
- **A7 — Malicious/compromised relay**: observes metadata, withholds, rejects,
  reorders or replays events, sends `CLOSED`, or overlaps AUTH and publish OKs.
  NIP-44/gift-wrap protects content, not relay availability or traffic metadata.
  Relay acceptance is not recipient receipt; enqueue alone is neither.
- **A8 — Authorized malicious NIP-46 client**: requests disallowed kinds,
  methods or another identity; binding, deny precedence, policy and writer
  custody must all hold. A policy-authorized operation is not human approval.
- **A9 — Unauthorized network sender**: probes management and floods refusals;
  authentication/authorization, bounded replay caches and refusal budgets limit
  access, not all relay/network denial of service.
- **A10 — Compromised delivery-grant requester**: can obtain the granting
  owner's credentials and scoped metadata, not general provisioner/signing
  authority. Owner/requester deny entries override the grant.
- **A11 — Listener/proxy-header spoofing**: accidental wildcard listeners or
  forged forwarding headers defeat assumed exposure boundaries. The 0.8.0
  contract binds numeric loopback addresses and trusts one `https` header only
  from a configured trusted peer. Under host networking, loopback trust
  separates **on-box from off-box only**: any local process can forge that
  header. This limitation is accepted; HTTP stays disabled at launch.
- **A12 — Image/config downgrade and operator secret mistakes**: wrong image,
  plaintext build, stale policy/config, argv/env leakage or writable authority
  misconfiguration. Digest pinning, provenance, encrypted-mode refusal,
  protected files and operator review are deployment controls, not defenses
  against a live administrator intentionally running an old binary.
- **A13 — Restart replay / lost replies**: RAM caches do not survive a restart;
  a committed mutation can outlive its reply. See §3.4 for the stop rule and
  retry boundary.
- **A14 — Optional passkeys**: theft of the fleet PSK plus exported containers
  compromises those credentials. Headless UP is asserted after policy, not a
  physical touch; UV-required requests default-deny unless explicitly opted in.
  Launch uses one instance/PSK, no cross-instance sync. UHID is experimental,
  local Linux-host functionality, not a remote browser authenticator.

Out of scope (accepted):

- Root or same-UID attacker on the live host while the daemon runs: such an
  attacker can read `/proc/<pid>/environ`, ptrace the process, or simply use
  enabled local daemon surfaces. No at-rest design defends against this.
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
- The 0.8.0 file-key contract adds `SIGNET_DB_KEY_FILE` /
  `SIGNET_BACKUP_KEY_FILE`; legacy value-bearing env inputs remain compatible.
  A nonempty value plus nonempty path is refused. Files must be regular,
  effective-UID-owned, owner-only, non-symlinks, ≤4 KiB and nonempty single-line
  (one final LF/CRLF stripped). BYO CLI keys use stdin (`--sec -`), never argv.
  File delivery avoids an environment copy, not live-process compromise.

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
  Without an independently retained checkpoint, whole-history replacement or
  truncation to an older valid chain is not detected. The JSON logger is not
  chained and has no generic field allowlist; redaction is a call-site duty.
  Coverage of credential operations is not proof that every signing/management
  operation appears in the database chain.

### 3.4 Management-plane interactions (A4)

Mutations reach the store only through the ContextVM management handler:

- **Confidentiality is transport-dependent**: the supported gift-wrapped
  kind-1059 route encrypts content and authenticates the seal author. Direct
  kind-25910 ingress can parse readable JSON-RPC content; authorization does
  not make that content confidential. Do not send secrets over direct ingress.
  `test_plaintext_mutation_rejected` covers a particular rejected input path,
  not universal rejection of plaintext management mutations. Reply encryption
  failures must not fall back to plaintext.
- **Replay**: with the replay cache attached, a delivered event id executes at
  most once; duplicates are dropped, missing ids fail closed
  (`test_credential_mutation_replay_rejected`, `test_mgmt_replay.c`). The
  request's own `created_at` (the rumor's, authenticated by the seal) must be
  within the skew window and ids are kept for twice the window; each sender
  class (provisioners, self-service agents, delivery-grant requesters) has
  its own cache, non-provisioner ids are charged to their sender, and a full
  sender or cache is refused rather than evicting, so flooding cannot make a
  seen request execute again (`test_replay_flood_cannot_reexecute`,
  `test_enabled_replay_classes`, `test_gift_wrap_uses_rumor_time`,
  `test_replay_cache.c`). Residual: colluding authorized senders of one class
  can fill that class's cache and have its requests refused until their ids
  expire (2 * skew); they cannot evict another class's cached ids. Caches are
  RAM-only and keyed by delivered event id, not durable logical-operation id.
  Rewrapping or re-signing can repeat a logical mutation; JSON-RPC ids only
  correlate replies. On restart the daemon resubscribes up to 49 hours back.
  A request processed shortly before stopping can execute again after a short
  restart while its authenticated timestamp still passes the 120-second skew.
  **Any deliberate stop must keep the daemon down ≥150 seconds before restart**
  (default skew + margin); online daily backup avoids a daily restart. Abrupt
  restart/crash remains a replay risk, and changing skew requires reassessing
  this rule. It is not durable exactly-once protection.
- **Lost replies**: enqueue success, relay ACK and recipient receipt differ.
  Rejection/no ACK may republish an already-signed reply to another relay, not
  re-sign or re-execute the request (the W2 contract). A client timeout does not
  prove a mutation failed; inspect state before deciding on a new request,
  never blindly re-sign/retry a mutation.
- **Refusal cost**: every non-provisioner sender is rate-limited before any
  refusal, audit, inner decryption or store read (the gift wrap is opened
  first, to authenticate it), and refusals of non-provisioner senders are
  audited within a global budget with an aggregate count beyond it, so no
  number of keys turns refusals into unbounded audit rows
  (`test_refusals_cannot_amplify_audit`).
- **Authorization**: most methods require provisioner authority. Self-service
  `agent/reissue-connect` accepts the target identity's signed sender (not merely
  its NIP-46 client), and `credential/inspect|list|deliver` accepts requests from a requester the owner's
  policy grants exactly `credential.deliver`. That grant is checked before the
  replay mark, is scoped to the granting owner's credentials, and is refused
  identically (byte for byte) for a missing credential and one the requester
  is not granted, including another owner's credential under a held grant (no
  existence oracle); a stolen requester key yields only those credentials and
  no management authority. The grant is one exact policy shape: any other
  rule or an unreadable entry voids it. Non-provisioner senders are
  rate-limited per sender before inner decryption, store reads or audit. Retrieval goes through the single
  `signet_credential_access_acquire` path (deny-list, for owner and requester →
  capability → rate limit → owner/status/expected version before decrypt →
  type rules → lease burn → mandatory audit).

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

- **Legacy value-bearing environment keys** cannot be reliably wiped and
  remain visible to same-UID/root observers (out of scope). The 0.8.0 file-key
  resolver uses caller-owned guarded buffers and wipes them on release; it
  cannot guarantee removal of every library/internal or transient copy.
- `sodium_malloc` memory locking and daemon `mlockall` are best-effort;
  `mlockall` failure warns and continues. These do not justify “never swapped”.
  Verify memlock limits and actual locked memory on the deployed host.
- SQLCipher's internal key schedule and page cache are managed by SQLCipher;
  it wipes its own key material on close but this is outside Signet's control.
- On Linux, `signetd` calls `prctl(PR_SET_DUMPABLE, 0)` at startup, but
  continues with a warning if that call fails. There is no global
  `madvise(MADV_DONTDUMP)` or non-Linux equivalent; deployments should still
  disable core dumps for signetd (Compose `ulimits.core: 0`, or systemd
  `LimitCORE=0`) and verify effective limits. At-rest encryption is not host
  custody: normal signing does not export nsecs, but imports, operator-held
  copies, backup recovery and explicit credential delivery are separate paths.
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

- The SQLCipher database backup contains current/archived secret rows,
  agents, leases, deny state and database audit records. It does **not** replace
  backup of the mutable file-backed policy, INI/deployment config, release
  manifest and separate `audit.jsonl`, nor separate master/bunker/PSK escrow.
  The database roundtrip test is not a fresh-host recovery drill.
- Launch daily backup is online through the isolated ops service, contingent
  on the concurrent-writer consistency test. If that gate fails, adopt offline
  backup with the ≥150-second stop rule. Restore is always offline; targets
  RPO 24 h / RTO 60 min require an actual restore drill, not source inspection.
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
| Specific plaintext-input refusal (not universal direct-ingress confidentiality) | `test_credential_lifecycle.c:test_plaintext_mutation_rejected` |
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
