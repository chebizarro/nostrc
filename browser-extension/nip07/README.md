# Nostr Signer Bridge — NIP-07 browser extension

`window.nostr` ([NIP-07](https://github.com/nostr-protocol/nips/blob/master/07.md))
for Firefox and Chromium, backed by the GNOME desktop signer
**`org.nostr.Signer`**. Web clients (snort.social, iris.to, …) use the
desktop-managed identity and the signer's own approval dialogs; the browser
never sees a secret key.

```
page (window.nostr, MAIN world)          src/page.js
  └─ window.postMessage (nonce id) ─▶    src/content.js      (isolated world)
       └─ runtime.sendMessage ─▶         src/background.js   (origin from MessageSender)
            └─ runtime.connectNative("org.nostr.signer_bridge")
                 └─ stdio frames ─▶      nostr-signer-webext-host
                      └─ D-Bus ─▶        org.nostr.Signer    (app_id = page origin)
```

Bead: `nostrc-jjyp` (NIP-07 half). The WebLN half waits on
`nostr-wallet-agent` / `org.nostr.Wallet1` (`nostrc-yka8`) — see
[WebLN](#webln-not-yet).

| API | Signer call | Who approves |
|-----|-------------|--------------|
| `getPublicKey()` → hex | `GetPublicKey` (npub → hex in the host) | extension prompt, per site ("read") |
| `getRelays()` → `{url: {read, write}}` | `GetRelays` (`NotFound` → `{}`) | extension prompt, per site ("read") |
| `signEvent(event)` → signed event | `SignEvent(event, identity, app_id=origin)` | **desktop signer** dialog, keyed on the origin |
| `nip04.encrypt/decrypt`, `nip44.encrypt/decrypt` | `NIP04*` / `NIP44*` | extension prompt, per site ("encrypt" / "decrypt") |

## Install

You need three things: the signer daemon (`gnostr-signer` / its
`nostr-signer-daemon`, owning `org.nostr.Signer` on the session bus), the
native host, and the extension.

### 1. Native host

Debian/Ubuntu: `sudo apt install nostr-signer-webext-host` — Fedora:
`sudo dnf install nostr-signer-webext-host`. Either installs

* `/usr/libexec/nostr-signer-webext-host`
* Firefox: `/usr/lib/mozilla/native-messaging-hosts/org.nostr.signer_bridge.json`
* Chromium: `/etc/chromium/native-messaging-hosts/org.nostr.signer_bridge.json`
* Google Chrome: `/etc/opt/chrome/native-messaging-hosts/org.nostr.signer_bridge.json`

Each manifest is pinned to this extension (`allowed_extensions` /
`allowed_origins`, see [Extension ids](#extension-ids)). The package does
**not** install the extension itself.

From a source build (`-DENABLE_NOSTR_SIGNER_WEBEXT_HOST=ON` or the
standalone `cmake -S apps/gnostr-signer/native-host -B build`), install
per-user manifests instead of the system ones:

```sh
apps/gnostr-signer/native-host/install.sh --host "$PWD/build/nostr-signer-webext-host" --all
#   --firefox          ~/.mozilla/native-messaging-hosts/
#   --flatpak-firefox  ~/.var/app/org.mozilla.firefox/.mozilla/native-messaging-hosts/
#   --chromium         ~/.config/chromium/NativeMessagingHosts/
#   --chrome           ~/.config/google-chrome/NativeMessagingHosts/
```

(Packaged: `/usr/share/nostr-signer-webext-host/install-user-manifests.sh`.)

### 2. Extension

```sh
browser-extension/nip07/build.sh          # add --check to run the tests first
# dist/nostr-signer-bridge-firefox-0.2.0.xpi   (unsigned)
# dist/nostr-signer-bridge-chromium-0.2.0.zip
```

**Firefox (≥ 140, incl. ESR 140)** — `about:debugging#/runtime/this-firefox` → *Load
Temporary Add-on…* → pick `manifest.json` in an unpacked copy (or the
`.xpi`). Temporary add-ons vanish on restart; for a permanent install either
use Developer Edition / Nightly / ESR with
`xpinstall.signatures.required=false` and *Install Add-on From File…*, or get
the `.xpi` signed through AMO (unlisted signing is enough; AMO listing is a
later step). Then in `about:addons` → the extension → *Permissions*, make sure
site access is granted (Firefox treats MV3 host access as opt-in).

**Chromium / Chrome (≥ 111)** — `chrome://extensions` → *Developer mode* →
*Load unpacked* → a directory with the zip's contents. The manifest carries a
`key`, so the id is always `ljigikpdhlameofnnalkmjnbeagdhbin`, which is what
the host manifest allows.

**Epiphany (GNOME Web)** — Epiphany loads Firefox-style WebExtensions, but its
WebExtension layer does not currently implement `runtime.connectNative`
(native messaging), so the bridge cannot reach the host there: every call
rejects with `signer_unavailable`. Nothing to install on the host side
beyond the package above; revisit when WebKit/Epiphany ship native messaging.

### Flatpak Firefox

A sandboxed Firefox cannot exec `/usr/libexec/...` and reads manifests from
its own home, `~/.var/app/org.mozilla.firefox/.mozilla/native-messaging-hosts/`.
Two routes:

1. **WebExtensions portal (preferred).** Recent Firefox Flatpaks can ask
   `xdg-desktop-portal` (`org.freedesktop.portal.WebExtensions`) to launch the
   host *outside* the sandbox, using the host-side system manifests above. In
   `about:config` set `widget.use-xdg-desktop-portal.native-messaging = 1`
   (needs a portal backend with that interface). The host then runs unsandboxed
   and reaches `org.nostr.Signer` normally.
2. **In-sandbox host.** Install the manifest with `install.sh
   --flatpak-firefox --host <path visible inside the sandbox>` and grant bus
   access: `flatpak override --user --talk-name=org.nostr.Signer
   org.mozilla.firefox`. The host binary and its GLib/json-glib dependencies
   must be resolvable inside the Firefox runtime, which is not generally true
   for a distro-built binary.

Neither route is exercised by CI yet: `nostrc-tumh` (also covers Epiphany).

## Security model

* **Who is asking is decided by the browser, not the page.** The background
  derives the origin from the `runtime.MessageSender` of the content script
  (`sender.origin` on Chromium, the frame URL on Firefox) — the page's message
  carries no origin, and a page-supplied one would be ignored. Messages
  without a tab `frameId` are refused. Iframes are identified by their own
  origin; the prompt additionally names the top-level page embedding them,
  and **decryption is refused from embedded frames** altogether.
* **Secure origins only.** `https://…`, or `http://` on `localhost`,
  `*.localhost`, `127.0.0.1`, `[::1]`. Everything else (`http://` sites,
  `file:`, `data:`, sandboxed/opaque `null` origins, extension pages) is
  refused with `origin_denied` — in the extension and again in the host.
* **app_id = the browser-serialized origin.** The host accepts the origin only
  in the browser's own canonical form (lowercase, no default port, no
  trailing `/`, punycode) — it validates, it never re-canonicalizes, so the
  key the extension gated on and the signer's ACL key cannot drift — and
  passes it verbatim (`https://snort.social`) as `SignEvent`'s `app_id`.
  The signer's `ApprovalRequested` dialog therefore names the site, and its
  remembered decisions (`~/.config/gnostr/signer-acl.ini`) are per site.
  Web app_ids always start with `https://` / `http://`, so they cannot
  collide with a desktop application's reverse-DNS app_id.
* **Signing is approved by the desktop signer.** The extension does not
  prompt for `signEvent`; the signer shows its own dialog (or applies a
  remembered per-site decision).
* **Everything else is gated here.** `org.nostr.Signer` does not take an
  app_id for `GetPublicKey`, `GetRelays` or `NIP04*`/`NIP44*`, and raises no
  dialog for them. The extension therefore asks per site in its own prompt
  window (categories *read*, *encrypt*, *decrypt*). "Remember" stores an
  expiring grant in `storage.local` — 30 days for read/encrypt, **24 hours for
  decrypt** — listed and revocable on the options page. Decryption is its
  own category so a site allowed to read your pubkey cannot silently read
  your DMs. Moving this gate into the signer is `nostrc-1e31`.
* **Channel hygiene.** Page ↔ content messages must come from the same
  window (`event.source === window`), carry the channel/direction markers and
  a per-request random id that the reply must echo. That id is only for
  correlation — the page is the requesting principal, so nothing it can
  forge on that channel grants it anything. The background assigns its own
  ids towards the host, so two tabs cannot collide or answer for each other.
  Prompt answers and grant edits are accepted only from extension pages.
* **Validation twice.** Unsigned events must be `{kind: int 0..65535,
  created_at: int ≥ 0 (defaulted to now), tags: string[][], content: string}`;
  only those fields are forwarded (`id`/`sig`/extras dropped). Pubkeys are
  64-hex. Strings containing NUL are refused (they would be truncated on the
  way to D-Bus). The host applies the same rules independently.
* **The signer's answer is checked.** The host verifies that the signed
  event it gets back has the requested kind/created_at/tags/content and that
  its `id` is the NIP-01 hash of those fields before handing it to the page.
* **The host holds no secrets.** It is a stateless forwarder: no keyring
  access, no key material, core dumps disabled; the protocol stream is moved
  off fd 1 so stray logging cannot corrupt it.

### Out of scope / trust assumptions

* **Other processes of the same user.** `allowed_extensions` /
  `allowed_origins` only control which *browser extension* may launch the
  host; any local process can exec it — or, more directly, call
  `org.nostr.Signer` itself. The daemon does not authenticate `app_id`, so a
  local process can claim `https://snort.social` (inheriting that site's
  remembered `SignEvent` decisions and naming it in the dialog), and it can
  call `NIP04Decrypt`/`NIP44Decrypt` with no dialog at all. The bridge adds
  no capability here; binding `app_id` to the D-Bus caller and gating
  decrypt/encrypt/`GetPublicKey` in the daemon is `nostrc-1e31`
  (nip55l). The extension's grants are a browser-side consent layer, not a
  boundary against local code.
* **Other extensions** running MAIN-world scripts share the page's world and
  can observe or race the page ↔ content channel; that is inherent to
  NIP-07.
* **Identity selector changes are trust changes.** The host forwards the
  empty identity selector unless `--identity` / `NOSTR_SIGNER_BRIDGE_IDENTITY`
  is set; the signer's ACL keys include the selector, so changing it
  activates a different set of remembered decisions.

## Wire protocol (extension ↔ host)

Native messaging frames: 32-bit little-endian length + UTF-8 JSON, ≤ 1 MiB
each way.

```jsonc
// request
{"id": "b17", "method": "signEvent", "origin": "https://snort.social",
 "params": {"event": {"kind": 1, "created_at": 1700000000, "tags": [], "content": "gm"}}}
// success
{"id": "b17", "result": {"id": "…", "pubkey": "…", "created_at": 1700000000, "kind": 1, "tags": [], "content": "gm", "sig": "…"}}
// failure
{"id": "b17", "error": {"code": "rejected", "message": "The user rejected the request"}}
```

`id` is 1–64 printable ASCII (integers are accepted and echoed as strings);
replies may arrive out of order. Frames that cannot be attributed to a
request get `"id": null`. Params: `signEvent {event}`,
`nip04/nip44.encrypt {pubkey, plaintext}`, `…decrypt {pubkey, ciphertext}`,
none for `getPublicKey` / `getRelays`. `host.hello` (no origin) returns
`{host, version, protocol, providers}`. At most 16 requests may be in flight.

### Error codes

Promises from `window.nostr` reject with an `Error` whose `.code` is one of:

| code | meaning | from `org.nostr.Signer` |
|------|---------|-------------------------|
| `rejected` | user or remembered policy said no | `Error.ApprovalDenied`, `Error.PermissionDenied` |
| `timeout` | nobody answered (host: 30 s, 120 s for signEvent/decrypt) | D-Bus `NoReply` / timeout |
| `signer_unavailable` | signer not running, host missing/exited | `ServiceUnknown`, `NameHasNoOwner` |
| `no_key` | signer has no identity | `Error.NoKeyConfigured` |
| `rate_limited` | signer throttled | `Error.RateLimited` |
| `invalid_request` | malformed method params / event | `Error.InvalidInput` |
| `origin_denied` | not a secure origin | — |
| `unknown_method`, `unsupported`, `busy`, `too_large`, `internal` | as named | `UnknownMethod` → `unsupported`; `Error.Internal` → `internal` |

## Extension ids

* Firefox: `signer-bridge@gnostr.org` (`browser_specific_settings.gecko.id`).
* Chromium: `ljigikpdhlameofnnalkmjnbeagdhbin`, derived from the public key in
  `manifest.chromium.json` → `key` (first 32 hex digits of SHA-256 of the
  DER key, mapped `0-f` → `a-p`). Only the public half is in the tree;
  publishing on the Chrome Web Store needs the matching private key or a
  new key + id (update `NMH_CHROMIUM_EXTENSION_ID` in
  `apps/gnostr-signer/native-host/CMakeLists.txt` and `install.sh`).

## Known limitations

* Chromium MV3 service workers / Firefox event pages may be suspended while a
  prompt window is open for a long time; an unanswered request then rejects
  (`signer_unavailable` / `timeout`) and the page can retry.
* Closing the tab does not withdraw a pending signer dialog.
* A `timeout` on `signEvent` means *unknown outcome*: the user may still
  approve the dialog after the page gave up. Clients should not blindly
  retry (that would raise a second dialog).
* `web-ext lint` reports one Android-only warning; Firefox for Android has no
  native messaging, so Android is not a target.
* The signer's "remember" for `SignEvent` currently stores the decision
  under the resolved npub while lookups use the empty identity selector, so a
  remembered per-site decision is not yet honoured (signer bug
  `nostrc-eie5`).

## WebLN (not yet)

`window.webln` is intentionally not injected. The host already routes
`webln.*` to `apps/gnostr-signer/native-host/nm_provider_webln.c`, a
documented stub answering `unsupported`; `src/providers/webln.js` holds the
page-side object and the enablement checklist. Both are waiting on
`org.nostr.Wallet1` (`nostrc-yka8`).

## Tests

* `node tests/policy.test.js` — origin derivation, validation, permission
  categories, page.js API shape and reply filtering.
* Host: `ctest -L nostr-signer-webext-host` — framing round-trip, origin
  policy, malformed events, D-Bus → NIP-07 error mapping, router, and
  `test_nmh_e2e` driving the real host against the real
  `nostr-signer-daemon` on a private bus (see
  `apps/gnostr-signer/native-host/README.md`).
