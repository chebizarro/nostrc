# Wave 4 #22c-a — Debian installed-unit acceptance (aarch64)

**Date:** 2026-09-26  
**Bead:** `nostrc-qngf`  
**Verdict:** **GREEN**, gated on three new P1/P2 packaging defects surfaced by the run (all fixed inline).

## Environment

- **Lab:** `bizarro@192.168.64.3` — Ubuntu 24.04 LTS, aarch64, systemd 255.
- **Tooling:** `debhelper 13.14.1ubuntu5`, `dpkg-dev 1.22.6ubuntu6.6`, `lintian 2.117.0ubuntu1.5`, `rpmbuild 4.19.1.1`. No `sbuild`, no `mock`, no `podman/docker`.
- **Source:** master @ `1859d936` (after inline fixes filed during this run — commits `6fde9b88`, `ec1ebd35`, `729f2b6f`, `1859d936`).

## Matrix

| # | Step | Result | Notes |
|---|---|---|---|
| 1 | `dpkg-buildpackage -b -uc -us` | **GREEN** post-fix | Initial build broke at target 596/609 with a `-Werror=format` in `nd-publisher.c` — filed `nostrc-sn0j`, fixed in `6fde9b88`, incremental resume completed. |
| 2 | `lintian` on each Wave-4 `.deb` | **GREEN** | Only 1 W-level: `nostrc-relayd: no-manual-page [usr/sbin/nostrc-relayd]` — non-fatal packaging warning. All 4 other packages: 0 warnings. |
| 3 | `dpkg -i` all packages | **GREEN** | 18 debs installed clean (`libnostr1`, `libnostrgo0`, `libnostr-json1`, `libhanami0`, `libnss-nostr`, `libpam-nostr`, `nostr-authd`, `nostr-homectl`, `nostr-homed-{smb,domain}`, `nostr-home-{sync,fuse}`, `nostr-notify`, `nostr-dav`, `nostrc-{session-relay,samba-server,relayd}`, `nostr-login`). One conffile prompt on `smb.conf` (existing operator file) resolved with `--force-confnew`. |
| 4 | Boot each Wave-4 unit | **GREEN post-fix** | Three P1/P2 defects surfaced and fixed inline: `nostrc-8oon` (notify user unit `ProtectClock`/`ProtectKernel{Logs,Modules}` cap-drop under `--user` → 218/CAPABILITIES); `nostrc-il9p` (notify daemon `libnip19.so` not shipped by any .deb → status=127); `nostrc-sn0j` (aarch64 `-Werror` build blocker). Final state below. |
| 5 | Dep-purity gate on installed artifacts | **GREEN** | `scripts/check-authd-dep-purity.sh /usr/sbin/nostr-authd /usr/lib/aarch64-linux-gnu/security/pam_nostr.so` → `ok: dep-purity gate clean` (exit 0). |
| 6 | `dpkg -P` purge test | **GREEN** with caveat | `nostrc-samba-server` + `nostrc-relayd` purge cleanly — package-owned files removed. Runtime state left behind: `/var/lib/nostr-auth/samba-state/*.tdb` (Samba-created at runtime; dpkg doesn't own it) and `/etc/nostrc/relay.toml` (operator-seeded, not a conffile). Both acceptable per Debian policy for daemon state directories; documented as ops-cleanup responsibility. |
| 7 | Fresh `nostr-smbd` start from purged+reinstalled `.deb` | **GREEN** | After `dpkg -P` + `dpkg -i`, `systemctl start nostr-smbd` → `active`; parent `/var/lib/nostr-auth` stays `0755` across start; `samba-state/` subdir at `0700` (Samba's mandatory chmod isolated). Validates that Wave 5 P1 fixes (`nostrc-0vuu`, `nostrc-45b2`) survive the packaging path with zero manual intervention. |

## Final active-unit roundup

```
nostr-smbd.service (system):        active
nostr-relayd.service (system):      active
nostr-session-relay.socket (user):  active
nostr-dav.service (user):           active
nostr-dav-dirs.service (user):      inactive (Type=oneshot terminal — correct)
nostr-notify.service (user):        active (idle: signer has no account)
```

## Defects filed during the run (all fixed inline)

- **`nostrc-sn0j` (P1)** — `nd-publisher.c:1031` uses `G_GINT64_FORMAT` (`"li"` on aarch64) with `sqlite3_int64` (always `long long int`). x86_64 typedefs happen to match; aarch64 `-Werror=format` breaks the build. Fix: `(gint64)` cast (`6fde9b88`).
- **`nostrc-8oon` (P2)** — `nostr-notify.service.in` had `ProtectClock=`, `ProtectKernelLogs=`, `ProtectKernelModules=` which drop `CAP_SYS_TIME`/`CAP_WAKE_ALARM`/`CAP_SYSLOG`/`CAP_SYS_MODULE` from the bounding set. `systemd --user` is not privileged to modify caps → unit fails with `218/CAPABILITIES` on start. Sibling `nostr-dav.service` had the same problem and documented the fix inline; not ported to notify. Fix: remove the three directives with matching commentary (`ec1ebd35`, `729f2b6f`).
- **`nostrc-il9p` (P1)** — `nostr-notify-daemon` has a dynamic link edge to `libnip19.so`, but `debian/not-installed` dropped the library citing plan D-6 aggregation into `libnostr1`. That aggregation wasn't done; no package shipped the file; the daemon exited with status 127 (`cannot open shared object file`). Also: `libnip19.so` is built without SONAME versioning. Interim fix: ship `libnip19.so*` in `nostr-notify.install`, remove from `not-installed` (`1859d936`). Follow-up needed: SONAME versioning + canonical libnostr1 aggregation.

## What #22c-a demonstrates for the shipped path

1. **Wave 5 P1 fixes (`nostrc-0vuu`, `nostrc-45b2`) flow through .deb correctly.** Shipped `nostr-smbd.service` binary has `--no-process-group`; shipped `smb.conf` has `state directory = /var/lib/nostr-auth/samba-state`; parent stays 0755 across fresh install → start → restart.
2. **Wave 4 packaging (`#22a`) landed the 4-new-packages + `nostrc-relayd` split cleanly.** All install/purge cycles clean.
3. **Dep-purity closure holds on installed artifacts.** `nostr-authd` + `pam_nostr.so` have zero symbol edges to `libhanami`/`porthome`/`nip55l-client`/FUSE — the Track-D closure gate is not violated by any Wave-4 addition.
4. **User-manager unit hardening is a known-gotcha class.** Two of the three defects (nostrc-8oon, nostrc-il9p) are about "user unit vs system unit" edge cases that only manifest on installed-unit acceptance. Argues for a systemd-analyze verify pre-flight in CI.

## Not tested (deferred to sibling beads)

- **`nostrc-cr0s`** — Fedora arm blocked on Fedora VM.
- **`nostrc-0xd3`** — full E2E live smoke lab (`-Werror` ctest sweep + real login) — this bead validates the packaging path, not the full stack.

## Verdict

**#22c-a GREEN** with three inline defect fixes. `nostrc-qngf` can close. `nostrc-cr0s` remains blocked on Fedora VM availability. `nostrc-0xd3` is unblocked on the packaging leg (this rerun validates the shipped `.deb`s boot cleanly).

## Follow-up beads

- **`nostrc-sn0j`** (P1) CLOSED — aarch64 `-Werror` fix landed.
- **`nostrc-8oon`** (P2) CLOSED — user unit cap directives fixed.
- **`nostrc-il9p`** (P1) — CLOSED for the interim ship; follow-up SONAME versioning + libnostr1 aggregation remains open (P3).
- **`nostrc-cr0s`** — Fedora arm, still blocked.
- **`nostrc-0xd3`** — full E2E smoke, ready to run.
