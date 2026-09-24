# nostrfs × portable-home — the Phase-4 read-through overlay

**Status:** Design (Phase 4 / P4-D). **No implementation in this document.**
**Bead:** `nostrc-h10m` (E-portable-home epic); implementation bead filed as the P4-I task, depends on `nostrc-p6qp` (Phase 3 syncd) and `nostrc-h10m`.
**Reads:** `docs/designs/home-from-relay.md` (§2 data model, §3 materialization / D3 / D10, §4 keys, §5.3 limited mode, §6.5 cache + retention, §7.2 packaging, §8 threat model), `docs/designs/porthome-crypto-spec.md`.
**Date:** 2026-09-24

> Decision numbering in this document (**D1…D16**) is local. Where the parent design's
> decisions are referenced they are written **HFR-D3**, **HFR-D10**, etc.

---

## 0. Executive summary

Phase 3 shipped an eager local `$HOME` plus a user-scope sync daemon
(`nostr-home-syncd`) that owns capture, push, pull/reconcile, a content-addressed
blob cache, an N=10 generation pin ring, and a weekly HEAD sweep. Phase 4 adds a
**read-only FUSE 3 mount, `nostr-home-fuse`, that presents the current portable-home
snapshot as a browsable filesystem and fetches file content on demand** from the
local blob cache, falling back to Blossom.

The mount is:

- **read-only** — every mutating operation returns `EROFS`. `syncd` remains the
  sole write path (HFR-D10, HFR-D3 stand);
- **namespace-sourced from `snapshot.json`**, not from the relay. It never opens a
  websocket, never signs, never publishes, never holds the account key;
- **content-sourced from `nh_syncd_cache` → `nh_porthome_blossom`**, sha-verified
  before decryption, decrypted in-process with `home_key`;
- **a separate process from `syncd`** with its own lock, its own systemd user unit,
  and its own copy of `home_key`;
- **additive and gated**: `NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL=OFF` by
  default, shipping a *new* binary. The existing `nostrfs` binary, its
  `nostrfs@.service` units and its three integration scripts are **not modified and
  not deleted**.

The honest caveat, stated up front: **with a complete eager copy the overlay is
largely redundant with `$HOME`.** Its payoff arrives when the reconciler is allowed
to *skip* materializing user-marked cold subtrees, at which point the overlay is the
only way to read them. That policy (`~/.config/nostr-homed/lazy`) is specified in
§10 and is the single largest open decision for the maintainer (§15, item 1).

---

## 1. Ground truth — what exists today

Read 2026-09-24 on `design/nostrfs-porthome-overlay` (merge `d3d5afb7`).

| Area | File | Reality |
| --- | --- | --- |
| Existing FUSE | `gnome/nostr-homed/src/fs/nostrfs.c` (928 lines) | FUSE 3 high-level ops. Namespace from a **flat `nh_manifest`** JSON blob stored in the NSS cache under `settings.manifest.<ns>`. Content from an unencrypted CAS at `/var/cache/nostrfs/<uid>/<cid>` where `cid = sha256(plaintext)`. Writeback commits via a libgo actor, uploads through `nh_blossom_upload` (**NIP-98 kind-27235** auth — the path HFR-D7 retires), publishes **kind 30081** (not 30078), hard-codes `wss://nos.lol,wss://nostr.wine`. **It does not reference `porthome` at all**: no `nh_porthome_*`, no `nh_syncd_*`, no encryption, no chunking. A read miss with no CAS entry returns the literal string `CID:<hex>` as file content. |
| Units | `systemd/nostrfs@.service`, `systemd/user/nostrfs@.service` | Both mount `/home/%i` with `--writeback`. Both set `ProtectHome=read-only` **and** `AmbientCapabilities=CAP_SYS_ADMIN` (the user-scope one cannot grant that). `BindsTo=nostr-homectl.service`. Legacy roaming stack. |
| Tests | `tests/integration/run_nostrfs_{basic,writeback,writeback_fake_blossom}.sh` | Skip-if-absent shells; write to `/etc/nss_nostr.conf` and `/var/cache/nostrfs`. Exercise the legacy CAS/manifest path only. |
| Build gate | `CMakeLists.txt:668` | `nostrfs` is built under `EXPERIMENTAL_ROAMING` when FUSE3 is found; links `nostr_homed_common` only. |
| Crypto | `include/nh_porthome_crypto.h`, spec §3–§5 | `home_key = HKDF(seed)`; per-chunk `nonce = HKDF(salt, home_key, SHA256(pt))`, `key = HKDF(salt, home_key, nonce)`; sealed blob `version‖nonce‖ct‖tag` (29 B overhead); address = `SHA256(sealed)`; strict decode, **address verified before decrypt**. D4 APPROVED 2026-09-23. |
| Manifest | `include/nh_porthome_manifest.h` | Flat entry list, CBOR, `path_enc` is `HMAC-SHA256(name_key, component)[0..24]` hex per component — **one-way**. There is no plaintext filename anywhere in the manifest. Per-entry `chunks[]` of `{sha256, size, chunk_key_id}`; `chunk_key_id != 0` is refused. |
| Blossom | `include/nh_porthome_blossom.h` | `_fetch(sha_hex, &buf, &len)` verifies content-sha and fails over across an ordered HTTPS-only server list; `_has()`, `_upload()`. 3 retries, 1/2/4 s backoff, 16 MiB default cap. |
| Cache + pins | `include/nh_syncd_cache.h` | `nh_syncd_cache_{open,has,get_path,put,pin,unpin,is_pinned,sweep}` at `$XDG_CACHE_HOME/nostr-homed/blobs`, 2-char sharded, 0600, LRU by atime, pinned blobs never evicted. `nh_syncd_pin_ring_{open,promote,promote_from_snapshot,effective_pins,apply}` persisted at `$XDG_STATE_HOME/nostr-homed/pinned.json`, N=10. |
| Snapshot | `include/nh_syncd.h` schema comment | `$XDG_STATE_HOME/nostr-homed/snapshot.json` v1 holds `generation`, `root`, `d_tag`, `root_id_hex`, and `files{ "<rel-plaintext-path>": {kind, mode, uid, gid, mtime_ns, size, content_hash_hex, chunk_addrs_hex[], symlink_target} }`. **Plaintext paths and chunk addresses, locally, at the current generation.** A sibling `generation` file holds the same number as one ASCII line, rewritten atomically. |
| Key handoff | `src/auth/auth_porthome.c:1010+`, `src/porthome-syncd/nostr-home-syncd.c:107+` | Broker drops 64 hex chars at `/run/nostr-auth/session/<uid>/home_seed`, mode 0600, `chown(uid,uid)`, tmp+rename. Syncd `read_seed_from_broker_drop()` reads it **once, unlinks it**, mlocks the value, and falls back to `NOSTR_HOMED_SYNCD_SEED_HEX`. Override path: `NOSTR_HOMED_SYNCD_SEED_FILE`. |
| Syncd lock | `nh_syncd_lock_acquire(state_dir)` | `flock` on `$XDG_STATE_HOME/nostr-homed/sync.lock`; single instance per home. |
| Ignore | `nh_syncd_ignore_*` | `fnmatch(FNM_PATHNAME|FNM_LEADING_DIR)` on `$HOME`-relative paths + a static set + **`st_dev != st_dev($HOME)` ⇒ ignored**. |

Two consequences shape everything below:

1. **`path_enc` is a keyed hash and cannot be inverted.** A FUSE layer cannot
   `readdir` a porthome manifest. It *can* answer "give me the entry for
   `Documents/notes.txt`" by hashing the query. Therefore the mount's namespace must
   come from somewhere else.
2. **`snapshot.json` is exactly that somewhere else.** It already carries plaintext
   relative paths, kinds, modes, sizes, mtimes and per-file chunk address lists at
   the generation `syncd` last reconciled. It is user-owned, `$HOME`-local,
   rebuildable, and updated atomically. The mount reads it and nothing else for
   metadata.

---

## 2. D1 — FUSE role: read-through overlay, `syncd` keeps the write path

### 2.1 The three options

**(a) READ overlay under the eager home.** The mount is a lazy read-through view of
the current snapshot. Writes go to the plain `$HOME`; `syncd` batches them. *(the
plan's §3 stance, HFR-D10's "Phase-4 lazy overlay for cold directories")*

**(b) FULL content-on-demand FUSE.** Replaces eager materialization for
large-home / low-disk accounts. Reads decrypt on demand; writes go through a FUSE
upcall into the syncd push path.

**(c) WRITE cache in front of the batcher.** nostrfs intercepts writes into a
per-session staging area that `syncd` then batches.

### 2.2 Recommendation: **(a)**

(b) is rejected for v1 on the same grounds HFR §3.2 rejected FUSE-as-`$HOME`, and
the grounds have not improved: a FUSE `$HOME` must be live before `pam_systemd` and
GDM touch it, `fusermount3` unmount races every lingering session process, and the
write path would need a coherent write-back cache *plus* everything `syncd` already
does. Nothing in Phase 3 made that cheaper.

(c) is rejected because it introduces a second writer of the same bytes. `syncd`'s
capture is `inotify` over `$HOME`; a staging area the batcher must additionally
drain creates two orderings for one change, and the reconciler's three-way diff has
exactly one base. The conflict semantics of HFR §6.3 would have to be re-derived.
That is a Phase-5 conversation at the earliest, and only if (a) proves the plumbing.

(a) survives because it can only *add* readable bytes. Every failure mode
degrades to "that file is not readable right now" — never to "that file was lost",
never to "the desktop will not start". It reuses `nh_syncd_cache`,
`nh_porthome_blossom` and `nh_porthome_crypto` unchanged, and it needs no new event
kind, no new manifest field, and no relay access.

### 2.3 What the mount covers, and what the user sees

**D2 — mount point: `$HOME/Portable`, a real empty directory in `$HOME`.**

```
$HOME/                       ext4/btrfs, st_dev = X   — syncd captures this
  Documents/ …                                          (eager copy, plaintext)
  Portable/                  nostrfs,     st_dev = Y   — syncd IGNORES this (xdev)
    Documents/ …                                         read-only snapshot view
```

Why this convention wins:

- **The xdev trap becomes the feature.** `nh_syncd_ignore` already refuses anything
  whose `st_dev` differs from `$HOME`'s, so a live mount inside `$HOME` is invisible
  to capture. The mount is a *view* of already-captured content; capturing it again
  would be a fixpoint bug. We rely on the existing rule rather than adding one.
- **When not mounted, it is an empty directory**, which `syncd` captures as an empty
  directory and which reconcile re-creates on every other machine — i.e. the
  mountpoint provisions itself on a fresh box. That is the desired behaviour and it
  costs one manifest entry.
- **Belt and braces:** the FUSE package ships
  `/usr/share/nostr-homed/ignore.d/portable` containing `Portable/`, and the mount
  process refuses to start if `readlink`/`stat` shows the mountpoint is a symlink or
  is already a mountpoint owned by another process.

Alternatives rejected: `$XDG_RUNTIME_DIR/nostr-homed/mnt` plus a `$HOME` symlink
(the symlink gets captured and points at a machine-local runtime path — actively
wrong on the second box); bind-shadowing parts of `$HOME` (an overlay whose lower
layer is the thing `syncd` watches is a coherence problem we have no reason to buy).

**D3 — user perception: an explicit, visible "cloud folder", not an invisible
overlay.** `Portable` shows in Nautilus as a normal folder that happens to live on
another filesystem. We mount with `-o fsname=nostr-home,subtype=porthome` so
`/proc/self/mounts`, `df`, and GNOME's volume monitor render something meaningful,
and `-o ro,nosuid,nodev,noatime`. We do **not** add a `.hidden` entry, a
`user-dirs.dirs` entry, or a GNOME-specific handler: an unexplained magic folder is
worse than a plain one. The name is configurable
(`NH_FUSE_MOUNTPOINT` / `~/.config/nostr-homed/fuse.conf: mountpoint=`) for anyone
who wants `Archive`.

---

## 3. D4 — Local cache seam and the read path

### 3.1 Tiers

A `read(path, off, len)` resolves in four tiers. The first tier that answers wins.

| Tier | Source | Condition |
| --- | --- | --- |
| **0** | the real file at `$HOME/<rel>` | `NOSTR_HOME_STATE=ready` **and** the snapshot entry's `content_hash_hex` is non-empty **and** the local file's `st_size`/`mtime_ns` match the snapshot entry. Serves with `pread` — no decrypt, no fetch. |
| **1** | plaintext chunk LRU (in-process, memory only) | chunk already decrypted this session |
| **2** | `nh_syncd_cache_get_path(cache, chunk_sha)` | blob present locally. Read, `nh_porthome_decrypt_chunk`, insert into tier 1. |
| **3** | `nh_porthome_blossom_fetch(bl, chunk_sha, …)` | wrapper verifies content-sha before returning; then `nh_syncd_cache_put` (re-verifies), decrypt, insert into tier 1. |

Tier 0 is disabled unless the home is `ready`, because during `partial` or `limited`
the local file may be a truncated or absent materialization and the snapshot entry
cannot be trusted to describe it (HFR §5.3).

Tier 1 exists because chunks are 4 MiB and a typical `read` is 4–128 KiB. Without it,
a sequential `cat` of a 4 MiB file would decrypt the same chunk hundreds of times.
Default budget 64 MiB, LRU by last use, `NH_FUSE_CHUNK_CACHE_BYTES` to override.
**Tier 1 is never written to disk** — it holds plaintext, and the mount is the one
process in the stack that has no business persisting plaintext.

Pinned blobs (§3.3) can only miss tier 2 if the cache file was deleted out from
under us; that is treated as a tier-3 fetch, not as an error.

### 3.2 Seam the FUSE handlers need

The handlers must not talk to four libraries directly. One adapter module,
`src/fs-porthome/nh_fuse_source.{c,h}`, owns the tier ladder:

```c
typedef struct nh_fuse_source nh_fuse_source;

typedef struct {
    const char           *home_dir;         /* $HOME, for tier 0            */
    bool                  tier0_enabled;    /* false unless state == ready  */
    nh_syncd_cache       *cache;            /* borrowed, tier 2             */
    nh_porthome_blossom_t*blossom;          /* borrowed, tier 3             */
    const uint8_t         home_key[NH_PORTHOME_KEY_LEN];  /* mlock'd page   */
    size_t                chunk_cache_bytes;/* 0 -> 64 MiB (tier 1)         */
    long                  offline_budget_ms;/* 0 -> 15000 (§6)              */
    void (*on_miss)(void *ud, const char *rel_path, int errcode);
    void                 *on_miss_ud;
} nh_fuse_source_cfg;

int  nh_fuse_source_open (const nh_fuse_source_cfg *cfg, nh_fuse_source **out);
void nh_fuse_source_close(nh_fuse_source *s);          /* cleanses tier 1   */

/* Tier 2/3 only. `sha256_hex` is a chunk address from snapshot.json.
 * On success *out_pt is a borrowed pointer into the tier-1 LRU, valid
 * until the next call on this source from the same thread (single-
 * threaded loop in v1; see D13). Returns 0, or:
 *   -ENOENT  no server has it            -EIO   sha mismatch / AEAD fail
 *   -EIO     offline budget exhausted    -ENOMEM
 */
int  nh_fuse_source_chunk(nh_fuse_source *s,
                          const char sha256_hex[65],
                          const uint8_t **out_pt, size_t *out_len);

/* Full read service: resolves tier 0 first, else walks `chunks[]`.      */
ssize_t nh_fuse_source_pread(nh_fuse_source *s,
                             const nh_fuse_entry *e,   /* from snapshot   */
                             void *buf, size_t len, off_t off);

/* Counters for the status file and the notification throttle. */
void nh_fuse_source_stats(const nh_fuse_source *s,
                          uint64_t *hits_local, uint64_t *hits_cache,
                          uint64_t *fetches, uint64_t *misses);
```

`nh_fuse_source_chunk` is the only function that ever holds `home_key` at a call
boundary; it is also the only place `nh_porthome_decrypt_chunk` is invoked.

### 3.3 Pin ring

At mount, after loading the snapshot:

```c
nh_syncd_pin_ring_open(nh_syncd_cache_default_pin_path(), &ring);
nh_syncd_pin_ring_apply(ring, cache);      /* every hash in the last N=10 gens */
nh_syncd_pin_ring_close(ring);             /* the cache keeps the refcounts    */
```

and again on every generation reload (§7.3). The mount **never** calls
`nh_syncd_pin_ring_promote*` — promotion is `syncd`'s job at push/pull commit
(Phase-3 wrap-up item W(1)) and a second promoter would corrupt the ring's ordering.
The mount also never calls `nh_syncd_cache_sweep()`: eviction policy belongs to the
daemon that owns the quota. It only ever `_get_path`s and `_put`s.

Consequence: **a file whose chunks are in any of the last 10 generations never
misses tier 2**, so the offline story (§6) applies only to older generations, to
blobs evicted before the pin ring was wired, or to a cache that was cleared.

---

## 4. D5 — Encryption at the mount boundary

**Ciphertext never crosses the mount.** `read()` returns plaintext or an error; there
is no path by which a sealed blob reaches userspace through `Portable/`.

- **`home_key` lives in the mount process**, in an `mlock`'d page allocated at
  startup, `MADV_DONTDUMP`, `OPENSSL_cleanse`d at exit and on every error exit.
  `syncd` holds its own independent copy in its own address space. Neither hands the
  other a key; they are separate PIDs with separate lifetimes and separate units.
- **`RLIMIT_CORE = 0`** is set by the unit (`LimitCORE=0`) *and* re-asserted in
  `main` before the seed is read, so a crash cannot dump the key.
- **Derivation is the spec's, unchanged**: seed (64 hex) → `nh_porthome_key_derive`
  → `home_key`; per chunk `nh_porthome_decrypt_chunk` re-derives key+nonce from the
  wire nonce. The mount performs no key derivation of its own.
- **`chunk_key_id != 0` is refused** at snapshot-load time and per entry, matching
  `nh_porthome_manifest_decode`'s strictness. An entry carrying a future key epoch is
  served as `EIO` with one log line, never as garbage.

### 4.1 D6 — How the mount obtains the seed

The Phase-3 W(3) drop is **read-once-and-unlink** by design. If both `syncd` and the
mount read `/run/nostr-auth/session/<uid>/home_seed`, whoever wins the race unlinks
it and the loser has no key. That is a real, reproducible startup race, not a
theoretical one.

Three ways out:

| Option | Shape | Verdict |
| --- | --- | --- |
| (a) second drop file | broker additionally writes `…/<uid>/home_seed.fuse`, same mode/owner/tmp+rename, consumed read-once+unlink by the mount | **Recommended for v1.** No protocol, no listener, no inter-daemon dependency, no ordering constraint. ~30 lines in `auth_porthome.c`, symmetric with what already works. |
| (b) `syncd` re-exports the seed | `syncd` writes it to `$XDG_RUNTIME_DIR/nostr-homed/seed` for the mount | Rejected. It turns a one-shot tmpfs handoff into a session-long plaintext key file, and makes the mount depend on `syncd` being alive. |
| (c) broker `GET_HOME_SEED` request | uid-checked request/response on the existing `/run/nostr-auth/auth.sock`, `SO_PEERCRED` | **The right long-term answer** — HFR §10.1 already lists "uid-checked `GET_HOME_KEY`" as Phase-2 work that was never built. It also fixes restart-after-consume for *both* daemons. Out of scope for P4-I; filed as a follow-up bead. |

**D6 = (a) now, (c) filed.** Known limitation to document in the unit file and the
bead: after the drop is consumed, a mount restart within the session has no seed and
exits cleanly with a log line (it does **not** loop — see §5.3). `syncd` has this
same limitation today; (c) retires both at once.

### 4.2 Remount and rekey

- **Remount in-session:** the seed is gone (consumed). The unit is
  `Restart=on-failure` with `RestartPreventExitStatus=` covering the clean
  "no seed" exit, so a crash retries (and fails cleanly) rather than spinning.
  Re-login re-drops the seed.
- **Rekey (`key_epoch` > 1):** every address changes. v1 behaviour is *fail closed*:
  entries with `chunk_key_id != 0` are `EIO`; a snapshot whose `root_id_hex` changes
  under a live mount triggers a full reload (§7.3) and a tier-1 flush. Actual
  rotation is HFR Phase 5; the mount must merely never serve bytes decrypted under
  the wrong epoch, and the AEAD tag guarantees it cannot.

---

## 5. D7 — Session lifecycle

### 5.1 New unit, not the old template

**D7: ship a new user unit `nostr-home-fuse.service` (non-templated, like
`nostr-home-sync.service`). Do not modify `nostrfs@.service`, system or user scope.**

Rationale: the legacy templates mount `/home/%i` with `--writeback`, set
`ProtectHome=read-only` (incompatible with mounting inside `$HOME`), `BindsTo=`
a roaming unit, and carry `AmbientCapabilities=CAP_SYS_ADMIN` that a `systemd --user`
unit cannot grant anyway. Editing them would silently change the legacy path that
HFR §3.4 requires to keep working. A new unit is cheaper and keeps the old road open.

```
[Unit]
Description=Portable-home read-only overlay (nostr-home-fuse)
After=nostr-home-sync.service
Wants=nostr-home-sync.service
PartOf=graphical-session.target          # torn down with the session

[Service]
Type=notify                              # sd_notify(READY=1) after the mount is live
ExecStart=@NH_FUSE_BIN_PATH@
ExecStopPost=-/bin/fusermount3 -u -z %h/Portable
Restart=on-failure
RestartSec=5
RestartPreventExitStatus=3 4             # 3 = no snapshot, 4 = no seed: clean no-ops
LimitCORE=0
# hardening — same shape as nostr-home-sync.service, NOT nostrfs@.service
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=%h
PrivateTmp=yes
DeviceAllow=/dev/fuse rw
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
SystemCallFilter=@system-service @mount
UMask=0077
# no capabilities, no ProtectHome, no allow_other/allow_root

[Install]
WantedBy=default.target
```

`ExecStopPost` uses `-z` (lazy) so a logout with a `cd`'d shell inside the mount does
not wedge session teardown. `PartOf=graphical-session.target` makes the mount die
with the session rather than outliving it as a stale `Transport endpoint is not
connected` directory.

### 5.2 Ordering vs. the provisioner and `syncd`

```
pam_sm_open_session → broker PROVISION_HOME → eager priority set on disk
                      broker drops home_seed + home_seed.fuse        (D6)
                      pam_putenv NOSTR_HOME_STATE=ready|partial|limited
          ↓ session starts, systemd --user reaches default.target
nostr-home-sync.service  ── acquires sync.lock, loads/creates snapshot.json
nostr-home-fuse.service  ── After=, acquires fuse.lock, loads snapshot.json, mounts
```

The provisioner necessarily completes (or gives up into `partial`/`limited`) before
`systemd --user` runs, because `pam_sm_open_session` is what releases the session.
So "FUSE up before the manifest is ready" cannot happen via that route. It *can*
happen on a cold box where `syncd` has not yet written `snapshot.json`. Rules:

- **No `snapshot.json` ⇒ exit 3 (clean), log one line, do not mount, do not retry.**
  `syncd` will write one; the mount comes up on next login, or immediately if an
  operator restarts the unit. An empty mount is worse than no mount — a user who
  sees an empty `Portable/` concludes their data is gone.
- **Malformed `snapshot.json` ⇒ exit 3** with the same treatment. The mount never
  "repairs" state; that is `syncd`'s `nh_syncd_rescan_home_additive` path.
- **`NOSTR_HOME_STATE != ready` ⇒ mount, but with tier 0 disabled** (§3.1). A
  `partial` home is exactly when the overlay is most useful.
- **`~/.nostr-home-limited` present ⇒ mount read-only as usual.** The limited-mode
  interlock (HFR §5.3) forbids *pushing*; the mount never pushes, so it is not
  implicated. It will simply miss a lot and notify (§6).

### 5.3 Locks — the mount must never touch `sync.lock`

**D8: the mount takes `flock` on `$XDG_STATE_HOME/nostr-homed/fuse.lock`, a
different file from `syncd`'s `sync.lock`, and never calls
`nh_syncd_lock_acquire`.** Sharing the lock would make the two daemons mutually
exclusive, which is the opposite of the intent. `fuse.lock` gives single-instance
semantics for the mount alone (§11, dual-mount refusal). Contention is zero: `syncd`
writes `snapshot.json` atomically via tmp+rename, and the mount only ever reads it.

### 5.4 Teardown

`SIGTERM` → stop the inotify watcher → `fuse_session_exit` → `destroy` cleanses
tier 1 and `home_key` → release `fuse.lock` → exit 0. `ExecStopPost` lazily unmounts
as a backstop for a crashed process. `-o auto_unmount` is **also** set so that a
`SIGKILL` cannot leave a stale mount behind.

---

## 6. D9 — Offline semantics

### 6.1 The choice

**D9: bounded attempt, then `EIO`. Never block indefinitely; never
block-with-cancellation.**

A tier-3 fetch on the FUSE path gets a *reduced* budget compared with `syncd`'s:
**one attempt per server, no exponential backoff, 5 s connect / 15 s total across
the whole server list** (`offline_budget_ms`). On exhaustion the handler returns
`-EIO` and records a miss.

Why not block-with-cancellation:

- FUSE 3's high-level API gives no usable interrupt hook; `INTERRUPT` handling lives
  in the low-level API and requires cooperating with `fuse_req_interrupt_func`.
  Rewriting the overlay against `fuse_lowlevel` to get cancellable reads is a
  disproportionate cost for a read-only cache.
- A blocked read blocks `fusermount3 -u` and the session's teardown. GNOME
  aggravates this: `gnome-shell`'s thumbnailer and `tracker-miner-fs` will walk
  `Portable/` unbidden, and a blocking miss hangs the indexer, then Nautilus, then
  the user's impression of the desktop. A hung `$HOME`-adjacent folder is the single
  most-reported FUSE failure mode and we get to simply not have it.
- SIGINT only helps interactive foreground processes. Nothing in the GNOME session is
  one.

Why `EIO` and not something more descriptive: `read(2)`'s documented errnos are what
applications branch on. `ENETDOWN`/`EHOSTUNREACH` are not read errnos and portable
code treats them as "unknown" — usually by retrying forever. `EAGAIN` is worse: it
invites exactly that loop. `ENOENT` is a lie (the file exists; its bytes don't).
`EIO` is honest, terminal, and universally handled as "this read failed".

`getattr`/`readdir` **never** hit the network — they are answered entirely from
`snapshot.json`. So an offline user still sees the complete tree with correct sizes
and dates, and only `read` fails. That is the right shape: browse always works,
content sometimes doesn't.

### 6.2 Notification

Reuse the sweep's pattern (`nh_syncd_notify_fn` → `notify-send` when
`NOSTR_HOMED_SYNCD_NOTIFY != 0`, dropped silently otherwise):

- at most **one notification per 10 minutes**, coalescing the misses in that window;
- body: *"N file(s) in Portable could not be fetched — the Blossom server is
  unreachable. They will read normally when you are back online."*;
- plus a machine-readable status file at
  `$XDG_RUNTIME_DIR/nostr-homed/fuse-status.json`
  (`{"schema":1,"mounted":true,"generation":N,"tier0":bool,"hits_local":…,
  "hits_cache":…,"fetches":…,"misses":…,"last_miss_epoch":…}`), rewritten atomically
  every 5 s when counters changed. This is also how root/`systemctl --user status`
  learns the mount's health **without** needing `allow_root` (§7.4).

---

## 7. Snapshot binding, coherence, invalidation

### 7.1 Loading

At mount the process parses `snapshot.json` into an in-memory read-only table:

- a sorted array of `{rel_path, kind, mode, uid, gid, mtime_ns, size,
  content_hash_hex, chunk_addrs[]}`, plus a directory index derived by splitting
  paths (the snapshot lists entries, not a tree);
- caps enforced on load, failing closed, mirroring HFR §5.4:
  ≤ 500 000 entries, path ≤ 4096 B, component ≤ 255 B, depth ≤ 64,
  ≤ 64 chunk addresses/entry in v1 (`NH_PORTHOME_MAX_CHUNKS_PER_ENTRY`);
- every path re-validated: relative, no leading `/`, no `.`/`..`/`/./`/`/../`, no
  NUL, no `\`. A snapshot is local state, but it is state a *remote* reconcile wrote
  into, so it gets the hostile-input treatment anyway.
- mode is masked `& 0777` and `07000` stripped; `uid`/`gid` are ignored and every
  entry is reported as the mounting uid/gid (HFR-D17).

### 7.2 Enumeration accessors (small additive change to Phase 3)

`nh_syncd_state` exposes `_find`, `_file_count`, and getters for kind/size/mtime/
content-hash — but no iterator and no chunk accessor. Rather than duplicating the
schema in a second parser (slop, and it would drift), P4-I adds **purely additive**
getters to `src/porthome-syncd/nh_syncd_state.c`:

```c
const nh_syncd_entry *nh_syncd_state_at(const nh_syncd_state *s, size_t i,
                                        const char **out_rel_path);
uint32_t    nh_syncd_entry_mode          (const nh_syncd_entry *e);
size_t      nh_syncd_entry_chunk_count   (const nh_syncd_entry *e);
const char *nh_syncd_entry_chunk_at      (const nh_syncd_entry *e, size_t i);
const char *nh_syncd_entry_symlink_target(const nh_syncd_entry *e);
```

No behaviour change, no struct layout change visible to callers, no new dependency.
This is the only Phase-3 file P4-I touches.

### 7.3 D10 — Generation advance while mounted

**Readers see a new snapshot on the next `open()`; in-flight handles keep the
generation they opened at.**

- `open()` resolves the entry from the *current* table and pins a refcounted
  `nh_fuse_handle{entry_snapshot, generation}` into `fi->fh`. All `read`s on that
  handle use that entry. A file cannot change size or content under a running `cat`.
- A watcher thread holds `inotify` on the state dir for `IN_MOVED_TO` of `generation`
  and `snapshot.json` (both are written by tmp+rename, so `IN_MOVED_TO` is the
  correct event and it fires exactly once per commit). On fire: debounce 500 ms,
  re-parse `snapshot.json` into a new table, swap it in, re-apply the pin ring, flush
  tier 1 of any chunk no longer referenced, drop the old table when its last handle
  closes.
- Kernel caches are invalidated explicitly: for each path whose entry changed or
  vanished, `fuse_invalidate_path(fuse_get_session(f), "/<rel>")`. If more than 512
  paths changed we skip per-path invalidation and instead rely on short timeouts for
  one cycle (an explicit, logged degradation — invalidating 50 000 paths one at a
  time stalls the loop).
- `nfs_init` config: `cfg->kernel_cache = 1` (content is immutable within a
  generation, so page-cache retention is correct and valuable),
  `entry_timeout = attr_timeout = 1.0`, `negative_timeout = 0.0` (a negative entry
  must not outlive a generation bump).

---

## 8. D11 — GDM, Nautilus, gvfs, and the `st_dev` trap

- **`syncd` ignores the mount** because `nh_syncd_ignore` excludes differing
  `st_dev`. This is exactly correct here: `syncd` owns writes to `$HOME`, and the
  mount contains no writable bytes. §2.3 documents the unmounted-empty-dir case.
- **No `allow_other`, no `allow_root`.** With neither, only the mounting uid can
  traverse `Portable/`. `root` cannot `stat` it — which is fine, because nothing
  needs to: the unit is `Type=notify` (systemd learns readiness from `sd_notify`, not
  from a `stat`) and the status file (§6.2) is world-unreadable-but-root-readable in
  `$XDG_RUNTIME_DIR`. This is a deliberate departure from `nh_open_session`'s
  "verify the mountpoint" invariant *in mechanism* while preserving it *in
  substance*: the mount process verifies its own mountpoint after
  `fuse_mount` (compare `st_dev` of `Portable/` against `$HOME`'s; they must
  differ) and only then calls `sd_notify(READY=1)`. A `systemctl --user start` that
  returns success therefore still means the mount is live. `allow_root` would be
  needed only if a root-scope unit had to poll the mount; we do not create one.
- **Nautilus** treats it as a folder on a separate filesystem: copy-out works,
  copy-in fails with `EROFS` and GTK renders "read-only filesystem", which is the
  truthful message. `statfs` reports the cache filesystem's free space with
  `f_flag |= ST_RDONLY`.
- **`tracker`/`gnome-shell` thumbnailers will walk it.** Since a miss is bounded and
  terminal (§6.1) an offline walk costs at most `offline_budget_ms` per unique file,
  once. *Online*, a full index walk would fetch the entire home over the network on
  first sight — which is a bandwidth policy question, not a correctness one. The
  mount cannot ship a `.trackerignore` (it is read-only), so the options are a
  packaged `tracker3` index-exclusion drop-in, a documented user action, or accepting
  the walk. **Maintainer call — §15, item 4.**
- **gvfs SMB is unrelated and does not clash.** gvfs mounts live under
  `$XDG_RUNTIME_DIR/gvfs` (or `/run/user/<uid>/gvfs`) via its own FUSE daemon and a
  different `fsname`; our mountpoint is under `$HOME` and our `fsname` is
  `nostr-home`. Nothing in `src/smb` references the FUSE path. Confirmed by
  inspection: `gnome/nostr-homed/src/smb/*` touches credentials and `net ads`, never
  a mount table.

---

## 9. D12–D13 — FUSE version, threading, and process shape

- **D12: FUSE 3 high-level API** (`FUSE_USE_VERSION=31` for the new target;
  the legacy `nostrfs` stays at 30 — they are separate targets). The low-level API
  buys interrupt handling we explicitly decided not to use (§6.1) at the cost of a
  complete rewrite.
- **D13: single-threaded first** (`fuse_loop`, `-s`). The mount's mutable state is
  the snapshot table, the tier-1 LRU and the counters; single-threaded means none of
  it needs a lock and every bug is a logic bug rather than a race. Documented path to
  multi-threading: (i) put tier 1 behind a mutex and make
  `nh_fuse_source_chunk` return an owned reference-counted buffer rather than a
  borrowed pointer; (ii) make the snapshot table swap an RCU-ish pointer swap with
  per-handle refcounts (already the shape in §7.3); (iii) switch to
  `fuse_loop_mt(cfg{.clone_fd = 1})` and measure. Do not do (iii) before (i) and (ii).
  The one-thread cost is real — a slow tier-3 fetch stalls every other read for up to
  `offline_budget_ms` — and that is the trigger condition for doing the work.
- **No libgo.** The legacy `nostrfs.c` runs a libgo actor plus four upload workers
  because it has a writeback path. The overlay has none. It also must not inherit the
  GNU statement-expression idiom (`({ … })`) used there. New code, new file, plain C.
- **No D-Bus, no relay, no signer.** `nh_porthome_blossom` needs a
  `hanami_signer_t` only for `_upload`; the mount passes `NULL` and never uploads.
  (P4-I must confirm `nh_porthome_blossom_new(opts, NULL, &c)` accepts a NULL signer
  for a fetch-only client, and add that contract to the header if it does not —
  §15, item 3.)

---

## 10. The lazy subtree — what makes the overlay load-bearing

With a complete eager copy, `Portable/Documents/notes.txt` and
`$HOME/Documents/notes.txt` are the same bytes and the overlay is a curiosity. It
becomes necessary when the reconciler is permitted to *not* materialize something.

**D14: v1 ships the overlay without the lazy policy; the policy is specified here and
implemented as the last item of P4-I, behind the same build gate, only if the
maintainer approves touching `nh_syncd_reconcile`.**

Shape, if approved:

- `~/.config/nostr-homed/lazy` — same parser and matcher as
  `~/.config/nostr-homed/ignore` (`fnmatch(FNM_PATHNAME|FNM_LEADING_DIR)` on
  `$HOME`-relative paths), loaded by `nh_syncd_ignore_new`'s sibling
  `nh_syncd_lazy_new`.
- `nh_syncd_reconcile_from_manifest` gains one optional `const nh_syncd_lazy *lazy`
  field in its cfg. A remote-new or remote-changed entry matching `lazy` is recorded
  in `snapshot.json` **but its bytes are not written to `$HOME`** and its chunks are
  not fetched. Deletions, conflicts, and the push path are untouched.
- The overlay then serves those paths from tier 2/3 — and only the overlay can.
- Interaction with tier 0: a lazy path has no local file, so tier 0 misses naturally;
  no special case.
- Interaction with the pin ring: lazy chunks are still in the current generation, so
  they are still pinned — meaning the *first* fetch populates the cache and
  subsequent reads are local. "Lazy" bounds the eager copy, not the cache.

This is the difference between "a read-only mirror of a folder you already have" and
"the only way you can read your 40 GiB photo archive on a 256 GB laptop". It is also
the one part of Phase 4 that modifies a Phase-3 module, which is why it is last and
why it needs an explicit yes.

---

## 11. D15 — Non-goals for v1, and the follow-up plan

| Punted | v1 behaviour | Follow-up |
| --- | --- | --- |
| **Concurrent multi-machine mounts** | Allowed and safe — the mount is read-only and derives everything from the machine's own `snapshot.json`. Two machines may be at different generations; neither can corrupt the other. HFR §6.3's multi-writer punt does not bite a read-only reader. | Nothing needed until (c) write-cache is reconsidered. |
| **Dual mount on one machine** | Refused. `flock` on `fuse.lock` (§5.3) plus a mountpoint check (`st_dev` already differs ⇒ someone is mounted) ⇒ exit 5 with a log line. | — |
| **Writes of any kind** | `create/write/mkdir/rmdir/unlink/rename/chmod/chown/truncate/link/symlink/setxattr` all return `-EROFS`, unconditionally and before any other work. `open` with `O_WRONLY`/`O_RDWR`/`O_TRUNC`/`O_APPEND`/`O_CREAT` ⇒ `-EROFS`. | HFR Phase 5, if ever. |
| **Windows / macOS** | Linux-only. FUSE-T / WinFsp are not on any roadmap; `inotify`, `/run/nostr-auth`, `flock` semantics and systemd user units are all Linux. | — |
| **MLS / group-shared homes** | Out of scope (HFR §2.7, HFR-D18). The mount consumes one `home_key`; an MLS exporter secret would arrive by the same seam and needs no mount change. | Epic-level. |
| **xattrs, ACLs, hardlinks, device nodes, FIFOs** | Not represented in the manifest (HFR §2.3 refuses them on capture). `listxattr`/`getxattr` return `-ENOTSUP`. | — |
| **Key rotation while mounted** | Fail closed (§4.2). | HFR Phase 5. |
| **`allow_other` / multi-user access to one mount** | Never. | — |

---

## 12. Decisions

| # | Decision | Options | Recommendation |
| --- | --- | --- | --- |
| **D1** | FUSE role | (a) read overlay under the eager home; (b) full content-on-demand replacing eager; (c) write cache in front of the batcher | **(a).** Only (a) can fail safely — every failure degrades to "unreadable now", never to lost data or a dead session. (b) re-imports every GDM/FUSE hazard HFR §3.2 priced out. (c) creates a second writer for one set of bytes and re-opens HFR §6.3's conflict semantics. |
| **D2** | Mount point | (a) `$HOME/Portable` real dir; (b) `$XDG_RUNTIME_DIR` + `$HOME` symlink; (c) bind-shadow parts of `$HOME` | **(a).** The existing xdev ignore rule makes it invisible to capture while mounted, and an unmounted empty dir is a self-provisioning mountpoint on every other machine. (b) captures a machine-local symlink. (c) is a coherence problem with no upside. |
| **D3** | User perception | (a) explicit visible "cloud folder"; (b) invisible/hidden; (c) GNOME-special handler | **(a).** Truthful `fsname`/`subtype`, `EROFS` where writes fail, no magic. A folder that behaves unlike a folder for unexplained reasons is worse than one that is plainly different. |
| **D4** | Cache seam | (a) handlers call cache/blossom/crypto directly; (b) one `nh_fuse_source` adapter owning a 4-tier ladder | **(b).** One place holds `home_key`, one place decrypts, one place implements the offline budget, one place counts. §3.2 fixes the signatures. |
| **D5** | Ciphertext exposure | (a) never — decrypt in the handler; (b) expose a raw blob view | **(a),** non-negotiable. Plaintext-only across the mount boundary; tier 1 is memory-only. |
| **D6** | Seed handoff | (a) second broker drop `home_seed.fuse`; (b) `syncd` re-exports; (c) broker `GET_HOME_SEED` over `auth.sock` | **(a) for v1, (c) filed.** The drop is read-once+unlink, so two consumers need two drops. (b) turns a one-shot into a session-long key file. (c) is the correct fix and also solves restart-after-consume for `syncd`; it is HFR §10.1 work that never landed. |
| **D7** | Unit | (a) extend `nostrfs@.service`; (b) new user unit `nostr-home-fuse.service` | **(b).** The templates mount `/home/%i --writeback` with `ProtectHome=read-only` and `CAP_SYS_ADMIN` — all wrong here, and editing them breaks the legacy path HFR §3.4 requires to survive. |
| **D8** | Locking | (a) share `sync.lock`; (b) own `fuse.lock` | **(b).** Sharing would make mount and daemon mutually exclusive. `snapshot.json` is tmp+rename; readers need no lock. |
| **D9** | Offline unpinned read | (a) block indefinitely; (b) block-with-cancellation; (c) bounded attempt then `EIO` | **(c).** FUSE 3 high-level has no usable interrupt hook, and a blocking miss hangs the thumbnailer, then Nautilus, then unmount. `EIO` over `EAGAIN`/`ENOENT`/`ENETDOWN`: honest, terminal, universally handled. `getattr`/`readdir` never touch the network, so browsing always works offline. |
| **D10** | Generation invalidation | (a) remount on bump; (b) per-`open()` binding + inotify reload + `fuse_invalidate_path` | **(b).** In-flight reads stay coherent; new opens see the new generation; the kernel cache is corrected explicitly, with a logged fall-back to timeout-only when > 512 paths changed. |
| **D11** | `allow_root` | (a) enable for root status checks; (b) never, use `Type=notify` + status file | **(b).** `sd_notify(READY=1)` after the process verifies its own mountpoint preserves `nh_open_session`'s "verify the effect, not the command" invariant without widening access. |
| **D12** | FUSE API | (a) high-level FUSE 3; (b) low-level | **(a),** `FUSE_USE_VERSION=31` on a new target. Low-level buys only the interrupt handling D9 declines to use. |
| **D13** | Threading | (a) single-threaded; (b) `fuse_loop_mt` now | **(a) first.** Mutable state is lock-free by construction. §9 records the exact three steps to (b) and the trigger (head-of-line blocking on tier-3 fetches). |
| **D14** | Lazy subtree policy | (a) ship with v1; (b) specify now, implement last, gated on maintainer approval to touch `nh_syncd_reconcile`; (c) defer to Phase 5 | **(b).** Without it the overlay duplicates `$HOME`; with it the overlay is the only reader of the cold tail. It is also the only Phase-3 module P4-I would modify beyond additive getters. |
| **D15** | Non-goals | see §11 | Read-only ⇒ concurrent multi-machine mounts are safe and permitted; same-machine dual mount refused by `flock`; Linux-only; MLS untouched. |
| **D16** | Packaging | (a) fold into `nostr-home-sync`; (b) separate `nostr-homed-portable-home-fuse` | **(b),** matching HFR §7.2's third row. Keeps `libfuse3` out of the sync package's dependency closure — the sync daemon is the artifact that ships first and must stay installable on a headless box. |

---

## 13. Implementation checklist for P4-I

Ordered by phase. Every item is behind
`NOSTR_HOMED_ENABLE_PORTHOME_FUSE_EXPERIMENTAL` (default `OFF`, requires
`NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL` which already requires
`NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL`, plus `FUSE3` found).
**`src/fs/nostrfs.c` is not edited and `nostrfs@.service` is not edited.**

**F0 — gate and skeleton**
- `gnome/nostr-homed/CMakeLists.txt`: add the option next to
  `NOSTR_HOMED_ENABLE_SYNCD_EXPERIMENTAL` (line ~66); `FATAL_ERROR` if enabled
  without SYNCD or without FUSE3. New target `nostr-home-fuse`,
  `FUSE_USE_VERSION=31`, links `nostr_syncd_core` + `nostr_porthome` + FUSE3.
  Nothing installs unless the option is ON. Add a feature-OFF `-Werror` CI job
  mirroring `nostrc-6quj`'s SMB-off precedent.
- `src/fs-porthome/nostr-home-fuse.c` — `main`, arg/env/config parse, exit codes
  (0 ok, 3 no/bad snapshot, 4 no seed, 5 already mounted, 6 mount failed).

**F1 — snapshot reader**
- `src/porthome-syncd/nh_syncd_state.c` + `include/nh_syncd.h`: the five additive
  getters in §7.2. No behaviour change.
- `src/fs-porthome/nh_fuse_table.{c,h}` — build the sorted path array + directory
  index from `nh_syncd_state`; enforce §7.1 caps and path validation; mode masking;
  uid/gid override.
- `tests/unit/test_fuse_table.c`.

**F2 — key + source ladder**
- `src/auth/auth_porthome.c`: `nh_auth_broker_porthome_drop_seed_for_uid` gains a
  second drop `home_seed.fuse` (D6a) — ~30 lines, same tmp+rename+fchown+fchmod
  shape, warn-only on failure.
- `src/fs-porthome/nh_fuse_key.{c,h}` — read-once+unlink+`mlock` of
  `/run/nostr-auth/session/<uid>/home_seed.fuse` (override
  `NH_FUSE_SEED_FILE`; env fallback `NH_FUSE_SEED_HEX` for headless tests),
  `nh_porthome_key_derive`, `RLIMIT_CORE=0`, `MADV_DONTDUMP`, cleanse on exit.
- `src/fs-porthome/nh_fuse_source.{c,h}` — §3.2 exactly; tier-1 LRU; tier-2 via
  `nh_syncd_cache_*`; tier-3 via `nh_porthome_blossom_fetch` + `nh_syncd_cache_put`;
  `nh_porthome_decrypt_chunk`; offline budget; counters; miss callback.
- `tests/unit/test_fuse_source.c` (tier ladder with injected cache + fake fetch).

**F3 — FUSE operations**
- `src/fs-porthome/nh_fuse_ops.c` — `init/getattr/readdir/readlink/open/read/
  release/statfs/destroy`; every mutator `-EROFS`; `open` refuses write flags;
  `getxattr`/`listxattr` `-ENOTSUP`; handle struct per §7.3.
- Mount: `fsname=nostr-home,subtype=porthome,ro,nosuid,nodev,noatime,
  default_permissions,auto_unmount`; **no** `allow_other`/`allow_root`;
  post-mount `st_dev` self-verification then `sd_notify(READY=1)`.
- `flock` on `fuse.lock`; mountpoint sanity (not a symlink, not already a mount).

**F4 — lifecycle and observability**
- `src/fs-porthome/nh_fuse_watch.c` — inotify `IN_MOVED_TO` on the state dir,
  500 ms debounce, table swap, pin-ring re-apply, `fuse_invalidate_path` with the
  512-path degradation rule.
- `src/fs-porthome/nh_fuse_status.c` — `$XDG_RUNTIME_DIR/nostr-homed/fuse-status.json`
  atomic rewrite; 10-minute-throttled `notify-send` on misses, honouring
  `NOSTR_HOMED_SYNCD_NOTIFY`.
- `systemd/user/nostr-home-fuse.service.in` (§5.1) + CMake `configure_file` +
  conditional `install`.
- `packaging`: `ignore.d/portable` drop-in shipped to
  `/usr/share/nostr-homed/ignore.d/`. **No `debian/` or `rpm/` changes in P4-I.**

**F5 — lazy subtree (only if §15 item 1 is approved)**
- `src/porthome-syncd/nh_syncd_lazy.{c,h}` (matcher, mirrors `nh_syncd_ignore`).
- `nh_syncd_reconcile_from_manifest`: one optional cfg field; skip materialization
  for matching entries; record them in the snapshot regardless.
- `tests/unit/test_syncd_lazy.c`, plus integration case (h) below.

---

## 14. Test plan

Headless cases run on CI with the fixtures already in the tree
(`tests/integ/fake_relay_fixture.py`, `tests/integration/fake_porthome_blossom.py`,
`tests/integration/nostr_home_publisher.c`, `tests/integration/mock_signer.c`) and
follow the existing skip-if-absent convention (`nostr-home-fuse` not built,
`/dev/fuse` absent, `python3` absent ⇒ `exit 0`). New scripts live in
`tests/integration/` as `run_porthome_fuse_*.sh`.

| # | Case | Script | Asserts |
| --- | --- | --- | --- |
| **a** | cold read of a **pinned** file | `run_porthome_fuse_pinned.sh` | Publisher seeds a home; `pinned.json` lists the generation; cache pre-populated. Read through the mount returns byte-identical content; the fake Blossom log shows **zero** GETs. |
| **b** | cold read of an **unpinned** file | `run_porthome_fuse_fetch.sh` | Cache empty for that chunk. Read succeeds; exactly one Blossom GET per chunk; the blob lands in `$XDG_CACHE_HOME/nostr-homed/blobs/aa/bb/…` with mode 0600; a second read issues no further GET. |
| **c** | unpinned read while **offline** | `run_porthome_fuse_offline.sh` | Fake Blossom stopped. `ls -lR Portable/` fully succeeds with correct sizes/mtimes (no network on `getattr`/`readdir`). `cat` of an uncached file returns `EIO` within `offline_budget_ms` + 2 s slack, never hangs. `fusermount3 -u` succeeds immediately afterwards. One `notify-send` invocation recorded via a stub on `$PATH`. |
| **d** | **content-sha mismatch** from Blossom | `run_porthome_fuse_corrupt.sh` | Fake Blossom serves flipped bytes for one chunk. Read returns `EIO`; **nothing** is written to the cache for that address; a log line names the address; the AEAD is never invoked (assert via a counter or log marker). Serving the correct bytes afterwards succeeds. |
| **e** | **no seed drop** | `run_porthome_fuse_noseed.sh` | `home_seed.fuse` absent and `NH_FUSE_SEED_HEX` unset ⇒ exit 4, no mount created, mountpoint still an ordinary empty dir, one log line, no key material in any output. |
| **f** | **generation advance while mounted** | `run_porthome_fuse_genadvance.sh` | Mount at gen N. A long `cat` of a large file is in flight while gen N+1 (different content, one file removed, one added) is committed by rewriting `snapshot.json`+`generation` via tmp+rename. Assert: the in-flight `cat` completes with gen-N bytes; a *new* `open` of the same path sees gen-N+1 bytes; the removed path becomes `ENOENT`; the added path appears in `readdir`; the pin ring was re-applied (new gen's hashes pinned). |
| **g** | **read-only enforcement** | `run_porthome_fuse_rofs.sh` | `touch`, `mkdir`, `rm`, `mv`, `chmod`, `truncate`, `ln -s`, `setfattr` inside the mount each fail with `EROFS`/`ENOTSUP`; `$HOME` outside the mount is unaffected; `syncd`'s snapshot is byte-identical before and after. |
| **h** | **lazy subtree** *(only with F5)* | `run_porthome_fuse_lazy.sh` | `~/.config/nostr-homed/lazy` matching `Archive/`. Reconcile records `Archive/**` in `snapshot.json` but writes nothing to `$HOME/Archive`. `Portable/Archive/big.bin` reads correctly via tier 3. Push path and conflict behaviour unchanged (re-run `test_syncd_push_e2e`). |
| **i** | **single-instance** | folded into (e) | Second `nostr-home-fuse` exits 5 without disturbing the first mount. |
| **j** | **feature-OFF closure** | extend `test_syncd_off_closure.sh` | With the option OFF: no `nh_fuse_*` symbols in `nostr-authd`, `pam_nostr.so`, `nostr-home-syncd`; no `libfuse3` in their `ldd`; the `nostr-home-fuse.service` unit is not installed. |
| **k** | **regression: legacy path untouched** | existing scripts | `run_nostrfs_basic.sh`, `run_nostrfs_writeback.sh`, `run_nostrfs_writeback_fake_blossom.sh` still pass unchanged with `EXPERIMENTAL_ROAMING=ON`. |

**Unit:** `test_fuse_table` (caps, hostile paths — `..`, absolute, NUL, 300-byte
component, depth 65, 500 001 entries, `chunk_key_id != 0` — each refused, nothing
mounted), `test_fuse_source` (tier ordering, LRU eviction, sha mismatch, budget
exhaustion, cleanse-on-close), `test_fuse_key` (hex validation, unlink-after-read,
env fallback, no key bytes in any log).

**Live smoke (maintainer, manual):** on `bizarro@192.168.64.3` **only** — *not*
`gnome-dev`. Build with the option ON, publish a ~100 MiB fixture home to
`wss://relay.sharegap.net` + `https://blossom.sharegap.net` with
`nostr_home_publisher`, clear the local blob cache, mount, and:
1. `find Portable -type f | wc -l` matches the snapshot entry count;
2. `sha256sum` of three files (one inline-small, one multi-chunk, one ~50 MiB)
   matches the originals;
3. `nmcli networking off` → `cat` of an uncached file returns `EIO` within budget
   and the notification fires; `nmcli networking on` → the same `cat` succeeds;
4. commit a new generation from a second machine, confirm §7.3 invalidation;
5. `systemctl --user stop nostr-home-fuse` leaves no stale mount
   (`mount | grep -c nostr-home` is 0);
6. record wall-clock for first-fetch of the 50 MiB file in the bead.

---

## 15. Open decisions the maintainer must confirm before P4-I dispatches

1. **D14 — may P4-I modify `nh_syncd_reconcile`?** Without the lazy-subtree skip the
   overlay is a read-only duplicate of `$HOME` and its value is limited to
   partial/limited-mode homes and evicted content. With it, the overlay is the only
   reader of the cold tail — which is the Phase-4 goal HFR §3.2 actually stated. The
   change is one optional cfg field and one `if`, but it touches a Phase-3 module the
   coordination note fences off. **This is the decision that determines whether
   Phase 4 is worth shipping.**
2. **D6 — the second seed drop.** Confirm that adding `home_seed.fuse` alongside
   `home_seed` in `auth_porthome.c` is acceptable as a v1 expedient, and that the
   broker `GET_HOME_SEED` request (HFR §10.1's never-built `GET_HOME_KEY`) is filed
   rather than built now. If the answer is "build (c) instead", P4-I grows a broker
   protocol change and should be re-scoped.
3. **`nh_porthome_blossom_new(opts, NULL, &client)` with a NULL signer.** The header
   does not say whether a fetch-only client may omit the signer. If it may not, P4-I
   either constructs a throwaway signer (undesirable — the mount should hold no
   signing key) or the header gains an explicit "signer may be NULL for fetch-only"
   contract plus a guard in `_upload`. Confirm which.
4. **Tracker / thumbnailer policy.** Do we accept that GNOME's indexer will walk
   `Portable/` and trigger one bounded fetch per file on first sight (potentially
   pulling the whole home over the network once), or does the mount ship a policy
   that keeps indexers out? Options: a `~/.config/tracker3` drop-in in the package, a
   documented user action, or accepting it. Low risk, but it is a *bandwidth* policy
   decision, not an engineering one.
5. **Mountpoint name.** `$HOME/Portable` is proposed. It is user-visible, it is
   captured as an empty directory into every machine's home, and renaming it later is
   a migration. Confirm the name (`Portable`, `Archive`, `Cloud`, …) now.
6. **`EIO` as the offline semantic.** Confirm that an unreachable Blossom server
   making a file unreadable — rather than making the reader wait — is the behaviour
   you want users to see, given that browsing (`ls`, sizes, dates) keeps working.

---

## 16. References

- `docs/designs/home-from-relay.md` — §2 (data model), §3.1–§3.4 (materialization,
  HFR-D3/D10), §4 (keys), §5.3 (limited mode), §5.4 (caps), §6.2–§6.5 (sync loop,
  interlocks, cache/retention), §7.2–§7.3 (packaging, build options), §8 (threat
  model), §10 (phasing), §11 (HFR-D1…D18).
- `docs/designs/porthome-crypto-spec.md` — §3 (key hierarchy), §4 (sealed-blob
  layout), §5 (decode strictness, address-before-decrypt), D4 approval 2026-09-23.
- `prompt-exports/porthome-phase3-wrapup-and-phase4-2026-09-24.md` — items W, P4-D,
  P4-I and the coordination fences.
- In-tree: `include/nh_syncd.h`, `include/nh_syncd_cache.h`,
  `include/nh_porthome_{crypto,manifest,blossom}.h`,
  `src/auth/auth_porthome.c` (W(3) drop), `src/porthome-syncd/nostr-home-syncd.c`
  (W(3) consumer), `src/fs/nostrfs.c` (legacy, untouched).
- Beads: `nostrc-h10m` (epic), `nostrc-p6qp` (Phase 3 syncd), `nostrc-6quj`
  (feature-off `-Werror` CI precedent).

---

## Decision: lazy-subtree skip — 2026-09-24

Maintainer APPROVED the lazy-subtree skip in `nh_syncd_reconcile` (design §15 item 1). P4-I MAY add a subtree-prefix skip inside the reconciler that suppresses eager materialization of paths under the porthome mountpoint (default `$HOME/Portable`), leaving those files to be fetched on demand by the FUSE overlay. The skip MUST be strictly opt-in via a new auth.conf knob (default OFF) so a syncd built against master remains behavior-preserving. The skip MUST NOT delete files that already exist under the skipped subtree from a prior reconcile — additive suppression only, never a destructive rewrite (§5.3/§6.4 no-push interlock discipline).
