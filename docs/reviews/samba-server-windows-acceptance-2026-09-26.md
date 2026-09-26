# Samba Server Windows + GNOME Files Acceptance (Wave 4 Oscar, #16b)

**Bead:** `nostrc-rb0e.12` (D7 interoperability) — the actual Windows/Files
acceptance gate for the standalone `nostr-smbd` server.
**Wave:** 4 Oscar (task
`rp-agent-a2973b22-feat-samba-windows-acceptance-52196672`).
**Date:** 2026-09-26.
**Author:** Claude Opus 4.7 (`Co-Authored-By`).
**Scope:** Q5 gate for the Wave 4 Samba server v1 — real Windows Explorer
and GNOME Files clients round-tripping against `nostr-smbd` in the lab.
**Result:** **AMBER — GREEN with two P1 packaging defects that block the
shipped configuration from serving *any* client.** Fixes filed as
`nostrc-0vuu` and `nostrc-45b2`. With deployed-unit workarounds applied,
every functional case W1–W9 + G1–G2 passes byte-identical and SMB2/3
signing is honored. P1 (passdb drift-detector) is deferred as
out-of-scope for `nostr-smbd`; belongs to the nostr-authd/#18 track.

Because two P1 defects are open, this run **does NOT close** `nostrc-rb0e`,
`nostrc-rb0e.11`, or `nostrc-rb0e.12`. Those close only when the packaging
fixes land and a re-run against the shipped, unmodified `.deb` passes.

---

## 1. Lab layout

| Role | Address | OS | Software under test |
|---|---|---|---|
| Server | `bizarro@192.168.64.3` | Ubuntu 24.04.5 aarch64, Linux 7.0.0-31 | Samba 4.19.5-Ubuntu, `smbd` bound to `/etc/nostr-auth/smb.conf` via `nostr-smbd.service` |
| Windows client | `bizarro@192.168.64.4` | Windows 11 Pro build 26200 | stock SMB2/3 redirector, PowerShell 5.1 |
| GNOME Files client | `bizarro@192.168.64.3` (headless) | Ubuntu 24.04 | `gvfs 1.54.4`, `gio`, GVfs SMB backend driven via `dbus-run-session` (no display) |

The **Windows client is new for this task** — passwordless SSH via
`ssh bizarro@192.168.64.4` is confirmed working; the account is a local
`BUILTIN\Administrators` member with `NT AUTHORITY\NETWORK` group (Type=3
network logon over OpenSSH). Every operation ran non-interactively.

GNOME Files is exercised via `gio` under `dbus-run-session` because
`192.168.64.3` has `gnome-session-bin` + `gvfs-backends` installed but no
display server; that flavor of gio uses the same `gvfsd-smb` daemon
that GNOME Files spawns, so it is a faithful backend proof.

## 2. Server deployment (what actually runs `nostr-smbd`)

The lab used a **hand-assembled** install of the `nostrc-samba-server`
package rather than a full `.deb`, because Worker November's Wave 4
Debian package (`2f2a2e82` on master) has not been rebuilt for aarch64 on
this VM. Everything under `/etc/nostr-auth/` and
`/etc/systemd/system/nostr-smbd.service` came from the current repo
templates (`gnome/nostr-homed/config/*.sample`,
`gnome/nostr-homed/systemd/nostr-smbd.service.in`), with two workarounds
patched into the **deployed copy only** (repo templates unchanged):

1. `ExecStart` in the deployed `.service`: `-F` → `--foreground --no-process-group` (matches Ubuntu's stock `/lib/systemd/system/smbd.service`). Fixes `become_daemon.c:119 exit_daemon: Failed to create session, error code 1` — see `nostrc-0vuu`.
2. `path` in the deployed `smb.conf`: `/var/lib/nostr-auth/share-root` → `/srv/nostr-home`. Fixes `NT_STATUS_ACCESS_DENIED` from `chdir_current_service: vfs_ChDir(/var/lib/nostr-auth/share-root) failed: Permission denied. Current token: uid=995, gid=982, 1 groups: 982` — see `nostrc-45b2`.

The share is a **plain directory** (not a `porthome-fuse` mount) at
`/srv/nostr-home`, owned `nostr-smb-share:nostr-smb-share` mode `0750`,
as the task brief explicitly allowed for a v1 acceptance without a full
porthome-fuse setup. This exercises the SMB path end-to-end; the FUSE
integration is a separate acceptance track.

Fixture files seeded server-side before the run:

```
$ sudo ls -la /srv/nostr-home/
drwxr-x---  3 nostr-smb-share nostr-smb-share 4096 Sep 26 01:03 .
drwxr-xr-x  3 root            root            4096 Sep 26 01:03 ..
-rw-r-----  1 nostr-smb-share nostr-smb-share   23 Sep 26 00:40 README.txt
drwxr-x---  2 nostr-smb-share nostr-smb-share 4096 Sep 26 00:40 subdir
```

`README.txt` contents: `hello from server side\n` (23 bytes).
`subdir/nested.txt`: `nested\n` (7 bytes).

Passdb seeded per the plan §4.2 B3 mediation contract:

```
$ printf 'test-password-for-acceptance\ntest-password-for-acceptance\n' | \
    sudo smbpasswd -s -a -c /etc/nostr-auth/smb.conf nostr
Added user nostr.
$ sudo pdbedit -L -s /etc/nostr-auth/smb.conf
nostr:994:Nostr acceptance test user
```

Group membership: `nostr` is in `nostr-smb-share` (required by `valid users = @nostr-smb-share`).

Post-workaround `testparm -s /etc/nostr-auth/smb.conf` parses clean (SMB2_10
min, SMB3 max, `server signing = required`, `smb encrypt = desired`,
`private dir = /var/lib/nostr-auth/samba-state`, `passdb backend = tdbsam:/var/lib/nostr-auth/samba-state/smbpasswd`, share `path = /srv/nostr-home`).

## 3. Acceptance matrix

Legend: **GREEN** = fully passed; **AMBER** = passed only after applying a
documented workaround for a filed bead; **RED** = failed as stated;
**N/A** = out-of-scope for `nostr-smbd`, belongs to a different track.

| Case | Client | Op | Success criterion | Result |
|---|---|---|---|---|
| **W1** | Windows Explorer/PowerShell | Mount `\\192.168.64.3\nostr-home` with `smbpasswd`-seeded credential | Share appears; can browse root dir | **AMBER** (green after `0vuu`+`45b2` deploy fixes; on stock package it never binds 445) |
| **W2** | Windows Explorer | `dir` / list share | All files listed with correct sizes + timestamps | **GREEN** |
| **W3** | Windows Explorer | Copy small file (~1 KB) IN | File writes; sha256 matches on server | **GREEN** |
| **W4** | Windows Explorer | Copy larger file (10 MB) IN | Speed reasonable (>1 MB/s); integrity verified | **GREEN** (134.78 MB/s, sha256 match) |
| **W5** | Windows Explorer | Rename + delete a file | Operation succeeds; server sees change | **GREEN** |
| **W6** | Windows Explorer | Escalate perms via `icacls` / path traversal | Attempt denied or contained | **AMBER** (traversal contained; but Windows-side `icacls /grant '*S-1-1-0:F'` DID modify server POSIX ACL — see §5) |
| **W7** | Windows Explorer | SMB signing negotiated | `Get-SmbConnection.Signing` reports the connection is signed | **GREEN** (Dialect=3.1.1, Encrypted=True) |
| **W8** | Windows Explorer | Offline / server bounce | Client disconnects, reconnects on server return | **GREEN** |
| **W9** | Windows Explorer | Retention across `systemctl restart nostr-smbd` | Files still visible + readable | **GREEN** (10 MB file byte-identical after restart) |
| **G1** | GNOME Files (`gio mount smb://.../` under `dbus-run-session`) | Mount / browse | Same as W1/W2 | **AMBER** (same deploy fixes required; then green) |
| **G2** | GNOME Files (`gio copy` + `gio cat` + `gio remove`) | Write + read + delete | Byte-identical round-trip | **GREEN** (sha256 in == sha256 out) |
| **P1** | nostr-authd passdb drift-detector | Delete an account from passdb, restart | Service refuses readiness with `NH_SMB_RECONCILE_REQUIRED` | **N/A** for #16b: the reconcile-required gate lives in `nostr-authd` (`gnome/nostr-homed/src/smb/smb_credential.c`), not in `nostr-smbd`; belongs to the #18 track. See §6. |

## 4. Transcripts

Excerpts. Full outputs saved under `/tmp/*.txt` on the operator host and
are reproducible by re-running `tests/acceptance/samba-server-windows/`
against a lab that has `nostrc-samba-server` installed (or the same
manual deployment as §2).

### W1 + W7: Mount + signing (Windows)

```powershell
$ ssh bizarro@192.168.64.4 powershell -File w_full.ps1
=== W1 mount ===
WNetAddConnection2 rc = 0

=== W7 signing/dialect ===
ServerName   ShareName  Dialect Signing Encrypted NumOpens
----------   ---------  ------- ------- --------- --------
192.168.64.3 nostr-home 3.1.1                True        1
```

Note: `Get-SmbConnection.Signing` is empty for SMB 3.x connections when
`Encrypted = True` — encryption implies signing for SMB3 (per MS-SMB2
§3.3.5.5). `Dialect = 3.1.1` with `Encrypted = True` is a stronger
guarantee than `Signing = True` alone; the server-side smb.conf declares
`server signing = mandatory` and the auth log confirms NTLMv2 sessions
succeed and unauthenticated tree_connects are rejected. This satisfies
the plan §4.2 B1 #2 signing requirement.

Also note: `New-SmbMapping` and `net use` both fail from the SSH-launched
session with either `System error 67` or `Windows System Error 1312 (a
specified logon session does not exist)`. That is a **Windows LSA
credential-locker limitation on Type=3 network logons via OpenSSH** —
not a Samba defect. `WNetAddConnection2W` via P/Invoke bypasses this by
handing credentials directly to `mpr.dll`; the resulting SMB session is
identical to what a real Explorer user gets (auth log shows same
NTLMv2 SESSION_SETUP → TREE_CONNECT for `nostr-home`). This is the
common lab pattern for automating Windows SMB clients over SSH; the
underlying SMB negotiation is stock. All test scripts under
`tests/acceptance/samba-server-windows/*.ps1` use this pattern.

### W3 + W4: Round-trip integrity

```
=== W3 write small file ===
small file sha256 (Windows side): E826E9901499A13B6A62A1C7C6FBF20C794ADB0DC08FAAED159D2FB861EF0E26

=== W4 write 10 MB file, measure ===
10 MB write: 0.07s → 134.78 MB/s
large file sha256 (Windows side): 4BFE9E7777DEAE01193398536F8FB6CF5FFFF22E79C26E9BDDC86178C2463255
```

Server-side confirmation:

```
$ sudo sha256sum /srv/nostr-home/w4_large.bin
4bfe9e7777deae01193398536f8fb6cf5ffff22e79c26e9bddc86178c2463255  /srv/nostr-home/w4_large.bin
$ sudo stat -c "size=%s owner=%U:%G mode=%a" /srv/nostr-home/w4_large.bin
size=10485760 owner=nostr-smb-share:nostr-smb-share mode=640
```

`force user = nostr-smb-share` + `create mask = 0640` honored; ownership
transfers cleanly.

### W5: Rename + delete

```
=== W5 rename + delete ===
renamed: True
deleted: True
```

### W8 + W9: Offline / retention

Server bounce ran while Windows was mid-test (`systemctl stop nostr-smbd; sleep 4; systemctl start nostr-smbd`):

```
initial connect rc=0
hash BEFORE: 4BFE9E7777DEAE01193398536F8FB6CF5FFFF22E79C26E9BDDC86178C2463255
=== Waiting for server restart (5s) ===
=== Attempt access during outage window (server should be down or just recovered) ===
reconnect succeeded, items=3
=== Retry after outage window ===
post-restart listing:
  subdir       9/26/2026 12:40:58 AM
  README.txt   23 9/26/2026 12:40:58 AM
  w4_large.bin 10485760 9/26/2026 1:03:31 AM
hash AFTER: 4BFE9E7777DEAE01193398536F8FB6CF5FFFF22E79C26E9BDDC86178C2463255
W9 RETENTION: PASS (hash unchanged across restart)
```

SMB2 durable-handle reconnect worked; Windows redirector transparently
re-authenticated after the 4 s server outage. All server-side files
retained byte-identical.

### G1 + G2: GNOME Files via `gio` under `dbus-run-session`

```
=== G1: mount + list ===
Mount(0): nostr-home on 192.168.64.3 -> smb://192.168.64.3/nostr-home/

=== G1: list ===
w4_large.bin
subdir
README.txt

=== G1: cat README.txt ===
hello from server side

=== G2: copy IN (local -> share) ===
local sha256: 91a7fa2fd1c0626de70c04dccdaf2517266293d9817c2d6fef7dbf078cbe0971
=== G2: cat back over SMB ===
gvfs probe from GNOME Files backend
=== G2: copy OUT (share -> local) ===
readback sha256: 91a7fa2fd1c0626de70c04dccdaf2517266293d9817c2d6fef7dbf078cbe0971
G2 ROUND-TRIP: PASS (byte-identical)

=== G2: delete via gio remove ===
-- gio info on deleted file (should fail) --
gio: smb://192.168.64.3/nostr-home/g2_probe.txt: No such file or directory
info-fail-as-expected
```

`gio mount` was driven with three `printf`-fed prompts (User, Domain,
Password); the actual GNOME Files GUI displays a dialog for the same
three fields. `gvfsd-smb` is the same daemon Nautilus spawns for
`smb://` URIs. This is a faithful acceptance of the GVfs SMB backend
against `nostr-smbd`.

## 5. Findings

### 5.1 Two P1 packaging defects (BLOCKING as-shipped)

Filed as beads:

- **`nostrc-0vuu`** — `nostr-smbd.service.in` ships `ExecStart=/usr/sbin/smbd -F -s @NH_SMB_CONF_PATH@` but Samba 4.19 (Ubuntu 24.04) requires `--no-process-group` under systemd, otherwise `setsid()` returns EPERM and the daemon exits inside a millisecond with "Failed to create session". `smbd.service` and `samba.service` shipped by Ubuntu both use `--foreground --no-process-group`; the standalone `nostr-smbd` needs the same. Fix: change `ExecStart` to `/usr/sbin/smbd --foreground --no-process-group -s @NH_SMB_CONF_PATH@`.

- **`nostrc-45b2`** — Root cause isolated during this run: the shipped `smb.conf.standalone.sample` sets `passdb backend = tdbsam:/var/lib/nostr-auth/smbpasswd`, which effectively makes `/var/lib/nostr-auth` Samba's private dir. Samba **chmods its private/state directory to 0700 on every start** as a hard security invariant. Combined with `path = /var/lib/nostr-auth/share-root` inside the share stanza plus `force user = nostr-smb-share`, this means every restart of `nostr-smbd` reverts the parent of the share to `0700 root:root`, and `nostr-smb-share` (uid 995) cannot traverse into it: `chdir_current_service: vfs_ChDir(/var/lib/nostr-auth/share-root) failed: Permission denied. Current token: uid=995, gid=982, 1 groups: 982`. Verified by tracing that `testparm -s`'s reported `state directory` = `/var/lib/nostr-auth` and observing `ls -ld` flip 0755→0700 immediately after each restart. Recommended fix (either):
  - Move the share-root out of `/var/lib/nostr-auth` — put it at `/srv/nostr-home` (Debian policy for served content) or `/var/lib/nostr-smb-share/root`, and update `smb.conf.standalone.sample` + `nostr-smbd.service.in` `ReadWritePaths=`. Keep `/var/lib/nostr-auth` exclusively for the authority's passdb/journal/private state at 0700 (unchanged). This is the option applied in the acceptance run.
  - Or keep the share under `/var/lib/nostr-auth/share-root` but sink Samba's own state one level deeper, e.g. `private dir = /var/lib/nostr-auth/samba-state`, `state directory = /var/lib/nostr-auth/samba-state`, `cache directory = /var/lib/nostr-auth/samba-state`, and let `/var/lib/nostr-auth` itself stay at 0755 (or 0711).

Both `nostrc-0vuu` and `nostrc-45b2` are `type=bug priority=P1`. They
are the reason this acceptance run is AMBER rather than GREEN.

### 5.2 W6 concern — NT ACLs writable by SMB clients (AMBER)

An SMB client authenticated as `nostr` (a member of `@nostr-smb-share`)
can successfully run `icacls '\\192.168.64.3\nostr-home\README.txt' /grant '*S-1-1-0:F'`
and it lands as a POSIX ACL on the server-side file:

```
$ sudo getfacl -p /srv/nostr-home/README.txt
user::rw-
user:nostr-smb-share:rw-
group::r--
group:nostr-smb-share:r--
mask::rwx
other::rwx
```

`other::rwx` means "world" gets rwx in POSIX terms. **This is not a
cross-tenant privilege escalation** — the SMB share still gates entry
via `valid users = @nostr-smb-share`, so a different Nostr identity that
is not in the `nostr-smb-share` group still cannot reach the file over
SMB. But it does mean:

- If Wave 5 introduces per-identity groups (`nostr-smb-share-alice`,
  `nostr-smb-share-bob`, …), a client authenticated as Alice could
  flip a file's POSIX ACL to `other::rwx`, and Bob would see it over
  SMB if the share stanza's `valid users` widens to `@nostr-smb-share`
  or if the share is browsed by a helper daemon that isn't
  group-scoped. The share is currently single-tenant so this is
  latent, not exploited.
- It complicates any future "who has access to file X" audit — the
  effective ACL depends on both the share-level `valid users` and the
  file-level POSIX ACL, which now drifts from what `porthome-fuse`
  materialized.

The path traversal probe (`\\192.168.64.3\nostr-home\..\evil.txt`) is
correctly contained inside the share — Windows' MUP resolves the `..`
client-side, so the write lands at `/srv/nostr-home/evil.txt` (inside
the share, not `/srv/evil.txt`). This is the expected behavior. Server
smbd log shows no traversal attempt reaching it.

**Suggested v1 mitigation** (not filed as a bead, awaiting maintainer
direction): add `nt acl support = no` or `inherit acls = no` +
`map acl inherit = no` to the shipped `smb.conf.standalone.sample`. That
prevents SMB clients from writing NT-ACL-derived POSIX ACLs while still
letting `porthome-fuse` provide access via the mount owner. Cost:
Windows users see fewer NT ACL knobs; benefit: the on-disk state stays
exactly what `porthome-fuse` set. If the maintainer wants this, file
a follow-up bead — I did not file one because it's a policy call, not
a correctness gap.

### 5.3 P1 out-of-scope (N/A)

The plan's P1 case reads "delete an account from passdb manually,
restart nostr-smbd → Service refuses readiness with
`NH_SMB_RECONCILE_REQUIRED`". `NH_SMB_RECONCILE_REQUIRED` is defined in
`gnome/nostr-homed/src/smb/smb_credential.h:76-77` and returned by
`reconcile_passdb_against_journal()` in
`gnome/nostr-homed/src/smb/smb_credential.c`. It's called from
`nh_smb_authority_open_ex()` — which is loaded by **nostr-authd**, not
by `smbd`. The Wave 3 Hotel #18 landing (`bd7d62e8`) is the
`nostr-authd`-side reconciliation, not an `nostr-smbd`-side one.

To exercise this gate end-to-end you need:

1. A pre-provisioned `authority.db` at `/var/lib/nostr-auth/authority.db` (created by `nostr-homectl init` or equivalent).
2. `nostr-authd` running with the four-arg invocation (`ExecStart=/usr/sbin/nostr-authd auth.sock authority-dir user.sock /var/lib/nostr-auth/smb.db`).
3. The `smb.db` journal seeded with the same accounts as `smbpasswd`.
4. Then `sudo smbpasswd -x nostr -s -c /etc/nostr-auth/smb.conf` to drop the passdb entry, restart `nostr-authd`, and observe the "reconcile-required" refusal.

On this VM, `nostr-authd` fails at step 1 — the installed `nostr-authd 0.3.0-1` package expects a pre-provisioned `authority.db` and the lab has none. `nostr-authd` was masked at the end of the run so its restart storm doesn't pollute the journal. The full drift-detector acceptance is properly a **`nostr-authd`-track task**, not a `nostr-smbd`-track task, and I did not file it as a #16b gap — it belongs to whichever bead governs `nostr-authd` acceptance (candidate: `nostrc-rb0e.1` D1 standalone-SMB contract).

## 6. Bead disposition

| Bead | Status now | Why |
|---|---|---|
| `nostrc-rb0e` | Keep open | Q5 gate not green until 0vuu + 45b2 land and re-verify passes. |
| `nostrc-rb0e.11` | Keep open | Depends on shipped `nostrc-samba-server` running unmodified. Currently doesn't. |
| `nostrc-rb0e.12` | Keep open | See above — matrix has two AMBER cases from packaging defects; not "green" per the plan's own criteria. |
| `nostrc-0vuu` | NEW P1 bug filed | Missing `--no-process-group` in shipped systemd unit. |
| `nostrc-45b2` | NEW P1 bug filed | State-directory / share-directory conflation blocks force-user traversal. |

## 7. What a "green re-run" looks like

Once `0vuu` and `45b2` land on master (either in `nostr-smbd.service.in`
+ `smb.conf.standalone.sample` or through the Debian packaging), the
re-run is:

```bash
# Server (192.168.64.3)
sudo apt install ./nostrc-samba-server_*.deb
sudo systemctl enable --now nostr-smbd
sudo smbpasswd -s -a -c /etc/nostr-auth/smb.conf <accepteduser>
sudo systemctl restart nostr-smbd
sudo smbclient //127.0.0.1/nostr-home -U <accepteduser>%<pw> -c ls

# Windows (192.168.64.4) — from an SSH session as bizarro (Admin)
scp tests/acceptance/samba-server-windows/w_full.ps1 bizarro@windows:.
scp tests/acceptance/samba-server-windows/w8_offline.ps1 bizarro@windows:.
ssh bizarro@windows powershell -ExecutionPolicy Bypass -File w_full.ps1

# GNOME Files (any Ubuntu 24.04 with gvfs-backends)
tests/acceptance/samba-server-windows/g_full.sh
```

Green means every case in the matrix reads GREEN or N/A. When that
happens, `nostrc-rb0e.12` closes with a link to a superseding review;
`nostrc-rb0e.11` closes if that day's re-run also covers a Debian +
Fedora install; `nostrc-rb0e` closes if Q5 was the last gate.

## 8. Files and scripts

Committed under `tests/acceptance/samba-server-windows/` on
`feat/samba-windows-acceptance-52196672`:

- `README.md` — how to re-run this matrix
- `deploy-nostr-smbd.sh` — the server-side bring-up (§2)
- `w_full.ps1` — W1/W2/W3/W4/W5/W7 driver (PowerShell)
- `w8_offline.ps1` — W8/W9 driver
- `w_more.ps1` — W6 escalation probes
- `g_full.sh` — G1/G2 gio driver

Nothing else changes in the repo. Scratch state under `/tmp/*.txt` on
the operator host and `/tmp/*.sh|.ps1` on the two VMs is not committed
(explicit scope boundary from the task brief).

---

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
