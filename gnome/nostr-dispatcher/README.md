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
3. NIP-89 recommendations (kind 31990). This is a **hook only** today; see
   nostrc-prqu.1.
4. Fallback: `*` in `[Default Handlers]`, then apps declaring `*`.

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
