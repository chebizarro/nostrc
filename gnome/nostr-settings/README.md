# Nostr Settings — `org.nostr.Settings`

A libadwaita `AdwPreferencesWindow` that looks and behaves like a GNOME
Settings panel, for the Nostr services on this desktop. gnome-control-center
has no plugin API (and GOA providers are in-tree only — see commit
`2cf193a7`), so this is a standalone app; `Categories=Settings;GNOME;GTK;` and
`Keywords=nostr;relay;identity;wallet;…` make GNOME Shell search list it next
to Settings. Bead `nostrc-janr`.

```sh
nostr-settings                 # D-Bus activatable (org.nostr.Settings)
nostr-settings --page relays   # identity | relays | notifications | wallet | media | files
```

## Where settings are stored

The app owns no preferences. GSettings `org.nostr.Settings` holds window
state only (size, maximized, last page); every page edits the owning
service's own store, so the service stays the single source of truth.

| Page | What | Backing store | Written by Settings? |
|---|---|---|---|
| Identity | active identity, relays | `org.nostr.Signer` `GetPublicKey` / `GetRelays` | no (read-only) |
| Identity | identities in the keyring | Secret Service, schema `org.gnostr.Signer/identity` (`gnostr_secret_store_find_all`, attributes only — no secrets, no unlock) | no |
| Identity | Passwords and Keys / signer settings | launches `org.gnome.seahorse.Application.desktop` / `org.gnostr.Signer.desktop` action `settings` | — |
| Relays | session relay on/off | systemd `--user`: `nostr-session-relay.socket` (+ `.service`) via `org.freedesktop.systemd1` | unit enablement |
| Relays | live statistics | `org.nostr.SessionRelay1.GetStats` (never auto-started) | no |
| Relays | storage limits | `~/.config/nostr/session-relay.conf`, flat `retention_*` keys | only once the relay reports `RetentionSupported` (not yet — `nostrc-prqu.17`) |
| Relays | your relays | NIP-65 kind **10002**, signed by the signer, published with libnostr-publish | yes (event) |
| Relays | relay details | NIP-11 over HTTPS (`Accept: application/nostr+json`, 5 s, 64 KiB cap) | no |
| Notifications | on/off | systemd `--user`: `nostr-notify.service` | unit enablement |
| Notifications | upstream mode, fallback relays | `~/.config/nostr-notify/nostr-notify.conf` `[notify]` `upstream_mode`, `home_relays` | yes |
| Notifications | groups / DMs / preview / sound | same file, reserved keys (`nostr-notify.conf.example`) | shown disabled: the daemon does not read them yet (`nostrc-prqu.16`) |
| Wallet | pairing | `org.nostr.Wallet1` `Paired`/`Lud16`/`WalletPubkey`/`Relays`; `Pair(uri)` / `Unpair()` — the agent shows its own confirmation | via the agent |
| Wallet | per-app budgets | listed from `$XDG_STATE_HOME/nostr-wallet/budgets.json` (read-only); changed with `SetBudget` — the agent confirms | via the agent |
| Media servers | Blossom list | BUD-03 kind **10063**, signed + published | yes (event) |
| Files | encryption defaults | `~/.config/nostr/seal.conf` `[seal]` `default_recipients`, `include_self`, `work_factor` (`$NOSTR_SEAL_CONFIG`) | yes |
| Files | sharing defaults | `~/.config/nostr-share/nostr-share.conf` `[nostr-share]` `default_text_kind`, `keep_metadata` (`$NOSTR_SHARE_CONFIG`) | yes |

Writers keep comments and keys they do not own, never add a key the user
did not change, and replace files atomically (new files `0600`; existing
files keep their mode). `session-relay.conf` is edited line by line because
the relay's parser rejects `[section]` headers.

**Why budgets are read from the file.** `GetBudget` for another app is
allowed only for *trusted* callers, and trust requires a Flatpak-verified
app id; an unsandboxed Settings app is refused. The agent's README documents
`budgets.json` as its store, so Settings lists it read-only and sends every
change through `SetBudget`, where the agent's dialog decides. No
`ListBudgets` method was added to the agent.

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
  insensitive, with a banner, until the running relay reports
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
nostr-settings` renders each page to `/tmp/x/nostr-settings-<page>.png`
(GTK draws the snapshot itself, so any backend works, e.g. under
`xvfb-run`) and quits; `NOSTR_SETTINGS_SCREENSHOT_DELAY` (seconds, default
6) lets asynchronous loads settle first.

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
| `wallet` | `budgets.json` enumeration (today vs. other days, corrupt file); produced by the agent's own `NwaBudgetStore` when built together; sats/app-id formatting |
| `identity` | `GetRelays` JSON forms, signer calls against a mock (locked signer, NotFound → empty) |

The relay side has its own integration test,
`relayd_session_relay_dbus` (real `nostr-session-relayd` on a private bus).

## Not done / follow-ups

- Retention is not enforced by the relay (`nostrc-prqu.17`, blocked by
  `nostrc-8rxk`); the rows stay disabled until `RetentionSupported`.
- nostr-notify ignores the presentation keys (`nostrc-prqu.16`).
- The packaged session relay is cache-less (`nostrc-prqu.5`).
- Kind-10002/10063 publishing does not retry in the background; partial
  results stay on screen with Publish enabled.
