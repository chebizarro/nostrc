# Signet single-writer fence for a same-pubkey service cutover

Signet can **adopt** an existing service identity without changing its Nostr
pubkey. The secret is delivered only through Signet's existing encrypted
`agent/adopt-existing` management path; it must not be logged, filed in an
issue, pasted into a command line, or requested by a client. Compare the
returned `pubkey` with the service's established pubkey before proceeding.

A provisioner can then **fence** the identity: assign it exactly one
authorized NIP-46 client. The fence is enforced by the authenticated client
identity, not by any request parameter. NIP-46 request shapes stay standard.

## Management operations

Provisioner-authorized, encrypted ContextVM kind-25910 intents:

| Method | Params | Result |
| --- | --- | --- |
| `agent/writer-acquire` | `agent_id`, `writer_pubkey` (NIP-46 client pubkey, 64 hex) | `epoch` |
| `agent/writer-revoke` | `agent_id` | `epoch` (no owner) |

CLI: `signetctl writer-acquire <agent_id> <owner_pubkey>`.

- Acquire takes effect immediately and permanently marks the identity as
  fenced (`agents.writer_fenced` never resets). It replaces any previous
  owner. Assignments do not expire; to stop a writer, transfer or revoke.
- **Writer client keys are single-use.** Acquire fails for any pubkey that
  has ever been a writer for any agent: the current owner, a former owner,
  or a revoked owner. A->B->A is impossible, so a delayed or replayed
  request from a former owner can never pass the owner check again.
- A writer key must be distinct from every provisioner key. Database
  triggers enforce this in both directions inside the acquire/grant
  transaction, and the writer-key history is permanent.
- Revoke leaves the identity fenced with no owner: nothing signs until a
  provisioner acquires a new, never-used key.
- `epoch` is an internal generation counter that advances on every acquire
  and revoke. It is for audit and operators only and never appears in a
  NIP-46 request.

## NIP-46 behavior

| Identity | `sign_event`, `nip44_encrypt`, `nip44_decrypt`, `nip44_encrypt_b64`, `nip44_decrypt_b64` | `nip04_*` |
| --- | --- | --- |
| never fenced | unchanged: any bound client, subject to policy | unchanged |
| fenced | only the current owner client | refused |

Params are the standard shapes (`sign_event [event_json]`,
`nip44_encrypt [peer_pubkey, plaintext]`, `nip44_decrypt [peer_pubkey,
ciphertext]`, and the `_b64` pair as in the README). The owner is the
pubkey of the authenticated NIP-46 envelope; the client also needs a valid
persistent binding and the usual policy allow.

The owner check runs in the same SQLCipher `BEGIN IMMEDIATE` transaction
that decrypts the key and performs the signature or NIP-44 operation, so
acquire and revoke serialize with in-flight operations. A failed check,
a missing or corrupt lease row for a fenced identity, or a failed commit
returns no signature, ciphertext, or plaintext.

Signet rejects decoded or literal NULs in NIP-46 request ids, methods,
params, and the decrypted request bytes. `nip44_decrypt` rejects plaintext
that is not NUL-free UTF-8 (`invalid_plaintext`); use `nip44_decrypt_b64`.

D-Bus, NIP-5L, and SSH-agent signing and crypto have no authenticated
NIP-46 client and reject fenced identities. Raw key borrows reject every
DB-backed identity, because a copy taken before a fence would outlive it.

## Supported topology

One active Signet daemon and one durable SQLCipher database
(`PRAGMA synchronous=FULL`). **Active-active daemons on separate copies,
restoring an old DB snapshot, and old daemon binaries that ignore the fence
are not supported**: a rewound database forgets which keys were writers.
Signet refuses to open a database that has writer leases but no writer-key
history, because former owners cannot be reconstructed.

The fence stops new signatures from non-owners through this Signet custody
path. It does not invalidate events signed earlier, erase old outboxes,
stop an external process that still holds the service secret, or protect
against a compromised Signet process, administrator, or database key holder.

## Upgrading from the lease-expiry version

There is no schema migration. The `agent_writer_leases.expires_at` and
`observed_at` columns stay but are ignored. Consequences:

- `writer_renew`, `agent/writer-renew`, `ttl_seconds`, `expires_at` results,
  `signetctl writer-acquire --ttl`, and the wall-clock rollback check are
  gone. Clients send standard NIP-46 params; an extra trailing parameter is
  ignored, as before the epoch work.
- A lease that had expired under the old version is live again for its
  owner. If you relied on a lapse to stop a writer, revoke before upgrading.
- Transferring back to an earlier writer key is no longer possible; mint a
  new client key for every assignment, including rollback.

## Operational rollout (not performed by this code change)

1. Mint a **new, dedicated NIP-46 client key** for the incoming writer (not
   a provisioner key, not any key used as a writer before). Confirm the
   adoption result matches the service's pubkey and the policy allows
   `sign_event` and any required NIP-44 methods.
2. On **stage-01**, stop and disable old signers, drain or quarantine
   already-signed outboxes, and verify no old daemon can use a retained
   secret or a stale DB copy. Acquire the new key. Prove that other clients
   and the previous client are refused, the new client signs with the same
   service pubkey, and a restart preserves both.
3. Repeat on **edge-01** only after stage-01 passes. Roll back by acquiring
   another new key or revoking; never restore a stale DB or re-enable an
   unfenced old signer.
