# Samba server Windows + GNOME Files acceptance harness

Drives the acceptance matrix documented in
`docs/reviews/samba-server-windows-acceptance-2026-09-26.md`
(bead `nostrc-rb0e.12`, plan item #16b XL).

## Prerequisites

- A Linux server VM with `nostrc-samba-server` installed and
  `nostr-smbd.service` runnable. Two known packaging defects
  (`nostrc-0vuu`, `nostrc-45b2`) block the shipped package from
  running out of the box — see the review for the deploy patches.
- A Windows 11 client reachable over passwordless SSH with a local
  administrator account. PowerShell 5.1+ is required.
- Optional: a Linux client with `gvfs-backends` + `gio` for the G1/G2
  cases. GNOME Files exercises the same `gvfsd-smb` daemon under the
  hood — the tests use `dbus-run-session` so they work headless.

## Server bring-up

```bash
# Adjust ADDR/USER/PW variables inside deploy-nostr-smbd.sh if needed.
scp deploy-nostr-smbd.sh user@server:/tmp/
ssh user@server 'bash /tmp/deploy-nostr-smbd.sh'
```

The helper:

1. Stops + masks the distro's `smbd`/`nmbd` (they hold 445/139).
2. Provisions the `nostr-smb-share` sysuser.
3. Creates `/var/lib/nostr-auth/` + `/srv/nostr-home/`.
4. Seeds a `nostr` test account in `/etc/passwd` and the passdb via
   `smbpasswd -s -a -c /etc/nostr-auth/smb.conf nostr`.
5. Deploys a patched `smb.conf` (adds LAN interface, moves state to a
   subdirectory) and a patched systemd unit (adds `--no-process-group`).
6. Starts `nostr-smbd` and verifies with `smbclient -L`.

The **patches applied are the ones documented in `nostrc-0vuu` and
`nostrc-45b2`**; when those beads close, this script simplifies.

## Windows client

Copy the PowerShell scripts and drive them from any host that has
passwordless SSH to the Windows box:

```bash
scp w_full.ps1 w8_offline.ps1 w_more.ps1 bizarro@win-vm:C:/Users/bizarro/
ssh bizarro@win-vm 'powershell -ExecutionPolicy Bypass -File C:/Users/bizarro/w_full.ps1'
ssh bizarro@win-vm 'powershell -ExecutionPolicy Bypass -File C:/Users/bizarro/w_more.ps1'
# W8/W9 need to bounce the server mid-run; open two SSH sessions or
# background the ps1 and drive systemctl from the other side.
```

The scripts use `WNetAddConnection2W` via P/Invoke rather than
`New-SmbMapping` or `net use`, because OpenSSH-on-Windows spawns a
`Type=3` network logon that cannot write to the LSA credential locker
that `net use` requires. The SMB negotiation on the wire is identical.

## GNOME Files (headless) client

`g_full.sh` is the GVfs SMB backend probe. It uses `dbus-run-session`
so it does not need a display server.

```bash
scp g_full.sh gio_feed.expect user@linux-client:/tmp/
ssh user@linux-client 'bash /tmp/g_full.sh'
```

## Interpreting results

Look for the string `ROUND-TRIP: PASS` (G2), `W9 RETENTION: PASS`,
`WNetAddConnection2 rc = 0` (W1), and the SMB signing table from
`Get-SmbConnection` reporting `Dialect = 3.1.1` and `Encrypted = True`.
Any failure prints an error and does NOT set an exit code — read the
transcript. The review document has an example of what a green run
looks like.
