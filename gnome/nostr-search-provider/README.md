# nostr-search-provider

A GNOME Shell search provider (`org.gnome.Shell.SearchProvider2`) for Nostr.
When you type an identifier or keywords into the Activities overview, it
returns matching profiles, notes and long-form articles from the **local**
per-user session relay (`$XDG_RUNTIME_DIR/nostr/relay.sock`). Activating a
result opens it through [`nostr-dispatcher`](../nostr-dispatcher/README.md),
so the kind→application registry decides which app handles it.

| | |
|---|---|
| Bus name / object | `org.nostr.SearchProvider` at `/org/nostr/SearchProvider` |
| Shell registration | `/usr/share/gnome-shell/search-providers/org.nostr.SearchProvider.ini` (`Version=2`) |
| Activation | D-Bus → `nostr-search-provider.service` (user unit, `Type=dbus`); exits after 60 s idle |
| Package | Debian `nostr-search-provider`, RPM `nostr-search-provider`; CMake `-DENABLE_NOSTR_SEARCH_PROVIDER=ON` |

## What you can type

| Input | Class | Session-relay REQ filter(s) |
|---|---|---|
| `npub1…`, `nprofile1…` (bare, `nostr:` or `web+nostr:`) | profile | `{"kinds":[0],"authors":[pk],"limit":1}` |
| `note1…`, `nevent1…` | event | `{"ids":[id],"limit":1}`, then the author's kind 0 |
| `naddr1…` | address | `{"kinds":[k],"authors":[pk],"#d":[d],"limit":1}` |
| 64 hex characters | hex | `{"ids":[h],"limit":1}` + `{"kinds":[0],"authors":[h],"limit":1}` |
| `alice@example.com` | NIP-05 | resolved pubkey → the profile filter; plus local claims `{"kinds":[0],"search":"alice@example.com","limit":20}` (exact `nip05` match only) |
| anything else, ≥ 3 characters | text | NIP-50 `{"kinds":[0,1,30023],"search":"…","limit":40}`; on `CLOSED "unsupported: search"` a bounded scan `{"kinds":[0],"limit":250}` + `{"kinds":[1,30023],"limit":250}` matched client-side (every word, case-folded) |
| `nsec1…` / `ncryptsec1…` anywhere, partial identifiers | none | nothing is sent anywhere |

Notes without a known author get one follow-up `{"kinds":[0],"authors":[…]}`
REQ if the deadline allows. Every event is id- and signature-validated
before use. The relay allows one subscription per connection, so each REQ
uses its own connection.

A NIP-05 address is only treated as one when its domain is publicly
resolvable (has an alphabetic TLD and is not an IP literal, `.local`,
`.localhost`, `.internal`, `.lan`, `.home.arpa` or `.onion`). This keeps the
overview from probing half-typed or private names.

## Deadline and degradation

Every `GetInitialResultSet` / `GetSubsearchResultSet` call returns within
**800 ms** (`NOSTR_SEARCH_PROVIDER_DEADLINE_MS`), with partial results if
necessary:

* **No relay socket**: identifier queries still return an actionable
  *Open Nostr profile / note / article* result that the dispatcher can
  open. Text queries return nothing. Both answer immediately.
* **Relay without NIP-50**: falls back to the bounded scan described above.
* **Relay that never answers**: the packaged session relay is built with
  nostrdb **off** (`-DWITH_NOSTRDB=OFF`), runs cache-less, and answers a
  plain REQ with no `EVENT` and **no `EOSE`**. The first such search waits
  for the deadline. After that, a circuit breaker skips the relay for 30 s,
  so later keystrokes answer instantly. Identifier queries keep returning
  the bare *Open …* result. The relay is retried after 30 s. One timeout
  trips the breaker if the relay has never sent EOSE; three consecutive
  timeouts trip it otherwise.

**Subsearch** (the Shell refining its terms): free-text queries are first
narrowed from the previous result ids with no I/O. A fresh search runs
only if nothing survives, or if the previous set was empty. Identifier
queries are exact look-ups and always run fresh.

## Results

The result id is the item's canonical NIP-21 URI (`nostr:npub1…`,
`nostr:nevent1…` with kind and author TLVs, `nostr:naddr1…`). It is also
the meta's `clipboardText`, and it lets `ActivateResult` work without any
state.

| | `name` | `description` |
|---|---|---|
| profile | display name, else name, else short npub | `✓ nip05 · npub1abc…xyz` (`✓` only if the NIP-05 resolved to this key this session) |
| note (kind 1) | `author · 3 h ago` | first 80 code points of the content |
| article (kind 30023) | `author · 2 d ago` | title, else summary, else content |
| bare identifier | `Open Nostr profile/note/article` | `nip05 ·` short bech32 |

**Escaping contract.** gnome-shell renders `name` as plain text. It passes
`description` (first line only) through its `Highlighter`, which calls
`GLib.markup_escape_text()` itself (verified in gnome-shell 46,
`js/misc/util.js`). Strings are therefore **not** markup-escaped here, which
would show `&amp;`. They are *sanitized* instead: valid UTF-8, a single
line, no C0/C1 controls, no bidi overrides or zero-width characters,
whitespace collapsed, truncated by code points with `…`.

**Icons.** The `icon` field is a serialized `GIcon`: a cached avatar if
there is one, else `avatar-default-symbolic` / `text-x-generic-symbolic`.
`GetResultMetas` never waits on the network. A cache miss queues a
background download, and the avatar shows up the next time the profile
does. Downloads are decoded **in this process** and re-encoded as 64×64
PNGs under `$XDG_CACHE_HOME/nostr-search/avatars/` (at most 8 MiB and 2000
files, oldest pruned first). GNOME Shell only ever decodes a small PNG we
wrote, never attacker-supplied image bytes.

## Activation

* `ActivateResult(id, …)` re-parses the id (refusing anything that is not a
  `nostr:` URI, including `nsec`) and calls
  `org.nostr.Dispatcher1.Open(uri, {})`. If nostr-dispatcher is not
  installed, the `nostr:` scheme default is launched instead.
* `LaunchSearch` (clicking the provider icon) is a **no-op**. No Nostr
  application declares a search entry point yet; GNostr has no search
  action.
* `XUbuntuCancel` (Ubuntu's Shell) finishes in-flight searches early.

### Why the desktop entry is visible

gnome-shell ignores a search provider whose `DesktopId` fails
`GDesktopAppInfo.should_show()` (`js/ui/remoteSearch.js`), and
`NoDisplay=true` makes it fail. `org.nostr.SearchProvider.desktop`
therefore stays visible. It names the provider ("Nostr") in the overview
and in *Settings → Search*. Launched from the app grid,
`nostr-search-provider launch` opens nostr-dispatcher's fallback (`*`)
handler, which is your general Nostr client.

## Network and privacy

Searching reads only the local relay. Two features touch the network, and
both can be switched off:

```ini
# ~/.config/nostr/search-provider.conf
[Network]
resolve-nip05=true   # GET https://<domain>/.well-known/nostr.json?name=<local>
fetch-avatars=true   # download kind-0 "picture" (https only)
```

* NIP-05 look-ups start only after the query has stayed unchanged for
  250 ms, so intermediate keystrokes are never looked up. They use the
  NIP-05 grammar, the `.well-known` parser and the TTL cache shared with
  nostr-homed (`gnome/nostr-homed/src/nip05/`). They follow no redirects
  (as NIP-05 requires), read at most 64 KiB and time out after 5 s. A
  look-up keeps running after the search deadline so the next keystroke
  finds it cached.
* Every request is `https://` only. At TCP-connect time, before TLS or any
  request byte, the peer must be a public unicast address: loopback, RFC
  1918, CGNAT, link-local, ULA, multicast, documentation and IPv4-embedding
  IPv6 ranges are refused. A hostile profile therefore cannot make the
  provider probe your LAN. Behind an HTTP proxy on a private address,
  fetches are refused.
* `NOSTR_SEARCH_PROVIDER_NO_NETWORK=1` disables both features.

## Debugging

```sh
nostr-search-provider search alice@example.com   # one in-process search: ids, names, descriptions
nostr-search-provider search 'hello world'
gdbus call --session --dest org.nostr.SearchProvider \
  --object-path /org/nostr/SearchProvider \
  --method org.gnome.Shell.SearchProvider2.GetInitialResultSet "['npub1…']"
journalctl --user -u nostr-search-provider   # G_MESSAGES_DEBUG=all for per-search timing
```

`NOSTR_SEARCH_PROVIDER_SOCKET` overrides the relay socket path.

## Tests

`ctest -L nostr-search-provider` runs without network:

* `query`: classification of every input form, secret refusal, filter JSON.
* `meta`: sanitizing (no escaping), truncation, relative time, profile /
  note / article / bare metas, forged-event rejection.
* `net`: public-address policy, https-only refusal, NIP-05 domain rules and
  cache, avatar cache pruning and PNG transcoding.
* `engine`: a mock session relay on a private AF_UNIX socket. Covers
  identifier look-ups, NIP-50 and scan fallback, a relay that never answers
  (deadline plus breaker), partial results without EOSE, a missing socket,
  subsearch narrowing, NIP-05 and cancellation.
* `dbus`: the real daemon on a private bus (`GTestDBus`) with a fake
  `org.nostr.Dispatcher1`. Skipped without `dbus-daemon`.
