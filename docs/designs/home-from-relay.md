# Home from Relay — a portable `$HOME` backed by relays + Blossom

**Status:** Design freeze (Phase 0). **No implementation.**
**Bead:** `nostrc-h10m` — `[E-portable-home]`
**Depends on:** `nostrc-rb0e` (Samba / packaging / installed release gates), and the merged roaming safety fixes `nostrc-nxpb.7` / `.8` (relay-list UAF and false-success in `nh_warm_cache` / `nh_open_session`).
**Date:** 2026-09-22

---

## 1. Context and scope

### 1.1 Goal

A user logs into GDM on *any* Linux box with their Nostr identity — local encrypted vault, external NIP-46 bunker, or the QR/NIP-05 flow — and their `$HOME` materializes from nostr-native storage. The home follows the *identity*, not the machine.

This is the epic after login + SMB + QR/NIP-05. It is deliberately parked; this document exists so Phase 1 has something to execute against, not so Phase 1 starts tomorrow.

### 1.2 What already exists (ground truth, read 2026-09-22)

This design is constrained by what is actually in the tree. Nothing below is aspirational.

| Area | File | What it gives us |
| --- | --- | --- |
| PAM session hook | `gnome/nostr-homed/src/pam/pam_nostr_broker.c:511` | `pam_sm_open_session` is a **no-op stub** — logs and returns `PAM_SUCCESS`, comment says "Local homes are provisioned at enrollment; nothing to mount here." This is the first-login gap. |
| Roaming CTL | `src/ctl/nostr-homectl.c` | `nh_warm_cache` (signer pubkey → author filter → NIP-65 relay discovery → manifest fetch → cache persist → secrets tmpfs + NIP-44 decrypt via signer); `nh_open_session` (mkdir → `systemctl start nostrfs@<user>` → **verify mountpoint** → persist `mounted`/`failed`). Test seams `nh_hook_*` for dbus/mountpoint/mkdir/systemctl. |
| Headless CTL | `src/ctl/nostr-homectl-headless.c` | GLib-free, FUSE-free admin CLI. Mutually exclusive with the roaming build. |
| Home staging | `src/identity/identity_home.c` | `nh_identity_home_prepare` — `openat2` with `RESOLVE_BENEATH|NO_SYMLINKS|NO_XDEV`, root-owned trusted-path walk, `.nostr-<opid>` staging dir, `renameat2(RENAME_NOREPLACE)` install, crash-recovery by recorded inode, `ambiguous` → `REPAIR_REQUIRED` and **never deletes**. Bounded skel copy: depth ≤ 16, ≤ 4096 entries, ≤ 64 MiB. |
| Identity store | `src/identity/identity_store.c`, `identity_projection.c` | account records, provider records, operation state machine (`RESERVED → STAGED → INSTALLED`). |
| Blossom (homed) | `src/common/blossom_client.c` | curl HEAD/GET/PUT, sha256-hex CID, **NIP-98 kind-27235** auth header signed over D-Bus `org.nostr.Signer`. |
| Blossom (libhanami) | `libhanami/include/hanami/hanami-blossom-client.h` | Full BUD-01 GET/HEAD/LIST, BUD-02 PUT/DELETE, **BUD-04 mirror**. Auth via `hanami-bud02-auth.c` (**kind 24242**, `t`/`x`/`expiration`/`server` tags, mandatory expiration, full validate path). |
| Git-over-Blossom | `libhanami/src/hanami-odb-backend.c`, `hanami-refdb-backend.c`, `hanami-index.c` | A libgit2 ODB whose objects live in Blossom, plus a **bidirectional `git_oid ↔ blossom sha256` index** (sqlite or lmdb, at a local path). `hanami_hash_blossom`, `hanami_hash_git_sha1/sha256`. |
| GRASP | `libhanami/src/hanami-grasp.c` | NIP-34 kind-30618 publish-then-push against a combined relay + git-smart-HTTP host. |
| FUSE | `src/fs/nostrfs.c` (928 lines) | Working `fuse_operations`: getattr/readdir/open/read/create/write/flush/release/fsync/rename/unlink/chmod/chown/mkdir/rmdir/statfs. Writeback gated by `opts.writeback`; non-writeback returns `-EACCES`. Gated by `NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING`. |
| Manifest type | `include/nostr_manifest.h` | `nh_manifest{version, entries[], links[]}`, `nh_entry{path,cid,size,mode,uid,gid,mtime}`, `nh_link{path, manifest_event_ref}` — **plaintext paths today**. |
| Secrets | `include/nostr_secrets.h` | `nh_secrets_mount_tmpfs`, `nh_secrets_decrypt_via_signer`. |
| Unprivileged fetch pattern | `src/profile/nostr-homed-profile-image.c` | The pattern to copy: separate binary, https-only `CURLOPT_PROTOCOLS_STR`, `MAXREDIRS 3` + https-only redirects, `TIMEOUT 10`/`CONNECTTIMEOUT 5`, `MAXFILESIZE` + hard in-callback cap, `OPENSOCKETFUNCTION`/`SOCKOPTFUNCTION` → `nh_profile_ssrf_check_sockaddr` refusing RFC1918/loopback/link-local, `DNS_CACHE_TIMEOUT 0`, `NOPROXY "*"`, `UNRESTRICTED_AUTH 0`, distinct exit codes (65 = SSRF refusal, 66 = network/size). |
| NIP-46 client | `nips/nip46/include/nostr/nip46/nip46_client.h` | `connect`, `get_public_key`, `sign_event`, **`nip44_encrypt` / `nip44_decrypt`** (plus `_rpc` and `_b64_rpc` variants), rate limit, timeout. |
| Test fixtures | `tests/integ/fake_relay_fixture.py`, `tests/integ/fake_blob_server.py`, `tests/integration/fake_blossom.py`, `run_nostrfs_writeback_fake_blossom.sh`, `mock_signer.c` | A mock relay, a mock blob server, a fake Blossom, and a mock signer already exist. |
| NIPs in tree | `docs/nips/B7.md`, `78.md`, `98.md`, `44.md`, `46.md`, `65.md` | B7 = Blossom media, mandates **kind 10063** server lists (BUD-03). 78 = arbitrary app data, **kind 30078** addressable + `d` tag. |
| Build gates | `gnome/nostr-homed/CMakeLists.txt:17-52` | Everything is opt-in `OFF`: `AUTH_CORE`, `IDENTITY_CORE`, `NSS`, `AUTH_RUNTIME`, `PAM`, `DOMAIN_CONFIG`, `EXPERIMENTAL_ROAMING`, `SMB`, `AUTH_INSTALL`, `CTL`, `GREETER_EXTENSION`, `PROFILE`. Root `CMakeLists.txt:351` has `BUILD_LIBHANAMI` (default ON, forces `ENABLE_NIP34`). |

### 1.3 Non-goals for v1

- Group-shared or multi-user homes (see §2.6 on Marmot/MLS).
- A numeric NIP proposal. We define the event *shape* we implement now; standardisation is a later, separate conversation.
- Replacing the enrollment-time local-home path. Portable home is **additive** and opt-in per account.
- Blossom server operation. We are a client.

---

## 2. Data model — what a "home" is on the wire

### 2.1 The shape

Three layers, strictly separated by mutability:

```
  RELAY  (mutable, tiny, signed by the account key)
    └── kind 30078 addressable "home pointer"        ~1 KB, NIP-44 self-encrypted
          content -> { snapshot_root, key_epoch, generation, device, ts }

  BLOSSOM (immutable, content-addressed, encrypted)
    ├── snapshot object        (the root manifest node)
    ├── manifest nodes         (directory listings, Merkle children)
    └── chunk blobs            (file content, fixed-size chunks)
```

The relay holds **one small mutable pointer**. Everything of size is an immutable encrypted blob on Blossom, addressed by the SHA-256 of its *ciphertext* (which is what BUD-01 addresses and what BUD-02 signs over in the `x` tag). A relay never sees file content, and a Blossom server never sees a name, a path, or a mode bit.

This inverts the current `nh_manifest` design, where the whole manifest JSON is stuffed into a relay event and cached verbatim (`nh_warm_cache` does `nh_cache_set_setting(&c0m, mkey, json)`). That does not survive a real home: relays cap events in the tens-to-hundreds of KB, and a 50k-file home is megabytes of manifest.

### 2.2 The home pointer event (relay)

**Kind 30078** (NIP-78 arbitrary app data, addressable). Chosen because it exists, is addressable by `(kind, pubkey, d)`, is explicitly sanctioned for "personal private data generated by apps… allow users to use Nostr relays as their personal database", and requires no new kind number.

```
kind:       30078
pubkey:     <account pubkey>
tags:       [["d", "nostr-homed.home.v1:<profile>"]]
            [["client", "nostr-homed"]]
            [["alt", "encrypted portable home pointer"]]
content:    NIP-44 ciphertext (self-encrypt: conversation key over own pubkey)
```

Plaintext inside `content`:

```json
{
  "v": 1,
  "profile": "personal",
  "generation": 421,
  "snapshot": "<64-hex blossom sha256 of the snapshot object>",
  "key_epoch": 1,
  "wrapped_home_key": "<NIP-44 ciphertext of the 32-byte home key>",
  "servers": ["https://blossom.example", "https://cdn.example"],
  "device": "<opaque 8-byte device id, stable per machine>",
  "ts": 1758499200,
  "prev": "<64-hex snapshot of the previous generation, or null>"
}
```

- `generation` is a monotonic counter. It, not `created_at`, is the ordering authority — relay clocks and device clocks both lie. A pointer with a lower `generation` than the locally recorded one is **rejected**, which is the rollback defence (§8.2).
- `prev` lets a client walk back a bounded history for recovery without any relay retaining more than the latest replaceable event.
- `servers` is a cached echo of the user's kind-10063 list so a cold boot can start fetching before it has resolved 10063 (§5.2).
- `d` is `nostr-homed.home.v1:<profile>`. `<profile>` defaults to `personal` and reuses the existing namespace notion already threaded through `nh_warm_cache(namespace_hint)` / `HOMED_NAMESPACE` / `settings.namespace.<user>`.

**The `d` tag is public.** Anyone reading the relay learns this pubkey has a nostr-homed portable home and roughly how often it changes. That is accepted (§8.1); hiding it would require a blinded `d`, which breaks the addressable-replaceable semantics we want.

### 2.3 Snapshot and manifest objects (Blossom)

A snapshot is a Merkle tree of **manifest nodes**. Each node describes one directory:

```json
{
  "v": 1,
  "type": "dir",
  "entries": [
    {"n":"Documents","t":"dir","r":"<64-hex>","md":16877,"mt":1758400000},
    {"n":"notes.txt","t":"file","sz":1731,"szb":2048,"md":33188,"mt":1758400111,
     "chunks":["<64-hex>","<64-hex>"],"cs":4194304},
    {"n":"link","t":"sym","tgt":"Documents/notes.txt","mt":1758400111}
  ]
}
```

Then serialized, NIP-44-sealed with the home key, and PUT to Blossom. Its address is `sha256(ciphertext)`.

Rules:
- **Names and paths never leave the device in clear.** They live inside the encrypted node body only. This is the whole reason for the Merkle split rather than the flat `nh_entry.path` the current `nh_manifest` uses.
- `sz` is the true size (needed for `stat`); `szb` is the coarse bucket a *network observer* can infer from the ciphertext length. Nodes are padded to power-of-two-ish buckets so the Blossom server's view of size is quantised (§8.3).
- `uid`/`gid` are **not** stored. They are meaningless across machines — the local NSS projection assigns them (`nh_cache_map_npub_to_uid`). On materialization every file is chowned to the local `account.uid`/`account.gid`, exactly as `identity_home.c:copy_tree` already does. Storing a remote uid would be a footgun.
- `md` (mode) is stored but **masked on apply**: `mode & 0777`, and set-uid/set-gid/sticky are stripped unconditionally. A portable home must never be able to introduce a setuid binary.
- Device nodes, FIFOs, sockets and hardlinks are refused on capture and on apply, matching `copy_tree`'s existing policy.

A snapshot object is a manifest node plus snapshot metadata (generation, parent, total size, entry count).

### 2.4 Chunking

- Fixed **4 MiB** chunks. Not content-defined chunking (CDC) in v1: fixed chunking is trivially testable, has no tuning surface, and its dedup loss is irrelevant for a first cut. CDC is a Phase-3+ optimisation and is explicitly *not* on the v1 critical path — see **D6**.
- Files ≤ 64 KiB are **inlined** into the parent manifest node rather than getting their own blob. A dotfile-heavy home is thousands of tiny files; one HTTP round trip each is the difference between a 20-second and a 20-minute first login.
- Per-file hard cap: **2 GiB** (v1 refuses larger files and records them in a skip report). Per-home hard cap: **see §5.4**.

### 2.5 Encryption of blobs and convergence

Each chunk and each manifest node is sealed with NIP-44 v2 using a key derived per-object:

```
object_key = HKDF-SHA256(ikm = home_key,
                         salt = "nostr-homed/blob/v1",
                         info = sha256(plaintext) || object_type)
```

This is **convergent within the home**: the same plaintext always produces the same ciphertext and therefore the same Blossom address, so re-uploading an unchanged file is a cheap HEAD, and dedup works. It is **not** convergent *across* homes, because `home_key` is per-account — so a Blossom server cannot confirm "this user stores the same file as that user", which plain convergent encryption would leak.

NIP-44's nonce must therefore be derived, not random: `nonce = HKDF(home_key, "nostr-homed/nonce/v1", sha256(plaintext))`. Nonce reuse across *different* plaintexts is impossible by construction (the derivation is over the plaintext hash); nonce reuse for the *same* plaintext under the same key is the deduplication property and is safe for a deterministic AEAD used this way. **This is the single most security-sensitive choice in the document and needs review before Phase 1 — see D4 and §11.**

### 2.6 The libgit2/libhanami alternative (rejected for v1, kept as Phase 4+)

libhanami already offers a git ODB backed by Blossom with an OID↔blossom index. A home could literally be a git repository: commit = snapshot, tree = manifest node, blob = file, ref = pointer. History, dedup, and three-way merge come free.

Rejected for v1 for four concrete reasons:

1. **The index is local.** `hanami_index_open(out, path, backend)` opens a sqlite/lmdb file on disk. The `git_oid → blossom sha256` mapping is *state that does not exist on a new machine*. Cold login on a fresh box is precisely our use case, so we would have to publish or rebuild the index — which is exactly the manifest problem again, re-solved worse.
2. **Git is not encrypted.** Interposing encryption under the ODB changes the object bytes, so git's own OIDs no longer address the plaintext, and the mapping table has to carry the secret. Workable, but it turns "get history for free" into a custom crypto layer anyway.
3. **Git's working-tree model is wrong for a 40 GiB home.** libgit2 checkout wants an index and a full working tree; we want partial, resumable, quota-bounded materialization.
4. **Blast radius.** libgit2 + libhanami in the login path is a large new dependency surface for the privileged provisioner.

Keep it as the natural substrate for a **Phase 4+ "versioned home" / GRASP-hosted home** once the primitives are proven. Reuse `hanami_blossom_*` (the client) from day one; skip `hanami_odb_backend_*` (the git layer) in v1. See **D3**.

### 2.7 Group-shared homes / Marmot / MLS

Out of scope for v1, explicitly. A shared home means multiple writers, which means real concurrency control, which means an MLS group (Marmot, `libmarmot` in tree) supplying the group key and epoch. The v1 key schedule (§4) deliberately leaves room: `key_epoch` in the pointer and `wrapped_home_key` as an indirection mean the home key can later be an MLS exporter secret instead of an identity-derived one, without changing the blob or manifest format. Note it; do not build it.

---

## 3. Materialization strategy

### 3.1 The two real options

**(a) Eager local copy.** At first login, the privileged provisioner walks the manifest and writes a real directory tree into `/home/<user>`. Later logins reconcile. A user-session daemon pushes local changes back.

- *Pros:* it is a normal home. `stat` is `stat`. No FUSE inside GDM's session isolation. Offline is free — the files are just there. gvfs, flatpak, snap, Electron apps, `inotify`, `O_DIRECT`, sqlite WAL on `~/.mozilla` — all behave. Backup tools work. Nothing is surprising.
- *Cons:* first login pays bandwidth and disk for the whole home. Multi-box consistency needs a real sync loop with real conflict handling.

**(b) FUSE mount via nostrfs.** Mount `nostrfs` on `/home/<user>`, fetch content on demand.

- *Pros:* instant first login. Disk is a cache, not a copy. Sync is "always live".
- *Cons:* FUSE in a GDM session is a known minefield — the mount must exist before the session's first `stat` of `$HOME`, `fusermount` needs the right privilege, unmount-on-logout races with lingering processes, and `systemd --user` and `pam_systemd` both want to touch `$HOME` early. Offline means a local write-back cache with its own coherence protocol — i.e. all the sync complexity of (a) *plus* FUSE. `nostrfs.c` returns `-EACCES` for every mutation unless `opts.writeback`, and the writeback path is exercised only by `run_nostrfs_writeback_fake_blossom.sh`. It is a 928-line experiment behind `NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING`, not a filesystem anyone has run a GNOME session on for a month.

### 3.2 Recommendation: (a) eager local copy for v1

**Recommend (a).** The defence is not that FUSE is bad; it is that **(b) does not remove any of (a)'s hard problems, and adds one of its own.** Both need encryption, chunking, a manifest format, a Blossom client, conflict policy, and a quota. (b) additionally needs a coherent write-back cache and a FUSE mount that survives GDM. Building (a) first produces every primitive (b) would need, and produces a shippable product at the end of Phase 2. Building (b) first produces a demo.

Two refinements that blunt (a)'s "first login is slow" cost:

1. **Priority materialization.** The manifest carries an ordered priority list (default: `~/.config`, `~/.local/share`, `~/.ssh`, `~/.gnupg`, dotfiles at the root, then `Desktop`, then the rest). The session is released as soon as the priority set is on disk; the remainder streams in under a progress indicator from the sync daemon. This is the difference between a 90-second and a 6-second login for a typical home.
2. **A local content cache keyed by Blossom address**, at `/var/cache/nostr-homed/blobs/<aa>/<sha256>`, shared across accounts on the box and LRU-evicted. Second login by the same user on the same box, or the *n*th user of a shared lab machine with overlapping content, is nearly free.

**(b) stays on the roadmap as Phase 4**, but re-scoped: not as the home itself, but as an *overlay for the cold tail* — `~/Archive`-style directories the manifest marks `lazy`, mounted read-mostly under the already-real home. That is a strictly additive change to a working system.

### 3.3 How this fits `pam_sm_open_session` and `nh_identity_home_prepare`

The privileged work does **not** go in the PAM module. `pam_nostr_broker.c` is, by its own header, "stateless glue — no key material, no policy decision of its own", and it runs inside GDM's process. Putting an HTTP client and a crypto layer there is wrong.

Flow:

```
pam_sm_open_session(pamh)
  1. resolve user; connect to the broker at /run/nostr-auth/auth.sock
     (same socket + nh_auth_client_connect the auth path already uses)
  2. send PROVISION_HOME { user, session_id }
  3. broker replies immediately with one of:
       READY            -> PAM_SUCCESS               (nothing to do / already materialized)
       PROVISIONING     -> block on a bounded wait for the priority set (§5.3)
       LIMITED          -> PAM_SUCCESS + pam_info(...) notice, session is limited-mode
       NOT_PORTABLE     -> PAM_SUCCESS               (ordinary local home; today's behaviour)
       unavailable/EIO  -> PAM_SUCCESS + notice      (NEVER block login on the network)
  4. pam_putenv NOSTR_HOME_STATE=ready|partial|limited so the session daemon
     and the shell profile can react.
```

The broker (`nostr-authd`) owns the privileged provisioner. It already holds the only fresh proof of the signer and already has the account record. Inside the broker:

- **`nh_identity_home_prepare` is unchanged and still does the staging.** Portable home is a *new `nh_identity_home_options.label` callback*: `home_prepare` opens the staging directory `.nostr-<opid>` and hands the caller a descriptor (`options->label(ctx, home_fd)`); the portable-home labeler materializes the priority set **into that descriptor** using `openat`-relative writes only. That means the whole materialization inherits the existing `RESOLVE_BENEATH|NO_SYMLINKS|NO_XDEV` discipline and the atomic `renameat2(RENAME_NOREPLACE)` install for free, and a partially-materialized home is never visible at `/home/<user>`. This is the single most important structural decision in §3 — see **D5**.
- Failure of the labeler already routes to `ambiguous` → `nh_identity_operation_fail(..., REPAIR_REQUIRED)` → **the staging directory is left intact and never deleted**. That invariant is preserved as-is.
- The bounded-copy limits in `copy_tree` (depth 16, 4096 entries, 64 MiB) are `skel`-specific and are **not** reused for home content; portable home gets its own, much larger, explicitly-configured caps (§5.4). They must be separate constants — reusing them would silently truncate real homes.

For an **existing** home (second login on a box that already has the tree), nothing goes through `home_prepare`. The broker calls `nh_identity_home_validate(store, account_id, &expected_evidence, &found)` — the existing `st_dev`/`st_ino` + ownership + `0700` check — and then hands the directory to the reconciler, which is the sync daemon's cold-start path (§6.3).

### 3.4 What happens to `nh_open_session` / nostrfs

`nh_open_session` in `nostr-homectl.c` (mkdir → `systemctl start nostrfs@<user>` → `nh_hook_is_mountpoint` verify → persist `mounted` or `failed`) stays exactly as it is, behind `NOSTR_HOMED_ENABLE_EXPERIMENTAL_ROAMING`. **It is not on the v1 portable-home path.** Its hard-won invariants are the template for the new path and are restated as requirements:

- A successful `systemctl start` is **not** evidence of success; verify the effect independently. (Portable-home analogue: a 200 from Blossom is not evidence the file is on disk; `fsync` + re-`stat` before recording `ready`.)
- On any failure, record `failed` — **never** a state that claims more than happened, and never publish a mount path or unit for a mount that isn't live.
- Every resource consumed on the error path is freed exactly once at a single `out:` label, and the relay list outlives its last consumer (`nostrc-nxpb.7`). The provisioner's server list has exactly the same lifetime hazard and gets the same single-exit shape.

---

## 4. Encryption and keys

### 4.1 Key hierarchy

```
identity signing key (secp256k1)        <- NEVER touched by the storage layer
        |
        | NIP-44 self-encryption (encrypt-to-self: conv key over own pubkey)
        v
wrap_key  (32 B, random at enrollment)  <- stored ONLY as `wrapped_home_key`
        |                                   inside the kind-30078 pointer content
        v
home_key  (32 B)  = HKDF(wrap_key, salt="nostr-homed/home/v1", info=profile||epoch)
        |
        +-- object_key/nonce per blob   = HKDF(home_key, ..., sha256(plaintext))   (§2.5)
        +-- manifest_key                = HKDF(home_key, "nostr-homed/manifest/v1")
```

The signing key is never handed to the storage layer, and the storage layer never needs it — it needs `home_key`, which it gets by asking the signer to NIP-44-decrypt one small blob. That is the *entire* privileged interaction with the identity.

### 4.2 Where key material lives

- `home_key` lives in **broker memory only**, in an `mlock`ed page, for the duration of the login plus the materialization, then is wiped.
- It is **never** written to disk. Not to `/var/lib/nostr-homed`, not to the cache DB.
- The user-session sync daemon (§6) needs it too. It gets its own copy over the existing `/run/nostr-auth/auth.sock` channel, peer-credential-checked against the session's uid, into an `mlock`ed page in the daemon. It is re-requested on daemon restart; it is never persisted.
- `/run/nostr-homed/secrets` is already a tmpfs via `nh_secrets_mount_tmpfs`. The home key does **not** go there either — a tmpfs file is readable by anything running as root and shows up in a core dump. The existing `secrets.json` path stays for what it does today; portable home does not extend it.

### 4.3 The external-signer (NIP-46) case

This is the case that decides whether the design works at all, so state it plainly: **yes, we can derive a decryption key when the private key never leaves the phone**, because NIP-46 exposes NIP-44 encryption as an RPC.

Methods required from the bunker, all already wrapped in `nips/nip46/include/nostr/nip46/nip46_client.h`:

| Method | Binding | Used for |
| --- | --- | --- |
| `get_public_key` | `nostr_nip46_client_get_public_key` | the account pubkey, author filter on every relay fetch |
| `sign_event` | `nostr_nip46_client_sign_event` | the kind-30078 pointer publish, and the kind-24242 BUD-02 upload auth |
| `nip44_encrypt` | `nostr_nip46_client_nip44_encrypt` | sealing `wrapped_home_key` at enrollment and on key rotation |
| `nip44_decrypt` | `nostr_nip46_client_nip44_decrypt` | **unwrapping `wrapped_home_key` at login** — the one call the whole design hinges on |

Consequences that must be designed around, not discovered:

- **One approval, one unwrap.** The bunker may prompt the user per RPC. The login must make **exactly one** `nip44_decrypt` call (the wrap), not one per blob. That is precisely why there is a `wrap_key → home_key` indirection instead of asking the signer to decrypt each manifest node.
- **Uploads need a signature per request.** BUD-02 is a signed kind-24242 event per upload, so a sync that pushes 500 chunks wants 500 `sign_event` round-trips to a phone. Unacceptable. Mitigations, in order: (i) batch — one auth event per *server* per *session* covering a batch, if the server accepts an auth event without a matching `x` tag for each blob (BUD-02 allows `x` to be omitted for some actions, but servers vary — **must be probed, D8**); (ii) fall back to a **session delegate key**: a fresh secp256k1 key generated in the broker, whose pubkey is recorded in the pointer's `writers` set, used for Blossom auth only. The delegate can upload blobs but cannot publish the pointer (that still needs the account key), so a stolen delegate can write garbage blobs nobody references. **Recommend the delegate key — see D9.**
- **Bunker offline = no unwrap = no home.** Fallback is limited-mode (§5.3), never a failed login.
- `provider_nip46.c` already learned that a real bunker gates `sign_event` behind an explicit `connect` with a permission string (`"sign_event,sign_event:1"`). The portable-home flow needs `"sign_event,nip44_encrypt,nip44_decrypt"` added to that grant, and the connect must be treated as authoritative — a bunker that refuses the permission must surface as a clear error, not a silent later failure. Same lesson, new methods.

### 4.4 Blossom upload auth: BUD-02, not NIP-98

`src/common/blossom_client.c` builds a **NIP-98 kind-27235** header. `libhanami`'s `hanami-bud02-auth.c` builds a **BUD-02 kind-24242** header. These are different protocols and servers do not universally accept both.

**Pick BUD-02 (kind 24242).** It is what the Blossom spec actually mandates, it is what libhanami implements with a full validate path and mandatory expiration, and it carries the `x` (blob hash) binding that makes an intercepted auth event useless for uploading a *different* blob. The homed NIP-98 path in `blossom_client.c` is a pre-libhanami artifact; portable home does not extend it, and §10 files its retirement.

---

## 5. Relay and Blossom configuration

### 5.1 Relays

Resolution order for the relay set used to read and write the home pointer — **first non-empty wins**, no merging (merging produces an unbounded fan-out and makes failures undiagnosable):

1. **Per-account override** in the identity store's provider/account record (`home_relays`). Set at enrollment; this is what an org pins.
2. **The user's own NIP-65 (kind 10002) list**, fetched with the account pubkey as author filter. This is the "the user owns their config" path, and reuses `nh_fetch_profile_relays` which `nh_warm_cache` already calls.
3. **Broker defaults from `auth.conf`.** `auth_conf.c` already parses `nip46_qr_relays` and `profile_relays` as bounded CSV into `NH_AUTH_CONF_RELAYS_MAX` slots. Add `home_relays` with the same parser shape.
4. Hard-coded fallback. `nh_warm_cache` today hard-codes `wss://nos.lol` and `wss://nostr.wine`. **Portable home ships no hard-coded relay**: if nothing is configured, the account is not portable-home-capable and we return `NOT_PORTABLE`. Silently trusting a public relay with a home pointer is not a default anyone chose.

Every relay read is filtered by `authors:[<account pubkey>]` and `kinds:[30078]` and `#d`. `nh_warm_cache` already learned this the hard way — its comment says the author filter is "required… Without this, any publisher on a shared relay could inject a fake manifest". The signature is verified locally regardless; the filter is bandwidth, the verification is security.

Writes publish to **all** relays in the resolved set and count success if **≥ 1** returns `OK`. A pointer that reached one relay is recoverable; a pointer that reached zero means the generation must not be considered committed and the local state must not advance.

### 5.2 Blossom servers

Discovery, in order:

1. The `servers` array cached in the last-known pointer (lets a cold client start immediately).
2. The user's **kind 10063** list (BUD-03, per `docs/nips/B7.md`), `authors:[pubkey]`, `server` tags.
3. `blossom_servers` in `auth.conf`, same bounded-CSV parser.

All must be `https://`. `http://` is refused outright, including after redirect, matching the profile-image helper.

**Redundancy: upload to 2 servers, then request BUD-04 mirror to a 3rd.** `hanami_blossom_mirror` exists. Two independent uploads bound the cost; a mirror request is cheap and gives a third copy without a third upload. An upload is committed only when **≥ 2** servers return success for that hash — one copy of a chunk is a home that dies with one server.

Before every upload, `hanami_blossom_head` — if the hash is already present on enough servers, skip. With convergent encryption (§2.5) this makes an unchanged 40 GiB home a few thousand cheap HEADs rather than a re-upload.

### 5.3 First-login timing and the limited-mode fallback

| Stage | Budget | On exceed |
| --- | --- | --- |
| Pointer fetch (relay) | 5 s connect, 10 s total | → limited mode |
| Key unwrap (signer / NIP-46) | 30 s (a bunker may need a human tap) | → limited mode |
| Priority-set materialization | 45 s wall clock | → release session as `partial`; remainder continues in background |
| Full materialization | no PAM budget | background only |

**`pam_sm_open_session` never blocks longer than the priority budget, and never fails the login for a storage reason.** Login is an authentication decision; it was already made.

**Recommended fallback: limited mode with a retry hook.** On any provisioning failure:

1. The home directory is created (staged and installed via the *existing* `home_prepare` path with the normal skel — i.e. exactly today's behaviour, a valid empty home).
2. A marker `~/.nostr-home-limited` is written (mode 0600, owned by the user) containing the failure reason and the last-known `generation`.
3. `NOSTR_HOME_STATE=limited` is set in the PAM environment.
4. `pam_info` shows one line: *"Your portable home could not be loaded (relay unreachable). Working in limited mode — files created now will sync when the connection returns."*
5. The session daemon (§6) sees the marker and **retries with backoff**, promoting to `ready` when it succeeds.

The critical invariant: **in limited mode the sync daemon must not push.** An empty local home reconciled against a populated remote with last-writer-wins would delete the user's entire home. The marker is a hard interlock — no marker removal, no push. This is the sharpest foot-gun in the design and is called out again in §6.4 and the test plan.

The two rejected alternatives: *refuse the login* is hostile and makes a relay outage a lockout; *silently empty home* is how users lose data.

### 5.4 Caps, timeouts, retries

| Limit | Value | Enforced where |
| --- | --- | --- |
| Per-home total | 20 GiB (configurable, `max_home_bytes`) | provisioner, running total, aborts to limited mode |
| Per-file | 2 GiB | capture and apply |
| Per-manifest-node | 1 MiB decrypted | apply (refuse and fail closed) |
| Manifest tree depth | 64 | apply |
| Total entries | 500,000 | apply |
| Chunk size | 4 MiB fixed | capture |
| Inline threshold | 64 KiB | capture |
| HTTP connect / total | 5 s / 120 s per blob | fetch helper |
| Retries | 3 per blob, exponential backoff 1/2/4 s, then next server | fetcher |
| Concurrent fetches | 4 | provisioner |
| Relay event size | 64 KiB (pointer is ~1 KB; anything larger is a bug) | publish |

Every one of these is a *hard* limit that fails closed, not a warning. `identity_home.c` already demonstrates the house style: bounded depth, bounded entries, bounded bytes, `goto entry_fail` on any violation.

---

## 6. The sync loop (user session)

### 6.1 Shape

`nostr-home-syncd`, a `systemd --user` unit (`nostr-home-sync.service`, `WantedBy=default.target`), running as the user, **not** as root. It has no privileges the user doesn't have, which is correct: it only ever touches that user's files.

```
  inotify(~) --> debounce 5s --> capture --> chunk --> encrypt --> HEAD/PUT Blossom
                                                                       |
                                                    publish kind 30078 pointer (gen+1)
  relay REQ (kinds:[30078], authors:[pk], #d) --live subscription--> remote generation
                                                                       |
                                                                    reconcile
```

### 6.2 Push path

1. `inotify` watches the tree (with `IN_EXCL_UNLINK`), excluding a default ignore set: `~/.cache`, `~/.local/share/Trash`, `~/.mozilla/firefox/*/lock`, `*.tmp`, sockets, FIFOs, anything on a different `st_dev`, and a user-editable `~/.config/nostr-homed/ignore`.
2. Debounce 5 s, then coalesce into a batch. A batch is also forced every 15 minutes and on session close.
3. For each changed file: chunk, derive keys, encrypt, `HEAD` each chunk on the server set, `PUT` the missing ones (≥ 2 servers), then rebuild the affected manifest nodes up the tree to a new snapshot.
4. Publish the pointer with `generation = local_generation + 1`.
5. Only after ≥ 1 relay `OK` does the daemon advance its local recorded generation.

**Use a live relay subscription, not polling.** The daemon holds a long-lived `REQ` with `kinds:[30078], authors:[<pk>], #d:[...]` and reacts to `EVENT`; it does not wake on a timer to re-query. The repo already has a lint for exactly this class of mistake (`nostr-protocol-smells`), and a polling sync daemon would trip it on day one. Handle `EOSE` to know when backfill is done, handle `CLOSED` and `AUTH`, and reconnect with backoff rather than tearing down on timeout.

### 6.3 Pull path and merge policy

Cold start (login, or daemon start) and every remote `EVENT` with `generation > local_generation` trigger reconcile:

- Diff the remote snapshot tree against the local snapshot tree recorded at last sync (`~/.local/state/nostr-homed/snapshot.json`, which is *local cache*, rebuildable by rescanning).
- Three-way, using the last common snapshot as the base:
  - Changed remote only → fetch and apply.
  - Changed local only → queue for push.
  - Changed both → **conflict**.
- **v1 conflict policy: last-writer-wins by mtime, and the loser is preserved.** The winner takes the real path; the loser is written to `<name>.conflict-<device>-<ISO8601>` beside it. Nothing is ever silently discarded. The daemon emits a desktop notification listing conflict files.
- Deletions: a remote deletion is applied only if the local file is unchanged since the common base. A remote deletion of a locally-modified file becomes a conflict (the local file stays, plus a `.conflict-deleted` marker). **Deleting a user's modified file because another machine deleted it is not acceptable behaviour**, and mtime cannot distinguish it from a race.

Punted to Phase 5, documented as known-bad in v1:
- Concurrent edits to the *same* file on two machines (→ `.conflict` file, no content merge).
- Directory rename vs. edit inside that directory (→ may materialize as copy + conflict).
- Non-atomic multi-file application state (a browser profile mid-write) — mitigated only by the ignore list.
- Clock skew making mtime lie. Mitigated by using `generation` for snapshot ordering and mtime *only* for within-conflict tie-breaking.

### 6.4 Interlocks

- **No push while `~/.nostr-home-limited` exists.** (§5.3)
- **No push while materialization is incomplete.** `NOSTR_HOME_STATE=partial` → pull/materialize only.
- **No push if the local snapshot base is unknown** (e.g. state file lost). Instead, rescan and reconcile against the remote as if every local file were a new-file conflict — additive only, never deleting.
- A single-instance lock (`~/.local/state/nostr-homed/sync.lock`, `flock`) so two sessions on the same box on the same home cannot both push.

### 6.5 Blob GC and retention

- **Local cache** `/var/cache/nostr-homed/blobs` is LRU-evicted against a configurable quota (default 10 GiB or 10 % of the filesystem, whichever is smaller). It is pure cache: eviction is always safe.
- **Remote retention is server policy and we do not control it.** Blossom servers are free to delete blobs (quota, age, payment lapse). Therefore: (i) never delete a blob referenced by any of the last **N = 10** generations, even locally; (ii) a periodic (weekly) `hanami_blossom_head` sweep over the current snapshot's blobs re-uploads anything that has gone missing — a home that silently rots is worse than one that fails loudly; (iii) surface "your Blossom server dropped *n* blobs" as a notification.
- We do **not** call BUD-02 `DELETE` in v1. Orphaned blobs cost storage; a wrongly-deleted blob costs a file. Garbage collection of unreferenced blobs is a Phase 5 item requiring the generation-history walk to be trustworthy first.

---

## 7. Packaging and install

### 7.1 The headless-purity problem

The base login stack (`nostr-authd` + `pam_nostr.so` + `libnss_nostr`) is deliberately GLib-free, FUSE-free and dependency-light; `NOSTR_HOMED_ENABLE_CTL` exists precisely to give a headless `nostr-homectl` "decoupled from the experimental FUSE/D-Bus roaming stack". Portable home pulls in libhanami → libgit2 → libcurl → OpenSSL, and optionally FUSE. **None of that may land in the base package.**

### 7.2 Package split

| Package | Contains | Depends |
| --- | --- | --- |
| `nostr-login` (existing, unchanged) | `nostr-authd`, `pam_nostr.so`, `libnss_nostr`, `nostr-homectl` (headless) | minimal |
| **`nostr-homed-portable-home`** (new) | `libnostr-home-store.so` (chunk/encrypt/manifest/Blossom), `nostr-homed-provision` CLI, `nostr-home-fetch` (unprivileged fetch helper), `nostr-home-syncd` + its `systemd --user` unit, config sample | libhanami, libcurl, OpenSSL, libnostr |
| `nostr-homed-portable-home-fuse` (Phase 4, new) | nostrfs overlay + `nostrfs@.service` | the above + libfuse3 |

The broker loads the provisioner **by dlopen of `libnostr-home-store.so` if present**, not by link-time dependency. Base install: the symbol is absent, `PROVISION_HOME` returns `NOT_PORTABLE`, behaviour is identical to today. That keeps one broker binary across both packages and keeps the dependency out of the base package's `Depends:`.

### 7.3 New build options

```
option(NOSTR_HOMED_ENABLE_PORTABLE_HOME
  "Build the portable-home store, provisioner, and user sync daemon (requires libhanami)" OFF)
option(NOSTR_HOMED_ENABLE_PORTABLE_HOME_FUSE
  "Build the nostrfs lazy overlay for portable home (requires portable home + libfuse3)" OFF)
```

Both default `OFF`, matching every other option in the file. `NOSTR_HOMED_ENABLE_PORTABLE_HOME` requires `IDENTITY_CORE` and `AUTH_RUNTIME`, and is **independent of** `EXPERIMENTAL_ROAMING` (which stays the legacy FUSE experiment). CI must gain a portable-home-OFF `-Werror` build, mirroring `nostrc-6quj`'s SMB-off job — the whole point is that the base stack still builds clean without it.

**No `debian/` or `rpm/` changes in this document.** Packaging lands in Phase 2 under `nostrc-rb0e`'s existing release-gate discipline.

---

## 8. Security

### 8.1 Threat model

| Adversary | Learns | Cannot |
| --- | --- | --- |
| **Hostile relay** | that this pubkey has a portable home; `d` tag (so: profile name); pointer event timing and rate → *activity pattern, roughly when the user is at a keyboard*; pointer size (~constant) | read any filename, path, size, mode, or content; forge a pointer (signature); roll back past the recorded `generation` |
| **Hostile Blossom server** | blob count; padded blob sizes; upload/download timing; the uploader pubkey (BUD-02 auth) or the delegate pubkey; the *shape* of access (a burst of N fetches ≈ a login) | read any content or name; link blobs to each other (addresses are hashes of independently-keyed ciphertext); confirm the user stores a known file (per-home key, §2.5) |
| **Both colluding** | the pointer names the servers, so they can correlate an account to its blob set and count/size its files; timing correlation of "pointer updated, then these blobs appeared" gives *which* blobs changed | read content or names |
| **Hostile local user on the box** | nothing beyond normal Unix: home is `0700`, root-owned staging, blob cache holds only ciphertext | read `home_key` (broker memory, `mlock`ed, uid-checked socket); read the home (`0700`); influence provisioning (root-only, `RESOLVE_BENEATH`) |
| **Offline attacker with the disk** | the *materialized plaintext home*, because it is a real directory on a real filesystem | — |

That last row is the honest, load-bearing limitation of choosing eager local copy (§3): **the materialized home is plaintext at rest.** Encryption protects the home in *transit and on the network*, not from someone who steals the laptop. The mitigation is the platform's, not ours: LUKS. **Ship a documented requirement that portable home is only supported on an encrypted root**, and have `nostr-homed-provision` warn loudly when `/home` is not on a `dm-crypt` device. Anything else is security theatre. (FUSE in Phase 4 improves this for the lazy tail only — the hot set is still cached plaintext.)

### 8.2 Specific attacks and answers

- **Rollback / freeze.** A relay serves an old pointer, or refuses the new one, to revert the user's home. → `generation` is monotonic and recorded locally (`~/.local/state/nostr-homed/generation`, plus the broker's copy in the identity store); a lower generation is rejected with a loud error. A *freeze* (relay serves the current pointer forever while accepting no writes) is detectable only as "my publishes are not being seen by other devices" — surfaced as a sync-stalled notification after 3 failed publish rounds. Multi-relay publish (§5.1) makes a single-relay freeze ineffective.
- **Chunk substitution.** → A chunk is addressed by `sha256(ciphertext)`; the fetcher **verifies the hash before decryption** and the AEAD tag after. A server that returns different bytes fails the hash check, and we try the next server.
- **Manifest substitution / cross-home splice.** → Manifest nodes are AEAD-sealed under a key derived from `home_key`. A node from another home does not decrypt. A node from an *older generation of the same home* does decrypt — so a snapshot node embeds its own `generation`, and the applier refuses a child node whose generation exceeds the root's.
- **Malicious manifest (decompression bomb / path traversal / symlink escape).** → Names are validated on apply: no `/`, no `.`/`..`, no NUL, ≤ 255 bytes, ≤ 4096-byte reconstructed path. All writes are `openat`-relative to the staging descriptor under `RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV` — the same primitive `identity_home.c:beneath()` already uses. Symlinks in the manifest are created with `symlinkat` only after their target is verified to be relative and non-escaping. Entry, depth, and byte caps (§5.4) bound the bomb.
- **SSRF via the Blossom server list.** The server list comes from a relay event, i.e. from the network. → All fetches go through an **unprivileged helper binary** (`nostr-home-fetch`), a direct clone of `nostr-homed-profile-image.c`'s hardening: https-only protocols *and* redirect-protocols, `MAXREDIRS 3`, `OPENSOCKETFUNCTION` + `SOCKOPTFUNCTION` calling the shared `nh_profile_ssrf_check_sockaddr` (which should be promoted to a shared `nh_net_ssrf_check_sockaddr`), `DNS_CACHE_TIMEOUT 0` to defeat DNS rebinding, `NOPROXY "*"`, `UNRESTRICTED_AUTH 0`, `MAXFILESIZE` plus a hard in-callback cap, and distinct exit codes (65 = SSRF refusal, 66 = network/size). The broker never dials a URL itself.
- **Set-uid injection.** → mode masked to `0777` on apply, `07000` stripped unconditionally. Non-regular, non-dir, non-symlink entries refused.
- **Quota exhaustion / disk fill.** → running byte total against `max_home_bytes` and a `statvfs` free-space floor (refuse to start if free space < home size × 1.2).

### 8.3 Metadata-hiding baseline (required, not optional)

1. Filenames and paths are encrypted — they exist only inside sealed manifest nodes (§2.3).
2. Blob sizes are padded to buckets: powers of two up to 64 KiB, then 64 KiB increments up to 4 MiB, so a chunk is always one of a small set of sizes. `szb` in the manifest records the bucket for accounting.
3. Manifest nodes are padded to 4 KiB multiples, so directory-size leakage is coarse.
4. Uploads within a batch are shuffled and jittered so ordering does not reveal tree structure.
5. Blob addresses are unlinkable across homes (per-home key, §2.5).

Not hidden, and documented as such: total blob count, total padded size, update frequency, and the fact that a portable home exists.

---

## 9. Test plan (no phone required)

Everything below runs on CI with the fixtures that already exist.

### 9.1 Existing fixtures to reuse

- `gnome/nostr-homed/tests/integ/fake_relay_fixture.py` — mock relay.
- `gnome/nostr-homed/tests/integ/fake_blob_server.py` and `tests/integration/fake_blossom.py` — mock blob/Blossom servers.
- `tests/integration/mock_signer.c` — a signer with a known key, so NIP-44 encrypt/decrypt and BUD-02 signing are exercised without a bunker.
- `tests/integration/seed_nip46_authority.c`, `qr_signer_standin.c` — NIP-46 path without a phone.
- `libhanami/tests/test_hanami_blossom_client.c`, `test_hanami_bud02_auth.c` — the Blossom layer already has coverage; extend rather than duplicate.

`fake_blossom.py` must be extended to support BUD-02 `x`-tag validation, BUD-04 `/mirror`, `HEAD`, and fault injection (drop, corrupt, truncate, 429, slow-loris).

### 9.2 Unit tests (no network)

| Test | Asserts |
| --- | --- |
| `test_home_chunker` | fixed 4 MiB split; inline threshold at 64 KiB; empty file; file of exactly 4 MiB; file of 4 MiB + 1 B; 2 GiB + 1 B refused |
| `test_home_crypto` | HKDF vectors are stable; convergence (same plaintext → same address); non-convergence across two `home_key`s; nonce determinism; tamper a byte → AEAD failure; wrap/unwrap round-trip |
| `test_home_manifest` | serialize/parse round-trip; unknown fields preserved; depth > 64 refused; entry count > 500k refused; node > 1 MiB refused |
| `test_home_manifest_hostile` | `..`, `/`, NUL, 300-byte name, absolute symlink, escaping relative symlink, device node, setuid mode, negative size, generation > root — **each refused, none written** |
| `test_home_apply_beneath` | apply into a staging fd with a symlink planted at each path component → refused, nothing written outside |
| `test_home_reconcile` | three-way diff matrix: local-only, remote-only, both-changed, remote-delete-of-unchanged, remote-delete-of-modified; conflict file naming; **never deletes a locally-modified file** |
| `test_home_generation` | lower generation rejected; equal accepted as no-op; gap accepted; local record advances only after publish OK |
| `test_home_caps` | each of §5.4's limits trips and fails closed with the right error |

### 9.3 Integration tests

1. **`run_portable_home_provision.sh`** — fake relay + fake Blossom + mock signer. Seed a fixture home (~200 files, one 12 MiB binary, nested dirs, a symlink, a 0-byte file, a name with spaces and UTF-8) → capture → publish → wipe → provision into a fresh staging dir → **byte-for-byte compare the tree**, plus mode bits and mtimes.
2. **`run_portable_home_offline.sh`** — relay unreachable. Assert: login succeeds, home exists with skel, `~/.nostr-home-limited` present, `NOSTR_HOME_STATE=limited`, **the sync daemon does not push**, and relay recovery promotes to `ready` and then pushes.
3. **`run_portable_home_partial.sh`** — Blossom drops 30 % of requests. Assert: priority set completes, session released `partial`, background completes after retries, no partial file is left at its final path (write to `.part`, `fsync`, `renameat2`).
4. **`run_portable_home_hostile_server.sh`** — Blossom returns corrupted bytes for 10 % of blobs. Assert: hash check catches every one before decryption, the next server is tried, and no corrupted byte reaches the home.
5. **`run_portable_home_ssrf.sh`** — a kind-10063 list advertising `http://`, `https://127.0.0.1`, `https://169.254.169.254`, `https://10.0.0.1`, and a public host that redirects to loopback. Assert `nostr-home-fetch` exits 65 for each and the broker never opens a socket.
6. **`run_portable_home_two_boxes.sh`** — two staging dirs as two "machines" against one fake relay. Edit different files on each → both converge. Edit the same file on each → one wins by mtime, the other becomes `.conflict-<device>-<ts>`, **both contents survive**.
7. **`run_portable_home_rollback.sh`** — relay replays generation *n−5*. Assert rejection and a logged error.
8. **`run_portable_home_nip46.sh`** — the `qr_signer_standin` bunker. Assert exactly **one** `nip44_decrypt` call for a full login, and that upload auth uses the delegate key (D9), not the account key.

### 9.4 Live end-to-end demo (maintainer, manual, Phase 2 exit gate)

1. On the pinned Linux lab box (`nostrc-rb0e.1`), build with `-DNOSTR_HOMED_ENABLE_PORTABLE_HOME=ON`.
2. `nostr-homed-provision enroll --user biz --profile personal --relays wss://<your relay> --blossom https://<real blossom>` with the maintainer's own account, local vault provider first.
3. Populate a small home (< 200 MiB — do not test a real home against a public Blossom server on the first run).
4. `nostr-homed-provision push`, confirm the kind-30078 pointer on the relay with an independent client, and confirm blobs with `curl -I https://<blossom>/<hash>`.
5. Wipe `/home/biz` on a **second** box, log in via GDM, confirm the home materializes and the desktop starts clean.
6. Repeat step 5 with the NIP-46 bunker provider and confirm exactly one approval prompt.
7. Record wall-clock timings (pointer fetch, unwrap, priority set, full) in the bead.

---

## 10. Phased rollout

**Phase 0 — design freeze.** This document. Exit: the maintainer answers §12's open decisions. *(bead `nostrc-h10m`)*

**Phase 1 — library primitives.** `libnostr-home-store`: key schedule, chunker, NIP-44 blob seal, manifest node format, Blossom client wrapper over `hanami_blossom_*`, the OID-free address index. No PAM, no broker, no daemon. Exit: §9.2 unit tests green; §9.3 test 1 green.

**Phase 2 — provisioner.** `nostr-homed-provision` CLI (enroll / status / pull / push / verify), the broker-side `PROVISION_HOME` command, the `home_prepare` labeler integration, `nostr-home-fetch`, and `pam_sm_open_session`. Exit: §9.3 tests 1–5 green; §9.4 live demo done; packaging split landed.

**Phase 3 — sync daemon.** `nostr-home-syncd`, inotify capture, live relay subscription, reconcile, conflict files, local blob cache + LRU. Exit: §9.3 tests 6–8 green; a week of maintainer dogfooding across two boxes.

**Phase 4 — FUSE overlay.** nostrfs re-scoped as a lazy overlay for manifest-marked cold directories on top of a real home. Exit: a GNOME session runs a week with a lazy `~/Archive`.

**Phase 5 — UX and hardening.** Greeter/session progress, quota UI, conflict resolution UI, key rotation (`key_epoch` > 1), blob GC, CDC chunking, retire the NIP-98 path in `blossom_client.c`.

### 10.1 Implementation checklist by phase

**Phase 1**
- `gnome/nostr-homed/src/home/home_key.c|.h` — HKDF schedule, wrap/unwrap, `mlock`ed key handle.
- `src/home/home_chunk.c|.h` — fixed chunker, inline threshold.
- `src/home/home_seal.c|.h` — NIP-44 blob seal/open, derived nonce, padding buckets.
- `src/home/home_manifest.c|.h` — node serialize/parse, Merkle build, hostile-input validation. *(new format; `include/nostr_manifest.h` stays for the legacy roaming path and is not extended)*
- `src/home/home_store.c|.h` — put/get object over `hanami_blossom_*`, multi-server quorum, HEAD-before-PUT, BUD-02 auth.
- `src/home/home_pointer.c|.h` — kind-30078 build/parse/verify, generation rules.
- `tests/unit/test_home_{chunker,crypto,manifest,manifest_hostile,caps}.c`
- CMake: `NOSTR_HOMED_ENABLE_PORTABLE_HOME`.

**Phase 2**
- `src/home/home_provision.c` — materialize into a descriptor; priority set; resume; `.part` + `renameat2`.
- `src/home/home_apply.c` — `openat2`-beneath writer (lift the `beneath()` helper out of `identity_home.c` into a shared `identity_internal` export rather than copying it).
- `src/identity/identity_home.c` — **no logic change**; portable home arrives as an `nh_identity_home_options.label` callback.
- `src/auth/auth_broker.c` + `nostr_auth_protocol.h` — `PROVISION_HOME` request/response, dlopen of the store lib, uid-checked `GET_HOME_KEY`.
- `src/pam/pam_nostr_broker.c:511` — replace the stub per §3.3; `pam_putenv NOSTR_HOME_STATE`.
- `src/home/nostr-home-fetch.c` — unprivileged SSRF-guarded fetcher (clone of `nostr-homed-profile-image.c`).
- `src/common/nh_net_ssrf.c` — promote `nh_profile_ssrf_check_sockaddr` to shared.
- `src/home/nostr-homed-provision.c` — CLI.
- `src/auth/auth_conf.c` — `home_relays`, `blossom_servers` keys; `config/auth.conf.sample`.
- `tests/integration/run_portable_home_{provision,offline,partial,hostile_server,ssrf}.sh`; extend `fake_blossom.py`.

**Phase 3**
- `src/home/home_watch.c` — inotify + debounce + ignore list.
- `src/home/home_reconcile.c` — three-way diff, conflict files, delete policy.
- `src/home/home_cache.c` — `/var/cache/nostr-homed/blobs` LRU.
- `src/home/nostr-home-syncd.c` + `packaging/systemd/nostr-home-sync.service` (user unit).
- `tests/integration/run_portable_home_{two_boxes,rollback,nip46}.sh`.

**Phase 4** — `src/fs/nostrfs.c` lazy-overlay mode; `NOSTR_HOMED_ENABLE_PORTABLE_HOME_FUSE`; reuse `nh_open_session`'s mount-verify invariant.

**Phase 5** — greeter progress, key rotation, GC, CDC, NIP-98 retirement.

---

## 11. Decisions

| # | Decision | Options | Recommendation |
| --- | --- | --- | --- |
| **D1** | Wire format for the home pointer | (a) NIP-78 kind 30078 addressable, NIP-44 self-encrypted; (b) a new dedicated kind (e.g. 30081); (c) NIP-51-style list kind | **(a) kind 30078**, `d = nostr-homed.home.v1:<profile>`. It exists, it is addressable, NIP-78 explicitly blesses this use. A new kind buys nothing until we want interop, and interop is not a v1 goal. |
| **D2** | Where the bulk manifest lives | (a) whole manifest in the relay event (today's `nh_manifest`); (b) Merkle manifest nodes as Blossom blobs, pointer on relay | **(b).** (a) does not survive a real home — relays cap event size, and it leaks every filename to the relay. |
| **D3** | Manifest substrate | (a) custom Merkle manifest; (b) libgit2 + libhanami ODB-over-Blossom | **(a) for v1.** libhanami's OID↔Blossom index is *local state*, which defeats cold login on a new box (§2.6). Use `hanami_blossom_*` (client) now; revisit the git layer at Phase 4+. |
| **D4** | Blob encryption / dedup | (a) random-nonce NIP-44 (no dedup); (b) per-home convergent: key+nonce derived from `HKDF(home_key, sha256(plaintext))`; (c) plaintext blobs | **(b)**, because (a) makes every sync a full re-upload and (c) is indefensible. **(b) needs an explicit crypto review before Phase 1** — deterministic AEAD is correct here but is the one place a mistake is fatal. |
| **D5** | Where materialization writes | (a) directly into `/home/<user>`; (b) into the `home_prepare` staging descriptor, installed by the existing `renameat2` | **(b).** Inherits `RESOLVE_BENEATH|NO_SYMLINKS|NO_XDEV`, atomic install, crash recovery, and the never-delete-on-ambiguity invariant for free. (a) re-implements all of it, worse, in a privileged process. |
| **D6** | Chunking | (a) fixed 4 MiB; (b) content-defined (FastCDC) | **(a).** Testable, no tuning surface. CDC is a Phase-5 optimisation, not a v1 requirement. |
| **D7** | Blossom upload auth | (a) NIP-98 kind 27235 (what `blossom_client.c` does); (b) BUD-02 kind 24242 (what libhanami does) | **(b).** It is what the Blossom spec mandates, libhanami implements it with validation and mandatory expiration, and the `x` tag binds the auth to the specific blob. Retire (a) in Phase 5. |
| **D8** | Can one BUD-02 auth event cover a batch of uploads? | (a) yes, one per server per session; (b) no, one per blob | **Unknown — must be probed** against real Blossom servers in Phase 1. Assume (b) and design the delegate key (D9) so the answer doesn't gate the schedule. |
| **D9** | Who signs Blossom uploads under NIP-46 | (a) the account key via the bunker (one prompt per blob — unusable); (b) a broker-generated session delegate key recorded in the pointer | **(b).** The delegate can upload blobs but cannot publish the pointer, so its compromise yields unreferenced garbage, not a modified home. |
| **D10** | Materialization model | (a) eager local copy; (b) FUSE-on-demand | **(a) for v1**, (b) as a Phase-4 lazy overlay for cold directories. (b) does not remove any of (a)'s hard problems and adds GDM/FUSE/offline ones. |
| **D11** | Network-failure behaviour at login | (a) refuse login; (b) silent empty home; (c) limited mode + notice + retry | **(c)**, with the hard interlock that **limited mode never pushes** (§5.3). (a) turns a relay outage into a lockout; (b) is how homes get deleted. |
| **D12** | Conflict policy | (a) LWW by mtime, loser discarded; (b) LWW by mtime, loser preserved as `.conflict`; (c) content merge | **(b).** Never discard user data on a heuristic. (c) is not a filesystem's job. |
| **D13** | Relay-set resolution | (a) merge all sources; (b) first non-empty of account-record → NIP-65 → `auth.conf` → none | **(b)**, and **ship no hard-coded relay fallback** (unlike `nh_warm_cache` today). An unconfigured account is `NOT_PORTABLE`, not "publish to nos.lol". |
| **D14** | Blossom redundancy | (a) 1 upload; (b) 2 uploads + 1 BUD-04 mirror; (c) 3 uploads | **(b).** Commit only at ≥ 2 confirmed copies. |
| **D15** | Packaging | (a) fold into `nostr-login`; (b) separate `nostr-homed-portable-home`, broker `dlopen`s the store | **(b).** Keeps libhanami/libgit2/libcurl out of the base login stack's dependency closure and preserves headless purity. |
| **D16** | Plaintext-at-rest | (a) accept, require LUKS, warn if absent; (b) per-file encrypted local store behind FUSE; (c) fscrypt on `/home/<user>` keyed from `home_key` | **(a) for v1** with a loud warning and a documented requirement. **(c) is the interesting one** and should be evaluated in Phase 4 — `fscrypt` with a key derived from the identity would close this properly without FUSE. |
| **D17** | uid/gid in the manifest | (a) store and restore; (b) omit; chown to the local account on apply | **(b).** UIDs are machine-local (`nh_cache_map_npub_to_uid`); a stored uid is at best noise and at worst a privilege bug. |
| **D18** | Group/shared homes (Marmot/MLS) | (a) design in now; (b) out of scope, leave a hook | **(b).** `key_epoch` + `wrapped_home_key` indirection means `home_key` can later come from an MLS exporter secret without a format change. |

---

## 12. Open decisions the maintainer must confirm before Phase 1

Ordered by how much they would cost to reverse later.

1. **D4 — convergent encryption.** Deterministic key+nonce derived from the plaintext hash is what makes dedup and cheap re-sync possible, and it is the one choice where a subtle error is unrecoverable. Confirm the construction, or accept random nonces and no dedup. *This blocks Phase 1.*
2. **D16 — plaintext at rest.** Is "portable home requires an encrypted root, and we warn if it isn't" an acceptable v1 security posture? If not, `fscrypt` (D16c) moves from Phase 4 to Phase 1 and changes the provisioner substantially.
3. **D10 — eager copy over FUSE.** Confirm the v1 model, because Phases 1–3 are shaped by it.
4. **D11 — limited mode.** Confirm that a login with an unreachable relay yields an empty-but-working home with a notice, rather than a refused login.
5. **D13 — no hard-coded relay fallback.** Confirm that an account with no configured relays is simply not portable, and that `nh_warm_cache`'s hard-coded `nos.lol`/`nostr.wine` default is *not* the pattern to follow here.
6. **D9 — session delegate key.** Confirm that a broker-generated key uploading blobs on the user's behalf is acceptable, given it cannot publish the pointer.
7. **D1 — kind 30078 and the public `d` tag.** Confirm that "this pubkey has a nostr-homed portable home" being publicly observable is acceptable.
8. **Quotas.** Are 20 GiB per home / 2 GiB per file / 500k entries the right v1 defaults for the lab and for the intended users?
9. **Scheduling.** This epic depends on `nostrc-rb0e`, which still has open VM/installed release gates. Confirm portable home does not start until those land, or explicitly authorise parallel work.
10. **D8 — batch BUD-02 auth.** Needs an empirical probe against real Blossom servers. Who runs it, and against which servers?

---

## 13. References

- In-tree NIPs: `docs/nips/78.md` (kind 30078), `docs/nips/B7.md` (Blossom media, kind 10063 / BUD-03), `docs/nips/44.md`, `docs/nips/46.md`, `docs/nips/65.md`, `docs/nips/98.md`.
- Blossom BUDs: BUD-01 (GET/HEAD by sha256), BUD-02 (kind 24242 auth, PUT/DELETE), BUD-03 (kind 10063 server list), BUD-04 (mirror) — implemented in `libhanami/src/hanami-blossom-client.c` and `hanami-bud02-auth.c`.
- Prior in-tree designs: `docs/designs/nip46-qr-login-greeter.md`, `docs/designs/packaging-plan-debian-fedora.md`, `docs/designs/nostrdb-retention-eviction-policy.md`.
- Beads: `nostrc-h10m` (this epic), `nostrc-rb0e` (Samba/packaging, blocks), `nostrc-nxpb` (identity authority / local homes), `nostrc-nxpb.7`/`.8` (roaming safety fixes whose invariants §3.4 preserves), `nostrc-6quj` (feature-off `-Werror` CI precedent).
