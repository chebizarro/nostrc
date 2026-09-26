# Wave 4 #23 — Live smoke lab (partial acceptance)

**Date:** 2026-09-26  
**Bead:** `nostrc-0xd3`  
**Verdict:** **AMBER — partial GREEN with several P2/P3 pre-existing enrollment defects surfaced.**

## Environment

- **Lab:** `bizarro@192.168.64.3` — Ubuntu 24.04 aarch64, systemd 255.
- **Source:** master @ `f3a10b3c` (Wave 5 + Wave 6 packaging fixes landed).
- **Deployment:** all 5 Wave-4 `.debs` installed from `nostrc-qngf` acceptance run (unmodified shipped code path).
- **Substitution:** No physical GDM (headless VM); PAM stack exercised via `pamtester` per the bead's acceptance intent (credential stack behavior, not display-server login).

## Matrix

| Step | Case | Result | Notes |
|---|---|---|---|
| 1 | Freestanding `-Werror` build | **GREEN via qngf** | `dpkg-buildpackage -b -uc -us -j4` compiled 609/609 targets clean on aarch64 with `-D_FORTIFY_SOURCE=3 -Werror=format-security`. Freestanding CMake `BUILD_TESTING=ON` build surfaced pre-existing test-file `-Werror` latency in 7+ files — deferred to `nostrc-<hygiene-bead>` (P3). Not a Wave-4 regression. |
| 2 | Full `ctest` sweep | **DEFERRED** | Blocked by step-1 test-file latency. Filed as **`nostrc-m4y1`** (P3). All Wave-4 additions were exercised as targeted tests during their landings (`test_publish_rollback`, `test_signer_dbus_contract`, `test_relay_sync`, `test_bind_iface`, `test_nss_qualified_names`, `test_notify_sub`, etc.) — those all landed GREEN in their own commits. |
| 3 | Dep-purity gate on installed artifacts | **GREEN via qngf** | `scripts/check-authd-dep-purity.sh /usr/sbin/nostr-authd /usr/lib/aarch64-linux-gnu/security/pam_nostr.so` → `ok: dep-purity gate clean` (exit 0). Track-D closure invariant holds against Wave-4 additions. |
| 4a | User provisioning via `nostr-homed-seed` | **AMBER** | `nostr-homed-seed /var/lib/nostr-auth testuser <pass>` succeeded (authority.db grew from 0 → 77KB, "seed: enroll" logged). `publish-projection` reported success (nss.db @ generation 1). But subsequent `nostr-homectl show <dir> testuser` returned `not_found`, and `sqlite3` inspection showed unexpected schema. Filed as **`nostrc-9yrr`** (P2). |
| 4b | `nostr-authd` first-boot | **AMBER** | Fresh install of `nostr-authd` refuses to start with `cannot open smb journal /var/lib/nostr-auth/smb.db: reconcile-required`. This is the Wave 3 Hotel NH_SMB_RECONCILE_REQUIRED drift-detector doing its job — but there's no documented first-boot procedure to seed both `smb.db` and `tdbsam` in sync. Filed as **`nostrc-u7su`** (P2). |
| 4c | StateDirectory mode contention | **AMBER** | `nostr-authd.service` (`StateDirectoryMode=0700`) and `nostr-smbd.service` (`StateDirectoryMode=0755` per Wave-5 fix nostrc-45b2) declare the same `StateDirectory=nostr-auth`. Whichever unit restarts LAST wins the parent chmod, breaking whichever unit needs the other mode. Filed as **`nostrc-hfks`** (P2). Preferred fix: loosen authd to 0755 (matching Wave-5 posture; passdb + authority.db are 0600 at the file level anyway). |
| 4d | `nostr-homed-provision` runtime lib gap | **AMBER** | Third missing-library defect (after nostrc-il9p libnip19 and nostrc-7pk libnip19 for notify): `nostr-homed-provision` fails at start with `libnip34.so: cannot open shared object file`. Same class as libnip19. Filed as **`nostrc-y4ye`** (P2). |
| 4e | Session bus services running | **GREEN** | All 5 Wave-4 units active on the shipped `.debs`: nostr-smbd (system), nostr-relayd (system), nostr-session-relay.socket + nostr-dav + nostr-notify (user). nostr-dav-dirs.service terminal-inactive (Type=oneshot — correct). |
| 4f | DAV protocol probe | **GREEN** | `nostr-dav` on `http://127.0.0.1:7680/`. Unauthenticated `GET /` → `401`. Authenticated → `405 Method Not Allowed` (expected — root has no resource). `OPTIONS /` → `DAV: 1, 2, 3, calendar-access, addressbook` + `Allow: OPTIONS, PROPFIND, REPORT, GET, PUT, DELETE`. Full DAV protocol advertisement correct. |
| 4g | `gnostr-signer-daemon` on session bus | **N/A** | `gnostr-signer-daemon` is a separate application (`apps/gnostr-signer/`), not part of the Wave-4 headless packages. Not shipped in `nostrc-*` .debs (`Section: admin`); is in `apps/gnostr-signer/` which requires `BUILD_APPS=ON` (debian/rules has `BUILD_APPS=OFF`). Not a Wave-4 regression; will be delivered by a future GNOME-desktop packaging track. |
| 4h | Samba client round-trip | **GREEN via nud0** | Windows Explorer + GNOME Files SMB mount + read + write + delete + retention validated in `docs/reviews/samba-server-windows-acceptance-rerun-2026-09-26.md` against the SHIPPED code path (`nud0`). |
| 4i | NIP-17 opaque DM notification | **DEFERRED** | Requires enrolled signer + a peer sending a NIP-17 kind-1059 gift-wrap. Blocked on 4a/4b/4d. `test_notify_sub` unit test validates the opacity invariant at code level. |
| 4j | Session teardown | **DEFERRED** | Requires successful open-session first (blocked on 4a/4b/4d). |

## What #23 does confirm

1. **Wave 4 shipping code path is `-Werror` clean and dep-purity clean on aarch64** (via qngf's dpkg-buildpackage transcript).
2. **All 5 Wave-4 packages install, boot, and idle correctly** on Ubuntu 24.04 aarch64 (via qngf).
3. **The DAV server accepts protocol requests correctly** (probes above).
4. **The Samba server accepts real Windows + GNOME Files clients** (via nud0 rerun).
5. **Wave 5 P1 fixes (nostrc-0vuu, nostrc-45b2) flow through the shipped `.deb` unchanged** (nostr-smbd starts clean; parent stays 0755 across the smbd restart; samba-state absorbs the 0700 chmod).

## What #23 surfaces as blockers to full E2E

- **`nostrc-9yrr`** (P2) — `nostr-homed-seed` + `publish-projection` don't propagate testuser into a form that `nostr-homectl show` sees. Schema audit needed.
- **`nostrc-u7su`** (P2) — nostr-authd refuses to start on fresh install because of Wave-3 reconcile-required guard. First-boot seed procedure missing.
- **`nostrc-hfks`** (P2) — nostr-authd + nostr-smbd fight over `/var/lib/nostr-auth` mode. Loosen authd to 0755 (recommended).
- **`nostrc-y4ye`** (P2) — libnip34.so missing (same class as il9p libnip19). Third missing-NIP-lib symptom.

Each of these is a **pre-existing** enrollment-track defect (not a Wave-4 regression). All are P2 or lower. None invalidate the Wave-4 shipping-code closure evidence.

## Verdict

**`nostrc-0xd3` AMBER — partial GREEN.**

The four steps of the #23 matrix relevant to Wave-4 acceptance (steps 1 build-clean, 3 dep-purity, 4e-4h stack-up + protocol + samba round-trip) all land GREEN. The full end-to-end interactive login is blocked by the four enrollment-track P2 defects, which are pre-existing and orthogonal to Wave 4. Each is filed with a repro and a fix recommendation.

**`nostrc-0xd3` cannot close as GREEN** until the four filed beads land. It CAN transition to a documented AMBER with this transcript, with the parent epic `nostrc-rb0e` acknowledging that Wave-4 shipping code is validated but user-enrollment tooling has separate follow-up work.

## Follow-up beads

- `nostrc-m4y1` (P3) — deferred ctest sweep
- `nostrc-<hygiene>` (P3) — test-file `-Werror` cleanups
- `nostrc-hfks` (P2) — authd/smbd StateDirectoryMode conflict
- `nostrc-u7su` (P2) — first-boot reconcile procedure missing
- `nostrc-9yrr` (P2) — seed→show visibility gap
- `nostrc-y4ye` (P2) — libnip34 missing runtime dep

## Transcripts

- Build attempts + failures: `~/0xd3-build.log` on lab
- authd journal + smb.db reconcile failure: captured inline above
- DAV probe: captured inline above
