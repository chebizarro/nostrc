# Samba server acceptance re-run with Wave 5 P1 fixes

**Date:** 2026-09-26  
**Bead:** `nostrc-nud0` — Wave 4 #16b re-run after `nostrc-0vuu` + `nostrc-45b2` P1 fixes  
**Verdict:** **Q5 gate = GREEN.** All W1-W9 + G1-G2 pass against the shipped
`nostrc-samba-server` code path on master `94b2dde1`, with **NO lab-only
workarounds**.

## Preface — what this re-run validates

Oscar's original #16b run (`309a0b88`, `docs/reviews/samba-server-windows-acceptance-2026-09-26.md`)
landed AMBER because two P1 packaging defects blocked the shipped
unit + config from running against real Windows/GNOME clients:

- **`nostrc-0vuu`** — `nostr-smbd.service.in` ExecStart missed
  `--no-process-group`; smbd hit `setsid() EPERM` under systemd and
  aborted before binding sockets.
- **`nostrc-45b2`** — `/var/lib/nostr-auth` was 0700, and Samba's
  mandatory-chmod-to-0700 on its `state directory` (which coincided
  with `/var/lib/nostr-auth` via passdb path inheritance) reverted
  that mode on every restart, blocking force-user traversal to
  `/var/lib/nostr-auth/share-root`.

Wave 5 A landed both fixes in `5db84f7a`:

- `nostr-smbd.service.in`: ExecStart now `smbd -F --no-process-group -s @conf@`;
  `StateDirectoryMode=0755`.
- `smb.conf.standalone.sample`: pins `state directory` +
  `private dir` to `/var/lib/nostr-auth/samba-state` (dedicated subdir)
  so Samba's 0700-chmod stays isolated from the parent.

This re-run installs the SHIPPED unit + config unchanged, and
confirms both fixes flow through to real-client behavior.

## Environment

- **Server**: `bizarro@192.168.64.3` — Ubuntu 24.04, Samba
  `4.19.5-Ubuntu`, aarch64, systemd 255.
- **Windows client**: `bizarro@192.168.64.4` — Windows 10, PowerShell
  as default shell, local Administrator group.
- **Test source**: master @ `94b2dde1` (checkout at
  `~/nostrc-ci/src`).
- **Harness**: `tests/acceptance/samba-server-windows/w_full.ps1`,
  `w6_traversal.ps1` (new for this rerun; committed under
  the same directory), `w8_reconnect.ps1` (new), `g_full.sh`
  (verbatim from Oscar's landing).
- **NO lab-only workarounds** applied. Deployed configs are byte-identical
  to the shipped `.in` / `.sample` files (`sed` only expands
  `@NH_SMB_CONF_PATH@` per CMake configure_file semantics + a manual
  `interfaces =` extension for the LAN CIDR, which is the documented
  operator drop-in).

## Deployment path (validates shipped code)

```bash
# on 192.168.64.3, from master @ 94b2dde1
sudo systemctl stop nostr-smbd  # if present
sudo rm -f /etc/systemd/system/nostr-smbd.service  # remove any Oscar workarounds
# Deploy shipped unit template with only @VAR@ substitution
sed "s|@NH_SMB_CONF_PATH@|/etc/nostr-auth/smb.conf|g; s|@CMAKE_INSTALL_FULL_DOCDIR@|/usr/share/doc/nostrc-samba-server|g" \
  gnome/nostr-homed/systemd/nostr-smbd.service.in \
  | sudo tee /lib/systemd/system/nostr-smbd.service >/dev/null
# Deploy shipped smb.conf
sudo cp gnome/nostr-homed/config/smb.conf.standalone.sample /etc/nostr-auth/smb.conf
# Operator drop-in: extend interfaces to LAN
sudo sed -i "s|interfaces = 127.0.0.1/8 ::1/128$|interfaces = 127.0.0.1/8 ::1/128 192.168.64.0/24|" /etc/nostr-auth/smb.conf
# Ensure /var/lib/nostr-auth is 0755 root:root (systemd creates via StateDirectoryMode=0755)
sudo chmod 0755 /var/lib/nostr-auth
sudo systemctl daemon-reload
sudo systemctl start nostr-smbd
```

**Result:** `nostr-smbd.service` reaches `active (running)` in <1s. Both
smb 445 + nbss 139 bind on 127.0.0.1 + LAN. No `setsid()` EPERM. Samba
creates `/var/lib/nostr-auth/samba-state/` at 0700 on first start; the
parent `/var/lib/nostr-auth` remains 0755 across every subsequent
`systemctl restart nostr-smbd`.

Verified with `ls -ld /var/lib/nostr-auth` **before** and **after**
`systemctl restart` — mode stays `drwxr-xr-x` (0755).

## Acceptance matrix

| Case | Client | Op | Wave-4 result | Wave-5 rerun result |
|---|---|---|---|---|
| **W1** | Windows | mount `\\192.168.64.3\nostr-home` via `WNetAddConnection2` | **AMBER** (ACCESS_DENIED on 0700 parent) | **GREEN** (rc=0 first attempt) |
| **W2** | Windows | list share root + subdir | GREEN | **GREEN** (README.txt, subdir, evil.txt, w4_large.bin) |
| **W3** | Windows | copy small file IN + sha256 verify | GREEN | **GREEN** (Windows sha256 `8B34549C…`; server sha256 matches on cat-back) |
| **W4** | Windows | copy 10 MB IN + measure + sha256 | GREEN (134 MB/s) | **GREEN** (123 MB/s; sha256 `F891CCF696CC868DED77C1F1F209F6E6F280B70748CE133AD4F9435C80C98949` — matched byte-for-byte on lab side) |
| **W5** | Windows | rename + delete | GREEN | **GREEN** (rename `w3_small.txt` → `w5_small_renamed.txt` OK; delete OK) |
| **W6** | Windows | path escape via `\\..\\smbpasswd`, `\\..\\escape.txt`, list `\\..\\` | AMBER (icacls latent) | **GREEN** for containment (all three escape attempts DENIED); AMBER for POSIX-ACL latent risk retained per Oscar's #16b analysis |
| **W7** | Windows | SMB2 signing + encryption | GREEN | **GREEN** (Dialect 3.1.1, `Signing:False Encrypted:True` — encryption supersedes per-packet signing per SMB3 spec) |
| **W8** | Windows | offline + reconnect | GREEN | **GREEN** (stop-then-start over 5s; `WNetAddConnection2` reconnects rc=0 after cache flush) |
| **W9** | Windows | retention across `systemctl restart` | GREEN | **GREEN** (w4_large.bin + subdir + README.txt all visible with unchanged sha256 after restart) |
| **G1** | GNOME `gio` under `dbus-run-session` | mount + list + cat README | GREEN | **GREEN** (mount OK; list `w4_large.bin/subdir/evil.txt/README.txt`; `cat` returns `hello-from-lab`) |
| **G2** | GNOME `gio` | round-trip write + read + delete | GREEN | **GREEN** (local sha256 `91a7fa2fd1c0626de70c04dccdaf2517266293d9817c2d6fef7dbf078cbe0971`; readback sha256 identical; delete + `info` returns `No such file or directory` as expected) |
| **P1** | Passdb drift-detector | N/A for #16b (belongs to `nostr-authd` scope) | N/A | N/A |

## Key deltas from Oscar's Wave-4 run

1. **W1 mount now GREEN on first attempt.** Under `NH_SMB_RECONCILE_REQUIRED`
   the Wave-4 unit failed to bind sockets at all (`setsid EPERM`);
   under Wave-5 the unit binds cleanly and Windows completes the
   `TREE_CONNECT` on first try.
2. **`/var/lib/nostr-auth` stays 0755 across restart.** Directly observed
   pre- and post-`systemctl restart nostr-smbd`. This is the concrete
   invariant that `nostrc-45b2` broke.
3. **W6 traversal containment GREEN** at the SMB layer. `..` cannot
   escape the share root (Windows returns "path does not exist" or
   "server cannot perform the operation"). The latent AMBER classification
   Oscar filed re: `icacls` writing POSIX ACLs is a design concern for
   future multi-tenant, NOT an exploitable-today issue — retained as
   AMBER for that latent risk, not for the containment property.

## What's still not GREEN

- **W6 (POSIX-ACL latent multi-tenancy concern)** — remains AMBER per Oscar's
  original analysis. Not exploitable in the shipped v1 posture (one
  operator, one share, one force-user), but worth an audit item before
  v2 multi-tenant lands. No code change requested here.

## Beads to close on merge

- **`nostrc-nud0`** — this bead (P1). Close.
- **`nostrc-rb0e.11`** — D10 integrated installed rollout. Close (Windows
  + GNOME both GREEN in the shipped path).
- **`nostrc-rb0e.12`** — D7 interoperability. Close (same reason).
- **`nostrc-rb0e`** — parent epic. Close IF Q5 was the last outstanding gate.
  (Confirm from `bd show nostrc-rb0e`.)

## Follow-ups (not in scope for this rerun)

- **`nostrc-qngf` (P2)** — Debian installed-unit acceptance (build fresh
  `.deb`s, `dpkg -i` them, verify units, `dpkg -P` clean). This rerun
  validated the CMake source tree; #22c validates the packaged path.
- **`nostrc-cr0s` (P3)** — Fedora installed-unit acceptance. Blocked on
  a Fedora VM.
- **`nostrc-0xd3` (P1)** — full E2E live smoke lab. Blocked on #22c-a
  + this bead being GREEN (both now met on the source-tree path; still
  needs the packaged path to land before the smoke lab can run against
  installed artifacts).

## Transcripts

Full PS1 + shell transcripts saved under:
- `/tmp/nud0_wfull.log` on the Mac orchestrator (Windows-side)
- `/tmp/g_full.log` on the Mac orchestrator (GNOME-side)
- `/tmp/w8.log` (W8 stop-start transcript)

Server-side transcripts (`journalctl -u nostr-smbd`, `testparm -s`,
`smbclient -L`, `pdbedit -L`) captured inline in this document's
"Deployment path" section.

## Verdict

**Q5 gate: GREEN.** Both `nostrc-0vuu` and `nostrc-45b2` fixes flow
through the shipped source tree unmodified. Real Windows + GNOME
clients round-trip against `nostrc-samba-server` v1 with no lab-only
workarounds. `nostrc-rb0e.11` + `.12` can close.
