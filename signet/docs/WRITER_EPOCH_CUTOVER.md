# Signet writer epochs for a same-pubkey service cutover

Signet can **adopt** an existing service identity without changing its Nostr
pubkey. The secret is delivered only through Signet's existing encrypted
`agent/adopt-existing` management path; it must not be logged, filed in an
issue, pasted into a command line, or requested by a client. Compare the
returned `pubkey` with the service's established pubkey before proceeding.

## Supported fence topology

One active Signet daemon and one durable SQLCipher database are supported.
The `agent_writer_leases` row and `agents.writer_fenced` marker live in that
database. The lease also records the highest wall-clock time observed by
acquire, renew, or sign. A backward clock step fails closed, including across
restart, until time catches up; administrative revoke remains available. This
detects observed rollback, not arbitrary clock tampering between operations. The marker never resets for the lifetime of an agent row. Epochs
increase on every acquisition/transfer and revocation; renewals preserve the
epoch. A DB writer transaction encloses validation, key decryption, signature
creation, and commit. The signature is not returned if commit fails. A corrupt
or missing lease row for an already-fenced agent fails closed, including after
restart. The database must use durable storage; Signet sets
`PRAGMA synchronous=FULL` in WAL mode for lease commits.

**Active-active Signet daemons against separate database copies, restoring an
old DB snapshot, and any old daemon binary that ignores this fence are not
supported.** A copied DB can rewind the epoch and cannot participate in the
SQLite transaction. Stop all old binaries and prove only the intended daemon
owns the key before acquiring the first lease. Do not restore an old backup
over the live DB; recovery must preserve the latest lease row and fenced marker
or use an operator-controlled new epoch after reconciling all copies.

## Authenticated protocol

Management operations are existing provisioner-authorized, encrypted
ContextVM kind-25910 intents. Their params are:

| Method | Params | Result |
| --- | --- | --- |
| `agent/writer-acquire` | `agent_id`, `writer_pubkey` (NIP-46 client pubkey, 64 hex), `ttl_seconds` (1–3600) | `epoch`, `expires_at` |
| `agent/writer-renew` | `agent_id`, `writer_pubkey`, `epoch`, `ttl_seconds` | `epoch`, `expires_at` |
| `agent/writer-revoke` | `agent_id` | next `epoch` (tombstoned) |

Acquire is an **administrative transfer**, not a claim by any connected
client. It increments the epoch even if the same owner is selected. Revoke
increments the epoch and disables signing. A previously bound client cannot
acquire or renew after a transfer unless a provisioner deliberately transfers
back to it with a new epoch.

The writer is the authenticated NIP-46 client pubkey, not the service identity
pubkey. **A writer client key must be distinct from every Signet management
provisioner key.** Signet rejects acquisition for a provisioner pubkey, and
records every writer client pubkey permanently so subsequent management grants,
configuration reloads, and seeds cannot promote a current or former writer.
Both directions are enforced by database triggers inside the same write
transaction as acquisition/grant. This is per-key role separation, not merely
per-agent policy. `writer_renew` does not grant management authority. On
upgrade, Signet refuses to open a pre-history database that already has writer
leases: former owners cannot be reconstructed from a current lease row. Such
a database needs operator-led reconciliation on a fresh, epoch-preserving
cutover; do not just delete the old lease or copy a stale DB. A corrupt
current owner/provisioner overlap also aborts database open.

A fenced NIP-46 client signs using
`sign_event` with params `[event_json, "<decimal epoch>"]`. Its authenticated
transport pubkey must equal the lease owner and its persistent client binding
must still be valid; Signet also applies the usual signing policy. Legacy
`sign_event` with only `[event_json]` remains available only to never-fenced
identities. The current owner may renew directly with NIP-46 method
`writer_renew`, params `["<decimal epoch>"]`; this extends expiry by 300
seconds and returns the new Unix `expires_at` as a decimal string. Its policy
must explicitly allow `writer_renew` (alongside `sign_event`). Renew before
expiry; an expired lease cannot be revived by renew and requires a
provisioner-authenticated acquire, which advances the epoch.

D-Bus, NIP-5L, and SSH-agent signing have no epoch-bearing contract and reject
fenced identities. Their legacy operations remain available to identities
that have never been fenced. Legacy raw-key borrows reject **all DB-backed
identities**, including never-fenced ones: a copy issued before acquisition could otherwise bypass a
later fence. All daemon signing and private-key crypto paths use the
transaction-scoped custody operation. Before first acquisition, stop/quiesce
old binaries and any previously issued raw-key borrowers; the new daemon
cannot claw back a key they already copied.

The authenticated NIP-46 lease owner may also use `nip44_encrypt`,
`nip44_decrypt`, `nip44_encrypt_b64`, and `nip44_decrypt_b64` with params
`[peer_pubkey, input, "<decimal epoch>"]`. The binary-safe variants encode
or return the plaintext as standard base64; ordinary NIP-44 uses UTF-8 strings.
The client pubkey comes from the authenticated NIP-46 envelope, never from a
request parameter. Signet checks owner, exact epoch, expiry and observed clock
inside the same SQLCipher write transaction as NIP-44 crypto; failed/stale/
expired/revoked or uncommitted operations return no ciphertext or plaintext.
Legacy two-param NIP-44 calls remain available only to never-fenced
identities. NIP-04 has no epoch-bearing contract and stays denied for fenced
identities. D-Bus and NIP-5L NIP-44 remain legacy-only and reject fenced
identities regardless of their caller.

There is no generic raw
digest/DSSE signing API in this change.

## Operational rollout (not performed by this code change)

1. Inventory every signer and previously signed service outbox. Assign a
   **new, dedicated NIP-46 client key** to the incoming writer; do not reuse
   the old daemon's client key or any configured Signet provisioner key. Confirm the Signet adoption result matches the
   service's current pubkey. Confirm the single active Signet/DB topology and
   policy for `sign_event`, `writer_renew`, and any required NIP-44 methods.
2. On **stage-01**, stop and disable old service-key signers, drain or quarantine
   already-signed outboxes, and verify no old daemon can still use a retained
   secret or stale DB copy. Acquire an epoch for the new client. Prove old
   client/no-epoch/wrong-epoch requests fail, new client signs with the same
   service pubkey, renew works, and restart preserves stale-epoch rejection.
3. Repeat the admission and negative proofs on **edge-01** only after stage-01
   passes. Rollback must revoke/transfer forward to a new epoch, never restore
   a stale DB or re-enable an unfenced old signer.

The privileged low-level store/master-key API and DB administrator can still
extract the secret; the fence is not protection against a compromised Signet
process, administrator, or database key holder.

A fence prevents **new stale signatures through this Signet custody path**. It
cannot invalidate events signed before cutover, erase old outboxes, or prevent
an external process still holding the service secret from signing. Those are
separate operational admission proofs; do not call the cutover complete without
them.
