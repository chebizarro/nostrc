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
