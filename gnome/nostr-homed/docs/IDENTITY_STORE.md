# Nostr identity authority and NSS projection

Status: version-one implementation contract. This document defines the A-owned
identity boundary consumed by the authentication broker. It does not define the
broker wire protocol, PAM conversation, NIP-46 transport, or service sandbox.

## Trust boundary

`nostr-authd` is the sole production writer. It holds an exclusive
`authority.lock` for its lifetime. Administration is handled inside that
already-authorized broker; `nostr-authctl` must send broker requests and must
never open `authority.db` directly.

| Data | Default path | Contract |
|---|---|---|
| Authority | `/var/lib/nostr-auth/private/authority.db` | private directory `0700`, database/lock/sidecars `0600` |
| NSS projection | `/var/lib/nostr-auth/nss.db` | root-owned directory `0755`, complete immutable snapshot `0644` |

The projection is not authentication evidence. It contains no account ID,
status, public key, provider, receipt, or secret. B reads `authority.db` and
calls `nh_identity_store_recheck()` before admitting a proof or session.
The legacy `/var/lib/nostr-homed/cache.db` remains a roaming cache and is never
opened through its schema-creating API during migration.

## Stable C API

`include/nostr_identity.h` is the authoritative in-tree API. Handles are opaque
and no SQLite, GLib, PAM, or protocol type crosses the boundary. Authority
records and projection records are deliberately different types.

B's stable read tier is:

```c
nh_identity_store_open();
nh_identity_store_get_info();
nh_identity_store_lookup_by_name();
nh_identity_store_lookup_by_uid();
nh_identity_store_lookup_by_id();
nh_identity_store_lookup_by_pubkey();
nh_identity_store_recheck();
nh_identity_store_provider_get();
```

`recheck` succeeds only for an active row whose key generation and global
authority generation both equal the transaction snapshot. It returns the
current row on stale/not-active outcomes when requested.

Every mutation carries a canonical lowercase UUID operation ID. Replaying the
same type and canonical request returns the original result without another
write. Reusing an ID for different input returns
`NH_IDENTITY_OPERATION_MISMATCH`.

## Identity invariants

- Names match `n_[a-z0-9_]{1,30}` and are immutable.
- Public keys are exactly 64 lowercase hexadecimal x-only bytes.
- UUIDs use lowercase canonical `8-4-4-4-12` form.
- New allocations use the lowest externally-free equal UID/GID pair in
  `200000..299999`.
- Configured domain ranges (`1000000..1099999`, `1100000..1999999`) and the
  standalone-SMB range (`500000..599999`) must not overlap the Nostr range.
- An external ownership probe checks local/domain ownership before reservation;
  uncertainty fails closed. The store never calls NSS while holding a write.
- Reviewed import is the only operation that accepts explicit UID/GID/home.
- Account ID, name, UID, GID, home, origin, and creation time cannot be updated.
- Accounts cannot be deleted. Retired IDs therefore remain reserved.
- There is no implicit active status.

Statuses are `enrolling`, `active`, `disabled`, `repair_required`, and
`retired`. Disabled and retired owners remain name-resolvable after projection;
only authority status can grant login readiness.

## Authority schema v1

`PRAGMA user_version=1`. Runtime opens enable foreign keys, WAL, full synchronous
durability, a busy timeout, and 64-bit integer reads.

- `metadata`: `authority_id`, `authority_generation`,
  `projection_generation`, `created_at`.
- `accounts`: immutable Unix identity, status, origin, projectable flag,
  key generation, timestamps.
- `identities`: one unique current lowercase pubkey per account.
- `providers`: public config and ciphertext only; at most one staged and one
  enabled provider of each type per account.
- `operations`: idempotence digest, stable mutation result ID, account, type, phase/outcome, safe details,
  local-home filesystem evidence, timestamps.

Triggers reject immutable-field changes, account deletion, projectable
regression, key-generation regression, or changes to a retired row. Uniqueness
conflicts are errors, never upserts.

Opening is explicit and fail-closed. A missing database without
`NH_IDENTITY_STORE_CREATE` is not initialized. Creation never upgrades an
existing schema. Newer/zero/malformed schema versions are refused. The
exclusive lock is `LOCK_EX|LOCK_NB` and `CLOEXEC`.

`authority_generation` increments in the transaction containing each mutation.
`key_generation` increments only for identity replacement. Replacement preserves
UID/GID/home, removes providers, and leaves the account disabled.

## Provider staging

`nh_identity_provider_stage()` writes a disabled row. It cannot make an account
usable. `nh_identity_provider_activate()` requires a
`nh_identity_proof_attestation` created by B only after proof verification. The
store checks its public key and key generation against current authority. This
is same-process evidence, not another cryptographic verifier. Providers return
signed material to B; they never return “login successful.”

## Home and crash-recovery phases

Enrollment, reviewed import, and repair advance monotonically:

```text
reserved -> staged -> installed -> projected -> complete
```

The local-home implementation records device/inode evidence for staging and
installation. Repeating an edge with identical evidence is idempotent; different
evidence is ambiguous and fails closed as `repair_required`. Recovery never
recursively chowns, deletes, replaces, copies, or unmounts an unexpected path.
Homes are descriptor-relative, same-filesystem, no-follow, no-replace, and mode
`0700`. Activation occurs only after verified provider readiness and projection.

## Projection format v1

SQLite `application_id=0x4e485031` (`NHP1`), `user_version=1`:

```sql
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE passwd(name TEXT PRIMARY KEY, uid INTEGER UNIQUE NOT NULL,
  gid INTEGER NOT NULL, gecos TEXT NOT NULL, home TEXT NOT NULL,
  shell TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE grp(name TEXT PRIMARY KEY, gid INTEGER UNIQUE NOT NULL)
  WITHOUT ROWID;
```

Required metadata: `authority_id`, `projection_generation`,
`source_authority_generation`, `format_minor`, `built_at`.

Publication builds `nss.db.new` in the destination directory with rollback
journal mode, validates it through the reader, closes SQLite, confirms no
journal remains, changes it to `0644`, fsyncs it, renames, then fsyncs the
directory. The installed inode is never mutated. Failure before rename leaves
the prior snapshot intact. Projection lag cannot authorize because B never
consults projection for policy.

Reader results are exact:

| Result | Meaning | NSS mapping |
|---|---|---|
| `FOUND` | Valid row; all strings copied to caller buffer | `SUCCESS` |
| `NOT_FOUND` | Valid snapshot, no row (or invalid Nostr name) | `NOTFOUND` |
| `UNAVAILABLE` | Missing, unreadable, symlinked, corrupt, wrong-format DB; SQLite/row/API error | `UNAVAIL` |
| `TOO_SMALL` | Valid row exists but buffer cannot hold it | `TRYAGAIN`, `ERANGE` |

Reader opens are read-only and never create a file, schema, journal, or WAL.
Strings are never truncated or fabricated. UID/GID values use checked 64-bit
reads. `initgroups` proves the account belongs to this projection, avoids the
primary group and duplicates, respects `limit`, and reports allocation errors.

## Reviewed legacy import

Migration opens legacy SQLite read-only and reports malformed keys/names,
duplicate identity/name/UID/GID/home, overwritten-owner ambiguity, external
ownership/range conflict, invalid or mounted homes, and exhaustion. It never
chooses a winner. Import requires per-account administrator review, preserves
only explicitly approved UID/GID/home exceptions, and requires fresh provider
proof. It does not copy or unmount roaming homes.

## Configuration and version

`config/identity.conf.sample` is identity-only. Unknown/duplicate keys,
malformed numbers, relative paths, and overlapping ranges fail. Environment
variables cannot override authority paths. `config/nss.conf.sample` exposes only
the projection path. General auth/PAM/socket/provider config belongs to B.

This new initial-development public surface contributes to the planned
`nostr-homed` 0.2.0 bump. D owns build-system and version-source integration.
Portable macOS tests do not certify Linux NSS ABI, labels, installed `/home`, or
GDM behavior; those remain pinned-Linux acceptance gates. Nothing in A enables
PAM or NSS by default.
