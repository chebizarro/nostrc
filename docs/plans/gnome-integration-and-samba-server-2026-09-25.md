# Wider GNOME Integration + Samba Server-Side: Plan

**Supersedes:** `docs/plans/nostr-linux-samba-login-2026-09-19.md` (and its critique
at `docs/reviews/nostr-linux-samba-login-2026-09-20.md`). This is the next-version
arc that picks up where the 09-19 shipping plan left off.

**Status:** APPROVED — Phase 6 design critique applied; six maintainer confirmations recorded (see Confirmed Design Decisions below); ready for orchestrated dispatch.

## Confirmed Design Decisions (from maintainer, 2026-09-25)

1. **Signer `SignEvent` migration → D1.a** (versioned semantic change, one PR, all consumers migrate). Switch `nips/nip55l/src/glib/signer_service_g.c:193-196,249-256` dispatch to `nostr_nip55l_sign_event_json`; align XML doc, README, in-tree consumers, and `mock_signer.c`.
2. **`nostr-dav` activation → single systemd user unit as primary**; DBus activation converges via `ExecStart=systemctl --user start nostr-dav.service` (or is dropped).
3. **Session-relay fallback → user-controllable policy** (`nostr_dav_upstream_mode` in config, three modes as designed in Track 2 D4).
4. **DAV publish commit → follow NIP-51/65 outbox model.** Resolve write-relay targets from the account's kind-10002 (NIP-65) `write`-marked relays; where NIP-51 lists tag additional relays (e.g. per-kind), honor those. Commit definition: `published` once ALL NIP-65-declared write relays for the account have ACK'd (not "≥1 OK" — that was the plan's placeholder). Fewer than all → row stays `pending` and retries; document how NIP-51 kind-specific lists override the default set.
5. **Samba server v1 acceptance gate → real Windows + GNOME Files client round-trip** (Item #16b XL applies as originally split). No "config-only" fast path.
6. **DM notifications → fully opaque** ("You have a new encrypted direct message", event-id passthrough) until GNostr decrypts.

All six answers are load-bearing for downstream orchestration — items #1, #5, #6, #8a/8b, #9, #13, #14a/14b, #16b are affected.

---

## Goal

Take the nostrc stack past its current login + portable-home posture into a full
desktop-native integration: a hardened generic Nostr signer usable by any GNOME
app (`nostr-signer-daemon` / `gnostr-signer`), a working Calendar + Contacts
bridge (via `nostr-dav`), a session-scoped local relay (with NIP-29 group and
DM notifications piped into gnome-shell), and a three-way Samba deepening —
client mount polish, standalone-server file sharing, and domain-controller-
adjacent (winbind/idmap) integration. Every new surface preserves the existing
dep-purity gate on `nostr-authd` + `pam_nostr` and stays `-Werror`-clean on the
aarch64 lab.

---

## Approach

Four tracks, each independently shippable, sequenced so shared seams land once:

1. **Track 1 — Signer hardening** (`nips/nip55l/`, `apps/gnostr-signer/`).
   Fix the three documented drifts (`SignEvent` return-type contradiction,
   `GetRelays` placeholder, unconfigured `@CMAKE_INSTALL_FULL_BINDIR@` literal),
   resolve the DBus activation name collision, and wire the unwired production
   paths (`bunker_service_start`, `sheet_qr_display_set_bunker_uri`, the
   deferred approval-signal subscription in `main_app.c:571-615`). The session
   signer stays app-facing; it never talks to `nostr-authd`. This preserves
   the boundary the 09-19 plan drew at
   `docs/plans/nostr-linux-samba-login-2026-09-19.md:182-199` while giving
   Tracks 2–4 a stable signing API.

2. **Track 2 — Calendar/Contacts (Path A)**. Turn `gnome/nostr-dav/` from a
   localhost DAV scaffold into a real background service: fix the auth bypass
   (`nd-dav-server.c:1461-1470` accepts any request when `account_id` is null,
   per `:166-169`), remove the LAN-rebind env override
   (`nd-application.c:18-19,49-68`), replace ephemeral `GHashTable` persistence
   (`nd-calendar-store.c:1-20`, `nd-contact-store.c:1-20`) with SQLite, and
   wire relay subscribe + signer publish for kinds 31922/31923/30085.
   End-user setup rides the stock GNOME 46 WebDAV Account dialog — no custom
   UI, consistent with the goa-overlay postmortem lesson
   (`docs/proposals/goa-overlay-postmortem.md:41-66`).

3. **Track 3 — Relay**. Piece A: a session-scoped local relay on
   `$XDG_RUNTIME_DIR/nostr/relay.sock` (mode 0600, SO_PEERCRED admission),
   sharing server code with the existing system `nostrc-relayd` via a new
   static `libnostr-relay-server` factored out of `relayd_core`/`relay_security`,
   and fixing the bind bug at `apps/relayd/src/relayd_main.c:381-408` (port
   extracted, interface never set). Piece B: a new `nostr-notify-daemon`
   user service subscribing to NIP-29 group kinds (9-12, 39000-39004) and
   kind-1059 gift-wrapped DMs through Piece A, surfacing through
   `GNotification` — only when GNostr is not running, to avoid duplicating
   the in-app badge path (`badge_manager.c:1222-1228`).

4. **Track 4 — Samba three-way split**. (a) Client polish: batch mounts,
   share discovery, wire the orphaned `nh_smb_authority_sweep_expired`
   (`smb_credential.h:120-137,165-174`), and close the logout-teardown gap
   (`pam_nostr_broker.c:634-637` returns success without SMB unmount).
   (b) Server-side: ship the six files proposed at 09-19 §1331-1336, choose
   **share-declaration** over a VFS plugin for v1, keep the credential
   authority inside `nostr-authd` with a rename-and-lift split path.
   (c) DC-adjacent: continue "configuration contract only" for winbind
   (`SAMBA_AD_LOGIN.md:1-36`), extend `nss_nostr` to optionally admit
   `DOMAIN\user`/`user@REALM`, and punt the keytab pipeline to a documented
   follow-up.

**Dep-purity gate (load-bearing, applies to every track):** `nostr-authd`
and `pam_nostr.so` gain **no** edges to libhanami, FUSE3, porthome, or the
signer client libraries. The gate from the 09-19 plan is re-verified
mechanically (`nm --undefined-only` + `ldd`) after each track lands, and a
CI script is added so it cannot silently regress.

**Ordering rationale:** Track 1 first because Tracks 2 and 3B consume
`org.nostr.Signer` for outbound signing. Track 3A before 3B because
notifications ride the session relay. Track 4 is largely independent of 2–3
and parallelizes after Track 1. Track 4(b) must not touch the authd
binary's link set in ways that violate dep purity — the Samba authority
code lives in authd's *source* tree but links only its existing deps.

---

## Wave 5 tail landed (2026-09-26)

Two-branch Wave 5 landed post-Oscar's #16b acceptance:

- **Wave 5 A — P1 packaging fixes + cleanup** (`5db84f7a`, `05183e11`):
  - `nostrc-0vuu` FIXED: `nostr-smbd.service.in` gains `--no-process-group`
    to escape systemd's `setsid()` EPERM abort.
  - `nostrc-45b2` FIXED: `smb.conf.standalone.sample` pins Samba's
    `state directory` + `private dir` to `/var/lib/nostr-auth/samba-state`;
    parent `StateDirectoryMode` loosened to 0755. Samba's mandatory 0700
    chmod is now isolated from the force-user parent.
  - `nostrc-kr6h` DONE: legacy `pam_nostr.c` + `pam_nostr.h` deleted;
    `SYSTEMD_TOPOLOGY.md` pointer updated to `pam_nostr_broker.c`.
- **Wave 5 B — nostrc-relayd subpackage** (`8589abf8`):
  - `nostrc-7zh1` DONE: Debian binary package `nostrc-relayd` + Fedora
    subpackage now own `/usr/sbin/nostrc-relayd` + `nostr-relayd.service`
    + `/usr/share/nostrc/relay.toml.example` (previously dropped via
    `debian/not-installed` + `%install`-time rm's).

Wave 6 tail (all filed as beads, requires lab time):

- **`nostrc-nud0` (P1)** — re-run #16b Windows + GNOME Files matrix
  against the FIXED shipped `.deb` to flip Q5 gate → GREEN.
- **`nostrc-qngf` (P2)** — #22c-a Debian installed-unit acceptance on
  the aarch64 lab.
- **`nostrc-cr0s` (P3)** — #22c-b Fedora installed-unit acceptance
  (blocked on a Fedora build+install VM).
- **`nostrc-0xd3` (P1)** — #23 live smoke lab (full E2E login),
  blocked on #22c-a + #16b re-run landing GREEN.

---

## Execution Index

Sizes: S = days, M = ~1 engineer-week, L = several engineer-weeks, XL =
multi-week with external acceptance.

| # | Item | Goal | Done-when | Key files | Dependencies | Size |
|---|---|---|---|---|---|---|
| ✅ 1 | Signer contract migration (D1.a) + GetRelays + bindir | LANDED on `feat/signer-hardening` (`960f9c03`+`9629e189`): dispatch → `sign_event_json`, XML + README + `dbus-interface.md` migrated, `VERSION_MANIFEST` adds nip55l 0.2.0, activation collision resolved with `ENABLE_NIP55L_STANDALONE_ACTIVATION=OFF`, `configure_file(@ONLY)` + `CheckDbusServiceFile.cmake` ctest, wizard fix (nostrc-0e7k CLOSED), new `test_sign_event_json.c` + `test_getrelays.c`, all in-tree consumers (`blossom_client`, `nostrfs`, `mock_signer`, `test-dbus`, `test_signer_integration`) migrated. Pre-merge blocker: **nostrc-yai2** (pam_nostr sig-parse fix, dispatching separately). | ~33 files across `nips/nip55l/`, `apps/gnostr-signer/`, `gnome/nostr-homed/tests/integration/`, `docs/dbus-interface.md`, `VERSION_MANIFEST.md` | None | M |
| ✅ 2 | DBus activation collision | LANDED with #1: gnostr-signer ships it; nip55l gated behind `ENABLE_NIP55L_STANDALONE_ACTIVATION=OFF`; post-install smoke asserts no `@` literal | `nips/nip55l/CMakeLists.txt`, `apps/gnostr-signer/CMakeLists.txt`, both `.service.in` files, `cmake/CheckDbusServiceFile.cmake` (new) | #1 | S |
| ✅ 3 | Wire unwired signer paths | LANDED with #1: `--bunker` flag wired with fail-fast diagnostics + open-ACL refusal, `sheet-create-bunker` calls `sheet_qr_display_set_bunker_uri`, deferred DBus init subscribes ApprovalRequested + ApprovalCompleted | `apps/gnostr-signer/daemon/main_daemon.c`, `apps/gnostr-signer/src/main_app.c`, `apps/gnostr-signer/src/ui/sheets/sheet-create-bunker.c` | #1, #2 | M |
| ✅ 4 | Signer fuzz + binary NIP-44 tests | LANDED (`55c777c8`): 4 libFuzzer harnesses (`fuzz_relays`, `fuzz_sign_event_json`, `fuzz_nip44_b64`, `fuzz_decrypt_zap_event`) gated by `ENABLE_FUZZING`/`ENABLE_FUZZING_RUNTIME`. Real-service `GTestDBus` contract test in `test_signer_dbus_contract.c` + Quebec's extension covering StoreKey→GetPublicKey race (`e8eafde7`). `nostrc-p7f6` + `nostrc-3m86` + `nostrc-tf3b` all close | `nips/nip55l/tests/fuzz/*`, `nips/nip55l/tests/test_signer_dbus_contract.c` | #1 | M |
| ✅ 8a | nostr-dav relay transport + cursor | LANDED (`05503a87`): `nd-relay-transport.{c,h}` abstract transport, fixture backend + libsoup WebSocket scaffold (returns NOT_SUPPORTED — real wiring in `nostrc-tu6y`); per-relay resumable cursor in `relay_cursor` SQLite table. `test_relay_sync` green | `gnome/nostr-dav/src/nd-relay-*` | #7 | L |
| ✅ 8b | nostr-dav format/fold + tombstones | LANDED (`05503a87`): kind-5 tombstones + 31922/31923/30085 fold via `nd-relay-sync`; last-writer-wins + LRU dedup; fold guards protect `publish_state='pending'` rows from remote clobber | `gnome/nostr-dav/src/nd-relay-sync.{c,h}` | #8a | L |
| ✅ 9 | nostr-dav publish via signer | LANDED (`05503a87`): `nd-signer{,-dbus}.{c,h}` vtable-backed proxy to `SignEventJson`; `nd-publisher` outbox worker with NIP-65 outbox commit (all-target-ACK), permanent/transient classification, 120s OK-wait deadline, quorum clamp. SQLite v1→v3 adds `relay_cursor` + `publish_targets`. Enable_publish gate defaults OFF pending `nostrc-tu6y`. `test_publish_rollback` green | `gnome/nostr-dav/src/nd-signer*.c`, `nd-publisher.{c,h}`, `nd-application.c` | #8, #1 | M |
| ✅ 11 | relayd bind bug fix | LANDED (`90ac64c9`): `relayd_config_parse_listen` parses host:port + rejects bare-port; sets both `info.iface` + `info.port`. `test_bind_iface` + `run_bind_iface_integ.sh` (both PASSED on the lab) | `apps/relayd/src/relayd_main.c`, `apps/relayd/relayd_config.c`, `apps/relayd/tests/` | None | S |
| ✅ 12 | `libnostr-relay-server` factor | LANDED (`3ae5e298`): new static lib with tagged-union listener spec (`NOSTR_RELAY_LISTENER_TCP` for today's daemon, `NOSTR_RELAY_LISTENER_UNIX_FD` locks in the Wave-3 session-relay API; returns -ENOSYS until #13 lands). `nostrc-relayd` shrinks to ~55-line `main()` over the lib | `apps/relayd/include/nostr-relay-server.h` (new), `apps/relayd/src/relay_server.c` (new), `apps/relayd/CMakeLists.txt` | #11 | L |
| ✅ 15 | Samba client polish | LANDED (`34a2adf1`): `--batch` + `--unmount --all` + session-scoped ledger, `nostr-smb-browse` CLI + autostart, sweep-on-open + systemd timer, `pam_sm_close_session` → per-user teardown user service via `systemctl --user --machine=<uid>@`. Dep-purity gate stays green (fork+exec, no library edges). 3 new tests | `gnome/nostr-homed/src/smb/*`, `pam_nostr_broker.c`, `nostr-authctl`, `systemd/user/nostr-smb-teardown.service.in` | None | M |
| ✅ 19 | nss_nostr domain-qualified names | LANDED (`34a2adf1`) — revised per Finding 15: zero C changes to nss_nostr/identity_common. `nsswitch.conf.snippet.sample` + policy doc ordering section + validator warns on legacy config + `test_nss_qualified_names` asserts DOMAIN\\user goes to winbind (or NOTFOUND), n_foo bit-exact | `config/nsswitch.conf.snippet.sample`, `packaging/pam/nostr-winbind-policy.md`, `packaging/domain/validate_domain_profile.py`, `tests/unit/test_nss_qualified_names.c` | None | S |
| ✅ 20 | DC keytab punt + manual join doc | LANDED (`34a2adf1`): SAMBA_AD_LOGIN.md gains 8-step manual join recipe + keytab pipeline PUNT with rationale | `gnome/nostr-homed/docs/SAMBA_AD_LOGIN.md` | #19 | S |
| ✅ 5 | nostr-dav auth fix | LANDED on `feat/nostr-dav-hardening` (`82e10a56`): fail-closed startup order, 0600 token in `$XDG_CONFIG_HOME/nostr-dav/token`, libsecret best-effort copy, `test_auth_required` 10 cases green | `gnome/nostr-dav/src/nd-application.c`, `nd-token-store.c`, `nd-dav-server.c`, `nd-config.c` (new) | None | M |
| ✅ 6 | nostr-dav bind policy | LANDED (`82e10a56`): both `NOSTR_DAV_ADDRESS` + `NOSTR_DAV_PORT` env deleted; compile-time `127.0.0.1:7680`; refuses non-loopback | `gnome/nostr-dav/src/nd-application.c` | #5 | S |
| ✅ 7 | nostr-dav SQLite persistence | LANDED (`82e10a56`): SQLite WAL at `~/.local/share/nostr-dav/store.sqlite` (0600); schema includes publish columns + `publish_log` for #9; monotonic ctag per collection; `test_store_sqlite` 5 cases green | `gnome/nostr-dav/src/nd-calendar-store.c`, `nd-contact-store.c`, `nd-file-store.c` | #5 | M |
| 8a | nostr-dav relay transport + cursor | REQ connection lifecycle + resumable cursor stable across restart | Reconnect backoff; cursor persisted; NIP-42 challenge cache | `gnome/nostr-dav/src/nd-relay-sync.c` (new) | #7 | L |
| 8b | nostr-dav format/fold + tombstones | 31922/31923/30085 upsert + kind-5 delete | Upsert keyed by `(kind,pubkey,d)`; tombstone replay test; dedup by event id | `gnome/nostr-dav/include/nd-ical.h:16-18`, `nd-vcard.h:15-17` | #8a | L |
| 9 | nostr-dav publish via signer | PUT/DELETE publish signed events | Outbound signed via `org.nostr.Signer.SignEvent`; rollback of local store on relay rejection | `gnome/nostr-dav/src/nd-dav-server.c:559-612` | #8, #1 | M |
| ✅ 10 | nostr-dav docs + port | LANDED (`82e10a56`): port 7680 everywhere; QUICKSTART rewritten for GNOME 46 WebDAV dialog + `--show-credentials`; systemd unit templated with real install path + companion `nostr-dav-dirs.service` for pre-start dirs; `ProtectKernelModules` removed (fails under user manager); SECURITY.md + README updated | `gnome/nostr-dav/docs/QUICKSTART.md`, `gnome/nostr-dav/systemd/*.service.in` | #6 | S |
| 11 | relayd bind bug fix | Listen addr actually honored | `info.iface` set from `cfg.listen`; test asserts 127.0.0.1 binding | `apps/relayd/src/relayd_main.c:381-408`, `apps/relayd/relay.toml.example:4-16` | None | S |
| 12 | `libnostr-relay-server` factor | Shared server lib, two consumers | Static lib builds; `nostrc-relayd` and `nostr-session-relayd` link it | `apps/relayd/CMakeLists.txt:25-56` | #11 | L |
| ✅ 13 | Session relay daemon + unit | LANDED (`f5ccbadd`): `nostr-session-relayd` main + `libnostr-relay-server` Unix-fd path implementation (lws_adopt_socket + poll/accept loop). `$XDG_RUNTIME_DIR/nostr/relay.sock` 0600 + SO_PEERCRED (mirrors `auth_peer.c:52-58`). `PartOf=graphical-session.target` with 5s SIGTERM drain. `session_routing.{c,h}` captures the §3.2 D4 event-class routing table | `apps/relayd/src/session/*` (new), `apps/relayd/src/relay_server.c` (Unix-fd path), `gnome/nostr-homed/systemd/user/nostr-session-relay.{service,socket}` (new) | #12 | M |
| ✅ 14a | nostr-notify-daemon relay intake | LANDED (`f5ccbadd`): `org.nostr.NotifyDaemon` GApplication ID (NOT sharing GNostr's per Finding 4). Subscribes kinds 9-12 (NIP-29) + 1059 with #p (NIP-17 DMs) via home_relays fallback (libnostr `unix://` transport pending — natural follow-up). Cursor at `~/.local/state/nostr-notify/cursor` atomic tmp+rename, GMutex-serialized | `gnome/nostr-homed/src/notify/*` (new), `systemd/user/nostr-notify.service.in` | #13 | L |
| ✅ 14b | nostr-notify-daemon shell UX | LANDED (`f5ccbadd`): DM notifications OPAQUE (Finding 14 fixed — outer gift-wrap pubkey is random per NIP-17). NIP-29 preview 80 code points markup-escaped + U+2026 truncation. Deep-link URIs `nostr://open?event=…` / `?group=…&event=…` via Gio.DesktopAppInfo.launch_uris(). Suppression **generation guard** with strict `set_suppressed(true)` BEFORE `bump()` order. Oracle review found + fixed 3 correctness bugs pre-commit (Q2 order, Q4 UAF, Q5 cursor UB) | `notify_gnotification.c`, `notify_subs.c`, `notify_cursor.c` | #14a | L |
| 15 | Samba client polish | Batch + discovery + sweep + teardown | `--batch` mounts all `servers.d/*.conf`; sweep on timer; `pam_sm_close_session` unmounts | `gnome/nostr-homed/src/smb/nostr-smb-mount.sh:16-29,40-54,165-176,259-266`, `smb_credential.c:327-363,553`, `gnome/nostr-homed/src/pam/pam_nostr_broker.c:634-637` | None | M |
| ✅ 16a | Samba server five-file config + service packaging | LANDED (`bd7d62e8`): FIVE files (not six — dead nostr-smb-credentiald.service dropped per Finding 21, name reserved in docs). testparm -s clean on the sample. Sysusers + share user provisioned | `config/smb-credentiald.conf.sample`, `config/smb.conf.standalone.sample`, `config/servers.d/example.conf`, `systemd/nostr-smbd.service.in`, `docs/SAMBA_STANDALONE.md`, `packaging/sysusers.d/nostr-smb-share.conf` | #15 | L |
| ⚠️ 16b | Samba server installed-client acceptance | LANDED (`a7bea570` merge / `309a0b88`): Q5 acceptance matrix run against Windows 10 client (`192.168.64.4`) + GNOME Files (`gio` under `dbus-run-session` on `192.168.64.3`). **Result: AMBER** — W2/W3/W4 (134 MB/s + sha256 match)/W5/W7 (SMB 3.1.1 encrypted)/W8/W9 + G1/G2 all GREEN. W1 mount + W6 ACL escalation AMBER. Two P1 packaging defects filed (`nostrc-0vuu`: `nostr-smbd.service.in` missing `--no-process-group`; `nostrc-45b2`: share path collides with samba private state dir 0700). P1 passdb drift-detector belongs to `nostr-authd` scope, not this matrix. Full transcripts + `tests/acceptance/samba-server-windows/` harness. `nostrc-rb0e`/`.11`/`.12` remain open until the two P1s land + re-run turns GREEN | `docs/reviews/samba-server-windows-acceptance-2026-09-26.md`, `tests/acceptance/samba-server-windows/*` | #16a ✅, #18 ✅ | XL |
| ✅ 17 | Share-declaration v1 + VFS follow-up | LANDED (`bd7d62e8`): [nostr-home] share path=/var/lib/nostr-auth/share-root (FUSE mountpoint), force user/group=nostr-smb-share; failure isolation (mount dies → share errors, smbd lives). VFS-plugin v2 bead `nostrc-b6h1` filed with acceptance criteria (per-share opt-in, smbd crash-budget, pinned-Samba-ABI matrix) | `config/smb.conf.standalone.sample` | #16a | M |
| ✅ 18 | Journal↔passdb reconciliation | LANDED (`bd7d62e8`) with Finding-2 correct flags: `smbpasswd -c <conf>` (config selector; `-s` silent-stdin preserved separately) + `pdbedit -s <conf>` (config selector, DIFFERENT flag). New `nh_smb_authority_open_ex` runs `pdbedit -L -s <conf>` on open, diffs vs journal, refuses readiness with `NH_SMB_RECONCILE_REQUIRED` on drift. Argv-capture harness asserts both flags | `gnome/nostr-homed/src/smb/passdb_tdbsam.c`, `nostr-authd.c` (small config-loader hook) | #16a | M |
| 19 | nss_nostr domain-qualified names | Optional `DOMAIN\user`/`user@REALM` admission | `nostr_domain_qualified_names=yes` gate; default off; winbind policy doc updated | `gnome/nostr-homed/src/nss/nss_nostr.c:82-113`, `identity_common.c:8-20`, `packaging/pam/nostr-winbind-policy.md:1-23` | None | M |
| 20 | DC keytab punt + manual join doc | Documented posture, follow-up bead | `SAMBA_AD_LOGIN.md` gains manual join recipe; keytab pipeline bead created | `gnome/nostr-homed/docs/SAMBA_AD_LOGIN.md:1-36`, `config/smb.conf.winbind.sample:1-18` | #19 | S |
| ✅ 21a | Dep-purity gate — baseline scaffold + allowlist | LANDED on `feat/dep-purity-gate-baseline`: `scripts/check-authd-dep-purity.sh` + pinned allowlist + `homed_authd_dep_purity` CTest, negative-case-verified, 0.05s green | `scripts/check-authd-dep-purity.sh`, `scripts/README.md`, `gnome/nostr-homed/CMakeLists.txt` (near existing install block) | None | S |
| 21b | Dep-purity gate — final integrated check | All tracks re-verified; installed-artifact check on lab | Green after every merge + at packaging | #21a, all tracks | S |
| ✅ 22a | Debian packaging | LANDED (`2f2a2e82`): 4 new Debian binary packages — `nostrc-session-relay`, `nostr-notify`, `nostr-dav`, `nostrc-samba-server`. `debian/*.install` maps + `debian/rules` overrides + `debian/control` split | `debian/*.install`, `debian/rules`, `debian/control` | #13, #14, #10, #16a | M |
| ✅ 22b | Fedora packaging | LANDED (`2f2a2e82`): mirror 4-subpackage split in `packaging/rpm/nostr-login.spec` with `%check` running dep-purity gate on staged tree | `packaging/rpm/nostr-login.spec` | #22a | M |
| ⚠️ 22c | Installed-unit acceptance | SPLIT into two beads: **nostrc-qngf** (Debian arm on aarch64 lab — priority-2) + **nostrc-cr0s** (Fedora arm — priority-3, blocked on Fedora VM). Deliberately deferred from this session because Wave 5 A+B (P1 packaging fixes `nostrc-0vuu`+`nostrc-45b2` + `nostrc-7zh1` relayd subpackage) had to land first before an installed-unit run would be meaningful | Both #22a + #22b landed | #22a, #22b, #21 | M |
| ⚠️ 23 | Live smoke lab | Filed as **nostrc-0xd3** (P1). Full E2E matrix in the bead: `-Werror` build, ctest sweep, dep-purity gate on installed artifacts, real login end-to-end (PAM → portable-home → session-bus → DAV publish + NIP-17 DM + Samba mount → session teardown). Blocked on #22c-a landing GREEN | bizarro@192.168.64.3 passes smoke matrix | All tracks | All | M |

---

## Background

### Existing planning + critique posture

- **`docs/plans/nostr-linux-samba-login-2026-09-19.md`** (DRAFT, "implementation
  dispatched; no release certification"). Bead references: `nostrc-1r5d`,
  `nostrc-80qa`, `nostrc-bmue`, `nostrc-br78`, `nostrc-diz6`, `nostrc-koso`,
  `nostrc-nxpb`, `nostrc-ot2c`, `nostrc-rb0e`, `nostrc-svsj`, `nostrc-zcll`.
  §1331-1336 lists 6 proposed-but-unbuilt Samba files (Track 4 Dimension b
  ships them). This plan supersedes it.
- **`docs/reviews/nostr-linux-samba-login-2026-09-20.md`** — bounded critique
  of the 09-19 plan.
- **`docs/designs/packaging-plan-debian-fedora.md:288-295`** — mentions a
  future `nostr-relayd` package; text is stale vs current CMake (which
  installs `nostrc-relayd` under `/usr/sbin` and a system unit, not a user
  unit).
- **`docs/designs/home-from-relay.md`** — FROZEN. The portable-home epic
  (`nostrc-h10m`) rides on top of this; every P2 code item was closed in
  the preceding session (only ops config `nostrc-e4v6` remains open).
- **`docs/proposals/goa-overlay-postmortem.md`** — records why a custom
  `GoaProvider`/`gnome-goa-overlay` was built then removed. Three concrete
  reasons cited at `:41-46` (no GOA plugin `dlopen` scan), `:48-52` (fragile
  daemon fork, untested with GNOME 46+, unsuitable for Flatpak), and
  `:63-66` (contradictory setup + duplicate-account bugs). Lesson: prefer
  stock desktop-integration points; don't fork desktop-service daemons.

### Signer track — `nips/nip55l` + `apps/gnostr-signer`

**`nips/nip55l/`** — real implementation, not a proposal. The **DBus name
and interface are `org.nostr.Signer`** at `/org/nostr/signer`, exposing per
`nips/nip55l/dbus/org.nostr.Signer.xml:2-88`: `GetPublicKey`, `SignEvent`,
`Nip04Encrypt/Decrypt`, `Nip44Encrypt/Decrypt`, `NIP44EncryptB64` +
`DecryptB64` (binary-safe), `DecryptZapEvent`, `GetRelays`, `StoreKey`,
`ClearKey`, `ApproveRequest`, plus two approval signals. Ships
`nostr-signer-daemon` + `nostr-signer-cli`, gated by `ENABLE_NIP55L`
(default ON) at `NipOptions.cmake:282-287`. Installs a DBus activation
`.service`, **not** a systemd user unit (`nips/nip55l/CMakeLists.txt:102-109`).

**Known bugs / drift** — Track 1 fixes these:
- **`SignEvent` return type contradiction**: XML says it returns a
  signature; README describes signed JSON. `nips/nip55l/dbus/org.nostr.Signer.xml:5-11`
  vs `nips/nip55l/README.md:66-67`.
- **`GetRelays` is a placeholder**: returns `[]` instead of the README's
  claimed `NOT_FOUND`. `nips/nip55l/src/core/signer_ops.c:580-589`.
- **DBus activation template has an unconfigured `@CMAKE_INSTALL_FULL_BINDIR@`
  literal**: `nips/nip55l/dbus/org.nostr.Signer.service.in:1-3` — CMake
  copies rather than `configure_file()`s.

**`apps/gnostr-signer/`** — GTK4/libadwaita GUI **plus** `gnostr-signer-daemon`.
The daemon owns `org.nostr.Signer` (same bus name as nip55l — collision
below) and exports the `nips/nip55l` GLib implementation
(`apps/gnostr-signer/CMakeLists.txt:137-173,290-303`;
`apps/gnostr-signer/daemon/main_daemon.c:27-65`). GUI consumes via
`StoreKey/ClearKey/ApproveRequest`. Includes libsecret/keychain optional
secret storage, NIP-46 bunker parsing, QR display/scanner helpers.
Ships a **systemd user unit** at
`apps/gnostr-signer/daemon/packaging/systemd/user/gnostr-signer-daemon.service:1-14`.

**Known bugs / drift**:
- **DBus activation `.service` file NAME COLLISION with nip55l** — both
  install `org.nostr.Signer.service`.
- **Same `@CMAKE_INSTALL_FULL_BINDIR@` literal drift** at
  `apps/gnostr-signer/data/org.nostr.Signer.service:1-5`.
- **Unwired production paths**: no caller of `bunker_service_start` or
  `sheet_qr_display_set_bunker_uri`; `main_app.c` (`:285-345,571-615`) sets
  up an approval signal callback but its deferred DBus setup never subscribes.
- **No auth-broker integration**: zero references to `nostr-authd` or
  `pam_nostr` in either tree. `docs/plans/nostr-linux-samba-login-2026-09-19.md:182-199`
  keeps the session signer separate from the pre-login broker on purpose.
- **Beads**: `nostrc-qfdg`, `nostrc-tz8w` (UI accessibility), `nostrc-orz`,
  `nostrc-lmhf` (multisig store), `nostrc-p7f6` (fuzz tests), `nostrc-3m86`
  (nip55l binary NIP-44 test).

Design refs: `docs/proposals/55L.md`, `docs/dbus-interface.md`,
`docs/plans/nostr-linux-samba-login-2026-09-19.md:182-199`.

### Calendar + Contacts track — `nostr-dav` (Path A confirmed)

**Decision**: flesh out `gnome/nostr-dav/` into a real background service
GNOME Calendar/Contacts talks to via CalDAV/CardDAV on loopback. Path B
(native EDS backend) rejected — no scaffold in-tree, would require TWO
new backends (calendar + book), couples us to distro-shipped EDS versions
(Ubuntu 24.04 = 3.52.x, Fedora 40 = 3.52.x, Fedora 41 = 3.54.x).

**Functional dimensions of the choice (per maintainer feedback):**

*Path A functional advantages:*
- Works with any DAV-capable client, not GNOME-only: Thunderbird,
  KDE Contacts, macOS Calendar, Outlook.
- Runtime isolation: a crash in `nostr-dav` doesn't kill evolution-data-server.
- Decoupled from GNOME release cadence — no per-distro EDS ABI bind.
- Inspectable protocol boundary — `curl` the localhost endpoint.
- GNOME 46 shipped a stock **WebDAV Account** in Settings → Online Accounts
  ([release notes](https://release.gnome.org/46/)), so end-user setup is
  one dialog with no custom UI.

*Path B functional advantages we give up:*
- Lower-latency change propagation (direct `notify_update` on `EBookBackend`
  vs a DAV refresh cycle).
- Native EDS integration: `evolution-alarm-notify` triggers, Contacts
  autocomplete without HTTP traversal.
- Ability to expose Nostr-native semantics (relay-of-origin, event
  signatures, gift-wrap delivery) that don't cleanly fit CalDAV/CardDAV
  and get flattened into iCal properties.
- Notifications integrate more naturally from within EDS.

**Current state of `gnome/nostr-dav/`** (from `gnome/nostr-dav/CMakeLists.txt:16-42`):
C/GObject daemon, 9 headers, 3645 physical LoC in `.c`, 693 in `.h`, 1763 LoC
of tests. Deps: GLib/GObject/GIO, libsoup 3, libxml2, json-glib; libsecret
optional.

- Recognizes **NIP-52 kinds 31922 (calendar) + 31923 (occurrence)** and
  application-specific **kind 30085 (contacts)** —
  `gnome/nostr-dav/include/nd-ical.h:16-18`,
  `gnome/nostr-dav/include/nd-vcard.h:15-17`.
- **Neither subscribes nor publishes any of those kinds today**: no relay/
  signer transport in the daemon source list; calendar PUT writes to a
  local store only. `gnome/nostr-dav/src/nd-dav-server.c:559-612`.
- **Persistence is ephemeral `GHashTable`**, not SQLite or nostrdb.
  `gnome/nostr-dav/src/nd-calendar-store.c:1-20`,
  `gnome/nostr-dav/src/nd-contact-store.c:1-20`.
- Systemd user unit exists but hard-codes `%h/.local/bin/nostr-dav`; needs
  packaging review. `gnome/nostr-dav/systemd/nostr-dav.service:1-26`.

**Release blockers (must fix before ship)**:

1. **Auth bypass**: `account_id` starts null and `check_auth()` accepts
   requests when it's null. Application generates + logs a token but
   never configures the server's account. Without libsecret, validation
   accepts any token. `gnome/nostr-dav/src/nd-dav-server.c:166-169`,
   `:1461-1470`; `gnome/nostr-dav/src/nd-application.c:49-68`;
   `gnome/nostr-dav/src/nd-token-store.c:190-196`.
2. **Non-loopback env override**: default bind is `127.0.0.1` but
   `NOSTR_DAV_ADDRESS` env can rebind off loopback.
   `gnome/nostr-dav/src/nd-application.c:18-19,49-68`.
3. **No relay wiring**: kinds 31922/31923/30085 are parseable but no
   subscription publishes updates; no signer bridge.

**Doc drift**: `gnome/nostr-dav/docs/QUICKSTART.md:47-50` says port 7654;
`nd-application.c:18-19` defaults to 7680.

### Relay track — (a) session-scoped local relay + (b) NIP-29/DM shell notifications

Confirmed maintainer intent: (a) AND (b).

**Piece A — session-scoped local relay.**

- Two relay-server binaries exist but neither is session-scoped:
  `apps/relayd/CMakeLists.txt:25-56` (builds `nostrc-relayd` + reusable
  `relayd_core` + `relay_security`) and `apps/grelay/CMakeLists.txt:22-33`.
- `nostrc-relayd` is installed under `/usr/sbin` with a **system** unit
  (`apps/relayd/systemd/nostr-relayd.service.in:1-20`), not a user-session
  unit.
- **Bind bug**: `apps/relayd/relay.toml.example:4-16` says `127.0.0.1:4848`
  but `apps/relayd/src/relayd_main.c:381-408` extracts only the port and
  never sets a bind interface for libwebsockets. `grelay_main.c:840-855`
  defaults to `0.0.0.0:4849` explicitly. **The stated loopback bind is
  not enforced by code.**
- `libnostr`'s `NostrRelay` is a relay-client API, not a server library
  (`libnostr/include/nostr-relay.h:25-35`). No `libnostr-relay` server lib.
- NIP-42 gates EVENT + REQ on signed Nostr auth
  (`apps/relayd/src/protocol_nip01.c:164-169`, `:275-281`) — that's a Nostr
  key identity, NOT a session-user identity.

**Session-service prior art in this repo:**
- `gnostr-signer-daemon` uses `%t/gnostr/signer.sock` (systemd `%t` =
  `$XDG_RUNTIME_DIR`), mode 0600, optional TCP endpoint requires a token
  and loopback check
  (`apps/gnostr-signer/daemon/packaging/systemd/user/gnostr-signer-daemon.service:7-15`,
  `apps/gnostr-signer/daemon/ipc.c:199-221`, `:240-290`).
- `nostr-authd`'s `user.sock` is a **system** broker socket at mode 0666
  with SO_PEERCRED-based UID admission
  (`gnome/nostr-homed/docs/AUTH_PROTOCOL.md:18-25`) — different model,
  not a template for a session-local relay.

**Piece B — NIP-29 group / DM shell notifications.**

- `libnostr/include/nostr-kinds.h:14-19,181-186` defines kinds 9-12
  (chat/reply) and 39000-39004 (group metadata). Kinds 39005-39009 are
  not defined in the tree.
- NIP-29 consumer exists **inside GNostr, not gnome-shell**. The plugin at
  `apps/gnostr/plugins/nip29-groups/gn-nip29-group-service.c:919-955,1091-1168`
  is gated by `ENABLE_NIP29` (default OFF at `NipOptions.cmake:123-126`).
- DM subscription lives at `apps/gnostr/src/ui/gnostr-dm-service.c:222-266`
  — subscribes to kind 1059 (NIP-17 gift-wrap) filtered by `#p`, starts
  when the main window authenticates. Badge manager at
  `apps/gnostr/src/notifications/badge_manager.c:1222-1228` calls the
  main-window `desktop_notify` path (`.c:452-504`). **Badge callback passes
  only the outer gift wrap's pubkey — no content preview**
  (`badge_manager.c:589-624,1584-1615`).
- `nh_porthome_notify` (`gnome/nostr-homed/include/nh_porthome_notify.h:7-29`)
  is portable-home-scoped, not a message-notification consumer.
- Shell extensions provide UI patterns only:
  `gnome/nostr-homed/greeter-extension/nostr-home-status@nostrc/extension.js:1-13`,
  `nostr-login-qr@nostrc/metadata.json:1-7`.
- **Nowhere in the tree is there an independent background service that
  subscribes to message relays and posts local notifications outside the
  GNostr app.**

**Cross-cutting relay-set contract:**
- Portable-home `home_relays` (`gnome/nostr-homed/config/auth.conf.sample:69-79`)
  resolves per account → NIP-65 → auth config.
- GNostr DM relays (`nostr-gobject/src/gnostr-relays.c:778-789`) have
  fallback to general relays.
- NIP-29 groups store their own recorded group relay URL
  (`apps/gnostr/plugins/nip29-groups/gn-nip29-group-service.c:1026-1033`).

**Beads:** `bd search relay` returned 4 — `nostrc-ot2c.4` (in_progress),
`nostrc-rb0e.1` (blocked), `nostrc-h10m` (open — porthome umbrella),
`nostrc-1cx` (open). No bead for a session-local relay exists.

### Samba track — three-way split (client / server / DC)

Maintainer intent: all three dimensions, split over "logical equivalent
samba divisions."

*Client-side deepening seams:*
- `nostr-smb-mount --mode gvfs` mounts ONE `smb://user@host/share` per
  invocation (`gnome/nostr-homed/src/smb/nostr-smb-mount.sh:16-29,40-54,
  165-176,259-266`). No batch, no discovery hook.
- `nh_smb_authority_sweep_expired` (`smb_credential.h:120-137,165-174`;
  `smb_credential.c:327-363,553`) is declared and implemented but has
  no production caller — integration tests only. Header comment says
  `open()` revokes expired, but current `open()` body just does SQLite
  schema install.
- Session-teardown gap: `pam_sm_close_session`
  (`gnome/nostr-homed/src/pam/pam_nostr_broker.c:634-637`) returns success
  without SMB unmount. Older `pam_nostr.c:254-266` calls `CloseSession`
  for home, not SMB. **No logout hook invokes `nostr-smb-mount --unmount`**.
- No `gvfsd-smb-browse` hook in the tree.

*Server-side file sharing seams:*
- Authority + adapter names: `nh_smb_authority_open`, `nh_smb_passdb_ops`,
  `nh_smb_passdb_tdbsam_ops` (`smb_credential.h:70-83`). Hosted in
  `nostr-authd` on `user.sock`; journal path is
  `/var/lib/nostr-auth/smb.db` (`nostr-authd.c:1-16,190-205`;
  `CMakeLists.txt:842-852`).
- **Journal ≠ Samba passdb**: `smb.db` is the authority's SQLite issuance
  journal (cred ID, username, UID, pubkey, binding, timestamps) —
  NOT the Samba passdb. The adapter shells out to `smbpasswd`/`pdbedit`
  (`passdb_tdbsam.c:205-277`) without selecting a dedicated Samba config.
  Interop depends on those CLI calls targeting the passdb `smbd` reads.
- **No shipped standalone `smb.conf` template.** The only sample is inert
  winbind (`config/smb.conf.winbind.sample:1-18`), installed only as docs
  (`CMakeLists.txt:613-628`).
- **No dedicated Samba service unit ships.** Names `systemd/nostr-smbd.service`
  and `systemd/nostr-smb-credentiald.service` are IN THE 09-19 PLAN, not
  files. Sysusers entries define `nostr-home-fetch` +
  `nostr-auth-greeter` — **no `nostr-smb-share`**
  (`packaging/sysusers.d/nostr-home-fetch.conf:25-28`,
  `nostr-auth-greeter.conf:22-24`).
- **The 09-19 plan §1331-1336 exactly proposes 6 files that don't exist
  yet** — Track 4 B1 ships them.

*Domain-controller-adjacent seams:*
- `SAMBA_AD_LOGIN.md:1-36` is explicit — it's a "configuration contract
  only." No Nostr→AD/Kerberos conversion. Samples are docs, not active
  policy.
- **NSS boundary refuses domain-qualified names**: `_nss_nostr_getpwnam_r`
  admits `n_` + `[a-z0-9_]` (`nss_nostr.c:82-113`,
  `identity_common.c:8-20`). `DOMAIN\user` and `user@REALM` are rejected.
  Candidate PAM policy at `packaging/pam/nostr-winbind-policy.md:1-23`
  reserves qualified names for `pam_winbind` but isn't an activatable file.
- No Kerberos in tree (`grep krb5` finds only a PAM comment).
- No custom idmap-backend scaffold.
- Validator at `packaging/domain/validate_domain_profile.py:94-118`
  only checks sample keys/ranges. Acceptance matrix at
  `tests/acceptance/matrix.json:126-158` reserves real winbind
  join/NSS/GDM/cache/outage cases.

**`nostrc-rb0e` tracker state** — epic open with 16 children: 2 closed
(`.15` arm64 dev loop, `.16` NDEBUG-vacuous tests), 4 in_progress (`.6`
D5 credential authority; `.7` D6 account/passdb/service; `.10` D9
build/package/rollback; `.12` D7 interoperability), 2 blocked (`.1` D0
acceptance lab; `.2` D1 standalone SMB contract approval), 8 open
(`.3` D2 matrix; `.4` D3 standalone Samba feasibility; `.5` D4 winbind
feasibility; `.8` D7 desktop acquisition; `.9` D8 winbind domain profile;
`.11` D10 integrated installed rollout; `.13` D8 joined-host acceptance;
`.14` D-arch acceptance architecture). Some notes have stale claims —
plan must not treat notes as ground truth without cross-checking.

### Auth-broker anchor points that all four tracks touch

- **`gnome/nostr-homed/docs/AUTH_PROTOCOL.md`** — FROZEN v1 PAM↔broker
  contract. Any signer/relay/DAV/SMB path that involves the broker MUST
  respect this.
- **`gnome/nostr-homed/docs/DESIGN.md`** — overall architecture.
- **`gnome/nostr-homed/docs/SYSTEMD_TOPOLOGY.md`** — unit + session-bus map.
- **`gnome/nostr-homed/docs/PAM_PROVIDER_UX.md`** — DRAFT, provider-choice
  flow.
- **Dep-purity gate** — `nostr-authd` + `pam_nostr.so` must not gain
  edges to libhanami/FUSE3/porthome/signer client libs.

### Portable-home epic ↔ this plan

The porthome epic (`nostrc-h10m`) has moved past this planning arc's
scope. As of `master@86aac4c3` all P2 code work is closed; only
`nostrc-e4v6` (ops nginx config) remains at P2. This plan touches
porthome only where a new track has to interoperate — specifically:
- The signer's `Nip44Encrypt/Decrypt` methods are already consumed by
  `auth_porthome`'s wrap-key path (via the L worker's `_ex` variants), so
  signer hardening must not break that (Track 1 D1's XML-doc-only change
  guarantees this).
- Track 4 B2's share-declaration approach materializes files via
  porthome/FUSE for `smbd` to read.

---

## Track 1 — Signer hardening (`nips/nip55l` + `apps/gnostr-signer`)

### 1.1 Current-state analysis

Per Background: real implementation, DBus name/interface `org.nostr.Signer`
at `/org/nostr/signer` (`nips/nip55l/dbus/org.nostr.Signer.xml:2-88`);
three known drifts (`SignEvent` return-type contradiction at
`org.nostr.Signer.xml:5-11` vs `README.md:66-67`; `GetRelays` placeholder at
`signer_ops.c:580-589`; unconfigured bindir literal at
`org.nostr.Signer.service.in:1-3`); the activation-file collision between
`nips/nip55l/CMakeLists.txt:102-109` and `apps/gnostr-signer/`; unwired
production paths in `main_app.c:285-345,571-615`; zero `nostr-authd` /
`pam_nostr` references in either tree, intentionally.

### 1.2 Design

**D1 — `SignEvent` return-contract change (verified: today returns a bare
signature; JSON dispatch is a real contract migration).** The plan's earlier
draft treated this as a documentation fix; a Phase-6 spot-check
disproved that. The dispatched implementation at
`nips/nip55l/src/glib/signer_service_g.c:193-196,249-256` calls
`nostr_nip55l_sign_event`, which returns `ev->sig` only
(`nips/nip55l/src/core/signer_ops.c:329-360`). A separate
`nostr_nip55l_sign_event_json` exists at `:363-379` but is **not** wired to
DBus. The in-tree mock also emits 128 hex characters
(`gnome/nostr-homed/tests/integration/mock_signer.c:3-10,55-63,76-83`).
So this is a **real semantic contract change**, even though the DBus
out-arg type stays `s`.

Decide between two migration paths:

- **D1.a (preferred, versioned semantic change):** switch
  `signer_service_g.c`'s DBus dispatch to `nostr_nip55l_sign_event_json`,
  update the XML doc-string + arg name from `signature` to `signed_event`,
  bump the nip55l component's semantic version marker (README + spec
  header), and update every in-tree consumer + the mock in the same PR.
  Consumers today: `apps/gnostr-signer/src/main_app.c`,
  `gnome/nostr-homed/tests/integration/mock_signer.c`, and any
  `nostr-dav` publisher (Track 2 D5) that gets built. Guarded by
  `test_sign_event_json` asserting id-recomputes-correctly + sig verifies.
- **D1.b (compatibility, less preferred):** add a new sibling method
  `SignEventJson` returning signed JSON, leave `SignEvent` returning the
  bare sig. Track 2 D5 targets `SignEventJson`. Downside: two methods
  returning different shapes for the same operation is a permanent API
  smell — accepted only if the maintainer wants stricter backward-compat.

Recommend D1.a. Reasoning: the plan's downstream consumers (Track 2
publish, Track 3B group admin) need id + pubkey + sig together for dedup
and relay-ACK correlation; JSON is what the README already promises;
version-and-migrate is one PR that flushes the ambiguity. The
"length-only acceptance" comparison to `pam_nostr` is dropped from the
rationale — a bare sig isn't intrinsically unsafe for an app-facing signer;
the pam_nostr defect was accepting it without cryptographic verification.

**Compatibility audit before merge (per Finding 20 preservation gap):**
audit exactly these named consumers — `nostr-dav` (Track 2 publish once
landed), the porthome `_ex` NIP-44 wrap-key path (unaffected — it uses
`Nip44Encrypt/Decrypt`, not `SignEvent`, but re-verify), and the mock
at `gnome/nostr-homed/tests/integration/mock_signer.c`. Fix each in the
same D1.a landing commit.

**D2 — `GetRelays` real implementation** (revised per Finding 7 —
don't invent an async refresh producer that doesn't exist). Replace the
`[]` placeholder at `signer_ops.c:580-589` with a read of the account's
**existing explicit per-user relay configuration**, falling back to
`org.nostr.Signer.Error.NotFound` when absent. Sources in order:
1. `$XDG_CONFIG_HOME/nostr/relays.conf` (a simple JSON list of relay URLs
   the user or an enrollment tool has configured — same shape as the
   09-19 plan's account-file relay hints).
2. If the account was enrolled through gnostr-signer's GUI, its account
   store already persists a relay set — read from there.
3. No kind-10002 NIP-65 network fetch at call time. If the maintainer
   later wants NIP-65 auto-refresh, that lands as a separate bead with
   an owned producer (subscription + persist) — DO NOT add a placeholder
   dependency now. The Track 3B notifier and Track 2 D4 must handle
   `NotFound` by falling back to their own configured relay sets (which
   they have — `home_relays` in auth.conf, or user-configured lists),
   NOT by treating it as a fatal error.

**D3 — Activation collision: `apps/gnostr-signer` owns
`org.nostr.Signer.service`.** Resolves scaffold Open Question 1. `nips/nip55l`'s
install rule becomes conditional on a new CMake option
`ENABLE_NIP55L_STANDALONE_ACTIVATION`, **default OFF**. Reasoning: (a) both
files claim the same bus name and install path — silent file conflict
between the two packages today; (b) gnostr-signer's daemon exports the
same `nips/nip55l` GLib implementation
(`apps/gnostr-signer/CMakeLists.txt:137-173`); (c) defaulting the flag OFF
means existing two-package installs converge to one owner with zero user
action. Tradeoff: a headless user who wants `nostr-signer-daemon` but not
GTK must build with the flag — documented in `nips/nip55l/README.md`.

**D4 — `@CMAKE_INSTALL_FULL_BINDIR@` literal.** Both trees currently ship
the literal because CMake copies rather than configures. Fix: rename the
gnostr-signer file to `.service.in`, switch both trees to
`configure_file(... @ONLY)` and install the **configured** output. Add a
post-install smoke test asserting the installed file contains no `@`.

**D5 — Wire the unwired production paths.**
- `bunker_service_start`: expose behind a `--bunker` daemon CLI flag and a
  corresponding `ExecStart` variant unit drop-in, so NIP-46 bunker mode is
  reachable without recompiling. Gate on non-empty bunker URI config; fail
  fast with a clear journal message otherwise.
- `sheet_qr_display_set_bunker_uri`: call it from the pairing flow when a
  bunker URI is configured, so the QR sheet actually displays the URI.
- `main_app.c:571-615` approval-signal subscription: the deferred DBus
  setup that installs the proxy never subscribes to the two approval
  signals the daemon emits. Subscribe in the deferred path's completion
  callback, matching the callback shape at `main_app.c:285-345`. Without
  this, approval requests emitted while the GUI is running silently never
  surface — hardening-relevant, not cosmetic.

**D6 — Signer↔broker boundary: they do not talk (SO_PEERCRED reference
fix — Finding 9).** The session signer is
**app-facing** (post-login, session bus, user-owned secrets); `nostr-authd`
is **pre-login authoritative** (system daemon, `auth.sock` 0600 /
`user.sock` 0666, SO_PEERCRED admission per
`gnome/nostr-homed/docs/AUTH_PROTOCOL.md:18-25`). No IPC edge in either
direction. Reasoning: (a) the signer's admission model is "same session
user, session-bus policy" — it has no pidfd/transaction machinery and must
not gain the broker's; (b) the broker's local-key provider signs
challenges in its own sandboxed worker with its own vault (09-19 §3.8) —
it must not depend on a user-session service that does not exist at the
greeter; (c) the one existing cross-consumer — `auth_porthome`'s
wrap-key path calling `Nip44Encrypt/Decrypt` — is a *porthome* consumer of
the signer, not an authd edge, and is preserved by D1's no-wire-break
guarantee. Document this boundary explicitly in `docs/dbus-interface.md`
so future contributors don't "helpfully" add a bridge.

### 1.3 File-by-file impact

| File | Change | Why |
|---|---|---|
| `nips/nip55l/dbus/org.nostr.Signer.xml` | Edit doc text at `:5-11` to specify signed-event JSON return | D1 |
| `nips/nip55l/README.md` | Document `GetRelays` refresh semantics + standalone-activation flag | D2, D3 |
| `nips/nip55l/src/core/signer_ops.c` | Implement `GetRelays` from account NIP-65 record (`:580-589`) | D2 |
| `nips/nip55l/dbus/org.nostr.Signer.service.in` | `configure_file(@ONLY)`; install configured output | D4 |
| `nips/nip55l/CMakeLists.txt` | Gate activation install behind `ENABLE_NIP55L_STANDALONE_ACTIVATION` (default OFF) at `:102-109` | D3, D4 |
| `NipOptions.cmake` | Add the new option near `:282-287` | D3 |
| `apps/gnostr-signer/data/org.nostr.Signer.service` → `.service.in` | Rename + `configure_file(@ONLY)` | D4 |
| `apps/gnostr-signer/CMakeLists.txt` | Install configured `.service`; assert single owner (`:290-303`) | D3, D4 |
| `apps/gnostr-signer/daemon/main_daemon.c` | `--bunker` flag → `bunker_service_start` (`:27-65`) | D5 |
| `apps/gnostr-signer/src/main_app.c` | Subscribe approval signals in deferred DBus setup (`:571-615`); call QR setter on bunker pairing (`:285-345`) | D5 |
| `apps/gnostr-signer/daemon/packaging/systemd/user/gnostr-signer-daemon.service` | Documented `--bunker` drop-in example (`:1-14`) | D5 |
| `nips/nip55l/tests/` (new: `test_sign_event_json.c`, `test_getrelays.c`, `test_nip44_binary.c`, fuzz harness) | Cover D1/D2 + beads `nostrc-p7f6`, `nostrc-3m86` | #4 |
| `docs/dbus-interface.md` | Add signer/broker no-bridge section | D6 |

No files deleted.

### 1.4 Errors + edge cases

- **Two daemons, one bus name** (stale packages post-split): the loser of
  name acquisition must log a distinct `org.nostr.Signer.Error.NameTaken`-style
  journal message and exit nonzero rather than sitting nameless. The user
  unit's `Restart=` policy must not flap-loop — add `StartLimitBurst`.
  Detection test: launch both binaries under a private session bus;
  exactly one owns the name.
- **Signer killed mid-`Nip44Decrypt` during the porthome wrap-key path**:
  the porthome `_ex` consumer treats a vanished name as transient and
  retries with backoff; the wrap-key cache flow must never treat a DBus
  `NoReply`/`NameHasNoOwner` as "key absent." Verification: kill -9 the
  daemon between request and reply; assert the consumer's cache state is
  unchanged and a retry after daemon restart succeeds. D1's full-event
  return is irrelevant here (encrypt/decrypt return ciphertext), so no
  wrap-key contract change.
- **`GetRelays` before first refresh**: returns `NotFound`; callers
  (Track 3B) must handle this as "use configured fallback relays," not an
  error.
- **Approval signal arrives with no GUI**: daemon-side timeout already
  exists conceptually in `ApproveRequest`; ensure the DBus method returns
  a typed denial on timeout rather than hanging the caller's transaction.

### 1.5 Tradeoffs

- Signed-JSON over bare signature: marginally larger payload, one JSON
  parse for sig-only callers — bought with a uniform, verifiable contract.
  Alternative (add a second method `SignEventFull`) rejected: two methods
  returning different shapes for the same operation is a permanent API smell.
- gnostr-signer owning activation: headless-minimal installs lose zero-config
  activation. Accepted because that audience builds from source anyway.
- No signer↔authd bridge: pre-login flows can't reuse session keys. That
  is the 09-19 architecture working as intended.

### 1.6 Risks

- Third-party clients may have coded against the XML's "signature" wording.
  Mitigation: the implementation already returns JSON per the README; the
  XML fix aligns docs with reality — audit in-tree consumers.
- `ENABLE_NIP55L_STANDALONE_ACTIVATION` default flip is a packaging-visible
  change; Debian/Fedora specs must drop the duplicate file simultaneously
  (§5.2).
- Fuzz harness additions must stay `-Werror`-clean on aarch64
  (`nostrc-rb0e.15` dev loop).

### 1.7 Verification

- **Unit**: `test_sign_event_json` (valid JSON, id == computed hash, sig
  verifies via `nostr_event_validate`); `test_getrelays` (populated →
  list; empty → `NotFound`; no network syscalls — assert via seccomp or
  interposed socket); `test_nip44_binary` (B64 round-trip with embedded
  NULs and 0xFF runs).
- **Integration** (reuse existing harnesses per Finding 18):
  `apps/gnostr-signer/tests/test-dbus.c:55-60,117-143` already sets up
  `GTestDBus` on a private bus; extend that infra for the D1.a migration
  test. `gnome/nostr-homed/tests/integration/mock_signer.c` provides a
  mock fixture — **update its `SignEvent` return** to a real signed-JSON
  fixture (the current 128-hex mock is evidence of the OLD contract and
  must migrate with D1.a). `run_e2e_real_signer_test.sh:39-48` shows the
  launch pattern. Exercise: valid signed-event round-trip; signer
  approval denial; `NameHasNoOwner` (daemon stopped); late reply after
  restart.
- **Collision**: install both packages into a `DESTDIR`; assert exactly
  one `org.nostr.Signer.service` and no `@` literal
  (`grep -R '@CMAKE_' _stage/`).
- **Dep purity**: see §5.1 — run the gate script after this track.

---

## Track 2 — Calendar/Contacts (`gnome/nostr-dav` Path A)

### 2.1 Current-state analysis

Per Background: Path A confirmed. Kinds 31922/31923/30085 recognized
(`nd-ical.h:16-18`, `nd-vcard.h:15-17`) but nothing subscribes or publishes;
persistence is ephemeral `GHashTable` (`nd-calendar-store.c:1-20`,
`nd-contact-store.c:1-20`); three release blockers — auth bypass
(`nd-dav-server.c:166-169`, `:1461-1470`; `nd-application.c:49-68`;
`nd-token-store.c:190-196`), non-loopback env override
(`nd-application.c:18-19`), no relay wiring; port drift
(`QUICKSTART.md:47-50` says 7654, code defaults 7680). 3645 LoC `.c`,
1763 LoC tests.

### 2.2 Design

**D1 — Auth: fail closed, token-first, single activation authority**
(revised per Finding 3). Two issues coupled here:

**a) Fail-closed startup.** Replace the null-account acceptance
(`nd-dav-server.c:1461-1470`: `if (self->account_id == NULL) return TRUE;`)
with unconditional validation. On first run: (1) generate a 256-bit
bearer token; (2) attempt libsecret storage; (3) **also** persist to
`$XDG_CONFIG_HOME/nostr-dav/token` mode 0600 (atomic create with `O_EXCL`,
fail if perms wrong on open); (4) configure the server's `account_id` and
token store **before** `soup_server_listen`. `nd-token-store.c:190-196`'s
accept-any behavior when libsecret is absent is replaced by file-backed
validation. Rationale: the current ordering (listen first, maybe configure
later, accept-all meanwhile) is the entire vulnerability.

**b) Single activation path.** Today the tree has BOTH a systemd user
unit (`gnome/nostr-dav/systemd/nostr-dav.service:12-18` with
`ProtectHome=read-only` + `ReadWritePaths=%h/.local/share/nostr-dav`) AND
a DBus activation file (`gnome/nostr-dav/systemd/org.nostr.Dav.service:1-3`)
launching the same binary outside that sandbox. `nd-application.c:151-158`
uses `G_APPLICATION_NON_UNIQUE`, which does not own the application ID
([GApplicationFlags docs](https://docs.gtk.org/gio/flags.ApplicationFlags.html)).
A lock-file is not a substitute. **Choose one authority**: the systemd
user unit owns the daemon; DBus activation converges by
`ExecStart=systemctl --user start nostr-dav.service` (or is dropped). The
unit's sandbox MUST be widened to permit token creation:
`ReadWritePaths=%h/.config/nostr-dav %h/.local/share/nostr-dav`. Both
activation routes then land in the same UID, same process instance, same
sandbox, same token-writable dirs. `G_APPLICATION_IS_SERVICE +
G_APPLICATION_ALLOW_REPLACEMENT` on the primary instance provides genuine
single-instance semantics; the lock-file added below is defense-in-depth
against a mis-configured install.

**D2 — Remove both env overrides (`NOSTR_DAV_ADDRESS` AND
`NOSTR_DAV_PORT`)** — the earlier draft only removed the address override;
Finding 19 flagged that `nd-application.c:57-67` also reads a port env,
which would keep runtime port drift possible even after fixing QUICKSTART.
Delete BOTH. Alternative considered (gate address behind
`NOSTR_DAV_ALLOW_LAN_BIND=1`) rejected: an
unauthenticated-then-bearer-token HTTP surface on a LAN with a token
stored in a world-readable-config-dir-adjacent location is not a surface
we want to offer even opt-in; anyone needing LAN DAV can run a reverse
proxy with TLS in front — documented recipe instead. Port: **7680**,
fixed at compile time.

**D3 — SQLite persistence + durable publish outbox** (revised per
Finding 6). Replace `GHashTable` stores with SQLite at
`~/.local/share/nostr-dav/store.sqlite` (WAL, `user_version` pragma,
created 0600). Schema:

- `events(uid PK, ical TEXT, etag TEXT, nostr_event_id TEXT, updated_at,
  publish_state TEXT DEFAULT 'idle', publish_attempts INT DEFAULT 0,
  publish_next_ts INT, signed_event_json TEXT)` — publish state machine
  co-lives with the row: `idle | pending | published | failed_permanent`.
- `contacts(uid PK, vcard TEXT, etag TEXT, nostr_event_id TEXT, updated_at,
  publish_state ..., signed_event_json TEXT)` — same shape.
- `files(path PK, content BLOB, mime TEXT, etag TEXT, nostr_event_id TEXT,
  modified_at, publish_state ..., signed_event_json TEXT)` — same shape.
- `publish_log(id PK, target_kind INT, target_uid TEXT, relay_url TEXT,
  attempted_at, http_status INT, error TEXT)` — audit + retry-scheduling
  history.

Keep existing store APIs (`nd_calendar_store_put/get/list_all`, ctag) as
thin SQL adapters so `nd-dav-server.c`'s handlers don't change shape.
ctag becomes a monotonically increasing `store_generation` counter bumped
in the same transaction as any mutation — stable across restart, unlike
a hash of an in-memory table. Signed-event JSON is stored in the row
(not just the id) so retries don't need to re-sign — reduces signer
round-trips and lets the queue drain while the signer is asleep.

**D4 — Relay subscribe (inbound) with tombstone kind + policy-controlled
fallback** (revised per Findings 8 + 11). New `NdRelaySync` object: on
startup and on account change, open REQ subscriptions to the account's
home_relays (resolved via the same chain,
`gnome/nostr-homed/config/auth.conf.sample:69-79`) with filters
`{kinds:[31922,31923,30085,5], authors:[account_pubkey]}` plus `#p`-tagged
addressed variants where the NIP allows. **Kind 5 is required** — without
it `test_store_sqlite`'s NIP-09 tombstone-deletion assertion can never
fire. On EVENT: `nostr_event_validate` → for kinds 31922/31923/30085,
convert (`nd_ical`/`nd_vcard` parsers reversed) → upsert keyed by
`(kind, pubkey, d-tag)` for addressable kinds; for kind 5, resolve
`e`/`a`-tag targets and delete the matching addressable rows.
Replaceable/parameterized-replaceable semantics: keep newest `created_at`
per `(kind, pubkey, d-tag)`. Deduplicate by event id.

**Fallback policy** (Finding 11 — this is a security/privacy decision,
not just resilience): a new auth.conf-adjacent key
`nostr_dav_upstream_mode = session_relay_only | session_relay_or_direct |
direct_only`, default `session_relay_or_direct` (v1). `session_relay_only`
refuses to fall back to home_relays if the session relay is absent —
suitable for local-only/privacy configurations. Test all three modes
plus "session relay temporarily down" (retry with backoff, do not fall
back until backoff exhausted OR mode permits).

**D5 — Publish (outbound) via the Track-1 signer — offline-first with
durable outbox** (revised per Finding 6 to resolve the plan's
self-contradiction). PUT/DELETE handlers today write only the local
store (`nd-dav-server.c:559-612` region). New flow:

1. Stage the mutation in SQLite inside a transaction that also inserts a
   `publish_state='pending'` row (or updates the existing one).
2. DAV client sees the local write immediately (return 201/204). This is
   the **offline-first contract**; there is no "rollback on relay
   rejection" — that path was self-contradictory in the earlier draft.
3. Async publisher worker picks up `publish_state='pending'` rows:
   (a) call the Track-1 signer's `SignEventJson` (or the migrated
   `SignEvent` per D1.a); (b) publish to the row's target relay set
   (§D4 defines routing per event kind); (c) on ≥1 relay OK, transition
   to `published`; (d) on transient failure, bump `publish_attempts`,
   schedule next attempt with exponential backoff (60 s → 60 min max),
   log to `publish_log`; (e) on permanent failure (e.g. signer denied by
   user, malformed event), transition to `failed_permanent` and surface a
   single deduped `GNotification`. `pending` rows survive daemon restart.
5. **Commit definition (revised per maintainer Q4 answer — NIP-51/65 outbox model):** target relays for a given event come from the account's kind-10002 (NIP-65) list, filtering for entries marked `write` (or unmarked, which is bidirectional per NIP-65). For event-classes that NIP-51 lists override (e.g. a per-kind NIP-51 list explicitly names a relay set), honor that list. A row transitions to `published` ONLY when EVERY relay in the resolved target set has ACK'd; anything less keeps the row `pending` and subject to retry. Rationale: this matches the outbox model's promise that a client's followers see the event on every relay the author declared for that content; ≥1-OK is a placeholder that silently drops the guarantee. Failure classes: (a) one relay permanently rejects → row goes `failed_permanent` (deduped `GNotification`); (b) transient outage of some relays → row stays `pending`, retries with exponential backoff. Operator override: `nostr_dav_publish_quorum = <int>|all` in config (default `all`) for the rare case where the outbox is misconfigured. Store the resolved target list in the `publish_log` rows so retries after an outbox change use the original target for auditability.
5. Local store remains authoritative for DAV reads (offline-first);
   `nostr_event_id` column links local row ↔ published event.

Tradeoff: DAV clients see writes locally while the relay copy lags —
matches CalDAV eventual-consistency expectations, but a signer-denial
leaves the mutation locally-only in `failed_permanent` state. Documented
in QUICKSTART with recovery recipe.

**D6 — End-user setup (zero custom UI).** Settings → Online Accounts →
**WebDAV** (stock GNOME 46): Server `http://127.0.0.1:7680`, Path `/`,
Username `nostr`, Password = contents of `$XDG_CONFIG_HOME/nostr-dav/token`
(displayed by a `nostr-dav --show-credentials` CLI helper, never logged).
Calendar appears in GNOME Calendar; contacts in GNOME Contacts via the
same account. QUICKSTART rewritten around this flow; the systemd user unit
(`gnome/nostr-dav/systemd/nostr-dav.service:1-26`) fixed to use the real
installed path (`/usr/bin/nostr-dav` via `%P` or configured bindir) instead
of hard-coded `%h/.local/bin/nostr-dav`.

**D7 — Functional tradeoffs stated (Path B advantages we give up):**
lower-latency change propagation (DAV refresh cycle vs direct
`notify_update`); native `evolution-alarm-notify` triggers and Contacts
autocomplete without HTTP traversal; ability to expose Nostr-native
semantics (relay-of-origin, signatures, gift-wrap) that flatten into iCal
properties; more natural in-EDS notifications. Mitigations:
ctag-bump-on-mutation keeps refresh cheap; alarm/notification gaps are
covered by Track 3B for messages (calendar alarms remain EDS-local against
the DAV source, which works).

### 2.3 File-by-file impact

| File | Change | Why |
|---|---|---|
| `gnome/nostr-dav/src/nd-application.c` | Reorder startup (token → account → listen); delete `NOSTR_DAV_ADDRESS`; fix port 7680 (`:18-19,49-68`) | D1, D2 |
| `gnome/nostr-dav/src/nd-dav-server.c` | Remove null-account acceptance (`:1461-1470`); require configured `account_id` before `nd_dav_server_start` succeeds; PUT/DELETE call publish pipeline (`:559-612`) | D1, D5 |
| `gnome/nostr-dav/src/nd-token-store.c` | File-backed token validation replacing accept-any (`:190-196`) | D1 |
| `gnome/nostr-dav/src/nd-calendar-store.c`, `nd-contact-store.c`, `nd-file-store.c` | SQLite backends behind existing APIs (`:1-20` each) | D3 |
| `gnome/nostr-dav/src/nd-relay-sync.c` + `include/nd-relay-sync.h` (new) | Subscribe/convert/upsert; publish-with-retry; pending_publish queue | D4, D5 |
| `gnome/nostr-dav/src/nd-signer-bridge.c` + header (new) | `org.nostr.Signer` proxy, `SignEvent` → JSON parse → publish | D5 |
| `gnome/nostr-dav/src/main.c` (or equivalent) | `--show-credentials` helper | D6 |
| `gnome/nostr-dav/systemd/nostr-dav.service` | Real install path; `Wants=`/ordering after signer | D6 |
| `gnome/nostr-dav/docs/QUICKSTART.md` | Port 7680; GNOME 46 WebDAV dialog walkthrough (`:47-50`) | D2, D6 |
| `gnome/nostr-dav/CMakeLists.txt` | Add new sources; link sqlite3; register new tests (`:16-42`) | D3–D5 |
| Tests: `test_store_sqlite.c`, `test_auth_required.c`, `test_relay_sync.c`, `test_publish_rollback.c` (new) | See §2.7 | — |

### 2.4 Errors + edge cases

- **Token file perms wrong** (not 0600, or dir group-writable): refuse to
  start with a journal message naming the path — never chmod-and-continue
  silently.
- **Signer absent at PUT time**: stage locally, queue publish, notify
  once (deduped) — the DAV write itself still succeeds (201/204) so clients
  don't wedge.
- **Conflicting remote replaceable event arrives with newer `created_at`
  after a local edit**: last-writer-wins by `created_at` with local staged
  row marked `superseded`; surfaced in logs. Documented limitation (NIP-52
  has no CRDT).
- **Replayed old event**: rejected by `created_at` comparison + id dedup.
- **SQLite corruption**: open fails → rename to `store.sqlite.corrupt-<ts>`,
  start empty, re-sync from relays (relays are the durable copy; the
  SQLite file is a cache + pending queue).
- **Two nostr-dav instances** (user error): lock-file in
  `$XDG_RUNTIME_DIR/nostr-dav.lock`; second instance exits with clear
  message rather than fighting over the DB and port.

### 2.5 Tradeoffs

Loopback-only removes legitimate LAN-tablet use; offset by a documented
reverse-proxy recipe. Offline-first local-authoritative store means DAV
clients can briefly see unpublished state; offset by ctag discipline and
pending-publish surfacing. SQLite over nostrdb: zero new heavy deps in a
GNOME service; nostrdb's Lmdb model is overkill for per-user calendar
volumes.

### 2.6 Risks

Track-1 coupling: if the signer DBus contract slips, D5 slips — mitigated
by the publish queue being independently testable against a mock signer.
EDS DAV client quirks (aggressive PROPFIND depth-infinity) —
`get_depth_header` already clamps infinity→1; add REPORT pagination if
profiling shows need. GNOME Contacts' CardDAV support is weaker than
Calendar's — acceptance must include an actual Contacts round-trip, not
just Calendar.

### 2.7 Verification

- `test_auth_required`: no `account_id` configured → server refuses to
  start; wrong token → 401; OPTIONS/well-known remain unauthenticated
  (per current dispatcher design) but expose nothing mutable.
- `test_store_sqlite`: put → daemon restart → get returns row; ctag
  strictly increases across restart; NIP-09 tombstone deletes.
- `test_relay_sync`: controlled relay fixture (deterministic EVENT/EOSE)
  feeds 31922/31923/30085; assert store contents + dedup.
- `test_publish_rollback`: mock signer approves, relay rejects →
  `pending_publish` set, notification emitted once; retry success clears it.
- Integration harness: `run_dav_e2e.sh` — real `curl` PROPFIND/PUT/REPORT
  cycle against a spawned daemon with fixture relay + mock signer.
- GNOME acceptance: Calendar + Contacts against the WebDAV account on the
  smoke lab (§5.3).
- Dep purity gate (§5.1): nostr-dav **may** depend on signer client libs;
  authd/pam must not gain any new edge from this track — verify.

---

## Track 3 — Relay: session-local + shell notifications

### 3.1 Current-state analysis

Per Background: two server binaries exist, neither session-scoped
(`apps/relayd/CMakeLists.txt:25-56`, `apps/grelay/CMakeLists.txt:22-33`);
`nostrc-relayd` is a system unit (`apps/relayd/systemd/nostr-relayd.service.in:1-20`);
**bind bug** at `relayd_main.c:381-408` (port extracted, `info.iface` never
set); `grelay_main.c:840-855` defaults `0.0.0.0:4849`; NIP-42 gating
(`protocol_nip01.c:164-169,275-281`) is Nostr-key auth, not session-user
auth; `libnostr`'s `NostrRelay` is client-only (`nostr-relay.h:25-35`).
Prior art for the SOCKET permission pattern (0600 + UDS + unlink stale):
`gnostr-signer-daemon` at `apps/gnostr-signer/daemon/ipc.c:199-221`. Prior
art for **SO_PEERCRED admission**: `gnome/nostr-homed/src/auth/auth_peer.c:52-58`
and `auth_boundary.c:127-145` (NOT `ipc.c`, which only does 0600+unlink;
SO_PEERCRED is not in that file — corrected per critique Finding 9). Piece B:
NIP-29 consumer is in-GNostr only (`gn-nip29-group-service.c:919-955,1091-1168`,
gated `ENABLE_NIP29=OFF` at `NipOptions.cmake:123-126`); DM subscription at
`gnostr-dm-service.c:222-266`; badge path passes outer gift-wrap pubkey
only (`badge_manager.c:589-624,1584-1615`); **no standalone background
notifier exists anywhere in the tree**.

### 3.2 Design — Piece A: session-scoped local relay

**D1 — Fix the bind bug first.** `apps/relayd/src/relayd_main.c:381-408`
must parse `cfg.listen` into host + port and set `info.iface` (host string)
alongside `info.port`. Reject non-`host:port` forms at config load.
Regression test: start with `listen = "127.0.0.1:4848"`, assert `ss -ltn`
shows only loopback; start with an explicit `0.0.0.0` config and assert it
*does* bind wide (proving the config is now actually honored, not just
clamped).

**D2 — Factor `libnostr-relay-server`.** Extract the reusable server core —
`relayd_core`, `relay_security`, protocol handlers (`protocol_nip01.c`,
NIP-11/45/50), storage driver abstraction, rate limiting (`rate_limit`,
`security_limits*`), `relay_policy`, verification budget — into a **static**
library `libnostr-relay-server`. The bind/listen surface becomes injectable:
the library accepts a "listener spec" (TCP host:port **or** a pre-bound
Unix-socket fd) so both consumers share everything except the listener and
policy defaults. Static, not shared: one repo, two in-tree consumers, no
ABI burden. `nostrc-relayd` keeps its system role and TCP listener;
behavior unchanged post-refactor except the D1 fix.

**D3 — `nostr-session-relayd` with a single socket owner + explicit
teardown** (revised per Findings 9 + 10). New small `main()`:

1. **`sd_listen_fds` first (systemd socket-activated path is authoritative).**
   The `.socket` unit owns creation, mode, and unlink; the daemon must
   NEVER unlink a live systemd-owned socket. Fallback bind (for
   headless/manual launch): create `$XDG_RUNTIME_DIR/nostr/`, `bind()`
   `relay.sock`, `fchmod(0600)` — mode set on the socket inode itself,
   not just umask (pattern from `ipc.c:199-221`). Stale-socket recovery
   only in fallback mode: probe for a live listener, unlink then retry
   once, else exit nonzero.
2. On each accepted connection, **`SO_PEERCRED`** → require `peercred.uid
   == getuid()`, else `shutdown()` + `close()` before reading a byte.
   Real in-tree SO_PEERCRED prior art: `gnome/nostr-homed/src/auth/auth_peer.c:52-58`
   or `auth_boundary.c:127-145`. UID-equality (not root-or-user) is right
   because this socket is single-user by construction — the 0600 +
   `$XDG_RUNTIME_DIR` dir perms are the primary gate, SO_PEERCRED is
   defense-in-depth against a co-UID-process-confusion mistake (per
   critique Finding 9's caveat that 0600 + same-UID does NOT stop a
   malicious process running as that same UID; only DBus-policy /
   AppArmor-profile layering can).
3. Hand the fd into `libnostr-relay-server`.

**Storage**: per-user nostrdb under `~/.local/share/nostr/session-relay/`,
with a retention cap (default 7 days / 256 MiB, config-file only, no env
overrides). NIP-42: **off** by default — the socket is the auth; enabling
NIP-42 on it is a config option for shared-machine paranoia.

**Shutdown / logout** (Finding 10): the session relay is a graphical-
session service. Unit: `PartOf=graphical-session.target`, so logout stops
it even under lingering user managers. On `SIGTERM`: drain upstream
subscriptions with a bounded timeout (default 5 s), flush queued writes
from the outbox to whatever upstreams accept in that window, log the
residual queue depth, close listener, exit. Fallback-bound (non-socket-unit)
process unlinks the socket on exit; socket-unit-owned launches leave the
socket alone. Restart test: kill -9 → next start recovers from stale
socket in fallback mode, refuses in socket-unit mode until the socket
unit is restarted (which handles the unlink).

**D4 — Federation policy — event-class routing table** (revised per
Finding 5 — the earlier "push upstream everything the user signs"
default was wrong for NIP-29 group events and NIP-17 DMs). The session
relay is a cache-and-queue, not an island. Routing is table-driven, per
event class:

| Event class | Read upstream | Write-back target | Rationale |
|---|---|---|---|
| Kinds 31922 / 31923 / 30085 (Track 2 addressables) | `home_relays` | `home_relays` (all) | Normal user publish, per NIP-65 |
| Kind 5 (NIP-09 tombstones) | `home_relays` | `home_relays` | Same as target replaceable |
| Kinds 9-12 (NIP-29 group messages) | Group's recorded relay URL only (per `gn-nip29-group-service.c:1026-1033`) | Same group relay only | NIP-29 requires `h` tag + originating relay; MUST NOT fan out to home_relays |
| Kind 39000-39004 (group metadata) | Group's recorded relay | READ-ONLY (metadata is authored by group admins) | Consumer cache |
| Kind 1059 (NIP-17 gift-wrap DMs, inbound) | Recipient's kind-10050 inbox relays only | N/A inbound | Per [NIP-17](https://github.com/nostr-protocol/nips/blob/master/17.md); NIP-42 recommended on inbox relays |
| Kind 14/1059 (NIP-17 outbound wraps) | N/A | Recipient's kind-10050 inbox relays | Follow NIP-17's routing, NOT sender's home_relays |
| All other kinds | `home_relays` | `home_relays` | Default |

The cache preserves **relay-of-origin** per stored event where
authorization depends on it (group events, gift wraps). Access-control:
local REQ from the session's own UID may read any locally-cached event
without re-authing to upstream. Upstream connections cache NIP-42 challenge
state; DAV / notifier fallback to direct upstream must reuse the same
auth cache to avoid double-challenging.

Store-and-forward with reconnect backoff for writes. No negentropy sync
in v1 (NIP-77 tracked separately).

**D5 — Systemd user unit.** `gnome/nostr-homed/systemd/user/nostr-session-relay.service`
+ `.socket`: socket unit owns `relay.sock` creation
(`ListenStream=%t/nostr/relay.sock`, `SocketMode=0600` — letting systemd
set the mode declaratively), service is `Type=notify`, sandboxed
(`NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome=read-only` except
a `ReadWritePaths=` for the storage dir, `RestrictAddressFamilies=AF_UNIX
AF_INET AF_INET6`). Placement under `gnome/nostr-homed/systemd/user/`
because this is session-desktop infrastructure, matching the signer's
user-unit packaging pattern (`gnostr-signer-daemon.service:1-14`).

### 3.3 Design — Piece B: NIP-29 + DM notification daemon

**D1 — New binary `nostr-notify-daemon`, separate from
gnostr-signer-daemon.** Reasoning: (a) the signer daemon is a security
surface holding key material — adding relay networking and notification
parsing widens its blast radius for zero benefit; (b) lifecycle mismatch:
the signer may be DBus-activated transiently, the notifier must run for
the whole session; (c) the goa-postmortem lesson (`:48-52` — don't
fork/overload desktop-service daemons) applies directly. The notifier
has **no secret access**; it needs no signing for v1 (read-only
subscriptions).

**D2 — Subscriptions via Piece A.** Connect to
`$XDG_RUNTIME_DIR/nostr/relay.sock` (fallback: home_relays directly, same
as nostr-dav's D4 policy). Filters: `{kinds:[9,10,11,12]}` for joined
NIP-29 groups (group set discovered from kind 39000-39003 metadata the
user has cached, matching the merge logic in
`gn-nip29-group-service.c:919-955` conceptually, reimplemented minimally —
do **not** link GNostr plugin code); `{kinds:[1059], "#p":[user_pubkey]}`
matching the DM subscription shape at `gnostr-dm-service.c:222-266`.
Group kinds 39005-39009 are not defined in-tree
(`nostr-kinds.h:14-19,181-186`) — subscribe only to defined kinds; note
the gap in an upstream-NIP-tracking bead (not blocking).

**D3 — Preview limitation honored — DMs are opaque** (revised per Finding
14). Kind-1059 gift wrap uses a **fresh random wrapper key per message**
([NIP-17](https://github.com/nostr-protocol/nips/blob/master/17.md)) —
the outer pubkey is NOT the sender; the notifier cannot derive sender
or thread ID without unwrapping. Corrected notification body:
**title "Nostr message"**, **body "You have a new encrypted direct message."**
— fully opaque. The default `GNotification` action passes only the outer
gift-wrap event ID to GNostr, which unwraps on click and routes to the
correct thread. For NIP-29 group messages (kinds 9-12 are plaintext),
title is the group's display name (from cached 39000-39001 metadata),
body is a truncated content preview (first 80 chars, markup-escaped).
No private-key unwrap in the notifier — that's GNostr's job.

**D4 — GNotification + activation — own GApplication ID, explicit GNostr
deep-link contract** (revised per Finding 4). A `GNotification` action
belongs to the sending `GApplication`; the notifier CANNOT share GNostr's
ID (`org.gnostr.Client`, per `apps/gnostr/src/main_app.c:337`) and
simultaneously be suppressed while GNostr owns it. Corrected design:

- Notifier registers under its own ID `org.nostr.NotifyDaemon`.
- Notification default action is `app.open-in-gnostr` with a target
  string carrying a **deep-link URI** the notifier and GNostr agree on:
  - DM: `nostr://open?event=<hex64_giftwrap_event_id>` — GNostr unwraps
    and routes to the correct thread.
  - Group message: `nostr://open?group=<h_tag>&event=<hex64>`.
- Activating this action requires the notifier to dispatch GNostr; two
  mechanisms available: (a) `Gio.DesktopAppInfo.launch_uris()` with the
  `nostr://` scheme registered in GNostr's `.desktop` file (preferred —
  works whether GNostr is running or not); (b) DBus call to
  `org.gnostr.Client` if GNostr is running (fallback for uri-handler-
  disabled installs). This is a **new receiving action GNostr must add**
  in its own commit; its current actions are `notify-open`,
  `notify-mark-read`, `notify-reply` per `desktop_notify.c:234-244,479-503`
  — none of which take an event-id deep link.
- Category `im.received` / `x-nostr.group` for shell hinting.
- Withdraw-on-read: notifier watches GNostr focus events via DBus (or
  session bus name-owner-changed); on GNostr focus of a matching thread,
  withdraw the notification id. In v1, coarser: withdraw all notifier
  IDs on any GNostr focus event (implementation-simple, occasional
  false-clear acceptable). Refine in a follow-up bead.

**D5 — Suppression when GNostr runs — generation guard cancels in-flight**
(revised per Finding 12 — "subscribes to nothing" is not sufficient; an
EVENT callback, metadata lookup, coalescing timer, or `send_notification`
may already be in flight when GNostr acquires its name).

Design:
- Notifier watches the session bus for `org.gnostr.Client`. On
  name-appear: (a) increment a monotonic `generation` counter guarded by
  a mutex; (b) close all upstream REQ subscriptions immediately; (c)
  cancel all pending grace/coalesce timers; (d) `withdraw` all
  outstanding notification IDs (or transfer them to GNostr via
  `org.gnostr.Client` if a DBus method for that exists — v1 just
  withdraws); (e) any in-flight callback (event validate, metadata
  lookup) checks `generation` at every yield boundary and its final
  `send_notification` call — mismatch → drop silently.
- On name-vanish: (a) increment `generation`; (b) start a 30 s grace
  timer; (c) on grace expiry, open subscriptions from a persisted
  cursor with a **catch-up limit**: skip events with `created_at`
  earlier than the cursor OR older than 10 min (whichever is stricter)
  to avoid a notification flood. The cursor persists in
  `~/.local/state/nostr-notify/cursor` (0600, atomic tmp+rename)
  updated on each successful notification send.
- Tests must exercise: name-appear during an in-flight `send_notification`
  (verify the notification is either delivered before the guard fires or
  suppressed after — never both); name-vanish → catch-up with old-event
  suppression.

**D6 — Unit.** `gnome/nostr-homed/systemd/user/nostr-notify.service`:
`After=nostr-session-relay.service`, sandboxed with network access (it
needs upstream relays when session relay absent), no keyring access,
`MemoryMax=` modest.

### 3.4 Errors + edge cases

- **relay.sock exists but stale** (unclean exit without socket unit):
  bind fails EADDRINUSE → verify no listener via SO_PEERCRED-less connect
  probe, unlink, retry once, else exit nonzero.
- **Co-UID malicious process** spamming the socket: per-connection rate
  limits from the shared `relay_security` code apply unchanged (that's
  why the factoring includes it).
- **Session relay upstream unreachable**: serve local store, queue writes,
  exponential backoff capped at 5 min; notifier keeps working against the
  local store.
- **Notification flood** (group catch-up sync): suppress notifications for
  events older than daemon start minus 10 min; per-thread coalescing (one
  notification per thread, updated count) instead of one per message.
- **User has no Nostr account configured**: notifier idles, posts nothing,
  exits cleanly on `NotFound` from the signer's `GetPublicKey`.

### 3.5 File-by-file impact + verification

| File | Change | Why |
|---|---|---|
| `apps/relayd/src/relayd_main.c` | Parse + set `info.iface` (`:381-408`); slim to thin main over the lib | D1, D2 |
| `apps/relayd/relay.toml.example` | Document enforced interface semantics (`:4-16`) | D1 |
| `apps/relayd/CMakeLists.txt` | Build `nostr-relay-server` static lib from `relayd_core`/`relay_security`/protocol sources (`:25-56`) | D2 |
| New `apps/relayd/include/nostr-relay-server.h` | Listener-spec + policy injection API | D2, D3 |
| New `apps/relayd/src/session/relayd_session_main.c` | Unix-socket main, SO_PEERCRED, sd_listen_fds | D3 |
| New `gnome/nostr-homed/systemd/user/nostr-session-relay.{service,socket}` | D5 | D5 |
| New `apps/relayd/src/session/` or `gnome/nostr-homed/src/notify/` — `nostr-notify-daemon.c`, `notify_subs.c`, `notify_gnotification.c` | Piece B | 3.3 |
| New `gnome/nostr-homed/systemd/user/nostr-notify.service` | 3.3 D6 | — |
| `NipOptions.cmake` | No change to `ENABLE_NIP29` default; notifier doesn't reuse the plugin | — |
| Tests: `test_bind_iface.c`, `test_session_relay_peercred.c`, `test_notify_sub.c`, `run_notify_e2e.sh` | Below | — |

Verification:
- `test_bind_iface`: the `ss`-based loopback/wide assertions (D1).
- `test_session_relay_peercred`: socket 0600 on disk; wrong-UID connection
  (via setuid helper in a VM/container test) closed pre-read; right-UID
  round-trips NIP-01 REQ/EVENT.
- Socket-activation path: `systemd --user` transient unit test asserting
  fd handoff.
- `test_notify_sub`: fixture relay replays kinds 9-12 + 1059; assert one
  coalesced GNotification per thread, 1059 body contains hex8 only (assert
  no cleartext leak), group messages preview-truncated ≤80 chars.
- Suppression: harness owns GNostr's bus name → notifier silent; release
  → notifier resumes after grace.
- Dep purity gate: session relay and notifier are user-session code;
  assert no new authd/pam edges (§5.1).

---

## Track 4 — Samba three-way split

### 4.1 Dimension (a) — client polish

Current seams per Background: single-mount
`nostr-smb-mount.sh:16-29,40-54,165-176,259-266`; orphaned sweep
(`smb_credential.h:120-137,165-174`; `smb_credential.c:327-363,553`);
teardown gap (`pam_nostr_broker.c:634-637`; `pam_nostr.c:254-266`); no
browse hook.

- **A1 — Batch mounts:** `nostr-smb-mount --batch` reads
  `/etc/nostr-auth/servers.d/*.conf` (the desktop enrollment files from
  the 09-19 plan's config contract) and mounts each declared share
  sequentially, aggregating failures into an exit bitmap rather than
  aborting at the first. Per-share mount continues to use the existing
  `--mode gvfs` path.
- **A2 — Discovery hook:** ship a `gvfsd-smb-browse`-adjacent helper:
  a `nostr-smb-browse` CLI that lists shares from enrolled `servers.d`
  entries (from each server's config, not network browsing — enrolled
  servers only), plus a `.desktop`/autostart integration that pre-populates
  GNOME Files' "Other Locations" via GVfs bookmarks for enrolled shares.
  No network-neighborhood scanning (that's unauthenticated surface).
- **A3 — Wire the sweep:** add a supervisor timer in the authd SMB
  authority path (or a `nostr-smb-sweep.timer` user/system unit — recommend
  a **systemd timer**, not an in-process thread, to keep authd's main loop
  untouched per dep-purity conservatism) invoking `nh_smb_authority_sweep_expired`
  through a small `nostr-authctl smb-sweep` subcommand. Also fix the
  header-comment drift (`smb_credential.h:120-137` says `open()` revokes
  expired; make it true: `nh_smb_authority_open` calls the sweep once at
  open — cheap, idempotent, and closes the integration-test-only gap).
- **A4 — Logout teardown, per-user service delegation** (revised per
  Finding 13 — a privileged PAM close hook cannot fork the GVfs mount
  helper because `--mode gvfs` explicitly requires the calling desktop
  user's session bus, per `nostr-smb-mount.sh:16-29,40-71`, and the
  current CLI only supports `--unmount MOUNTPOINT` singular, not
  `--all`). Corrected design: introduce a per-user teardown service
  `nostr-smb-teardown.service` (systemd `--user` unit) that owns a
  mount-ledger file at `$XDG_STATE_HOME/nostr-smb/mounts.json` (mounts
  that were opened via `nostr-smb-mount --mode gvfs` register themselves
  here). `pam_sm_close_session` in `pam_nostr_broker.c:634-637` uses
  `systemctl --user --machine=<uid>@ start nostr-smb-teardown.service`
  (or the equivalent user-scope DBus activation) to request the teardown
  — the actual `gio mount --unmount` runs in the user's session context
  where the session bus lives. Timeout-bounded; failures logged, not
  fatal; session close returns success even if teardown times out. Extend
  `nostr-smb-mount.sh` with a real `--unmount --all` that iterates the
  ledger. Multiple concurrent sessions per UID: the ledger has
  session-id keys so the teardown only touches THIS session's mounts.
  The sweep (A3) is the backstop for credential hygiene.

### 4.2 Dimension (b) — server-side file sharing

**B1 — Ship five files (revised per Finding 21 — don't ship a dead unit).**
The 09-19 plan proposed 6 files; entry #4 (`nostr-smb-credentiald.service`)
is dropped from the v1 ship set because there is no v1 executable behind
it — installing a dead unit is packaging litter. The rename-and-lift
target name is instead RESERVED in `SAMBA_STANDALONE.md` §Future so the
name is claimed but no artifact ships until the split daemon exists.

1. `gnome/nostr-homed/config/smb-credentiald.conf.sample` — server
   authority + expiry policy (named for forward-compat even though v1
   hosts the authority in authd, see B4).
2. `gnome/nostr-homed/config/smb.conf.standalone.sample` — dedicated
   standalone Samba config (`security = user`, `tdbsam`, SMB2/3 only,
   signing required, no guest).
3. `gnome/nostr-homed/config/servers.d/example.conf` — desktop-enrolled
   server identity/policy.
4. `gnome/nostr-homed/systemd/nostr-smbd.service` — dedicated Samba
   instance bound to supervisor readiness.
5. `gnome/nostr-homed/docs/SAMBA_STANDALONE.md` — mediation contract,
   caching/rotation/active-session semantics + reserved-name note for
   the future `nostr-smb-credentiald` unit.

**B2 — VFS plugin vs share-declaration: choose SHARE-DECLARATION for v1.**
Resolves scaffold Open Question 4. `smbd` reads from a directory
materialized by the existing porthome/FUSE sync path
(`docs/designs/home-from-relay.md` consumer), declared as a normal share
in `smb.conf.standalone.sample`. Reasoning: (a) a Samba VFS `.so` runs
**in-process in smbd** — a crash or leak in our porthome/Blossom
chunk-fetch code takes down file serving for every client, and the VFS
ABI must track the pinned Samba 4.19 per-distro (the same coupling
argument that killed Path B in Track 2); (b) share-declaration needs no
new code executing as the smbd user beyond what already exists — the
mount-point owner exports, Samba reads; failure isolation is total (mount
dies → share errors, smbd lives); (c) VFS's real win (on-demand fetch
instead of full materialization, no FUSE in the serving path) is a
performance/robustness upgrade, not a v1 correctness requirement.
**VFS plugin filed as a follow-up bead** with the acceptance criteria:
per-share opt-in, smbd crash-budget tests, pinned-ABI build matrix.
Tradeoff accepted: v1 requires the whole shared tree materialized on the
server (disk cost, sync lag).

**B3 — Journal↔passdb reconciliation** (verified against Samba man pages
+ current adapter). `smb.db` (`nostr-authd.c:190-205`) is the issuance
journal; `passdb_tdbsam.c:220-245` today already invokes `smbpasswd -a -s
USERNAME` — but here `-s` is **silent-stdin mode**, NOT a config selector
(per the [smbpasswd manpage](https://www.samba.org/samba/docs/current/man-html/smbpasswd.8.html)).
The file's own inline comment correctly identifies `-s` as silent.
At `:249-275` `smbpasswd -d` and `pdbedit -x -u` run with NO dedicated
config, so today's interop depends on Samba ambient defaults — the exact
posture the 09-19 plan forbade.

Correct fix (using Samba's actual flag semantics):
- `smbpasswd`: config selector is **`-c <config>`**. Silent-stdin `-s`
  stays. So invocations become `smbpasswd -c <smb.conf.standalone> -a -s
  USERNAME` (add), `smbpasswd -c <smb.conf.standalone> -d USERNAME`
  (disable), etc.
- `pdbedit`: config selector is **`-s <config>`** (see the
  [pdbedit manpage](https://www.samba.org/samba/docs/4.9/man-html/pdbedit.8.html)).
  Invocations become `pdbedit -s <smb.conf.standalone> -x -u USERNAME`,
  `pdbedit -s <smb.conf.standalone> -L`, etc.
- **Reconciliation pass**: on authority open, run
  `pdbedit -L -s <smb.conf.standalone>` to enumerate the dedicated
  passdb, diff against the SQLite journal (`smb.db`), and refuse
  readiness on drift (unknown accounts, disabled-state mismatch),
  entering `RECONCILE_REQUIRED` per the 09-19 state machine rather than
  auto-repairing.
- **Test**: `test_passdb_explicit_config.sh` uses strace/argv-capture to
  assert `smbpasswd` invocations carry `-c <conf>` (not `-s`, which is
  present but for a different reason) and `pdbedit` invocations carry
  `-s <conf>` for add / disable / remove / reconciliation. Argv-exactness
  matters here.

**B4 — Authority split: keep in `nostr-authd` for v1.** Resolves scaffold
Open Question 3. The SMB credential authority stays hosted in authd on
`user.sock`. Reasoning: (a) the split daemon (`nostr-smb-credentiald`)
adds a second HTTPS surface, second database, second watchdog —
operational complexity the 09-19 plan itself gated behind a feasibility
spike (`nostrc-rb0e.6/.7` still in_progress); (b) blast radius is
contained differently: the authority code paths run under authd's existing
sandbox with no new network listener (issuance API is served by the
existing authd socket surface, not a new HTTPS frontend, for v1);
(c) **design for the rename-and-lift**: all authority code lives in
`gnome/nostr-homed/src/smb/` behind `nh_smb_authority_*`
(`smb_credential.h:70-83`) with zero globals touching authd internals, so
the v2 split is: move files, new main(), new unit — the already-shipped
`nostr-smb-credentiald.service` (B1 #4) documents that target. Tradeoff:
authd binary grows; mitigated because the added deps are SQLite + exec of
Samba CLIs — **no libhanami/FUSE3/porthome/signer-client edges**, keeping
the dep-purity gate intact.

**B5 — Sysusers:** add `nostr-smb-share` user+group entries alongside the
existing `nostr-home-fetch`/`nostr-auth-greeter` files
(`packaging/sysusers.d/nostr-home-fetch.conf:25-28`,
`nostr-auth-greeter.conf:22-24`): dedicated unprivileged user owning the
materialized share mountpoint, group for Samba read access via group-ACL
on the share path.

### 4.3 Dimension (c) — DC-adjacent

- **C1 — Winbind posture: "configuration contract only" continues.**
  Resolves scaffold Open Question 5. No automated join machinery, no
  Nostr→Kerberos conversion, consistent with `SAMBA_AD_LOGIN.md:1-36`.
  What we add: a **documented manual join recipe** (DNS/time prereqs,
  `net ads join`, idmap range deployment before users create files, the
  pinned PAM graph from 09-19 §3.12) plus expansion of the validator
  (`packaging/domain/validate_domain_profile.py:94-118`) to check the new
  name-admission option (C2) for consistency with winbind config (warn if
  qualified names enabled but winbind NSS absent). Reasoning: the
  acceptance matrix already reserves real join/NSS/GDM cases
  (`tests/acceptance/matrix.json:126-158`) and the lab (`nostrc-rb0e.1`)
  is blocked — automating an untested join path would repeat the exact
  failure mode the 09-19 critique flagged (notes ≠ ground truth).
- **C2 — nss_nostr qualified-name posture: leave validator untouched, fix
  ordering docs + test** (revised per Finding 15 — the earlier "gated
  admission returns NOTFOUND" design was functionally a no-op that widened
  a validator shared with enrollment; simpler complete design is to leave
  admission alone and lean on nsswitch ordering). Concretely: (a) DO NOT
  extend `nss_nostr.c:82-113` or `gnome/nostr-homed/src/identity/identity_common.c:8-20`
  (path corrected — the earlier draft said `src/common/identity_common.c`
  which does not exist). The `n_` + `[a-z0-9_]` admission stays bit-exact.
  (b) DO document + install a validated `nsswitch.conf` snippet:
  `passwd: files winbind nostr` — `nostr` after `winbind` so qualified
  names hit winbind first and never reach nostr's module (which correctly
  returns NOTFOUND). (c) `packaging/pam/nostr-winbind-policy.md:1-23`
  gets a section explaining the ordering rationale. (d) Test
  `test_nss_qualified_names.c` becomes: assert `getent passwd 'DOMAIN\user'`
  is answered by winbind (or NOTFOUND if no winbind) and never by nostr;
  assert `n_foo` behavior bit-exact vs today. No config option needed.
- **C3 — Keytab pipeline: documented PUNT.** No keytab issuance/renewal
  pipeline in this plan. Rationale: a keytab pipeline is Kerberos
  credential materialization — the 09-19 plan's PKINIT section already
  established that any Nostr-authorized Kerberos bridge is "a separate
  identity-federation program" requiring CA/KDC machinery. Follow-up
  bead to be filed: scope = machine-credential refresh only (not user
  TGTs), gated on the domain lab (`nostrc-rb0e.1`) unblocking. Documented
  in `SAMBA_AD_LOGIN.md` so the punt is discoverable, not silent.

### 4.4 File-by-file impact + tests + verification

| File | Change | Dim |
|---|---|---|
| `gnome/nostr-homed/src/smb/nostr-smb-mount.sh` | `--batch`, `--unmount --all` (`:16-29,40-54,165-176,259-266`) | a |
| `gnome/nostr-homed/src/smb/nostr-smb-browse` (new) | Enrolled-share listing + GVfs bookmark install | a |
| `gnome/nostr-homed/src/smb/smb_credential.c` | `open()` calls sweep once (`:327-363,553`) | a |
| `gnome/nostr-homed/src/smb/smb_credential.h` | Comment ↔ behavior alignment (`:120-137,165-174`) | a |
| `gnome/nostr-homed/src/pam/pam_nostr_broker.c` | `close_session` unmount hook (`:634-637`) | a |
| `gnome/nostr-homed/src/ctl/nostr-authctl.c` | `smb-sweep` subcommand | a |
| New systemd `nostr-smb-sweep.timer/.service` | Sweep scheduling | a |
| `gnome/nostr-homed/src/smb/passdb_tdbsam.c` | Explicit config selector — `smbpasswd -c <conf>` + `pdbedit -s <conf>` on every invocation; startup reconciliation via `pdbedit -L -s <conf>` (`:220-275`) | b |
| `gnome/nostr-homed/src/auth/nostr-authd.c` | Load dedicated smb.conf path into authority config; no new libs (`:190-205`) | b |
| The six files of B1 (listed above) | New | b |
| `packaging/sysusers.d/nostr-smb-share.conf` (new) | B5 | b |
| `gnome/nostr-homed/src/nss/nss_nostr.c`, `src/common/identity_common.c` | Qualified-name gate (`:82-113`, `:8-20`) | c |
| `gnome/nostr-homed/config/nss_nostr.conf.sample` | Document `nostr_domain_qualified_names` | c |
| `packaging/domain/validate_domain_profile.py` | Cross-check gate vs winbind config (`:94-118`) | c |
| `gnome/nostr-homed/docs/SAMBA_AD_LOGIN.md` | Manual join recipe; keytab punt section (`:1-36`) | c |
| `packaging/pam/nostr-winbind-policy.md` | Mark activatable sections vs reserved (`:1-23`) | c |

Tests: `test_smb_batch_mount.sh` (three servers, one failure → partial
success + exit bitmap); `test_sweep_on_open.c` (expired cred revoked by
`open()`); `test_close_session_unmount.sh` (mock mount → user-scope
teardown service called; hung unmount → session close still succeeds
within timeout);
`test_passdb_explicit_config.sh` (argv-capture harness asserting every
`smbpasswd` invocation carries `-c <conf>` and every `pdbedit`
invocation carries `-s <conf>` — the two flags are NOT the same);
`test_nss_qualified_names.c` (verify `DOMAIN\user` and `user@REALM`
reach winbind (or return NOTFOUND without winbind) — never nostr;
`n_foo` behavior bit-exact vs today); `testparm -s` against
`smb.conf.standalone.sample` in CI. Verification includes §5.1 dep-purity
re-run and the B3 drift-detector refusing readiness against a
hand-corrupted passdb fixture.

---

## Cross-cutting

### 5.1 Dep-purity gate re-verification

New script `scripts/check-authd-dep-purity.sh`, wired into CTest as a
required test:
- `nm --undefined-only` on `nostr-authd` and `pam_nostr.so` build artifacts;
  assert no symbols from hanami/porthome/signer-client namespaces
  (`nh_porthome_*`, `nip55l_*` client, `hanami_*`, FUSE `fuse_*`).
- `ldd` output asserted to not list `libfuse3`, `libhanami`, porthome or
  signer-client shared objects.
- Link-line audit: root `CMakeLists.txt` and
  `gnome/nostr-homed/CMakeLists.txt:842-852` target-link lists for the two
  artifacts contain only their pre-approved dep set (the list is pinned in
  the script; adding a dep requires editing the script, which is the
  tripwire).
- Runs after **every** track merge (it's cheap); Track 4(b) is the most
  likely violator and gets an explicit pre-merge run.

### 5.2 Packaging changes (Debian + Fedora)

- New packages/subpackages: `nostrc-session-relay` (binaries + user units
  — note: user units install to `/usr/lib/systemd/user/`, not the system
  path the stale text at `docs/designs/packaging-plan-debian-fedora.md:288-295`
  implies; update that doc), `nostr-notify`, `nostr-dav`, `nostrc-samba-server`
  (six-file set + sysusers + timer).
- The signer activation-file dedup (Track 1 D3) lands in the same
  packaging commit: both specs drop nip55l's duplicate file.
- `nostrc-relayd` system package unchanged except inheriting the D1 bind fix.
- Each spec gains a `%check`/CI step invoking the dep-purity script for
  the auth package.

### 5.3 Live smoke lab (bizarro@192.168.64.3)

aarch64 `-Werror` clean build of all touched trees; then a scripted smoke
matrix: signer StoreKey→SignEvent→GetRelays round-trip; nostr-dav token
bootstrap + curl PROPFIND/PUT + GNOME WebDAV account dialog manual pass;
session relay socket mode/SO_PEERCRED check + notifier notification on a
fixture DM; `nostr-smb-mount --batch` against a lab Samba VM; `getent passwd
'DOMAIN\user'` with gate on (expect NOTFOUND from nostr, winbind answering
if the lab join exists — otherwise documented skip). Results appended to
`docs/plans/` as a dated smoke-report file.

---

## Implementation Order + Sequencing

- **Milestone A (Signer, items 1–4):** blocks Track 2 publish (item 9
  needs the D1 signed-JSON contract) and Track 3B account discovery.
  Nothing else blocks it. Land first.
- **Milestone B (nostr-dav, items 5–10):** 5→7→8→9 is the internal
  critical path (auth → persistence → subscribe → publish); 6 and 10 are
  small and parallel. Depends on A for #9 only; #5–#8 can start in
  parallel with A.
- **Milestone C (Relay, items 11–14):** 11→12→13→14 is strictly ordered
  (fix → factor → session daemon → notifier). Independent of B except
  that #14 prefers #13; both independent of A except notifier's account
  lookup via signer (soft dep, graceful `NotFound` handling).
- **Milestone D (Samba, items 15–20):** fully parallel with B and C after
  A. Internal: 16 blocks 17/18; 19/20 independent of 15–18.
- **Milestone E (Cross-cutting, items 21–23):** SPLIT per Finding 16.
  **#21a lands FIRST chronologically** — it depends only on today's
  authd/PAM build artifacts + a pinned baseline allowlist; landing this
  before tracks A–D is what makes "every subsequent merge is gated" true.
  **#21b** is the final integrated check running after each track and at
  packaging. **#22a → #22b → #22c** land after the first binary-bearing
  milestone (A) and again after each subsequent one. **#23** last.

Parallelizable: A ∥ B(#5–8) ∥ D(#15,19,20). Serialized: 11→12→13→14;
5→7→8→9; 16→{17,18}.

---

## Risks (top-level)

1. **Track coupling on the signer contract**: B#9 and C#14 consume
   `org.nostr.Signer`; a Track-1 slip cascades. Mitigation: mock-signer
   harnesses land in the same commits as the consumers.
2. **libwebsockets refactor blast radius** (item 12): `relayd_main.c` is
   thin but the core touches storage/policy/rate-limit code. Mitigation:
   no behavior change permitted in the factoring commit except D1; the
   system daemon's existing tests must pass unmodified.
3. **Samba passdb ambient-default drift** (B3, per Finding 2): if any
   code path still invokes Samba CLIs without their real config selector
   (`smbpasswd -c <conf>` and `pdbedit -s <conf>` — DIFFERENT flags for
   the same idea, per Samba's manpages), the dedicated server corrupts
   the host's passdb. Mitigation: the argv-capture test asserts BOTH
   flags on their respective binaries.
4. **Notification UX regressions**: duplicate or missing notifications
   are immediately user-visible. Mitigation: suppression logic tested
   both directions; grace delay.
5. **Scope creep into the frozen porthome epic**: only the mountpoint-
   consumption seam (B2) and the existing signer `_ex` consumption are
   touched; `docs/designs/home-from-relay.md` stays FROZEN, `nostrc-e4v6`
   remains the only open P2.
6. **aarch64 `-Werror` regressions** from new code (noted in Background
   as the lab standard): smoke lab runs per milestone, not just at E.

---

## Open Questions — resolutions

1. **Bus-name collision** → Resolved: gnostr-signer owns
   `org.nostr.Signer.service`; nip55l's activation install gated behind
   `ENABLE_NIP55L_STANDALONE_ACTIVATION=OFF` (Track 1 D3). Reasoning /
   tradeoff in §1.2.
2. **Session-relay bind + auth model** → Confirmed as proposed:
   `$XDG_RUNTIME_DIR/nostr/relay.sock`, 0600, SO_PEERCRED UID-equality
   admission, signer-daemon pattern (not authd's multi-UID model).
   Reasoning in §3.2 D3: single-user socket makes UID-equality the correct
   check; 0600+dir perms primary, SO_PEERCRED defense-in-depth.
3. **Single authd vs split credentiald** → Resolved: v1 in authd,
   rename-and-lift design, split unit shipped-but-disabled (Track 4 B4).
   Reasoning: operational complexity vs contained blast radius;
   feasibility beads `.6/.7` still open.
4. **VFS vs share-declaration** → Resolved: share-declaration v1 (smbd
   reads materialized mountpoint), VFS as follow-up bead with ABI /
   crash-budget gates (Track 4 B2). Reasoning: in-process risk + distro
   ABI coupling, same argument as Track 2's Path B rejection.
5. **DC posture** → Resolved: configuration-contract-only continues;
   manual join recipe + validator cross-checks land; nss_nostr gains the
   gated qualified-name passthrough; keytab pipeline punted with a
   follow-up bead tied to the blocked lab (`nostrc-rb0e.1`) (Track 4
   C1–C3).

**New items surfaced** (for beads, not blocking this plan):
- NIP-29 kinds 39005-39009 undefined in `nostr-kinds.h` (upstream NIP
  drift — track, don't invent).
- nostr-dav REPORT pagination if EDS profiling demands it.
- Negentropy (NIP-77) sync for the session relay as a post-v1 performance
  item.

---

## Component-Version Policy Application (per Finding 22)

Before merging any Milestone, apply the repository's `AGENTS.md`
component-version policy per affected component and record the outcome
in the merge commit:

- **nostr-homed** (tracked component per `VERSION_MANIFEST.md:18-27`): bump
  minor version when any Milestone A/B/C/D lands runtime code; patch bump
  for the packaging-only landings.
- **nip55l** (not yet manifest-tracked but semantic contract migrating
  per Track 1 D1.a): add to `VERSION_MANIFEST.md` in the same commit that
  lands D1.a, start at version 0.2.0 to reflect the breaking change from
  "return bare signature" → "return signed JSON."
- **gnostr-signer** (if manifest-tracked; add if not): coordinate with
  nip55l bump because both trees ship the same DBus service.
- **nostr-dav**: bump on Milestone B landing (patch for #5-#7, minor for
  #8/#9 which change the network posture and DAV write semantics).
- **relayd / session-relay**: minor bump on #12 factoring; new session-relay
  binary starts at 0.1.0.
- **Samba subpackages**: patch bump on config/service additions only.

Each merge commit MUST include the manifest update (or an explicit "no
bump" line with justification). This is a hard gate per repo policy.

---

## Verification Master Checklist

- [ ] Signer: `test_sign_event_json`, `test_getrelays`, `test_nip44_binary`,
      fuzz harness green; installed `.service` set deduped with no `@`
      literal; approval-signal round-trip harness passes; mid-decrypt
      kill test leaves porthome cache intact.
- [ ] nostr-dav: `test_auth_required` (fail-closed), `test_store_sqlite`
      (restart-stable ctag), `test_relay_sync`, `test_publish_rollback`;
      `run_dav_e2e.sh`; port 7680 consistent; loopback-only asserted;
      GNOME Calendar **and** Contacts manual round-trip on the lab.
- [ ] Relay: `test_bind_iface` (loopback enforced, wide-bind honored);
      `test_session_relay_peercred` (0600 + UID-equality + pre-read close);
      socket-activation fd-handoff test; `test_notify_sub` (hex8-only DM
      body, ≤80-char group preview, per-thread coalescing, suppression
      both directions).
- [ ] Samba: batch/partial-failure bitmap test; sweep-on-open test;
      close-session unmount (incl. hung-unmount non-blocking); strace test
      proving `-s` on every Samba CLI; drift-detector refuses readiness
      on corrupted passdb fixture; `testparm -s` in CI; sysusers entry
      installed; nss gate test (both modes, `n_` behavior bit-exact);
      validator cross-check.
- [ ] Dep purity: `check-authd-dep-purity.sh` green after **each**
      milestone; wired as a required CTest.
- [ ] Packaging: all new units install to correct user/system dirs;
      activation-file dedup in specs; `%check` purity step.
- [ ] Smoke lab: aarch64 `-Werror` clean; full §5.3 matrix; dated smoke
      report filed under `docs/plans/`.
- [ ] Docs: QUICKSTART port fix; `SAMBA_AD_LOGIN.md` join recipe + punt
      section; `packaging-plan-debian-fedora.md:288-295` staleness
      corrected; `docs/dbus-interface.md` signer/broker boundary section.

---

## References

Design + prior-art docs:
- `docs/plans/nostr-linux-samba-login-2026-09-19.md`
- `docs/reviews/nostr-linux-samba-login-2026-09-20.md`
- `docs/proposals/goa-overlay-postmortem.md`
- `docs/proposals/55L.md`
- `docs/proposals/nip-contacts-draft.md`
- `docs/dbus-interface.md`
- `docs/designs/home-from-relay.md`
- `docs/designs/packaging-plan-debian-fedora.md`
- `docs/designs/nip46-qr-login-greeter.md`
- `docs/gnome-integration.md`
- `gnome/nostr-homed/docs/AUTH_PROTOCOL.md` (FROZEN)
- `gnome/nostr-homed/docs/DESIGN.md`
- `gnome/nostr-homed/docs/SYSTEMD_TOPOLOGY.md`
- `gnome/nostr-homed/docs/PAM_PROVIDER_UX.md`
- `gnome/nostr-homed/docs/SAMBA_AD_LOGIN.md`
- `gnome/nostr-dav/SECURITY.md`
- `gnome/nostr-dav/docs/QUICKSTART.md`

External:
- [GNOME 46 release notes — generic WebDAV Account](https://release.gnome.org/46/)
- [EDS backend libraries](https://gnome.pages.gitlab.gnome.org/evolution-data-server/)
- [Ubuntu 24.04 evolution-data-server package](https://packages.ubuntu.com/noble/evolution-data-server)
- [NIP-52 Calendar Events](https://github.com/nostr-protocol/nips/blob/master/52.md)
- [NIP-29 Relay-based Groups](https://github.com/nostr-protocol/nips/blob/master/29.md)
- [NIP-17 Private Direct Messages](https://github.com/nostr-protocol/nips/blob/master/17.md)
- [Samba VFS module developer docs](https://wiki.samba.org/index.php/Writing_a_Samba_VFS)
