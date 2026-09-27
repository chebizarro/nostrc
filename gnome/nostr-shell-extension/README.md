# Nostr — GNOME Shell extension (`nostr@nostr.org`)

A **Nostr Relay** Quick Settings toggle (next to Wi-Fi, Bluetooth, VPN) for
the per-user session relay, plus a small status menu. GNOME Shell 46, 47
and 48. Bead `nostrc-33f1`.

| Where | Shows | Source |
|---|---|---|
| Toggle (on/off) | whether the relay is **switched on** — the socket unit's `UnitFileState` | systemd `--user`, `nostr-session-relay.socket` |
| Toggle subtitle | runtime state; while running, `1,234 events · 12.3 MB` | systemd + `org.nostr.SessionRelay1.GetStats` |
| Panel icon | shown while the relay listens, starts or runs | systemd |
| Menu header | full status (`Listening — starts when an app connects`, errors) | systemd |
| Menu | clients and uptime while running | `SessionRelay1` |
| Menu | `3 unread direct messages` / `No unread direct messages` while the notifier runs; else `Message notifications: on/off/failed` | `org.nostr.NotifyDaemon1` (count only), systemd `nostr-notify.service` |
| Menu | `Wallet: 21,000 sats` / `Wallet: —` / `not paired` / `agent not running` | `org.nostr.Wallet1` |
| Menu | **Nostr Settings** | launches `org.nostr.Settings.desktop` |

## Semantics — same as Nostr Settings

The toggle and the Settings *Relays* page use one model
(`lib/state.js` mirrors `gnome/nostr-settings/src/core/nss-systemd.c`):

* **On** = `EnableUnitFiles([nostr-session-relay.socket])` → `Reload` →
  `StartUnit(socket)`.
* **Off** = `DisableUnitFiles([socket])` → `Reload` →
  `StopUnit(nostr-session-relay.service)` → `StopUnit(socket)`. Disabling
  first means an app connecting in between cannot socket-activate the relay
  after you switched it off. (An enabled `nostr-notify.service` `Wants=` the
  relay and will still start it.)
* A failing step stops the plan; the subtitle says *Error* and the menu
  header shows the systemd message.

| Case | Toggle | Clickable | Subtitle | Panel icon |
|---|---|---|---|---|
| systemd `--user` unreachable | off | no | Status unavailable | hidden |
| socket unit not installed | off | no | Not installed | hidden |
| masked | off | no | Disabled by administrator | hidden |
| disabled, inactive | off | yes | Off | hidden |
| enabled, socket inactive | on | yes | Enabled, not listening | hidden |
| enabled, socket listening | on | yes | Listening | shown |
| service activating | on | yes | Starting… | shown |
| service active, stats not yet read | on | yes | Running | shown |
| service active + stats | on | yes | `1,234 events · 12.3 MB` | shown |
| … cache-less relay (`StorageBackend = none`) | on | yes | No storage | shown |
| … event count unknown (`-1`) | on | yes | `12.3 MB on disk` | shown |
| running but started by hand (disabled) | off | yes | Running | shown |
| service failed / `auto-restart` crash loop | on | yes | Failed | hidden |
| plan running | — | no | Turning on… / Turning off… | — |
| plan failed | — | yes | Error | — |

`tests/run-tests.js` prints this table from the code.

## What it never does

* **Start the relay by looking.** Statistics are read only while
  `org.nostr.SessionRelay1` has an owner (`g_bus_watch_name`); every call
  uses `NO_AUTO_START`; the relay ships no D-Bus activation file.
* **Start the wallet agent.** The `org.nostr.Wallet1` proxy is created with
  `DO_NOT_AUTO_START`; with no agent running the menu says so.
* **Prompt from the panel.** Automatic reads use the agent's
  `GetBalanceNonInteractive`: it answers when GNOME Shell holds a read grant
  and otherwise fails with `org.nostr.Wallet1.Error.InteractionRequired`
  without showing anything (an older agent without the method counts the
  same — never a fallback to the prompting call). The extension therefore
  does not reimplement the agent's caller identification or read its grant
  store. Without a grant the menu shows `Wallet: —` and an **Allow balance
  access…** item; only clicking it sends the plain `GetBalance`, which the
  agent may answer with its dialog (*Always allow this app* keeps the
  balance visible). The grant can also be switched in Nostr Settings ›
  Wallet (its *GNOME Shell* row); the agent's `AppsChanged` signal makes the
  panel ask again. The grant is for GNOME Shell as a whole
  (`exe:/usr/bin/gnome-shell`), so other extensions could read the balance
  too.
* **Poll fast.** A background resync and stats read run every *refresh
  interval* (default 60 s, never below 30 s), once each time Quick Settings
  opens, and after every switch. systemd's `PropertiesChanged`,
  `UnitFilesChanged` and `Reloading` are matched too, for faster updates
  whenever systemd broadcasts them. The extension never calls
  `Manager.Subscribe`/`Unsubscribe` itself: gnome-shell and all extensions
  share one bus connection and systemd tracks subscriptions per connection,
  so unsubscribing could break another subscriber. The balance is fetched
  when the agent or the pairing changes, on `AppsChanged` (a grant changed),
  on `PaymentReceived`/`PaymentSent`, and on menu open when it is older than
  the refresh interval.
* **Start the notifier or reveal messages.** The unread count comes from
  nostr-notify-daemon's `org.nostr.NotifyDaemon1.UnreadDirectMessages`
  (`DO_NOT_AUTO_START`; the daemon has no activation file anyway), updated
  by `PropertiesChanged`. It is a number only — the daemon exposes no
  content, sender or conversation. While the daemon is not on the bus the
  row shows the service state instead.

`disable()` destroys the indicator and the toggle, and with them both
monitors. That cancels in-flight calls, unsubscribes every D-Bus signal,
unwatches the relay name, disconnects the wallet and notifier proxies,
removes every timeout, and disconnects every
GSettings, toggle and menu signal.

## Settings

`org.gnome.shell.extensions.nostr` (Preferences in the Extensions app):

| Key | Default | |
|---|---|---|
| `refresh-interval` (u, 30–3600) | 60 | background resync, seconds |
| `show-balance` (b) | true | wallet line |
| `show-dm-status` (b) | true | message-notification line |

## Install

**Distribution package** (system-wide, schema compiled by the glib
trigger):

```sh
sudo apt install gnome-shell-extension-nostr      # Debian/Ubuntu
sudo dnf install gnome-shell-extension-nostr      # Fedora
```

Then log out and back in (Wayland loads new system extensions at login),
and enable it:

```sh
gnome-extensions enable nostr@nostr.org           # or the Extensions app / Extension Manager
gnome-extensions info nostr@nostr.org             # State: ACTIVE
```

**From source** (system-wide): configure with
`-DENABLE_NOSTR_SHELL_EXTENSION=ON`. `cmake --install` puts the extension
into `<prefix>/share/gnome-shell/extensions/nostr@nostr.org/`. The schema
goes into `<prefix>/share/glib-2.0/schemas/` and is compiled when `DESTDIR`
is unset.

**Per-user** (no root; Extension Manager or the CLI):

```sh
cmake --build build --target nostr-shell-extension-zip   # or, from this directory:
gnome-extensions pack --force --extra-source=lib --extra-source=icons \
    --schema=schemas/org.gnome.shell.extensions.nostr.gschema.xml .
gnome-extensions install --force nostr@nostr.org.shell-extension.zip
# log out/in, then:
gnome-extensions enable nostr@nostr.org
```

Extension Manager (`com.mattjakeman.ExtensionManager`) installs the same zip
via *Install from file*.

## Files

| File | |
|---|---|
| `extension.js` | `SystemIndicator` + `QuickMenuToggle`; wiring only |
| `lib/state.js` | pure logic: status model, plans, subtitles, formatting — no `gi://` imports |
| `lib/relay.js` | systemd + `SessionRelay1` monitor and on/off plans (Gio only) |
| `lib/wallet.js` | `Wallet1` proxy, non-interactive reads, cached balance (Gio only) |
| `lib/dm.js` | `NotifyDaemon1` proxy: unread direct-message count (Gio only) |
| `lib/dbus.js` | async call helpers |
| `prefs.js` | libadwaita preferences |

## Tests

```sh
node tests/run-tests.js          # lib/state.js (also: gjs -m tests/run-tests.js)
tests/run-monitors.sh            # lib/relay.js, lib/wallet.js, lib/dm.js on a private bus (Linux)
ctest -R nostr-shell-extension   # all of the above + schema --strict
```

`test-monitors.js` runs the real monitors on a private `dbus-run-session`
bus. It uses
fake `org.freedesktop.systemd1`, `org.nostr.SessionRelay1`,
`org.nostr.Wallet1` and `org.nostr.NotifyDaemon1` services and checks:

* the exact on/off call order;
* the plan stops at a failing step;
* stats are read only while the relay owns its name;
* automatic reads are only ever `GetBalanceNonInteractive`;
  `InteractionRequired` shows `—` plus the grant item (not an error),
  `AppsChanged` refetches, a revoked grant drops the balance on the next
  payment, and the plain `GetBalance` runs once, on the explicit click;
* the unread count appears with the notifier, follows `PropertiesChanged`
  and disappears with it;
* after `destroy()`, nothing runs;
* neither the relay, the agent nor the notifier is ever D-Bus-activated
  (the fakes are activatable and would leave a marker file).
