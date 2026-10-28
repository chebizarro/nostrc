# Portable-home operator guide

> **Audience.** Sysadmins and site reliability engineers operating the
> nostr-homed portable-home stack. Assumes familiarity with systemd user
> units, PAM, and the general shape of a Nostr relay + Blossom deployment,
> but not with the internals of any specific component in this repository.
>
> **Scope.** Everything from a fresh account file through a first push,
> including day-2 operations (rotate, verify, disaster recovery). For the
> underlying protocol, see `gnome/nostr-homed/docs/DESIGN.md`. For the
> per-tool reference, see `man nostr-homed-provision(1)` and friends.

## 1. What the stack does

The portable-home stack turns a Nostr identity + Blossom capacity into a
POSIX home directory that follows the user between machines. There are
four components, three of which are daemons or long-running processes and
one of which is the operator front-end this guide focuses on.

| Component               | Role                                                    | Runs as         |
|-------------------------|---------------------------------------------------------|-----------------|
| `nostr-authd`           | PAM broker; drops the seed to `/run/nostr-auth/`.       | root (systemd)  |
| `nostr-home-syncd`      | Snapshot + chunk + encrypt + upload + republish.        | login user      |
| `nostr-home-fuse`       | Read-only overlay at `$HOME/Portable`.                  | login user      |
| `nostr-homed-provision` | Operator CLI: enroll, push, pull, verify, status.       | login user or root |
| `nostr-home-fetch`      | Unprivileged helper spawned by broker and provisioner.  | dedicated uid   |

The four data movements are:

1. **Provisioning.** The operator (or an ansible role) runs
   `nostr-homed-provision enroll` to mint an account file and publish an
   empty (gen-0) kind-30078 pointer.
2. **Login.** The broker (`nostr-authd`) reads the account file, drops the
   seed at `/run/nostr-auth/session/<uid>/home_seed`, and forks
   `nostr-home-fetch` to materialize the current snapshot into a staging
   directory before the PAM session opens.
3. **Steady state.** `nostr-home-syncd` watches `$HOME`, batches changes,
   uploads chunks to Blossom, and republishes a gen+1 pointer.
   `nostr-home-fuse` serves the last-known-good snapshot at
   `$HOME/Portable` so applications can read historical content without
   racing the writer.
4. **Recovery.** On a fresh machine, `nostr-homed-provision pull` runs the
   same fetch helper the broker uses — but into an arbitrary staging
   directory, so the operator can inspect it before promoting.

## 2. Fresh install: from zero to a live home

### 2.1 Prerequisites

* Debian/Ubuntu with `nostr-home-sync` and (optionally) `nostr-home-fuse`
  packages installed, OR Fedora with the same subpackages from the
  `nostr-login` RPM.
* A running relay reachable at `wss://...` and a Blossom server reachable
  at `https://...`. Two of each is the minimum for the default
  `--min-replication 2`.
* Root on the target machine (needed to drop the seed).

Verify the CLI is on `$PATH`:

```sh
which nostr-homed-provision nostr-home-status nostr-home-syncd
man nostr-homed-provision
```

### 2.1.1 Required disk encryption and the warning policy

Portable homes require an encrypted root or home volume. Relay/Blossom
ciphertexts do **not** protect materialized local files, including the hot cache,
from an offline attacker. Use LUKS/dm-crypt on the filesystem holding the actual
home (or the `pull --dest` target), not merely on another partition.

Following [D16(a) and §8.1](designs/home-from-relay.md#81-threat-model), v1 defaults
to a **loud warning**, not a new login refusal. This preserves existing hosts
while making their plaintext-at-rest limitation visible. Sites that need
fail-closed provisioning can opt in:

```ini
# /etc/nostr-auth/auth.conf; restart nostr-authd after changing it
porthome_require_encrypted=on
```

For a manual restore use `nostr-homed-provision pull --require-encrypted
--account-file PATH --dest DIR` (on one command line). The flag is independent
of the broker configuration; the CLI does not load `auth.conf`. Both routes
refuse **unencrypted and unknown** storage before fetching plaintext. The CLI
returns 64; first-login provisioning fails and PAM refuses to open the session.
The setting defaults off; explicit `off`, `0`, `false`, or `no` disables it.
Other values, including empty or malformed values, enable refusal rather than
silently weakening it. It governs provisioning, not retroactive locking or
migration of already-installed homes.

The detector reads the target directory's device number and walks
`/sys/dev/block/MAJ:MIN`, partitions and `slaves` through LVM/MD. A `dm/uuid`
with a recognized `CRYPT-LUKS1-`, `CRYPT-LUKS2-`, or `CRYPT-PLAIN-` prefix is
encryption evidence. `CRYPT-INTEGRITY-` and `CRYPT-VERITY-` are not encryption;
their backing devices are inspected instead. Unknown `CRYPT-` types stay unknown. Every branch below a
non-crypt mapping must be covered: one encrypted mirror/PV does not prove the
whole filesystem encrypted. Missing/inaccessible sysfs, incomplete stacks,
cycles, loop devices, network/overlay filesystems and unsupported platforms are
**unknown**, never inferred encrypted. XFS (including possible realtime storage),
F2FS and Btrfs are currently unknown because their additional file-data devices
cannot be proven covered by walking the primary device's sysfs slaves. This is a dm-crypt check, not detection
of fscrypt, hardware encryption, or protection against a privileged attacker
changing mounts or mapping tables. Nonstandard dm-crypt mappings without the
UUID marker may be unknown. Root/swap, external journal devices, other mounts inside the home, and other
storage locations must be secured separately.

Warnings appear on CLI stderr and in the system log, at first login through
PAM, and in the panel status menu. After provisioning, the existing status file
contains `at_rest.state` (`encrypted`, `unencrypted`, or `unknown`) and
`at_rest.checked_at` (Unix seconds). Read them with:

```sh
nostr-home-status --field at_rest.state
nostr-home-status --field at_rest.checked_at
```

This is a provisioning-time observation, not continuous attestation. The panel
labels encryption evidence accordingly and treats missing/invalid status as
unknown. Refusal before a home exists does not create a home just to write a
status file; consult the login warning and broker journal for that failure.

Developer verification in a disposable Linux CI image (not on an operator host):

```sh
cmake --build /build --target test_porthome_at_rest test_auth_conf \
  nostr-homed-provision nostr-home-status nostr-authd pam_nostr nh-seed-authority
ctest --test-dir /build -R '^homed_(porthome_at_rest|auth_conf)$' --output-on-failure
NH_DISPOSABLE_LOGIN_LAB=1 python3 \
  gnome/nostr-homed/tests/integration/test_porthome_at_rest.py --build /build
```

The integration check requires root, `pamtester`, and a CMake build with auth,
identity, PAM and experimental porthome enabled. It uses the container's real
sysfs, checks CLI helper dispatch with a sentinel (not a restore proof), and
exercises authenticated PAM plus configured authd refusal without relay traffic.
It requires unencrypted/unknown test storage. The fixture unit tests cover both
LUKS/LVM orderings, partition inheritance, MD including mixed branches, loop,
missing metadata, integrity/verity mappings, and cycle refusal. This is not graphical GDM or hosted CI proof.

### 2.1a Opt-in fscrypt home lock

D16(a) remains the baseline. D16(c) adds a **per-home at-rest lock**, not
protection from root while unlocked, swap, filesystem metadata, external mounts,
or files copied elsewhere. Keep encrypted root/swap. One policy covers the
whole home; vault subdirectories inherit it and keep their separate wire keys.

Build the Linux broker with `-DNOSTR_HOMED_ENABLE_FSCRYPT=ON` (default **OFF**),
alongside auth runtime and experimental porthome. This requires libsystemd
headers/libraries at configure time; only authd links the watcher, not PAM.
CMake is the supported auth runtime build. Meson's `fscrypt` option defaults to
`disabled`; enabling it fails clearly and names the CMake option. It does not
build or port the auth runtime.
Then explicitly enable **new** portable homes in `/etc/nostr-auth/auth.conf`:

```ini
porthome_fscrypt=on
# Leave OFF normally. Required for a consciously accepted unknown/plain backing
# device, including F2FS whose full dm-crypt topology cannot currently be proven.
porthome_fscrypt_allow_without_luks=off
```

Unknown or unencrypted backing storage is refused unless the second setting is
explicitly `on`. That override does not bypass `porthome_require_encrypted=on`.
Malformed fscrypt settings refuse broker startup. Ext4 and F2FS require their
`encrypt` feature; btrfs, XFS and ext4 without it are refused, **not** restored
as plaintext. Enrollment performs a real encrypted-file create/write/read test
on the same filesystem, not just SET_POLICY. Linger is refused for new
fscrypt homes; do not enable it later unless you accept delayed locking.

The broker adds the key at authenticated `PROVISION_HOME`; the empty
root-owned stage receives a v2 AES-256-XTS/AES-256-CTS/PAD32 policy **before**
any skeleton or fetched content. The existing rename publishes that stage.
Every subsequent login verifies the recorded inode and exact policy, then
requires a fresh seed unwrap even if another session already loaded the key.
An approved signature with a refused unwrap is **not** a successful session.
The existing signer-denial cache may require the provider's wrap-key reset
before retrying after a permission change. LOCAL vault unlock remains offline.

Keep `pam_nostr`'s session line **before** `pam_systemd`/`common-session` on
all login routes (as in the shipped `gdm-password.sample`). The watcher follows
logind and `user@UID.service`, not the broker socket: PAM closes that socket
before `pam_systemd` runs. Durable per-process admissions protect that handoff.
A live PAM process that never reaches a session deliberately delays locking;
terminate the failed login process, rather than using a timeout to revoke a
possibly successful login. Sessions still closing and an exiting user manager
remain blockers. A new admission invalidates old lock work under the same
serialization used for key add/remove. If files remain open, status is
`locking`, never `locked`; local retries check current lifecycle state again.
A lost system bus suspends locking and refuses new encrypted admissions until
broker restart; inspect its journal. Startup reconciles recorded kernel key claims,
including interrupted maintenance intents without discarding their recovery trees.
Derivation metadata and the expected kernel identifier are committed **before**
ADD_KEY. Bus snapshots run outside the mutation lock and admissions have a bounded
five-second lock wait; contention fails the attempt instead of hanging login.
Removal releases only authd's root claim. Another UID's independently installed
claim keeps the state `locking`; it is not forcibly revoked.

Public derivation metadata and admission/maintenance journals live in
`/var/lib/nostr-auth/fscrypt/state.db`, below a root-only `0700` directory;
there are **no persistent raw fscrypt keys**. Back up this directory with the
identity authority. Turning the runtime setting off does not decrypt existing
homes or stop their lock lifecycle. A build without fscrypt refuses startup
when fscrypt state exists; keep a feature-enabled broker for those accounts.
Do not delete metadata to disable encryption.

`nostr-home-status --field fscrypt.state` and the panel merge the root-owned
`/run/nostr-auth/fscrypt/<uid>.json` snapshot into the normal status model.
This separate nonsecret snapshot stays readable while the home is locked.
States include `preparing`, `unlocked`, `locking`, `locked`, `migrating`, and
`repair-required`; missing status is `unknown`, not proof of a lock.

**Migration/re-key is explicit offline maintenance.** Stop authd, log out the
account, and wait for its user manager to exit. Stop scheduled jobs and other
privileged writers too; root is outside this at-rest threat boundary. Existing plaintext homes are
not converted merely by switching on the config. Supply the existing raw
32-byte wrap seed through a private file descriptor (not argv/environment):

```sh
sudo nostr-authd --fscrypt-migrate /var/lib/nostr-auth ACCOUNT_UUID SEED_FD
# Add NEW_SEED_FD after SEED_FD only for a deliberately coordinated seed change.
# --allow-without-luks is a separate explicit maintenance override if needed.
```

The command holds the identity authority's exclusive process lock and refuses
live sessions, user managers, pending admissions, residual account processes,
and other processes with open descriptors/mappings into the affected trees.
The old/new tree remains below a root-only recovery directory during exchange;
busy-key checks precede deletion of the original. It copies the complete
local cache, including **unpushed edits**, into a new encrypted stage, exchanges
the directories, commits exact inode evidence, then deletes the old tree.
The relay copy is unchanged; sync before maintenance if fresher remote content
must be incorporated. Re-key increments the local derivation generation. It
preserves ordinary files, symlinks, ownership, modes, timestamps and xattrs/ACLs;
hardlinks, special files, symlink xattrs and nested mounts are refused rather
than silently omitted. Resolve those cases explicitly before migration.
Interrupted copying can be resumed with the same command and seeds. A
journaled exchange is replayed only when both inodes match; ambiguous stages
are retained for repair, never guessed or automatically discarded.

**Seed loss is unrecoverable** without the old seed or an independently
decryptable backup: both the local home and remote content use keys derived
from it. A new seed cannot restore old ciphertext. A planned seed rotation
must retain the old seed until both remote migration and local re-key finish.
Lost device salt locks out the local copy; re-provisioning can recover only
relay state, **not unpushed edits**. Deleting an old plaintext tree is **not
secure erasure**: free blocks, SSD remapping, snapshots and backups may retain
plaintext. Prefer enrollment on fresh encrypted storage. File backups see
plaintext only while unlocked; raw filesystem images contain ciphertext and
unencrypted filesystem metadata.

#### Focused Linux verification

Use a **disposable privileged container/VM** with systemd as PID 1, logind,
PAM development files, `libsystemd-dev`, `python3-websockets`, GJS, e2fsprogs and
f2fs-tools. Never mount these test images on a developer's real `/home`.
Build the enabled CMake auth/PAM/porthome targets and `test_fscrypt_kernel`,
`test_fscrypt_pam_session`, `test_fscrypt_unwrap`, and `nh-seed-authority`.
Provide a private Blossom fixture (the repository's
`packaging/lab-blossom/blossom-server.config.yaml`), not a public upload target.

For each ext4 `mkfs.ext4 -O encrypt` and F2FS `mkfs.f2fs -O encrypt` loop image,
mount it at `/home` **inside that disposable environment** and run:

```sh
BUILD=/build
"$BUILD/gnome/nostr-homed/test_fscrypt_kernel" /home
NH_DISPOSABLE_LOGIN_LAB=1 python3 \
  gnome/nostr-homed/tests/integration/run_fscrypt_login.py \
  --build "$BUILD" --blossom http://PRIVATE_BLOSSOM:3000
NH_TEST_FSCRYPT_ROOT=/home "$BUILD/gnome/nostr-homed/test_fscrypt_unwrap"
```

Add `--migrate-plaintext` to exercise plaintext migration, residual-writer and
hardlink refusal, then resume and re-key without losing local edits. For ext4
formatted with `-O ^encrypt`, or btrfs/XFS, pass `--unsupported` to the kernel
and login runners: success means refusal without publishing a plaintext home.
The login runner exercises real `pam_nostr` + `pam_systemd`, manager exit,
busy removal/re-login, offline LOCAL unlock, re-key, CLI status and downgrade
refusal. The unwrap test uses a real in-process encrypted NIP-46 bridge, not an
external signer. Unit CTest cases cover independent HKDF vectors and lifecycle
eligibility/epoch fencing. Run the dependency fence in both default-OFF and
fscrypt-ON builds; only the latter receives `--fscrypt`. Unmount loop filesystems
and remove the task-owned containers/images/network after verification.

### 2.2 Enrol

```sh
sudo nostr-homed-provision enroll \
    --relay wss://relay.example \
    --relay wss://relay.backup \
    --blossom https://blossom.example \
    --blossom https://blossom.backup \
    --out-dir /var/lib/nostr-auth/accounts \
    --seed-drop --uid $(id -u alice) \
    --publish
```

This produces:

* `/var/lib/nostr-auth/accounts/<pubkey8>.account.json` (mode 0600,
  owned root).
* `/run/nostr-auth/session/<uid>/home_seed` (mode 0400, owned alice).
* An empty gen-0 pointer published to both relays.

The account file is the durable record. Back it up out-of-band; losing it
means losing the ability to publish new pointers against the same identity
(there is deliberately no recovery path from Blossom alone).

### 2.3 Start the daemons

```sh
sudo -u alice systemctl --user start nostr-home-sync.service
sudo -u alice systemctl --user start nostr-home-fuse.service
sudo -u alice nostr-home-status --field syncd.state   # → "running"
sudo -u alice nostr-home-status --field fuse.mounted  # → "true"
```

The syncd will read the seed drop and unlink it on first start. If either
daemon reports "waiting" or "not_mounted" for more than a few seconds,
consult §5.

### 2.4 First push

Nominally, the syncd handles this by itself: any writes into `$HOME`
trigger a batch, and the batch closes with a `push` publish that bumps
the generation. To force a full-tree push before the first user login (so
the recovery path works), run manually:

```sh
sudo -u alice nostr-homed-provision push \
    --account-file /var/lib/nostr-auth/accounts/<pubkey8>.account.json \
    --bump-gen
```

Verify:

```sh
sudo -u alice nostr-homed-provision verify \
    --account-file /var/lib/nostr-auth/accounts/<pubkey8>.account.json
```

If every chunk reports "≥ 2" the deployment is healthy.

## 3. Day-2 operations

### 3.1 Where things live

| Kind                            | Path                                                       |
|---------------------------------|------------------------------------------------------------|
| Account file                    | `<out-dir>/<pubkey8>.account.json` (0600)                  |
| Seed drop                       | `/run/nostr-auth/session/<uid>/home_seed` (0400, ephemeral)|
| Syncd state                     | `$XDG_STATE_HOME/nostr-homed/`                             |
| Status file                     | `$XDG_STATE_HOME/nostr-homed/porthome-status.json`         |
| Fuse mountpoint                 | `$HOME/Portable` (or `$NH_FUSE_MOUNTPOINT`)                |
| Fuse status                     | `$XDG_RUNTIME_DIR/nostr-homed/porthome-fuse-status.json`   |
| Sync lock                       | `$XDG_STATE_HOME/nostr-homed/sync.lock`                    |
| Manual disable                  | `~/.nostr-home-limited` (touch to refuse startup)          |
| Quiet-hours config              | `$XDG_CONFIG_HOME/nostr-homed/quiet-hours`                 |
| Quota override                  | `$XDG_CONFIG_HOME/nostr-homed/quota-override`              |

### 3.2 Reading state

The single source of truth is `porthome-status.json`. Query it via
`nostr-home-status`:

```sh
nostr-home-status                  # pretty-print everything
nostr-home-status --json | jq .    # machine-readable dump
nostr-home-status --field syncd.state
nostr-home-status --field fuse.mounted
nostr-home-status --field provisioner.last_push
```

For another user's status (support scripts, monitoring), use
`nostr-homed-provision status --uid <uid>`.

### 3.3 Quiet hours and quotas

To disable syncd chunk-publish traffic during quiet hours:

```sh
nostr-home-status --quiet-hours-set 22:00-06:00
```

To install a per-user quota override:

```sh
nostr-home-status quota --set-override $((5 * 1024**3))  # 5 GiB
nostr-home-status quota --reload                          # SIGHUP the daemon
```

To restore defaults:

```sh
nostr-home-status --quiet-hours-set off
nostr-home-status quota --clear-override --reload
```

### 3.4 Manual push, JSON summary, dry run

Force a push (e.g. before rebooting a mobile machine):

```sh
nostr-homed-provision push \
    --account-file ~/.config/nostr-homed/<pubkey8>.account.json \
    --bump-gen --json | jq .
```

Preview what a push would do, without any traffic:

```sh
nostr-homed-provision push \
    --account-file ~/.config/nostr-homed/<pubkey8>.account.json \
    --dry-run
```

### 3.5 Cross-machine restore

On a fresh machine with only the account file (no seed drop, no live
syncd), materialize the current snapshot into a staging directory:

```sh
nostr-homed-provision pull \
    --account-file ~/.config/nostr-homed/<pubkey8>.account.json \
    --dest /tmp/restore
ls /tmp/restore
```

Then promote it into place (out of scope for this guide — usually a
`rsync -a` into a fresh `$HOME` before the PAM session opens for the
first time).

### 3.6 Nightly replication check

```sh
nostr-homed-provision verify \
    --account-file ~/.config/nostr-homed/<pubkey8>.account.json \
    --json > /var/log/porthome-verify.$(date +%F).jsonl
```

Alert on any exit code other than 0. Exit 5 means at least one chunk is
missing from every server; exit 71 usually means a transient network
issue; anything else is worth a look.

## 4. Exit-code slot map (all tools)

The stack uses a shared exit-code namespace so operators can wire the same
alerting logic against every binary:

| Code | Meaning                                       | Emitters                                   |
|------|-----------------------------------------------|--------------------------------------------|
| 0    | Success                                       | all                                        |
| 1    | Malformed input / field miss                  | status                                     |
| 2    | Bad argument                                  | provision, status                          |
| 3    | Missing state / limited mode                  | status, syncd, fuse                        |
| 4    | NOSTR_HOME_STATE=partial                      | syncd                                      |
| 5    | Chunk missing on verify / write failure       | verify, enroll                             |
| 64   | Bad argument                                  | fetch                                      |
| 65   | SSRF gate rejection                           | fetch, pull                                |
| 71   | Network failure                               | fetch, pull, push, verify, enroll(publish) |
| 72   | Manifest decode failure                       | fetch, pull, verify                        |
| 73   | Size cap exceeded                             | fetch, pull                                |
| 74   | Timeout                                       | fetch, pull                                |
| 75   | Crypto (encrypt/decrypt/sign) failure         | fetch, pull, push, verify, enroll          |
| 76   | Internal error                                | fetch, pull, push, verify                  |
| 77   | Insufficient replication (push only)          | push                                       |

Anything in the 64–79 slot is an ex-`sysexits.h` code, chosen so shell
scripts can pattern-match against classes of failure.

## 5. Troubleshooting

**`syncd.state = "waiting_for_seed"`.** The broker did not drop
`/run/nostr-auth/session/<uid>/home_seed`, or the daemon started before
PAM did. Verify with `ls -l /run/nostr-auth/session/<uid>/`. On a
non-interactive login (SSH into a machine that has never had a graphical
session), the broker may not have run — provision manually with
`nostr-homed-provision enroll --seed-drop --uid <uid>` and restart the
daemon.

**`fuse.mounted = "false"` and `journalctl --user -u nostr-home-fuse`
shows "table_reload failed".** Usually a stale inotify watch on the state
directory. Set `NOSTR_HOMED_PORTHOME_FUSE_NO_INOTIFY=1` in the user
environment (via `systemctl --user edit nostr-home-fuse.service`) to fall
back to polling. File a bug against the kernel of the affected machine —
inotify on state dirs backed by network filesystems is a known source of
flakes.

**Push returns 77 (insufficient replication).** One of the Blossom
servers is silently 5xxing on PUT. Verify with:

```sh
nostr-homed-provision verify \
    --account-file ...account.json --json | jq '.[] | select(.status != "ok")'
```

Add or remove servers with `--blossom`; retry the push. If a server is
permanently gone, edit the account file to drop it, then push again.

**Push returns 75 (crypto).** Almost always a corrupted seed drop or a
mismatch between the account file's `home_key` and the running daemon's
in-memory key. Restart the syncd; if that does not fix it, re-run
`enroll --seed-drop` (the identity does not change; only the derived key
material is re-materialised into `/run/`).

**Push returns 71 immediately.** Relay rejected the pointer. Common
causes: relay requires NIP-42 auth and the daemon is not carrying it
(known-broken; use a relay that does not gate kind-30078), or the
account is out-of-quota on the relay. Check the relay's own logs.

### 5.1 Blossom body-sniffer 415 (community servers)

Some community Blossom servers (`blossom.band`, `blossom.primal.net`)
run body-sniffing content-policy filters that reject encrypted portable-
home chunks with `415 Unsupported Media Type` — the chunks look like
random bytes and libmagic-style detectors classify them as
`application/octet-stream`. Two knobs are available to the operator:

* **`NOSTR_HOMED_HANAMI_UPLOAD_CONTENT_TYPE`** — swap the outgoing
  `Content-Type` header of PUT /upload requests. Empirical evidence
  (see `docs/reviews/porthome-blossom-content-type-2026-09-25.md` §2)
  says this alone does not unlock band/primal — their sniffers ignore
  the header. Useful only when an operator conversation opens a
  specific allowlisted CT for a specific server.

* **`NOSTR_HOMED_BLOSSOM_PNG_SHIM=1`** (opt-in — off by default) —
  wrap every uploaded chunk in a deterministic 41-byte PNG prefix so
  the body sniffer classifies the blob as `image/png`. The prefix
  becomes part of the addressable blob (`sha256(shim||ciphertext)`
  is what goes in the manifest); the fetch helper recognises the PNG
  signature at the head of the downloaded blob and strips 41 bytes
  before decrypting. D4 convergence is preserved because the shim is
  deterministic in the ciphertext bytes and length. Live-probe
  validation on blossom.primal.net returned `200 OK` (hy3e §2);
  blossom.band still 500s (its full PNG decoder blows up on the
  truncated IDAT). See
  `docs/reviews/porthome-hy3e-shim-live-probe-2026-09-25.md`.

  Turn it on for the entire push run and every server in the
  destination set — the design does not support mixed-mode manifests
  in v1. Runtime cost is +41 bytes per chunk. Fetchers detect and
  strip regardless of the env var, so upgrading the pusher to shim
  without redeploying fetchers is safe.

  **Default is now auto-decide (`nostrc-si30`).** With the env var
  unset, the pusher runs a body-sniffer probe against each
  destination server at push start (1 raw + 1 shim PUT per server,
  capped) and flips the shim on iff at least one server has
  `raw_random_ok=NO && png_shim_ok=YES`. The decision is recorded on
  the `push:` log line:

  ```
  push: shim=on  (auto: https://blossom.primal.net requires it — raw_random_ok=no, png_shim_ok=yes)
  push: shim=off (auto: all 2 servers accept raw bytes)
  push: shim=on  (forced by NOSTR_HOMED_BLOSSOM_PNG_SHIM=1)
  ```

  Explicit `=1` / `=0` still wins over auto-decide. Set
  `NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE=1` to disable the probe
  entirely — the pusher then falls back to env-var-only semantics
  (default off).

Neither knob affects existing pushes if unset — the legacy raw-bytes
wire is bit-identical to the pre-bpum behaviour.

**Suggested default Blossom set (as of 2026-09-25):**

```
--blossom https://blossom.sharegap.net
# primal.net is now known-good with the auto-decide shim:
--blossom https://blossom.primal.net
# blossom.band is NOT in the default set — its full PNG decoder
# rejects the shim (500). Reintroduce only after the fuller-PNG shim
# lands.
```

Recommend keeping a self-hosted or partner Blossom server in the
default enroll suggestions — community-run servers may change
content policy at any time. The `packaging/lab-blossom/` recipe
in this repository is a minimum-fuss lab / CI Blossom you can stand
up on your own public IP; see its `README.md` for the SSRF
pre-check requirement (the fetcher refuses private-range Blossom
URLs — the recipe MUST run on a publicly-routable host).

## 6. Uninstall / rotate

To rotate an identity (compromised seed, or migrating to a new hosting
provider):

1. `nostr-homed-provision enroll --relay ... --blossom ... --out-dir ... --publish`
   to mint the new identity.
2. Copy the current home tree into the new identity's staging area:
   `cp -a ~/Portable/. /tmp/rotate/` (this reads through the fuse mount,
   so you get the last-known-good snapshot, not a live directory).
3. `nostr-homed-provision push --account-file <new>.account.json --home /tmp/rotate --bump-gen`.
4. Replace the seed drop for the target uid with the new identity's seed;
   restart the syncd and fuse.
5. Retain the old account file offline for 30 days in case a chunk needs
   to be pulled from the old identity's Blossom set.

The old kind-30078 pointer is not deleted — Nostr has no reliable delete
semantics — but no live daemon will publish against it again.

## 7. Further reading

* `man nostr-homed-provision(1)` — subcommand reference (per-subcommand
  pages linked from SEE ALSO).
* `man nostr-home-syncd(8)`, `man nostr-home-fuse(8)` — the daemons.
* `gnome/nostr-homed/docs/DESIGN.md` — protocol, event schema, threat
  model.
* `gnome/nostr-homed/docs/AUTH_PROTOCOL.md` — broker seed-drop contract.
* `gnome/nostr-homed/docs/SECURITY.md` — the security model in prose.
