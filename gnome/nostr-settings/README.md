# Nostr Settings — `org.nostr.Settings`

A libadwaita sidebar window that hosts shared Grotto management panels and
service-specific preferences, for the Nostr services on this desktop. gnome-control-center
has no plugin API (and GOA providers are in-tree only — see commit
`2cf193a7`), so this is a standalone app; `Categories=Settings;GNOME;GTK;` and
`Keywords=nostr;relay;identity;wallet;…` make GNOME Shell search list it next
to Settings. Bead `nostrc-janr`.

```sh
nostr-settings                 # D-Bus activatable (org.nostr.Settings)
nostr-settings --page relays   # identity | apps | activity | remote-signing | relays | notifications | wallet | media | files
```

## Shared panels and manager authority

Requires GTK 4.14, libadwaita 1.5 and **grotto-panels-0 >= 0.1.0** at build and
runtime (`libgrotto-panels-0-0` on Debian; `grotto-panels` on RPM). CMake can use
`-DGROTTO_USE_SYSTEM_PANELS=ON`; Meson resolves the installed runtime/SDK.
The RPM Settings package explicitly depends on that runtime.

`nss_window_new` mounts the real Identity, Connected Apps, Activity and Remote
Signing widgets through one long-lived injected Control1 service. The manager
role is Linux PID/path/device/inode authenticated; a bus name or claimed app ID
is not authority. Pending request contents are approver-only (`ListRequests`,
`GetApprovalInfo` and `ListPendingRequests` are refused); the manager can see
pending counts/state only. It permits public management reads and **client labels only**,
not tiers, permissions, pause/resume, approvals, signing, imports or exports.
Security edits, backup, profile work and review use **Open in Grotto**. Activation
passes public navigation targets only; Grotto refetches state and the user makes
the change there. No management lock redesign or Settings unlock is implied.
Missing daemon/authorization/version support produces an unavailable/read-only
state, not fabricated identities or a local permissive fallback. An inventory
or model-read failure remains a data-unavailable error until the read recovers,
never “No Identity Yet” merely because GetStatus still succeeds. Cosmetic
labels appear only as secondary “called …” text in Grotto reviews and notification
bodies, never as verified identities or notification titles. Missing Grotto
activation reports a toast. Daemon loss clears models/capabilities; restart
resubscribes before snapshots. Setup/backup state and identity app counts that
are unavailable in Control1 are explicitly unknown. GUI-private local labels,
watch-only inventory and backup acknowledgement remain Grotto-owned; Settings
does not inspect that file or infer that a backup is recoverable.

The existing service pages are preferences-page wrappers, mounted on demand.
`--page`, `last-page`, window state, screenshot mode, and panel-local search are
retained; the split shell does not claim universal preferences search.

The focused affected route is
`scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --profile grotto.settings.host.linux`
(add `--explain` for a plan only). It includes every Settings core suite, the
real host, Control1 clients/activity compatibility, Grotto approval/navigation
integration and panel boundary/smoke suites. This dedicated route replaces the
old missing-Settings-profile gap for normal affected selection. It does not
claim unrestricted repository coverage, full-universe attestation or release
qualification.

CMake: configure the root with `-DENABLE_NOSTR_SETTINGS=ON -DBUILD_APPS=ON
-DBUILD_SIGNER_TESTS=ON -DBUILD_TESTING=ON -DNIP55L_TEST_TRUST_ENV=ON`, then build
`nostr-settings grotto test-nostr-settings-panel-mount`. Run
`ctest --test-dir <build> -R '^nostr-settings/panel-mount$' --output-on-failure`.
It uses the real window and daemon on `nostrc_test_bus_up`, disposable trusted
executable copies, Xvfb and an environment fixture identity. All non-manager
Control1 methods are tested for `NotAuthorized`, including both pause values
and pending-content reads against a real sentinel-bearing pending request;
it verifies real identity/app rows, label signals, exact handoff targets,
untrusted-name rejection, daemon loss/restart, real inventory failure/recovery
and inode replacement. Only the
Grotto desktop activation receiver is a capture fixture, not the signer or
panels. Missing required bus/display/role support fails this selected test.

Meson: install libnostr, nip19, nostr-publish and grotto-panels first, then
`meson setup <build> gnome/nostr-settings` and `meson compile -C <build>`.
To select the same real-daemon test, add
`-Dtest_daemon=/absolute/path/to/test-built/grotto-daemon` and run
`meson test -C <build> --print-errorlogs` with Grotto schemas available in
`GSETTINGS_SCHEMA_DIR`. This is local host proof, not VM or release acceptance.

## Where settings are stored

The app owns no preferences. GSettings `org.nostr.Settings` holds window
state only (size, maximized, last page); every page edits the owning
service's own store, so the service stays the single source of truth.

| Page | What | Backing store | Written by Settings? |
|---|---|---|---|
| Identity | shared Identity panel; canonical signing npubs/default | daemon `Control1.ListIdentities`, never a direct keyring/accounts.ini inventory | no |
| Identity | signer relay summary and Relays link | `org.nostr.Signer.GetRelays` | no |
| Connected Apps | shared apps/permission views; cosmetic labels | `Control1.ListClients`, `ListPermissions`, `SetClientLabel` | labels only |
| Activity | shared activity view | `Control1.ListActivity` | no |
| Remote Signing | shared status and configuration handoff | `Control1.GetStatus` | no |
| Relays | session relay on/off | systemd `--user`: `nostr-session-relay.socket` (+ `.service`) via `org.freedesktop.systemd1` | unit enablement |
| Relays | live statistics | `org.nostr.SessionRelay1.GetStats` (never auto-started) | no |
| Relays | storage limits | `~/.config/nostr/session-relay.conf`, flat `retention_*` keys | only once the relay reports `RetentionSupported` (not yet — `nostrc-prqu.17`) |
| Relays | your relays | NIP-65 kind **10002**, signed by the signer, published with libnostr-publish | yes (event) |
| Relays | relay details | NIP-11 over HTTPS (`Accept: application/nostr+json`, 5 s, 64 KiB cap) | no |
| Notifications | on/off | systemd `--user`: `nostr-notify.service` | unit enablement |
| Notifications | upstream mode, fallback relays | `~/.config/nostr-notify/nostr-notify.conf` `[notify]` `upstream_mode`, `home_relays` | yes |
| Notifications | groups / DMs / preview / sound | same file, `notify_groups`, `notify_dms`, `group_preview`, `sound` (`nostr-notify.conf.example`) | yes |
| Wallet | pairing | `org.nostr.Wallet1` `Paired`/`Lud16`/`WalletPubkey`/`Relays`. *Connect with your wallet app* (default): `BeginWalletAuth` → opens the returned `nostr+walletauth://` link in the wallet app (or offers to copy it), waits for `WalletAuthFinished`, Cancel → `CancelWalletAuth`; the agent creates the key. Fallback: paste a `nostr+walletconnect://` link → `Pair(uri)`. `Unpair()`. The agent always confirms | via the agent |
| Wallet | per-app (and per-website) read access and daily budgets | `org.nostr.Wallet1` `ListApps`, `SetReadAccess(app, b)`, `SetBudget(app, msat)`; re-listed on `AppsChanged`. A standing *GNOME Shell* row grants the panel indicator (every Shell extension shares gnome-shell's identity) | via the agent: read access applied directly (Settings is its grant admin), budgets confirmed in the agent's dialog |
| Media servers | Blossom list | BUD-03 kind **10063**, signed + published | yes (event) |
| Files | encryption defaults | `~/.config/nostr/seal.conf` `[seal]` `default_recipients`, `include_self`, `work_factor` (`$NOSTR_SEAL_CONFIG`) | yes |
| Files | sharing defaults | `~/.config/nostr-share/nostr-share.conf` `[nostr-share]` `default_text_kind`, `keep_metadata` (`$NOSTR_SHARE_CONFIG`) | yes |

Writers keep comments and keys they do not own, never add a key the user
did not change, and replace files atomically (new files `0600`; existing
files keep their mode). `session-relay.conf` is edited line by line because
the relay's parser rejects `[section]` headers.

**How Settings proves itself to the wallet agent.** The agent trusts the
installed Settings the way it trusts the browser bridge: an unsandboxed,
same-user caller whose `/proc/<pid>/exe` is `<bindir>/nostr-settings` by
path *and* inode (a copy, hard link, replaced binary or sandboxed process
does not qualify). That caller may list every app (`ListApps`) and grant and
revoke read access without the agent's confirmation dialog; other apps may
only give up their own access, or ask the user. Budget changes are still
confirmed in the agent's dialog (an unsandboxed binary is a softer identity
than a Flatpak id, so it administers grants, not money). Like every
unsandboxed identity this keeps sandboxed apps out, not malware already
running as you (which could edit `budgets.json`). Settings run from a build
tree is not trusted: the page then lists `budgets.json` read-only and the
agent confirms each change.

## Relays page semantics

- **The switch is the persistent intent** — the socket unit's
  `UnitFileState`. The status row is runtime truth: *Off*, *Listening —
  starts when an app connects*, *Running*, *Starting*, *Failed* (including a
  crash loop), *Enabled but not listening* (with *Start now*), *masked*
  (switch insensitive), *Not installed*.
- **On** = `EnableUnitFiles([socket])` → `Reload` → `StartUnit(socket)`.
  **Off** = `DisableUnitFiles([socket])` → `Reload` → `StopUnit(service)` →
  `StopUnit(socket)`. Disabling first means a client connecting in between
  cannot socket-activate the relay after you switched it off. An enabled
  `nostr-notify.service` (`Wants=` the relay) will still start it; the page
  says so.
- **Statistics** come from `org.nostr.SessionRelay1` only while that name
  has an owner (`g_bus_watch_name`, polled every 2 s while the page is
  visible). There is no D-Bus activation file for it, and every call uses
  `NO_AUTO_START`, so looking at the page never starts the relay. When the
  relay is idle the page shows the on-disk size of
  `~/.local/share/nostr/session-relay` measured locally.
- **Honest storage line.** With `StorageBackend = "none"` (packaged builds
  today, `nostrc-prqu.5`) the page says *No storage — this relay keeps
  nothing* and shows no event count; an unknown count (`-1`) is shown as
  "event count unavailable", never as a number. Sizes are labelled
  *on disk* (allocated blocks, like `du`).
- **Storage limits** mirror `docs/designs/nostrdb-retention-eviction-policy.md`
  §5/§7.1 (`retention_enabled`, `retention_cache_max_mb`,
  `retention_high_watermark_pct`, `retention_low_watermark_pct`,
  `retention_min_age_days`, `retention_note_ttl_days`,
  `retention_reaction_ttl_days`, `retention_interval_mins`). The rows are
  insensitive, under an explanatory note row, until the running relay reports
  `RetentionSupported = true` — so nothing is written while the relay cannot
  enforce it. Validation (low < high, 1–99 %, 0 or ≥ 64 MB, TTLs ≥ minimum
  age, 1 min–7 d interval) runs before any write.
- **NIP-65 publishing.** The new kind 10002 goes to the union of the new
  list, the previous list (so relays you remove stop serving the old one)
  and the signer's relays. It counts as published only when **every write
  relay in the new list** accepted it; otherwise the page shows
  "Partly published — N of M" with each relay's answer and keeps Publish
  enabled. The current list is read from the signer's relays and the session
  relay; only events whose id and signature verify and whose author is the
  active identity are considered, newest `created_at` wins.

`org.nostr.SessionRelay1` itself is documented in
`gnome/dbus/org.nostr.SessionRelay1.xml`; `nostr-session-relayd --stats`
prints the same data.

## Design

`src/core/` is GTK-free and holds every decision the UI shows (config
stores, list models and event builders, NIP-11, relay/publish I/O,
systemd state model and op plans, relay-stats client and strings, wallet
budget reader, signer/keyring helpers). `src/ui/` renders it: one file per
page, blocking work on `GTask` threads, remote or user text always with
`use-markup = FALSE`. The API is pinned to GLib 2.80 / GTK 4.14 /
libadwaita 1.5 (`*_VERSION_MAX_ALLOWED`) so newer dev headers cannot leak
symbols the oldest supported distro lacks.

For offscreen verification, `NOSTR_SETTINGS_SCREENSHOT_DIR=/tmp/x
nostr-settings --page relays` renders that page to
`/tmp/x/nostr-settings-relays.png` and quits (GTK draws the snapshot
itself, so any backend works, e.g. under `xvfb-run`; one page per run);
`NOSTR_SETTINGS_SCREENSHOT_DELAY` (seconds, default 6) lets asynchronous
loads settle first.

## Tests

`ctest -R nostr-settings` (configure with `-DENABLE_NOSTR_SETTINGS=ON
-DBUILD_TESTING=ON`). No display, network, real signer, systemd or keyring:

| Test | Covers |
|---|---|
| `config` | every backing store: defaults, round trip, comments/foreign keys kept, duplicates collapsed, no `[section]` in `session-relay.conf`, validation before write, file modes; cross-parsed by `nseal_config_load`, `ns_config_load` and `relayd_config_load` when those components are in the build |
| `lists` | relay/Blossom URL rules; 10002/10063 builders and parsers (also checked with libnostr-publish's NIP-65 reader); publish-target rules; fetch of the newest *verified* event (forged/other-author events ignored); publish through fixture relays with a real-key vtable signer — all-write-relays rule, partial rejection report, signer refusal, no targets |
| `nip11` | parser hardening (types, control chars, length cap, sorted unique NIPs) and fetch against an in-process libsoup server: Accept header, 64 KiB cap, HTTP error, non-JSON, 1 s timeout, cancellation |
| `relay_stats` | client against a mock exporting the shipped `SessionRelay1` XML on GTestDBus; proof that a stats call never D-Bus-activates the relay; storage/uptime/NIP strings incl. `none` backend and unknown count |
| `systemd` | status model table (incl. the q9ba crash loop), exact on/off plans, and the plans replayed against a mock `org.freedesktop.systemd1` Manager (state transitions, call order, failure stops the plan, unknown unit) |
| `wallet` | `ListApps` reply parsing; `budgets.json` enumeration (fallback; today vs. other days, corrupt file), produced by the agent's own `NwaBudgetStore` when built together; sats/app-id/GNOME Shell/website labels |
| `identity` | `GetRelays` JSON forms, signer calls against a mock (locked signer, NotFound → empty) |

The relay side has its own integration test,
`relayd_session_relay_dbus` (real `nostr-session-relayd` on a private bus).

## Not done / follow-ups

- Retention is not enforced by the relay (`nostrc-prqu.17`, blocked by
  `nostrc-8rxk`); the rows stay disabled until `RetentionSupported`.
- The packaged session relay is cache-less (`nostrc-prqu.5`).
- Kind-10002/10063 publishing does not retry in the background; partial
  results stay on screen with Publish enabled.
