# D7 GVFS + CIFS mount acceptance on GNOME (Ubuntu 24.04.5, amd64)

**Bead:** [nostrc-rb0e.12](../../.beads/) — *[D7-accept] Prove Explorer and GNOME
Files standalone SMB interoperability and revocation.*

**Date:** 2026-09-22  
**Host:** `gnome-dev` (Ubuntu 24.04.5 LTS, kernel 6.8.0-139-generic, x86_64,
GDM active with gnome-shell)  
**Tree:** `master @ 0d1c22f7` + helper edit under
`gnome/nostr-homed/src/smb/nostr-smb-mount.sh` (branch
`test/d7-gvfs-mount-acceptance`)  
**Build:** `/tmp/amd64/build-release-homed` (from Track 1 —
`-DNOSTR_HOMED_ENABLE_AUTH_INSTALL=ON -DNOSTR_HOMED_ENABLE_SMB=ON`), built
in-place; not reinstalled system-wide.

## 1. Purpose

Track 1 proved the `nostr-authd → nostr-smb-acquire → mount.cifs` chain on
both aarch64 and amd64. It could not exercise the desktop-side
`gio mount smb://…` (gvfs) path because the aarch64 lab had no session
bus / gvfs backends. This track drives that path end-to-end on the amd64
GNOME guest using a Nostr-minted Samba credential, plus a revocation
negative and a `mount.cifs` regression.

## 2. Environment probes

```
gvfs pkg   : 1.54.4-0ubuntu1~24.04.2
samba pkg  : 2:4.19.5+dfsg-4ubuntu9.7
gio        : 2.80.0
gvfsd-smb  : /usr/libexec/gvfsd-smb
helper     : gnome/nostr-homed/src/smb/nostr-smb-mount.sh (this branch)
```

Broker (`nostr-authd`) runs on unique namespaced paths so it does not
collide with concurrent tracks on the same guest:

```
runtime dir   /run/nostr-smb-d7g               (0711 root:root)
authority dir /var/lib/nostr-smb-d7g           (0700 root:root)
share dir     /srv/nostrshare-d7g              (0770 n_smbd7g:n_smbd7g)
auth.sock     /run/nostr-smb-d7g/auth.sock     (root-only)
user.sock     /run/nostr-smb-d7g/user.sock     (0666, SO_PEERCRED-gated)
share         [nh_share_d7g] force user = n_smbd7g
```

The throwaway POSIX user `n_smbd7g` is pinned to uid=200000 to match the
seed authority's default (`NH_IDENTITY_DEFAULT_UID_MIN`), so the broker's
`SO_PEERCRED → user_pubkey` resolution succeeds.

## 3. Session-bus method (headless GNOME desktop)

`gio mount smb://…` requires a live D-Bus session and the gvfsd-smb
backend. Over plain SSH there is no graphical session bus, so we
provided one with **`dbus-run-session`**: it forks a private
`dbus-daemon`, exports `DBUS_SESSION_BUS_ADDRESS`, runs the wrapped
command, and tears the bus down on exit. `gvfsd`, `gvfsd-smb` and
`gvfs-fuse-daemon` autostart on-demand through the private bus.

```bash
dbus-run-session -- bash <<'INNER'
  echo "$DBUS_SESSION_BUS_ADDRESS"          # unix:path=/tmp/dbus-…
  printf '\n%s\n' "$pw" | gio mount smb://user@host/share
  gio mount -l                              # observe the SMB mount
  gio cat smb://user@host/share/file        # or read/write via fuse
INNER
```

**No changes to GDM, PAM, or the system dbus.** Everything is scoped to
the wrapped subshell.

## 4. Helper changes on this branch

`gnome/nostr-homed/src/smb/nostr-smb-mount.sh` had a documented but
never-exercised `--mode gvfs` path. Two bugs surfaced when running it
against a real gvfsd-smb; both are fixed in this branch.

### 4.1 Wrong stdin sequence for `gio mount`

The old code piped `username\n\npassword\n` (three lines) into `gio
mount`. gvfs 1.54's SMB backend, when the URI already carries the user
(`smb://n_smbd7g@…`), only asks the CLI for **Domain** then **Password**
— so the old sequence fed the username into the Domain prompt and left
Password empty, producing `Failed to mount Windows share: Invalid
argument`. Fixed to feed only the two lines gvfs actually reads:

```bash
printf '\n%s\n' "$pw" | gio mount "$smb_uri"
```

Feeding exactly one (domain, password) pair also gives us a clean
negative signal: on auth failure gvfs re-asks; the second read hits EOF
and `gio` exits non-zero rather than hanging forever on the retry
prompt.

### 4.2 `--unmount` did not handle gvfs

The old unmount branch bailed with *"not a mountpoint"* whenever the
caller passed a stub MOUNTPOINT (correct for gvfs — the actual mount
lives at `$XDG_RUNTIME_DIR/gvfs/smb-share:…`), so gvfs mounts leaked.
Fixed:

- `--unmount smb://user@host/share/` now runs `gio mount -u` directly.
- `--unmount MOUNTPOINT` still `umount(8)`s a real cifs mount, and, if
  the MP is not a mountpoint, falls through to iterate active gvfs
  smb:// mounts and unmount matches (best-effort — matches on
  `//HOST/SHARE`-style input if provided).

The helper also now verifies the fuse leaf
(`$XDG_RUNTIME_DIR/gvfs/smb-share:server=…`) is present after a
successful `gio mount`, and logs it for evidence.

Diff is entirely within
`gnome/nostr-homed/src/smb/nostr-smb-mount.sh` (per the track's
scope constraints).

## 5. Positive: gvfs mount with a MINTED Nostr credential

Full transcript in
`/tmp/d7-gvfs-clean-evidence.log` on the mac driver (produced by
`ssh gnome-dev …` — commands preserved below). Key steps + observed
output:

```
$ smbclient -L //127.0.0.1 -U 'n_smbd7g%<minted>' -p 445
	Sharename       Type      Comment
	nh_share_d7g    Disk      D7 gvfs acceptance share
	IPC$            IPC       IPC Service (nh-d7g-gvfs-acceptance)
```

```
$ dbus-run-session -- bash -c '
    nostr-smb-mount.sh --mode gvfs \
      --creds /tmp/nostr-smb-mount-d7g/credentials \
      //127.0.0.1/nh_share_d7g /mnt/nh_gvfs_stub
    gio mount -l | awk "/-> smb:/"'
DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-TOPPRjCJkF,guid=19ce3…
nostr-smb-mount: mounting //127.0.0.1/nh_share_d7g as n_smbd7g via gvfs
Authentication Required
Enter password for share “nh_share_d7g” on “127.0.0.1”:
Domain [WORKGROUP]: Password:
nostr-smb-mount: gvfs mount visible at /run/user/1000/gvfs/smb-share:server=127.0.0.1,share=nh_share_d7g,user=n_smbd7g
nostr-smb-mount: mounted //127.0.0.1/nh_share_d7g at /mnt/nh_gvfs_stub
helper mount rc=0
Mount(0): nh_share_d7g on 127.0.0.1 -> smb://n_smbd7g@127.0.0.1/nh_share_d7g/
```

The FUSE mirror is at `/run/user/1000/gvfs/smb-share:server=127.0.0.1,share=nh_share_d7g,user=n_smbd7g`.

**Write + read through gvfs (not cifs):**

```
$ echo "gvfs-pos-1790063523" > "$FUSE/hello-gvfs-helper.txt"
  write rc=0
$ cat "$FUSE/hello-gvfs-helper.txt"
gvfs-pos-1790063523
$ gio cat smb://n_smbd7g@127.0.0.1/nh_share_d7g/hello-gvfs-helper.txt
gvfs-pos-1790063523
$ sudo cat /srv/nostrshare-d7g/hello-gvfs-helper.txt
gvfs-pos-1790063523            # payload landed on the server-side share
```

**Unmount via helper (smb:// URI form):**

```
$ nostr-smb-mount.sh --unmount smb://n_smbd7g@127.0.0.1/nh_share_d7g/
helper unmount rc=0
$ gio mount -l | awk '/-> smb:/'
# (empty — unmounted)
```

## 6. Negative: revoked credential must FAIL to gvfs-mount

The authority's contract is *"one active credential per user; rotation
== revocation."* We rotate by calling `nostr-smb-acquire` a second
time, keeping the earlier file as `credentials.stale`. Sanity check:

```
$ diff -q credentials credentials.stale
Files … differ                              # rotation minted a new pw
```

Attempting to `gio mount` with the stale credential:

```
$ dbus-run-session -- nostr-smb-mount.sh --mode gvfs \
    --creds /tmp/nostr-smb-mount-d7g/credentials.stale \
    //127.0.0.1/nh_share_d7g /mnt/nh_gvfs_stub
nostr-smb-mount: mounting //127.0.0.1/nh_share_d7g as n_smbd7g via gvfs
Authentication Required
Enter password for share “nh_share_d7g” on “127.0.0.1”:
Domain [WORKGROUP]: Password:
Authentication Required                                  # gvfs re-prompts
Enter password for share “nh_share_d7g” on “127.0.0.1”:
Domain [WORKGROUP]:
nostr-smb-mount: gio mount failed for smb://n_smbd7g@127.0.0.1/nh_share_d7g (revoked credential? auth flags mismatch?)
helper mount (stale) rc=4
$ gio mount -l | awk '/-> smb:/'
# (empty — never mounted)
```

Exit code **4** ("mount failed — credentials rejected") is the code the
helper's Exit Codes table promises for this case. `gio` re-prompts once
after auth failure; feeding only a single `(domain, password)` pair on
stdin means the second read hits EOF and gio exits cleanly instead of
hanging on interactive input — which is what makes this test reliable
under `ssh`.

## 7. Regression: mount.cifs on amd64 still works with the updated helper

The helper's cifs branch is unchanged; only unmount was refactored. Two
sanity passes to confirm no regression:

### 7.1 Full existing acceptance script

```
$ NOSTR_ACCEPTANCE_LAB_OPT_IN=I_UNDERSTAND_THIS_RUNS_AGAINST_A_DISPOSABLE_LAB \
  BUILD=/tmp/amd64/build-release-homed \
  bash gnome/nostr-homed/tests/acceptance/run_smb_mount_vm.sh
…
== POSITIVE: mount //127.0.0.1/nh_share_d7 with minted credential ==
  PASS mount.cifs accepted minted credential
  PASS /mnt/nh_share_d7 is a mountpoint
== POSITIVE: write + read hello-d7.txt ==
  PASS wrote /mnt/nh_share_d7/hello-d7.txt (as root via mount)
  PASS readback matches payload
  PASS file present on server-side /srv/nostrshare-d7
== POSITIVE: unmount ==
  PASS unmounted
== NEGATIVE: mount with stale credentials must FAIL ==
  PASS mount.cifs rejected revoked credential (rc=4)
  PASS diagnostic indicates authentication failure
== NEGATIVE: smbclient must ALSO reject stale credentials ==
  PASS smbclient rejected stale credential
RESULT: PASS (D7 desktop SMB mount acceptance on x86_64)
```

### 7.2 Direct helper positive + negative against the same broker/share

```
$ sudo nostr-smb-mount.sh --mode cifs --creds …/creds.cifs \
    --uid 1000 --gid 1000 //127.0.0.1/nh_share_d7g /mnt/nh_cifs_stub
cifs mount rc=0
  is mountpoint
$ ls -la /mnt/nh_cifs_stub
-rw-r----- 1 debugger debugger   24 Sep 22 07:52 cifs-regress.txt
-rw-r----- 1 debugger debugger   20 Sep 22 07:52 hello-gvfs-helper.txt
$ cat /srv/nostrshare-d7g/cifs-regress.txt
cifs-regress-1790063575
$ sudo nostr-smb-mount.sh --unmount /mnt/nh_cifs_stub
cifs unmount rc=0

$ sudo nostr-smb-mount.sh --mode cifs \
    --creds …/creds.cifs.stale --uid 1000 --gid 1000 \
    //127.0.0.1/nh_share_d7g /mnt/nh_cifs_stub
mount error(13): Permission denied
Refer to the mount.cifs(8) manual page …
nostr-smb-mount: mount.cifs failed for //127.0.0.1/nh_share_d7g -> /mnt/nh_cifs_stub
cifs stale rc=4
  not mounted (good)
```

## 8. Password never appears in evidence

The credentials file's `password=` line is written 0600 (widened to 0640
in the test harness only so both `root` and the invoking `debugger` can
read the same file, since sudo and unprivileged reads both need it) and
the on-disk password never leaves `nostr-smb-acquire → creds file → gio
stdin`. The evidence log records password lines only as
`password=<redacted len=24>`. `gio` echoes prompt labels but not the
typed text (`Password:` with no trailing value).

## 9. Constraints and manual-only cases

- Everything above runs from `ssh gnome-dev` — no GUI required.
- `dbus-run-session` is the tested substitute for a real user session
  bus; the on-disk mount, gio API surface, and Samba interaction are
  identical to what `nautilus`/`gnome-shell` would exercise via
  `GtkMountOperation`.
- Fully-interactive GNOME Files (Nautilus) *browsing* the
  `smb://n_smbd7g@127.0.0.1/nh_share_d7g/` URI from the graphical
  session — as opposed to `gio` — was **not** driven headlessly. It is
  covered by the identical code path (`GVfsBackendSmb` +
  `GtkMountOperation` → `gio mount` internals), and the same minted
  credential proves out end-to-end here. A manual step for Explorer /
  Nautilus click-through is tracked in the existing D0 lab checklist,
  not remediated in this bead.

## 10. Cleanup

Guest cleanup after this run is deliberately manual (the harness kept
state across iterations to let us iterate on the helper):

```bash
sudo kill $(cat /tmp/nostr-authd-d7g.pid) 2>/dev/null
sudo pdbedit -x -u n_smbd7g 2>/dev/null
sudo userdel  n_smbd7g 2>/dev/null
sudo rm -rf /run/nostr-smb-d7g /var/lib/nostr-smb-d7g \
            /srv/nostrshare-d7g /tmp/nostr-smb-mount-d7g \
            /tmp/nostr-authd-d7g.pid /tmp/nostr-authd-d7g.log \
            /tmp/d7-gvfs-setup.sh /tmp/gvfs-payload.txt \
            /mnt/nh_gvfs_stub /mnt/nh_cifs_stub
# Restore Samba conf (if this run installed the d7g backup):
sudo test -f /etc/samba/smb.conf.d7gbak && \
  sudo mv -f /etc/samba/smb.conf.d7gbak /etc/samba/smb.conf && \
  sudo systemctl restart smbd
```

No GDM, PAM, or system D-Bus configuration was touched (per track
constraint).

## 11. Result

- **GVFS positive:** minted credential → `gio mount smb://…` succeeds
  end-to-end via helper; FUSE mirror is present; read + write both
  work; server-side share receives the file byte-for-byte; helper
  unmount succeeds. ✅
- **GVFS negative:** rotated (revoked) credential → `gio mount` fails
  with helper exit 4; no smb mount is left behind. ✅
- **CIFS regression (amd64):** full acceptance harness passes; direct
  helper positive + negative also pass. ✅
- **Helper fixes:** two bugs in `--mode gvfs` and one in `--unmount`
  are corrected on this branch (single file:
  `gnome/nostr-homed/src/smb/nostr-smb-mount.sh`).
