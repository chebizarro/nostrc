# fscrypt on `/home/<user>` keyed from `home_key` — evaluation of D16(c)

**Bead:** `nostrc-h10m.7` (parent epic `nostrc-h10m`)
**Date:** 2026-10-28
**Status:** Evaluation. Docs only; no code changed.
**Design under evaluation:** [`docs/designs/home-from-relay.md`](../designs/home-from-relay.md) D16(c) and §8.1. v1 ships D16(a): require LUKS and warn.
**Empirical probe:** [`fscrypt-home-key-probe-2026-10-28/`](fscrypt-home-key-probe-2026-10-28/) (`probe.sh` + `policy-params.sh`, the `fsc.py` ioctl driver, verbatim outputs)
**Follow-ups filed:** `nostrc-h10m.12` (implementation, deferred), and the prerequisites/bugs found while grounding this evaluation: `nostrc-h10m.9`, `nostrc-h10m.10`, `nostrc-h10m.11`.

---

## 0. Recommendation (summary)

**Adopt later.** fscrypt v2 keyed from `home_key` is feasible and fits the existing provisioning shape with little friction. It closes a gap LUKS cannot close for this product's core scenario: a portable home on *someone else's* Linux box. Once the lock completes after logout, the home is cryptographically locked against that box's root and other users, and against anyone who images the disk later. That holds **only if the host was not already compromised while the user was logged in**: a root that captured the seed, `home_key` or plaintext during the session defeats it, and fscrypt gives no integrity or rollback protection. It is a **complement to D16(a), not a replacement**. It leaks metadata, does not cover anything outside `$HOME`, and is unavailable on the default filesystems of Fedora (btrfs) and RHEL (XFS).

Implementation should not start yet. The login path it hooks into does not currently work end-to-end, and the D16(a) baseline it complements was never implemented:

- `nostrc-h10m.9`: PAM `open_session` sends `PROVISION_HOME` without the `account_id`/`tx_id` the broker requires. Login-time provisioning therefore never starts (§2.4).
- `nostrc-h10m.10`: there is no dm-crypt/LUKS detection or warning anywhere in the tree, so v1's actual posture is "plaintext at rest, silently" (§2.6).

The phased plan is in §10. It is filed as `nostrc-h10m.12`, blocked on those two beads.

---

## 1. Context and scope

D16 offers three answers to "the eagerly materialised home is plaintext at rest" (HFR §8.1):

- **(a)** Accept it, require LUKS and warn.
- **(b)** A per-file encrypted local store behind FUSE.
- **(c)** fscrypt on `/home/<user>` keyed from the identity-derived `home_key`.

The design picked (a) for v1 and said (c) "should be evaluated in Phase 4"; that never happened. This document is the evaluation.

**In scope:**
- feasibility against the code as it exists on this branch;
- key derivation and custody;
- failure modes and recovery;
- interaction with shared vaults;
- platform support;
- the threat-model delta against LUKS-required;
- a recommendation.

**Out of scope:** implementing anything, and D16(b).

All code references are to this branch. Each one was either read directly or spot-checked after a read-only explore probe.

---

## 2. Ground truth: the code fscrypt would plug into

### 2.1 Key hierarchy as implemented (libporthome)

| Derivation | Salt (HKDF-SHA256 extract key) | IKM | Where |
| --- | --- | --- | --- |
| wrap seed (LOCAL vault) | `porthome/v1/wrap` | unlocked account private key | `libporthome/include/porthome/wrapkey.h:46`, `libporthome/src/wrapkey.c:35-58`; inline copy in `gnome/nostr-homed/src/auth/provider_local.c:185-202` |
| wrap seed (NIP-46) | — (random 32 B, NIP-44 self-encrypted, stored as the provider's `wrapped_home_key`) | — | `wrapkey.c:80-87`, `auth_porthome.c` enrol/unwrap path |
| PCS v1 `home_key` | `porthome/v1/home` | seed | `libporthome/src/crypto.c:28,96-102` |
| PCS v1 chunk / manifest / name | `porthome/v1/chunk`, `porthome/v1/manifest`, `porthome/v1/name` | `home_key` | `crypto.c:29-31` |
| PCS v2 root node key | `porthome/v2/home` | seed | `crypto.c:415,430-432` |
| PCS v2 node / name / head / chunk / manifest | `porthome/v2/{node,name,head,chunk,manifest}` | parent or node key | `crypto.c:416-420` |
| Vault root ring (MLS) | exporter label `porthome/v2/vault` | MLS exporter secret | `docs/designs/shared-vault-composition.md:279-319` |

Two properties matter for fscrypt:

1. **The LOCAL seed is a deterministic function of the identity key.** Every device that can open the vault derives the same seed, so the same `home_key`, with no stored state. The NIP-46 seed is random, but it is wrapped once per provider record and unwrapped by the bunker on every login. Either way, the broker holds `home_key` at login, which is what fscrypt needs.
2. **Only the broker derives `home_key`, and it does not keep it.** The broker derives it in the provisioning job (`auth_porthome.c:366`) and wipes it at worker exit. The plaintext seed is cached for at most 300 s (`auth_porthome.c:770-792`) and consumed once by `PROVISION_HOME`. syncd and the FUSE mount each receive the seed through a one-shot drop file, `/run/nostr-auth/session/<uid>/home_seed{,.fuse}`, mode 0600, read once and unlinked (`auth_porthome.c:1604-1615`). The comment there notes that `GET_HOME_SEED` re-request is unbuilt.

### 2.2 Provisioner (first login)

`nh_identity_home_prepare` (`gnome/nostr-homed/src/identity/identity_home.c:160-205`) runs as root in nostr-authd:

1. `mkdirat(root, ".nostr-<opid>", 0700)` (line 172).
2. `copy_tree(skel …)` (line 175-177).
3. `options->label(ctx, home_fd)`: the porthome materialiser (line 181).
4. `fsync`, record inode evidence, then `renameat2(root, stage, root, user, RENAME_NOREPLACE)` (line 193).
5. `fchown` and `fchmod 0700` (line 202).

The materialiser writes `.part` siblings with `openat(O_EXCL|O_NOFOLLOW)` and renames within the stage (`nh_porthome_provision.c:226-302`, `nh_porthome_rename.c`). There are no cross-filesystem or cross-tree moves.

The *existing-home* path (second login) runs no `home_prepare`. It validates ownership, mode and inode evidence, and syncd reconciles the contents.

`nostr-homed-provision` (`src/porthome/nostr-homed-provision.c`) is an experimental operator CLI. It fetches into `--dest` and is **not** the login installer. The login installer is the broker path above.

### 2.3 syncd and the FUSE overlay

- **syncd** (`src/porthome-syncd/`) is a `systemd --user` service running as the user, ordered only `After=default.target` (`systemd/user/nostr-home-sync.service.in`).
  - It writes pulls as a temp sibling under the home dirfd, then calls `fsync` and `renameat` within the same dirfd (`nh_syncd_reconcile.c:383-412`).
  - It does not use `O_TMPFILE`, `linkat`, or moves from outside `$HOME`.
  - State lives in `~/.local/state/nostr-homed`, including `snapshot.json`, which holds **plaintext relative paths**.
  - The ciphertext blob cache lives in `~/.cache/nostr-homed/blobs`.
  - All of this is under `$HOME`, so a home policy covers it.
- **porthome-fuse** (`src/porthome-fuse/`) is a read-only user mount at `$HOME/Portable`, overridable with `NH_FUSE_MOUNTPOINT` (`nostr-home-fuse.c:725`).
  - Mount options: `ro,nosuid,nodev,noatime,default_permissions,auto_unmount` (line 922), with no `allow_other`.
  - Its plaintext tier-1 cache is memory-only (`docs/designs/nostrfs-porthome-overlay.md:165-177`), and its ciphertext goes to the same `~/.cache` blob cache.

### 2.4 PAM and broker login and logout

- `pam_sm_authenticate`:
  - **LOCAL:** opens the vault with the passphrase, then copies that passphrase into `PAM_AUTHTOK` (`pam_nostr_broker.c:448-453`).
  - **NIP-46:** gets a fresh signed challenge.
  - After verification, the provider deposits the wrap seed in the broker's 300 s cache.
- `pam_sm_open_session` (`pam_nostr_broker.c:537-566`) opens a **fresh** broker connection and sends `PROVISION_HOME {username, service}` (`auth_client.c:595-610`). The broker handler (`auth_broker.c:1362-1394`) requires an in-flight transaction on the connection or a payload `account_id`, **and** a non-empty `tx_id`. Neither is sent.
  - Result: `UNKNOWN_ACCOUNT`, which PAM maps to a successful ordinary session.
  - **Login-time portable-home provisioning therefore does not run today.** Filed as `nostrc-h10m.9`.
- `pam_sm_close_session` only triggers SMB teardown. It does not lock, unmount, or wipe portable-home state.
- The README tells packagers to append `session optional pam_nostr.so` "near the end" of `common-session`, i.e. *after* `pam_systemd`.

### 2.5 Shared vaults

Vaults are home-relative subtrees: `vaults[].path`, e.g. `Portable/Team`, validated disjoint (`nh_syncd_vault_config.c:12-135`). syncd materialises them under `$HOME/<path>` through the same home dirfd (`nh_syncd_vault_manager.c:285-331`). Their content keys are PCS v2 node-key rings, whose roots come from MLS exporter epochs or NIP-59 grants (`shared-vault-composition.md:106-148,279-321`). Members share the vault content keys for an epoch; nothing is device-specific.

### 2.6 D16(a) is not implemented

There are no `dm-crypt`, `LUKS` or `crypt` checks in `gnome/nostr-homed`, `libporthome`, `packaging` or `docs/porthome-operator-guide.md`. HFR §8.1 says `nostr-homed-provision` should "warn loudly" when `/home` is not on dm-crypt; nothing does. Filed as `nostrc-h10m.10`.

### 2.7 Other provisioning inconsistencies found (not fscrypt-specific)

These were found at source level and not reproduced at runtime. Filed as `nostrc-h10m.11`.

- The real-fetch path hands the root-owned 0700 stage dirfd to `nostr-home-fetch` after `setuid()` to an unprivileged account (`nh_porthome_sandbox.c:219`). The block comment at `auth_porthome.c:1414-1420` says materialisation stays in the broker as root, which contradicts the code.
- `nostr-authd.service.in:54-58` sets `ProtectSystem=strict`, `ProtectHome=yes` and `ReadWritePaths=/var/lib/nostr-auth`. That hides `/home` from the very process that runs `nh_identity_home_prepare` against it.

---

## 3. Empirical validation (what actually ran)

**Environment:**
- Docker Desktop on macOS (arm64).
- VM kernel **7.0.14-linuxkit**, with `CONFIG_FS_ENCRYPTION=y`, `CONFIG_FS_ENCRYPTION_ALGS=y`, `CONFIG_EXT4_FS=y`, `CONFIG_F2FS_FS=y` and `CONFIG_BTRFS_FS=y`. ext4 advertises `encryption` in `/sys/fs/ext4/features`.
- One throwaway container: `docker run --rm --privileged --name h10m7-fscrypt-probe -v <probe dir>:/probe:ro debian:trixie bash /probe/probe.sh`.
- e2fsprogs 1.47.2, f2fs-tools, btrfs-progs and python3 from trixie.

**`fscryptctl` is not packaged for Debian trixie/bookworm or Ubuntu 24.04** (arm64 apt search). Only `fscrypt` 0.3.5 / 0.3.3 and `libpam-fscrypt` are. The probe therefore drives the raw `<linux/fscrypt.h>` ioctls from a 40-line Python driver (`fsc.py`):
- `FS_IOC_SET_ENCRYPTION_POLICY` and `FS_IOC_GET_ENCRYPTION_POLICY_EX`;
- `FS_IOC_ADD_ENCRYPTION_KEY` and `FS_IOC_REMOVE_ENCRYPTION_KEY[_ALL_USERS]`;
- `FS_IOC_GET_ENCRYPTION_KEY_STATUS`.

These are the same calls nostr-authd would make in C.

**Run history:**
- The first run failed: the apt install failed silently. It also produced one false "deterministic" line from comparing two empty files. That run is discarded.
- The second run's busy-file holder leaked a child process. The run was fixed and repeated.
- `output.txt` is the final, clean run of `probe.sh`. It uses `FSCRYPT_POLICY_FLAGS_PAD_16` (`flags=0x2`).
- A follow-up run, `policy-params.sh` → `policy-params-output.txt`, tested the parameters proposed in §5.1 (`PAD_32`, and AES-256-HCTR2 filenames). Its first attempt passed the wrong mode number for HCTR2 (6 is AES-128-CTS) and got `EINVAL`; it was corrected to mode 10 and rerun.
- The containers were `--rm`. Afterwards no `h10m7*` containers existed and no loop devices were attached in the VM. Pulled base images (`debian:trixie`, `debian:bookworm`, `ubuntu:24.04`) were left in the local image cache.

Keys were derived in-container exactly as proposed in §5: `seed → home_key (porthome/v1/home) → HKDF(salt="porthome/local/fscrypt/v1", info=…, L=64)`.

| # | Claim | Result |
| --- | --- | --- |
| E1 | Re-deriving from the same seed gives the same 64-byte master key; a different device salt gives a different key and a different kernel key identifier | **Confirmed** (`output.txt`: device A `9dcc…` vs device B `1c98…`; `9dcc…` returned again on re-add after full removal) |
| E2 | A v2 policy (AES-256-XTS / AES-256-CTS) can be set on the empty `.nostr-<opid>` stage | **Confirmed** (`v2 contents=1 filenames=4`) |
| E3 | A policy cannot be set on a non-empty directory | **Confirmed**: `ENOTEMPTY`. The policy must therefore be applied *before* `copy_tree` (§4.1) |
| E4 | The provisioner's install step — `renameat2(RENAME_NOREPLACE)` of the encrypted stage into an unencrypted `/home` — works | **Confirmed** (`rc=0`) |
| E5 | A key added by root is usable by uid 1000 (the v2 keyring is per-filesystem; access is gated only by DAC) | **Confirmed**: uid 1000 read the file |
| E6 | A subdirectory of an encrypted home cannot take a different (per-vault) policy | **Confirmed**: `EEXIST`; the subdirectory inherits the home's key identifier |
| E7 | Cross-policy `rename`/`link`, and unencrypted→encrypted `rename`, are refused | **Confirmed**: `EXDEV` for all three |
| E8 | Removal is per-claim: when root and uid 1000 both added the key, root's plain remove leaves it `PRESENT` (user_count 1) | **Confirmed**: `other_users=True` |
| E9 | `REMOVE_…_ALL_USERS` while a file is open yields `INCOMPLETELY_REMOVED`, and the open file **and cached directory names stay readable** | **Confirmed**: `files_busy=True`; `cat` still returned plaintext; `ls` still showed plaintext names |
| E10 | After the holder exits, a repeated remove completes (`ABSENT`) | **Confirmed** |
| E11 | Locked view: names are ciphertext and creates fail with `ENOKEY`; **sizes, mtimes, modes, owners and tree shape stay visible** | **Confirmed**: the 17-byte file is still shown as 17 bytes |
| E12 | The plaintext marker is absent from the raw image | **Confirmed** (`grep -c` = 0) |
| E13 | Root can `rm -rf` a locked tree without the key (recovery) | **Confirmed** |
| E14 | A 32-byte master key is accepted for an AES-256-XTS v2 policy | **Confirmed**. We still specify 64 bytes (§5) |
| E15 | f2fs `-O encrypt` supports the same v2 policy | **Confirmed** |
| E16 | Default `mkfs.ext4` (trixie e2fsprogs 1.47.2) does **not** enable `encrypt`; add-key and set-policy fail with `EOPNOTSUPP` | **Confirmed** |
| E17 | `tune2fs -O encrypt` on an existing ext4, mounted or unmounted, enables the feature without reformatting | **Confirmed** on a loop image (both online and offline) |
| E18 | btrfs: fscrypt ioctls fail with `ENOTTY` on this 7.0 kernel | **Confirmed** |
| E19 | The proposed policy (v2, AES-256-XTS / AES-256-CTS, `PAD_32`) sets and round-trips a file | **Confirmed** (`flags=0x3`) |
| E20 | AES-256-HCTR2 filenames: the kernel *accepts* the policy, but the first create fails with `ENOPKG` on this kernel (the algorithm is not built in) | **Confirmed**. Setting a policy does not prove it is usable; a trial create does (§4.1) |

This is diagnostic evidence from one kernel in a Linux VM on a loop image. It is not evidence from a GNOME session, a distro kernel, real hardware, or the nostr-homed binaries, none of which were exercised.

---

## 4. Feasibility

### 4.1 Provisioner: set the policy on the empty stage, before skel

The policy has to be applied in `nh_identity_home_prepare` itself, immediately after `mkdirat` and `fchmod` (`identity_home.c:172-173`) and **before** `copy_tree` (line 175). The `label` callback is too late, because skel has already populated the stage (E3).

Concretely, `nh_identity_home_options` gains an optional `encryption` member: a v2 policy plus a "key already added" precondition. When it is set, `home_prepare`:

1. Uses `FS_IOC_GET_ENCRYPTION_POLICY_EX` on `home_root` only to *inspect* it (`ENODATA` = no policy; `ENOTTY` = no fscrypt at all, e.g. btrfs). This does not prove that ext4's `encrypt` feature or the chosen algorithms are available.
2. Relies on the real capability check, which is done once at enrolment, not here. It is a **trial**: add the key, set the exact policy on a scratch empty directory on the same filesystem, create, write and read back a file, remove the directory, then remove the key. That catches `EOPNOTSUPP` (no `encrypt` feature: E16), `ENOTTY` (E18), `EINVAL` (bad mode pair) and `ENOPKG` (algorithm missing: E20) before anything is committed.
3. Sets the policy on the stage.
4. Records the key identifier in the operation's evidence, alongside `st_dev`/`st_ino`.

Everything after that works unchanged:
- skel copy and materialisation write through the policy, because the key is in the filesystem keyring (E5);
- `.part` + `renameat` inside the stage stays same-policy;
- the `RENAME_NOREPLACE` install into unencrypted `/home` works (E4).

Crash recovery is also unchanged. A stage left behind is encrypted and readable only while the key is present. The `ambiguous → REPAIR_REQUIRED` invariant (never delete) holds, and root can still remove it later without the key (E13).

**The existing-home path needs one new check: policy verification on every login.** fscrypt provides no integrity. An attacker with offline write access can replace `/home/alice` with an *unencrypted* directory holding a hostile `.bashrc` or `.config/autostart`, and today's ownership+mode+inode check would accept it after a re-install. With fscrypt enabled, `nh_identity_home_validate` must compare `FS_IOC_GET_ENCRYPTION_POLICY_EX` against the stored identifier and fail to `REPAIR_REQUIRED` on mismatch or absence.

### 4.2 Unlock at login

- **Where:** in the broker, inside `PROVISION_HOME`, which is the only place `home_key` exists (`auth_porthome.c:366`).
  1. Derive the fscrypt master key (§5).
  2. Call `FS_IOC_ADD_ENCRYPTION_KEY` on an fd for `home_root`.
  3. Check that the returned identifier equals the stored one, then wipe the key.
  4. Continue with materialisation (first login) or validation (subsequent logins).

  `PROVISION_HOME` must not answer `IN_PROGRESS`/`READY` to PAM until the key is added and its identifier verified. `IN_PROGRESS` today means only "job started", which is not proof of unlock. Unlock and lock must also be **serialised per account** (§4.5).

  The kernel holds the key from then on. Nothing new is written to disk, and nothing goes into `/run`.
- **Only root adds the key.** The broker adds it once per login as root, and is the single claimant. User processes never learn the key, so E8's claim semantics stay simple. A second concurrent session for the same user just re-adds the same key, which is idempotent (the claim already exists).
- **Ordering:** the key must be in the kernel before anything stats or opens `$HOME`. Above all, it must precede `pam_systemd`, which starts `user@UID.service`; that service reads `~/.config/systemd/user`, and GNOME's session services start under it. pam_fscrypt documents the same constraint. Today the README places `pam_nostr` after `pam_systemd` (§2.4), so the packaged PAM snippet and the README must move the session line before `pam_systemd`. `PROVISION_HOME` is already synchronous up to `READY`/`IN_PROGRESS` with a bounded wait, which is compatible.
- **Prerequisite:** none of this runs until `nostrc-h10m.9` is fixed, because `PROVISION_HOME` currently never reaches the job.

### 4.3 syncd

No change to syncd's I/O is needed.
- Its writes are same-directory temp+`renameat` (`nh_syncd_reconcile.c:383-412`), and it never moves files in from outside `$HOME`, so E7's `EXDEV` cannot arise.
- Its state, including the plaintext-path `snapshot.json`, and the ciphertext blob cache both sit under `$HOME` and become encrypted at rest for free. That closes a metadata leak LUKS-less boxes have today.
- The cost is double encryption of the blob cache. It is negligible.

Two lifecycle requirements:
- syncd must flush its push queue on its own stop path (SIGTERM, before exit), not after it has stopped. Otherwise unpushed edits become unreachable until the next login on this device.
- Because syncd is a `systemd --user` service, it stops with `user@UID.service`. That is exactly the event the lock should follow.

### 4.4 FUSE overlay

The `~/Portable` mountpoint is a directory inside the encrypted home. FUSE content never touches that filesystem, and its plaintext tier is memory-only. The interaction is limited to the lock:
- the mount must be gone before the key is removed, or the mountpoint dentry and the daemon's open files keep the key `INCOMPLETELY_REMOVED` (E9);
- the user unit's `auto_unmount` and stop-on-session-end ordering already provide this, provided the lock waits for `user@UID.service` to stop.

One existing defect is relevant here: the unit's cleanup is hardcoded to `%h/Portable` even when `NH_FUSE_MOUNTPOINT` overrides it.

### 4.5 Lock at logout

`pam_sm_close_session` is the **wrong hook**:
- it runs per session, before the user manager exits;
- GNOME, tracker and gnome-keyring outlive it;
- with linger, the user manager never exits at all.

The lock belongs to the broker, driven by logind:

1. The broker watches `org.freedesktop.login1` for the user's last session ending, then waits for `user@UID.service` to finish stopping. syncd's final flush happens on its own stop path (§4.3), so by then it is done. A systemd-unit alternative is *not* equivalent. `After=user@%i.service` makes a unit stop **before** the user manager, and `PartOf=` only propagates stop, so neither runs an action after the user manager has gone. Use the broker watcher.
2. It calls `FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS`.
3. On `FILES_BUSY` (E9), it records `locking` in `nh_porthome_status` and retries as stragglers exit. The kernel finishes the job on a later remove (E10). Until then, open files and cached directories remain readable (E9), so the status must not claim `locked` early. The same "verify the effect, never over-claim" rule applies as in HFR §3.4.

**Serialise unlock and lock per account.** The root claim is idempotent, so it is *not* a session reference count. A stale lock worker (or a `FILES_BUSY` retry loop) from the previous logout could otherwise remove the key out from under a login that has just re-added it. The broker keeps a per-account lifecycle generation and an active-session count. A new `PROVISION_HOME` cancels any pending lock or retry before adding the key, and a lock acts only if its generation is still current and no session is active. The same path cleans up after a failed provisioning or session start, so that a key added for a session that never started is removed again.

**Linger** keeps the user manager alive indefinitely, so a lingering portable-home user is never locked. Refuse to enable fscrypt for lingering accounts, or warn.

**Suspend** keeps the key in RAM. This is the same as LUKS, and out of scope.

### 4.6 Multi-device

fscrypt sits entirely below the sync layer. It changes no wire format, manifest, event or Blossom object.
- Each device has its own local policy, keyed per device (§5), and its own lock lifecycle.
- Two devices with the same identity derive independent fscrypt keys from the same `home_key` plus their own device salt. Neither device's key opens the other's disk.
- The kernel's key identifier is published in every policy on disk. Per-device diversification also makes those identifiers unlinkable across devices: two stolen disks cannot be matched to one identity by comparing identifiers.

---

## 5. Key custody

### 5.1 Derivation

```
fscrypt_master (64 B) = HKDF-SHA256(
    salt = "porthome/local/fscrypt/v1",
    ikm  = home_key,                       # PCS v1 home_key, as derived at auth_porthome.c:366
    info = "home" || 0x00 || account_id    # 36-char identity-store UUID
                  || 0x00 || device_salt   # 32 B random, minted at first fscrypt provision on this box
                  || be32(fscrypt_generation),
    L    = 64)
```

**Why it cannot collide with PCS v1/v2:**
- Every existing salt lies in one of the two wire-format namespaces, `porthome/v1/*` and `porthome/v2/*`. That includes `porthome/v1/wrap`, and the MLS exporter label `porthome/v2/vault` lives in the same string space.
- These namespaces are tied to `PORTHOME_WIRE_VERSION` and bump in lockstep (`crypto.c:26-27`, `porthome-crypto-spec.md:33`).
- `porthome/local/…` sits outside both, so no future `porthome/v3/*` wire bump can reach it. The `local/` segment also marks a key that must never appear on the wire.
- The HKDF salt is the HMAC key for the extract step. Distinct salt strings therefore give independent PRKs even though the IKM (`home_key`) is shared with the chunk, manifest and name derivations.
- The name-key derivation has the identical shape (`info=""`). Separation must not rest on `info`, which is why the salt carries it. The non-empty `info` additionally binds account, device and generation.

The salt must be added to the `porthome-crypto-spec.md` §2 purpose table, with test vectors, in the same change that introduces it.

**Why anchor on `home_key`, not on the seed or the v2 root:** the broker already holds `home_key` at the exact point the key is needed. If the broker ever stops deriving PCS v1 `home_key` (a v1 sunset), the anchor changes. That is handled like any re-key: bump `fscrypt_generation` and re-provision (§6).

**Why 64 bytes:** v2 accepted 32 bytes for AES-256-XTS (E14), but 64 is the kernel maximum and costs nothing. The kernel then derives its per-file keys and the public key identifier from it with its own HKDF-SHA512.

**`device_salt`:**
- It is not secret.
- It is stored with the account record under `/var/lib/nostr-auth`, root-only, together with the expected key identifier and the generation.
- Losing it locks the local copy out. Recovery is re-provisioning from relays (§6).
- It buys three things: unlinkable identifiers (§4.6); containment if a master key is extracted from one running machine; and a *conditional* per-device erase. Deleting the salt removes derivation metadata, not key material. The key identifier stays in the on-disk policies, and anyone holding `home_key` plus a surviving copy of the salt (from a backup of `/var/lib`, or the store's own unlinked blocks) can regenerate the key. This is not secure erasure unless every copy of the salt is gone and the key is not loaded.

**Policy parameters:** v2, AES-256-XTS contents, AES-256-CTS filenames and `FSCRYPT_POLICY_FLAGS_PAD_32` (E19). AES-256-HCTR2 filenames (kernel ≥ 6.0) are optional, only when the enrolment trial passes: a kernel can accept an HCTR2 policy yet lack the algorithm (E20).

### 5.2 Signer availability at unlock

- **LOCAL vault:** the seed comes from the vault private key after `vault_open` with the passphrase, entirely local. Offline login unlocks fine.
- **NIP-46:** the seed exists only after the bunker answers the one `nip44_decrypt` of `wrapped_home_key` per login. fscrypt adds **zero** extra signer round-trips; it rides on the unwrap the login already makes.
  - **Bunker offline:** NIP-46 *authentication* already requires a fresh signed challenge, so an offline bunker already means no login. fscrypt is no regression here.
  - **Approved sign, refused or failed unwrap:** this is the new hazard. Today an unwrap failure maps to `NOT_SUPPORTED` and the user gets an ordinary home (`auth_broker.c:1408-1415`). With fscrypt, that would drop the user into a *locked* home: ciphertext names, `ENOKEY` on every create, and a broken GNOME session. The broker must **fail closed** for fscrypt-enrolled accounts. Either refuse the session with a clear `pam_error`, or (later) offer an explicit tmpfs ephemeral home. Never silently open a locked one.
  - The provider permission grant must keep `nip44_decrypt` mandatory (HFR §4.3). For fscrypt-enrolled accounts, a bunker that declines it is a hard error.

### 5.3 Offline login, and why not pam_fscrypt-style protectors

An offline NIP-46 login could be supported by caching the fscrypt key under a local protector: a passphrase, or a TPM, as `fscrypt`/pam_fscrypt do with `/.fscrypt` metadata. **Not recommended.**
- It creates a second, persistent key-custody system on disk.
- It re-introduces exactly the box-owner and cold-disk exposure that (c) exists to remove.
- It does nothing for LOCAL, which is already offline-capable.

### 5.4 Where the key lives

| Location | Lifetime |
| --- | --- |
| Broker job memory (`mlock`ed, wiped at worker exit; same as `home_key`) | Microseconds |
| Kernel per-filesystem keyring, while unlocked | Login → lock |
| Disk, `/run`, the user session, the PAM process | Never |

Root on a running, unlocked machine can read the plaintext through normal DAC (E5). That is inherent to every at-rest scheme.

---

## 6. Recovery and failure modes

The local home is a **cache of relay state plus unpushed edits**. Almost every lock-out is therefore recovered by deleting the locked tree (root can, without the key: E13) and re-provisioning. The real loss surface is only unpushed edits, the same as a stolen laptop.

| Failure | Effect | Recovery / mitigation |
| --- | --- | --- |
| Bunker refuses or fails `nip44_decrypt` | No seed, so no key: the home stays locked | Fail closed (§5.2). Retry the login when the signer is available. |
| Bunker unreachable | Authentication itself fails today | None needed; no change from today. |
| Seed changes (provider re-enrolled with a new random seed, account key rotated) | New `home_key`. The old local home *and* the remote content sealed under the old seed are both unreadable. | **Not re-provisionable.** Re-provisioning only fetches what the new seed can open. Without the old seed or an independently decryptable backup, the data is lost. A planned rotation must keep the old seed until the remote home has been migrated to the new key *and* the local home re-keyed (`fscrypt_generation` bump); only then is the old seed discarded. This is a pre-existing hazard of the wrap design that fscrypt does not change. |
| `device_salt` or identity-store record lost (reinstall of `/var/lib`) | Local home unreadable | Delete and re-provision from relays. Unpushed edits are lost. |
| Lock with busy files (E9) | `INCOMPLETELY_REMOVED`: open files and cached names stay readable | Retry on process exit and report `locking`, not `locked`. Waiting for `user@UID.service` to stop avoids most of it. |
| Linger enabled | The home never locks | Refuse or warn at enrolment. |
| Filesystem lacks `encrypt` (E16) | `EOPNOTSUPP` | Fall back to D16(a). The operator can run `tune2fs -O encrypt` online (E17). Check bootloader compatibility first if `/boot` shares the filesystem: older GRUB builds refused ext4 with unknown incompat features. |
| btrfs / XFS (E18) | `ENOTTY` / unsupported | D16(a) only. |
| Policy swapped or stripped offline (downgrade) | A hostile plaintext home could be accepted | Verify the policy identifier on every login (§4.1); `REPAIR_REQUIRED`. |
| Migrating an existing plaintext home | fscrypt cannot encrypt in place, and old plaintext remains in free blocks (no secure delete on SSDs) | Re-provision into a new encrypted stage, exchange, delete the old tree. Document the residue; recommend enrolling fscrypt on fresh homes. |
| Broker crash after add-key | Key stays loaded until the lock or a reboot | The lock watcher reconciles on broker start. Reboot clears the kernel keyring. |
| Logout lock racing a new login (stale `FILES_BUSY` retry) | Key removed under a live session | Per-account serialisation and lifecycle generation (§4.5). |
| Provisioning or session start fails after add-key | Key loaded with no session | The failure path removes the key under the same serialisation (§4.5). |
| Admin reset of user credentials | Cannot reveal the home | By design. |
| File-level backup tools | See plaintext only while unlocked; raw-image backups hold ciphertext | Document. |

---

## 7. Shared vaults: per-subtree keys vs one home policy

**Recommendation: one home policy. Vault keys stay at the wire layer.**

1. **Per-subtree policies are impossible under an encrypted home.** A subdirectory inherits its parent's policy and refuses a different one (E6). Per-vault fscrypt keys would require `/home/<user>` itself to be *unencrypted*, with each top-level subtree separately encrypted. The kernel refuses a `rename`/`link` whose **destination directory** is encrypted under a different policy (or is encrypted while the source is not) with `EXDEV` (E7). Moving an encrypted entry into an *unencrypted* parent is allowed (E4). Into-vault and between-vault moves would therefore become copies: syncd's conflict moves between a vault and `Portable/.conflicts`, a user's `mv` of a file into `~/Portable/Team`, and any tool that stages temp files outside the vault before renaming them in. Ordinary same-directory atomic saves are unaffected.
2. **fscrypt cannot re-key in place.** Vault keys rotate on every MLS epoch (`shared-vault-composition.md:279-304`). Mirroring them into fscrypt would force a full re-materialisation of the subtree per membership change.
3. **No security gain on a single-user device.** The at-rest threat is per device, and the user's home key and vault keys are loaded and dropped together at login and logout. Revocation of a vault member is enforced by the wire layer: the member can't decrypt new epochs. A local fscrypt key cannot claw back plaintext the member already materialised on their own device either way.

The one case that would justify per-subtree keys is a vault shared between *different Unix users on the same box*. That is out of scope (HFR §1.3 and D18).

---

## 8. Platform support

| Platform | Status |
| --- | --- |
| **ext4** | fscrypt since Linux 4.1. v2 policies and `FS_IOC_ADD_ENCRYPTION_KEY` since 5.4; HCTR2 filenames since 6.0. Needs the `encrypt` feature, which default `mkfs.ext4` does **not** set (E16) but `tune2fs -O encrypt` adds without reformatting (E17). Debian/Ubuntu default root filesystem. **Supported** (E2–E14). |
| **f2fs** | Same v2 API (since 5.4); needs `-O encrypt`. **Supported** (E15). Rare on desktops. |
| **btrfs** | No fscrypt in the tested 7.0 kernel (E18: `ENOTTY`). Fedora Workstation default since F33. **Unsupported** → D16(a). |
| **XFS** | No fscrypt support. RHEL default. **Unsupported** → D16(a). |
| **Kernel config** | `CONFIG_FS_ENCRYPTION` (plus `_ALGS`). Enabled in the tested linuxkit kernel. Mainstream distro kernels are believed to enable it, but we have not verified this per release; the capability probe (§4.1) makes this a runtime check rather than an assumption. **Minimum: 5.4** (v2 + add-key ioctl). |
| **fscrypt (Go) / pam_fscrypt** | Packaged: Debian trixie 0.3.5, bookworm and Ubuntu 24.04 0.3.3, with `libpam-fscrypt`. **Do not use.** Its protector model (`/.fscrypt` metadata, login-passphrase or raw-key protectors, its own PAM unlock) is a second, on-disk key-custody system. It has no notion of `home_key`, and no passphrase exists for NIP-46 logins. It may coexist on a box but must not manage portable-home directories. |
| **fscryptctl** | Not packaged in the Debian/Ubuntu arm64 repos checked. Not needed. |
| **Our implementation** | Five ioctls from `<linux/fscrypt.h>`, issued by nostr-authd. The probe's 40-line driver (`fsc.py`) exercised all of them. No new runtime dependency. |
| **Tests / CI** | Need a privileged Linux container with loop devices, as the probe used. That is an explicitly invoked, Linux-only check, never part of the push hook. |

---

## 9. Threat-model delta vs LUKS-required (D16a)

| Adversary / situation | LUKS-required (a) | fscrypt home (c) | Both |
| --- | --- | --- | --- |
| Powered-off device theft, passphrase-unlocked LUKS | Everything protected: contents, names, metadata, swap if encrypted, `/tmp`, `/var` | Contents and names only. **Leaks** sizes, timestamps, modes, owners and tree shape (E11), plus everything outside `$HOME`: coredumps in `/var/lib/systemd/coredump`, journald, `/var/tmp`, unencrypted swap | Best |
| Theft of a TPM-auto-unlock LUKS device (unlocks at boot) | Disk opens at boot; logged-out homes are protected only by DAC against a booted-system attacker | Logged-out homes stay locked until that user logs in | (c) adds real value |
| **Shared/lab box: the portable-home core scenario.** Box admin or other users, after the user logs out | **No protection**: LUKS is the box owner's key, and the home is plaintext to root | **Protected** once the lock reaches `ABSENT` (E10–E12), *provided the host was not compromised during the session*. The broker holds `home_key`, and seed drops pass through `/run` (§2.1), so a root that was already hostile can have kept them. | (c) is the only answer for an honest-at-the-time host |
| Disk disposal / decommissioning of a box a user once used | Plaintext home survives unless wiped | Unreadable without `home_key` and the device salt. This is not a guarantee of erasure: the salt may survive in backups (§5.1), and `home_key` is recoverable by the user's signer. | (c) |
| Malicious root *while* the user is logged in, or at their next login | Defeated | Defeated (captures the key or reads through DAC) | Neither |
| Running or suspended machine | Key in RAM | Key in RAM; busy-file window after logout (E9) | Neither |
| Offline tampering | No integrity (XTS), but structure hidden | No integrity, structure visible; **policy-swap downgrade** must be checked at login (§4.1) | Check required |
| Box without disk encryption at all (common on lab machines and VMs) | Not satisfiable; v1 merely warns, and that warning is not implemented (`nostrc-h10m.10`) | Works on any ext4/f2fs with `encrypt` | (c) |

**Net:** (c) is strictly better than (a) for the scenario this product exists for: your home on a machine you do not own. It is worse than (a) only on metadata and on the non-`$HOME` leak surface. The right posture is therefore **(a) always plus (c) where available**, never (c) instead of (a).

---

## 10. Recommendation and phased plan

**Adopt later.** Accept D16(c) as the target, as an opt-in complement to D16(a), and file the implementation now (`nostrc-h10m.12`). Do not start it until:

- `nostrc-h10m.9`: PAM→broker `PROVISION_HOME` works end-to-end. fscrypt's unlock lives inside that call.
- `nostrc-h10m.10`: the D16(a) baseline exists. fscrypt is the upgrade from a working warning, not a substitute for a missing one.

`nostrc-h10m.11` (fetch helper vs stage ownership; `ProtectHome=yes`) should be settled first too, because F2 changes the same function.

**Phases (`nostrc-h10m.12`):**

- **F1 — Key and capability.** No behaviour change.
  - Add `porthome_fscrypt_master_derive()` to libporthome per §5.1.
  - Register the salt in `porthome-crypto-spec.md` §2, with vectors.
  - Add the broker's enrolment trial on `home_root`'s filesystem (§4.1): add key, set policy, create and read back, clean up.
- **F2 — New homes.**
  - Add the `nh_identity_home_options.encryption` policy, applied right after `mkdirat` and before `copy_tree` (§4.1).
  - The broker adds the key in `PROVISION_HOME`, before materialisation.
  - Persist `device_salt`, the identifier and the generation.
  - Verify the policy on every login.
  - Fail closed when the seed is unavailable (§5.2).
  - Move the PAM session line before `pam_systemd` in packaging and docs.
  - `PROVISION_HOME` answers only after the key is verified (§4.2).
  - F2 must not be enabled operationally until F3 exists: an unlock with no lock leaves homes unlocked indefinitely.
- **F3 — Lock lifecycle.**
  - Add the logind-driven lock after `user@UID.service` has stopped, serialised per account with a lifecycle generation (§4.5).
  - Retry on `FILES_BUSY` and surface `locking`/`locked` in `nh_porthome_status`.
  - Refuse or warn on linger.
- **F4 — Migration and re-key.**
  - Re-provision into a new encrypted stage (relay plus unpushed local), exchange, then delete the old tree.
  - Use a generation bump for re-key.
  - Document plaintext residue in free blocks.

**Exit criteria:**
- a two-machine run with independent device keys;
- offline LOCAL login unlocks;
- a NIP-46 unwrap denial fails closed;
- a re-login during a busy-lock retry keeps its key;
- after logout, key status reaches `ABSENT`;
- the locked view shows ciphertext names and `ENOKEY`;
- the policy-swap downgrade is refused.

When F2 lands, update HFR D16 to "(a) always; (c) opt-in where the filesystem supports it".

**Versioning:** this change is documentation only. It alters no shipped artifact, so no component version is bumped.
