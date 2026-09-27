# nostr-dispatcher

`nostr-dispatcher` routes Nostr links to the right application **by event
kind**. It is the *only* desktop entry that claims the `nostr:` scheme:

| Claimed by `org.nostr.Dispatcher.desktop` | |
|---|---|
| `x-scheme-handler/nostr` | NIP-21 links (`nostr:nevent1…`) |
| `x-scheme-handler/web+nostr` | the browser-registered variant |
| `application/vnd.nostr.event+json` | event files (`*.nostr`, `*.nevent`), a sub-class of `application/json` |

Applications never claim `x-scheme-handler/nostr` themselves. If two apps
claim it, `xdg-mime` picks one nondeterministically (nostrc-2thp). Instead,
each app declares **which kinds it handles**, and the dispatcher picks one
per link, much as `mimeapps.list` does for MIME types.

```
nostr:nevent1…  ──▶  nostr-dispatcher  ──▶  kind 30023  ──▶  org.example.Reader.desktop
                     (org.nostr.Dispatcher1)   kind 14     ──▶  org.nostr.Groundhog.desktop
                                               kind ?      ──▶  X-Nostr-Kinds=* app (e.g. GNostr)
```

## For application authors

### 1. Declare kinds: `X-Nostr-Kinds=`

Add one key to your `.desktop` file:

```ini
[Desktop Entry]
Type=Application
Name=Example Reader
Exec=example-reader %U
X-Nostr-Kinds=30023;30024;30000-39999;
```

The value is a `;`-separated list of:

| Token | Meaning |
|---|---|
| `N` | one kind, `0`–`65535` (e.g. `1`, `30023`) |
| `A-B` | inclusive range (e.g. `30000-39999`) |
| `*` | generic viewer, used as the **fallback** for kinds no app declares and for links whose kind cannot be determined |

Malformed tokens are ignored individually. When several apps declare a
kind, the narrowest declaration wins: an exact kind beats a range, and a
narrow range beats a wide one. Ties are broken by desktop id, so the result
is deterministic. `*` only takes part in the fallback step.

Do **not** add `x-scheme-handler/nostr` to `MimeType=`. To accept dropped
or opened event files too, add `application/vnd.nostr.event+json` to your
`MimeType=`. The dispatcher then passes you the `file://` URI instead of a
`nostr:` URI.

### 2. Accept the URI

The dispatcher launches your desktop entry through `GDesktopAppInfo` with a
single argument (argv, no shell). So `Exec=` needs `%u`/`%U`, and a
`GApplication` needs `G_APPLICATION_HANDLES_OPEN`. You receive a
**canonical** URI that the dispatcher re-encodes itself. It is never the
string the user clicked:

- `nostr:note1…`, `nostr:nevent1…` (event; `nevent` carries a kind TLV when
  the dispatcher knows the kind, and at most 3 relay hints)
- `nostr:naddr1…` (addressable event)
- `nostr:npub1…` / `nostr:nprofile1…` (profiles; routed as kind `0`)

Relay hints are restricted to `wss://` (plus `ws://` to loopback). Query
strings and fragments are never forwarded. `nsec`/`ncryptsec` links are
refused outright, and `nrelay` links are rejected as unsupported.

### 3. Optional: take the event directly (`org.nostr.Handler2`)

If your app is **already running** when a link is clicked, and the
dispatcher already holds the event, the dispatcher hands the event to your
app instead of launching a URI, which saves you a second fetch. Implement
[`org.nostr.Handler2`](../dbus/org.nostr.Handler2.xml) on your
`GApplication` object:

```xml
<interface name="org.nostr.Handler2">
  <method name="OpenEvent">
    <arg name="kind" type="u" direction="in"/>
    <arg name="event_json" type="s" direction="in"/>
    <arg name="relays" type="as" direction="in"/>
    <arg name="platform_data" type="a{sv}" direction="in"/>
  </method>
</interface>
```

- Bus name: your desktop id without `.desktop`. Object path: the
  `GApplication` convention (`org.example.Reader.desktop` →
  `org.example.Reader` at `/org/example/Reader`).
- `event_json` has **always** passed canonical-id and Schnorr-signature
  validation. It is still untrusted content: signed, not vetted.
- `platform_data` is shaped like `org.freedesktop.Application`'s:
  `activation-token` (and the same value as `desktop-startup-id`) lets you
  present your window under Wayland focus-stealing prevention. Ignore
  unknown keys.
- The dispatcher calls `Handler2.OpenEvent` first. If your app answers
  UnknownMethod/UnknownInterface/UnknownObject, it calls the older
  [`org.nostr.Handler1`](../dbus/org.nostr.Handler1.xml) `OpenEvent(u s as)`
  instead (no activation token), so either interface works.
- The dispatcher never auto-starts you through these interfaces. On any
  other error or a 2 s timeout, it falls back to launching the URI.
- Any same-user session-bus peer can call them; do not expose privileged
  operations through them.

## For users: `handlers.list`

Defaults are overridden with `mimeapps.list`-style files. Highest
precedence first:

1. `$XDG_CONFIG_HOME/nostr/handlers.list` (`~/.config/nostr/handlers.list`)
2. `$XDG_CONFIG_DIRS/nostr/handlers.list` (e.g. `/etc/xdg/nostr/handlers.list`)
3. `$XDG_DATA_DIRS/nostr/handlers.list` (e.g. `/usr/share/nostr/handlers.list`, shipped empty)

```ini
[Default Handlers]
# kind (N), range (A-B) or * (fallback) = desktop ids, first installed wins
14=org.nostr.Groundhog.desktop;
1059=org.nostr.Groundhog.desktop;
30000-39999=org.example.Reader.desktop;
*=org.gnostr.gnostr.desktop;

[Removed Handlers]
# hide these apps' X-Nostr-Kinds declarations for the listed kinds (* = all)
1=org.example.NoisyApp.desktop;

[Dispatcher]
# false: never contact relays named inside a clicked link (privacy)
fetch-relay-hints=true
```

Or use the CLI, which mirrors `xdg-mime`:

```sh
nostr-dispatcher set-default 14 org.nostr.Groundhog.desktop
nostr-dispatcher query-default 14       # prints the effective handler
nostr-dispatcher query-default '*'      # handler for unknown kinds
nostr-dispatcher resolve nostr:nevent1… # dry run: kind, handler, source
nostr-dispatcher open nostr:nevent1…    # what xdg-open does
nostr-dispatcher open ~/Downloads/event.nostr
```

`set-default` edits the user file in place and keeps your comments and
other sections.

### Resolution order

For a link whose kind is known, first match wins:

1. `[Default Handlers]` in the handlers.list files, file by file. Within a
   file, the exact key beats the narrowest range, and equal ranges go by
   file order. The **first file with a usable entry wins outright**, so a
   user *range* beats a system *exact* key. Your configuration always wins,
   as with per-file precedence in `mimeapps.list`. Entries naming apps that
   are not installed are skipped.
2. `X-Nostr-Kinds=` declarations, minus `[Removed Handlers]`.
3. Fallback: `*` in `[Default Handlers]`, then apps declaring `*`.

NIP-89 is deliberately **not** a step: nothing discovered on the network is
ever chosen or launched automatically. See "NIP-89 suggestions" below.

The dispatcher's own desktop entry is never a candidate.

### How the kind is found

1. The link itself: `naddr` always carries a kind, `npub`/`nprofile` mean
   kind 0, and `nevent` *may* carry a kind TLV.
2. Otherwise (a `note1`, or a `nevent` without a kind), the event is fetched
   from the per-user session relay (`$XDG_RUNTIME_DIR/nostr/relay.sock`,
   about 1.5 s).
3. Then from the link's relay hints, via libnostr's relay client (4 s in
   total, at most 3 relays; skipped when `fetch-relay-hints=false`).
4. If every step fails, the fallback handler gets the link and fetches it
   itself.

A fetched event is used only if its id hash and signature verify **and** it
is what the link points at (same id; for `naddr`, same kind, author and `d`
tag). A hostile relay therefore cannot re-route a link by lying about its
kind.

When the kind is already known, nothing is fetched unless the chosen app
is running and could take the event over `org.nostr.Handler2`/`Handler1`.

## NIP-89 suggestions (kind 31990)

When no installed app handles a link's (known) kind and there is no `*`
fallback, `Open` fails with `NoHandler` as before. The service then looks
for NIP-89 *handler information* events (kind 31990 tagged `["k","<kind>"]`)
and **offers** the best one. It never opens one by itself:

- **Where it looks:** first the per-user session relay. Unless
  `fetch-relay-hints=false`, it then tries the user's NIP-65 **read** relays
  (from the user's kind 10002 on the session relay), else the relays
  configured in the signer (`org.nostr.Signer.GetRelays`), at most 4.
  The user's pubkey comes from `org.nostr.Signer.GetPublicKey` with
  `NO_AUTO_START`, so discovery never starts a signer. Without a signer it
  still searches the session relay.
- **Trust:** kind-31990 events are signed by anybody. Candidates are ranked
  by kind-31989 recommendations for that kind from the user and the people
  the user follows (kind 3, at most 250). The offer always names the web
  handler's **host**, and an unrecommended handler says "Nobody you follow
  recommends it: check the address before opening." Display names are
  sanitised (no control or bidi characters, 48 characters at most).
- **What is offered:** a notification with an "Open in <name> (<host>)"
  button. The link goes into the handler's `web` template
  (`https://…<bech32>…`); a template whose entity marker matches the link's
  entity (`naddr`, `nevent`, `nprofile`, …) wins over an unmarked one, and
  only `https://` URLs with a host and no userinfo are used. If the 31990
  carries `["flatpak","<app-id>"]` or `["linux","<app-id>"]`, a "Show in
  Software" button opens `appstream://<app-id>`. Clicking the banner itself
  does nothing.
- **Button safety:** each presented offer gets a random 128-bit token,
  stored single-use for 24 h under
  `$XDG_RUNTIME_DIR/nostr-dispatcher/nip89-offers/` (0600, so a click still
  works after the service has exited on idle). The buttons
  (`app.nip89-open`, `app.nip89-install`) carry only that token, so another
  program on the session bus calling `ActivateAction` cannot open anything
  that was not offered, not even another cached handler. The URL is then
  rebuilt from the cached, re-verified 31990 rather than stored. The host
  shown is the URL's parsed host. Look-alike (IDN) hosts are an accepted
  risk that the always-visible host and the recommendation count mitigate.
- **Cache:** `$XDG_CACHE_HOME/nostr-dispatcher/nip89/<kind>.json`, fresh
  for 24 h (1 h when nothing was found). Events are re-verified on load.
- **Switches:** `[Dispatcher] nip89-discovery=false` turns this off;
  `fetch-relay-hints=false` keeps it on the session relay.
- **From a terminal:** the in-process `open` prints the suggestions to
  stderr, and `nostr-dispatcher discover KIND [URI]` lists them. Neither
  opens anything.

The service's GApplication id is `org.nostr.Dispatcher` (the desktop id),
so GNOME Shell accepts its notifications and routes button clicks back
through D-Bus activation (`org.nostr.Dispatcher.service`). The API stays on
`org.nostr.Dispatcher1`, owned by the same process.

## Deep links from nostr-notify

The notification daemon (`gnome/nostr-homed/src/notify/`) now emits
standard `nostr:nevent1…` links. Each carries the event id, the kind TLV
(`1059` for DM gift wraps, `9`–`12` for NIP-29 group messages) and the
relay the event arrived on. Group links carry no `h` parameter: NIP-21
defines no query parameters, the `h` tag is inside the signed event, and
the relay hint is the group relay. The old invented forms
`nostr://open?event=…` and `nostr://open?group=…&event=…` were accepted
for one transition release and are now rejected as invalid URIs
(nostrc-prqu.7).

## Service and packaging

- `org.nostr.Dispatcher1` on the session bus at `/org/nostr/Dispatcher1`
  ([interface XML](../dbus/org.nostr.Dispatcher1.xml)) provides `Open`,
  `OpenEvent`, `Resolve` and `QueryDefault`.
- D-Bus activated through `nostr-dispatcher.service` (user unit,
  `Type=dbus`). It exits after 30 s idle, and nothing is enabled at install
  time. Apps it launches are moved into their own transient
  `app-nostr\x2ddispatcher-<app id>-<pid>.scope` on the user manager
  (`StartTransientUnit`, as gnome-shell does), so they neither share the
  dispatcher's cgroup nor get reaped with it; `NOSTR_DISPATCHER_NO_SCOPE=1`
  disables that. `KillMode=process` stays as the safety net if the move
  fails. The unit deliberately has no sandboxing directives, because
  launched apps inherit the unit's execution environment.
- `man 1 nostr-dispatcher`. libnostr's relay wire traces are debug-level
  and only shown with `NOSTR_LOG_LEVEL=debug`.
- `nostr-dispatcher open` forwards to the service and dispatches
  in-process only when no session bus or service is available (for
  example, headless). Event files are always handled in-process.
- Packages: Debian `nostr-dispatcher`, RPM `nostr-dispatcher`. Build with
  `-DENABLE_NOSTR_DISPATCHER=ON` (needs GLib ≥ 2.66 with `GDesktopAppInfo`,
  json-glib, libsoup-3 and libnostr). On platforms without
  `GDesktopAppInfo` (macOS), only the portable core and its tests build.
