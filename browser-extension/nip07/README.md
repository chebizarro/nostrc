# Nostr Signer Bridge — NIP-07 + WebLN browser extension

`window.nostr` ([NIP-07](https://github.com/nostr-protocol/nips/blob/master/07.md))
and `window.webln` ([WebLN](https://www.webln.guide/)) for Firefox and
Chromium, backed by the GNOME desktop signer **`org.nostr.Signer`** and the
desktop wallet agent **`org.nostr.Wallet1`** (`nostr-wallet-agent`). Web
clients (snort.social, iris.to, …) use the desktop-managed identity and
Lightning wallet with their own approval dialogs; the browser never sees a
secret key or the wallet's pairing secret.

```
page (window.nostr / window.webln, MAIN world)   src/page.js, src/providers/webln.js
  └─ window.postMessage (nonce id) ─▶    src/content.js      (isolated world)
       └─ runtime.sendMessage ─▶         src/background.js   (origin from MessageSender)
            └─ runtime.connectNative("org.nostr.signer_bridge")
                 └─ stdio frames ─▶      nostr-signer-webext-host
                      └─ D-Bus ─▶        org.nostr.Signer    (app_id = page origin)
                                         org.nostr.Wallet1   (*For methods, principal = page origin)
```

Bead: `nostrc-jjyp`. WebLN is described in [WebLN](#webln).

| API | Signer call | Who approves |
|-----|-------------|--------------|
| `getPublicKey()` → hex | `GetPublicKey` (npub → hex in the host) | extension prompt, per site ("read") |
| `getRelays()` → `{url: {read, write}}` | `GetRelays` (`NotFound` → `{}`) | extension prompt, per site ("read") |
| `signEvent(event)` → signed event | `SignEvent(event, identity, app_id=origin)` | **desktop signer** dialog, keyed on the origin |
| `nip04.encrypt/decrypt`, `nip44.encrypt/decrypt` | `NIP04*` / `NIP44*` | extension prompt, per site ("encrypt" / "decrypt") |

## Install

You need three things: the signer daemon (`grotto` / its
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
standalone `cmake -S apps/grotto/native-host -B build`), install
per-user manifests instead of the system ones:

```sh
apps/grotto/native-host/install.sh --host "$PWD/build/nostr-signer-webext-host" --all
#   --firefox          ~/.mozilla/native-messaging-hosts/
#   --flatpak-firefox  ~/.var/app/org.mozilla.firefox/.mozilla/native-messaging-hosts/
#   --chromium         ~/.config/chromium/NativeMessagingHosts/
#   --chrome           ~/.config/google-chrome/NativeMessagingHosts/
```

(Packaged: `/usr/share/nostr-signer-webext-host/install-user-manifests.sh`.)

### 2. Extension

```sh
browser-extension/nip07/build.sh          # add --check to run the tests first
# dist/nostr-signer-bridge-firefox-0.3.0.xpi   (unsigned)
# dist/nostr-signer-bridge-chromium-0.3.0.zip
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

**Epiphany (GNOME Web)** — not usable: Epiphany loads Firefox-style
WebExtensions, but it does not implement native messaging. Verified against
Epiphany 46.5 (Ubuntu 24.04, `nostrc-tumh`): the content-side shim
(`webextensions.js` in `libephywebprocessextension.so`) declares
`browser.runtime.connectNative` / `sendNativeMessage` and forwards them to the
UI process, but the UI process (`libephymain.so`) registers no handler for
either (its `runtime` handlers are `getBrowserInfo`, `getPlatformInfo`,
`openOptionsPage`, `sendMessage`, `setUninstallURL`), so the call is
answered with "'…' not implemented by Epiphany!"; the shim's
`connectNative` also returns a Promise where WebExtensions return a `Port`.
Every bridge call therefore rejects with `signer_unavailable`. Nothing on
the host side would change that; it needs native messaging in Epiphany
(ideally through the WebExtensions portal, as below) — tracked as a
follow-up bead.

### Flatpak Firefox

A sandboxed Firefox cannot exec `/usr/libexec/...` and reads manifests from
its own home, `~/.var/app/org.mozilla.firefox/.mozilla/native-messaging-hosts/`.
Two routes:

1. **WebExtensions portal (the supported route).** Firefox asks
   `xdg-desktop-portal` (`org.freedesktop.portal.WebExtensions`) to start the
   host *outside* the sandbox from the host's system manifests (above); in
   `about:config` set `widget.use-xdg-desktop-portal.native-messaging = 1`.
   The portal asks once ("Allow Firefox to start WebExtension backend?",
   remembered in the permission store table `webextensions`; `flatpak
   permission-set webextensions org.nostr.signer_bridge org.mozilla.firefox
   yes` pre-answers it). The host then runs unsandboxed, as a child of the
   portal, with the session bus — so the wallet agent identifies it as the
   browser bridge (exe path + inode) exactly as with a distro Firefox, and
   per-site WebLN works; the signer treats it the same way.

   Status (`nostrc-tumh`, aarch64 Ubuntu 24.04, Flathub Firefox 156.0.1,
   xdg-desktop-portal 1.18.4-1ubuntu2 — Ubuntu carries the WebExtensions
   portal; upstream it is newer than 1.18): with the extension loaded
   (`web-ext run --firefox=flatpak:org.mozilla.firefox`) Firefox made the
   portal calls `CreateSession` → `GetManifest("org.nostr.signer_bridge",
   "signer-bridge@gnostr.org")` → `Start` → `GetPipes`, and the portal
   started `/usr/libexec/nostr-signer-webext-host` outside the sandbox (in
   the session scope, with the right bus address). The round trip did **not**
   complete: right after `GetPipes` Firefox logged
   `subprocess_unix.worker.js: Error: Invalid process ID: 0`, no frame
   reached the wallet agent and `window.webln` was not injected. Where that
   breaks (Firefox's portal client vs. this portal version) is still open —
   follow-up bead. `window.nostr` was injected and the extension's own
   permission prompt worked inside Flatpak Firefox.
2. **In-sandbox host.** Install the manifest with `install.sh
   --flatpak-firefox --host <path visible inside the sandbox>` and grant bus
   access: `flatpak override --user --talk-name=org.nostr.Signer
   --talk-name=org.nostr.Wallet1 org.mozilla.firefox`. The host binary and
   its GLib/json-glib dependencies must be resolvable inside the Firefox
   runtime, which is not generally true for a distro-built binary. Even
   then the daemons see the caller as the Flatpak `org.mozilla.firefox`:
   the wallet agent refuses per-site (`*For`) calls from any Flatpak caller
   by design (a sandboxed process must not assert web origins), so WebLN
   answers `rejected`, and every site shares Firefox's identity. Prefer the
   portal.

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
  access, no key material, core dumps disabled (`RLIMIT_CORE=0`); the
  protocol stream is moved off fd 1 so stray logging cannot corrupt it. It
  stays *dumpable* on purpose: the wallet agent verifies the host's
  `/proc/<pid>/exe` before accepting a web origin, which `PR_SET_DUMPABLE=0`
  would make unreadable (every WebLN call would be refused).

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
  NIP-07 (and WebLN).
* **WebLN origins.** The wallet agent trusts the origin because only the
  installed host binary may assert one (it checks the caller's executable
  by path and inode); the host trusts it because it comes from this
  extension, which takes it from the browser. A local process running as
  you can exec the host and claim any site's wallet budget — the same
  same-user trust limit as the agent's unsandboxed app budgets (see the
  agent README, "Web origins"). Sandboxed (Flatpak/Snap) apps cannot.
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
none for `getPublicKey` / `getRelays`; `webln.makeInvoice {amount,
defaultAmount, minimumAmount, maximumAmount, defaultMemo}` (all optional,
sats as integers or digit strings), `webln.sendPayment {paymentRequest}`,
none for the other `webln.*`. `host.hello` and `webln.status` need no
origin; `host.hello` returns `{host, version, protocol, providers}`. At most 16 requests may be in flight.

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
  `apps/grotto/native-host/CMakeLists.txt` and `install.sh`).

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

## WebLN

`window.webln` is served by the desktop wallet agent
[`nostr-wallet-agent`](../../gnome/nostr-wallet-agent/README.md)
(`org.nostr.Wallet1`, a NIP-47 Nostr Wallet Connect pairing managed by
GNOME). Install `nostr-wallet-agent` alongside the native host and pair a
wallet there (click a `nostr+walletconnect://` link); the extension itself
stores nothing about the wallet.

| WebLN | Host method | Wallet1 call | Result | Who approves |
|---|---|---|---|---|
| (injection) | `webln.status` (no origin) | `Introspect` + `Paired` property | `{available, paired}` | nobody — never prompts |
| `enable()` | `webln.enable` | (same probe) | resolves `undefined` | **extension prompt**, per site (`webln` grant) |
| `getInfo()` | `webln.getInfo` | `GetInfoFor(origin)` | `{node: {alias, pubkey?, color?}, methods, supports: ["lightning"], version}` | wallet agent ("wallet access", per site) |
| `getBalance()` | `webln.getBalance` | `GetBalanceFor(origin)` | `{balance, currency: "sats"}` (whole sats, rounded down) | wallet agent ("wallet access", per site) |
| `makeInvoice(args)` | `webln.makeInvoice` | `MakeInvoiceFor(origin, msat, memo, 0)` | `{paymentRequest, rHash}` | wallet agent ("wallet access", per site) |
| `sendPayment(bolt11)` | `webln.sendPayment` | `PayInvoiceFor(origin, bolt11, 0)` | `{preimage}` | **wallet agent**: its payment dialog, or this site's daily budget — never the extension |
| `keysend`, `signMessage`, `verifyMessage`, `lnurl` | — | — | rejects `unsupported` (page-side) | — |

Also: `isEnabled()`, the `enabled` getter, and the `webln:ready` (after
injection) / `webln:enabled` events on `window`.

* **Injected only with a paired wallet.** At document start the content
  script asks the background, which asks the host for `webln.status`
  (cached 30 s, one query in flight): only when the agent is reachable,
  supports per-site calls and has a wallet paired does `src/providers/webln.js`
  define `window.webln` (and dispatch `webln:ready`). Otherwise
  `window.webln` stays undefined and sites fall back to QR codes / other
  flows. An existing `window.webln` from another provider is never replaced.
* **`enable()` is the extension's consent gate.** First use shows the same
  prompt window as NIP-07 ("use your desktop Lightning wallet"). *Remember*
  stores a `webln` grant for 30 days; without it the site stays enabled for
  1 hour. Grants are listed and revocable on the options page (which also
  shows whether a wallet is paired). Every other WebLN call needs a valid
  grant and otherwise rejects `not_enabled`. `enable()` checks the wallet
  before prompting, so a site is never asked about a wallet that isn't there.
* **The wallet agent approves the rest, per site.** The host calls the
  agent's `*For` methods with the page origin; the agent only accepts that
  from the installed `nostr-signer-webext-host` binary (path + inode) and
  then treats the origin as the application: `getInfo` / `getBalance` /
  `makeInvoice` raise its "wallet access" dialog until you tick *Always
  allow this site*, and `sendPayment` is paid automatically only within a
  daily budget you gave **that site** in the payment dialog (*Always allow up
  to N sats/day*) — otherwise you are asked every time. Budgets are never
  shared between sites or with the browser. The extension never approves a
  payment.
* **Amounts** are sats in WebLN and msat (`uint32`) at the agent: at most
  **4 294 967 sats per call**; more rejects `too_large`. `makeInvoice` takes
  a number, a numeric string or `{amount | defaultAmount | minimumAmount,
  maximumAmount, defaultMemo}` (the amount is `amount`, else
  `defaultAmount`, else a non-zero `minimumAmount`, and must lie within
  min…max); amount-less invoices are not supported (`invalid_request`).
  `defaultMemo` ≤ 639 bytes.
* **Timeouts.** Wallet calls may wait for the agent's dialog (120 s) plus the
  wallet (60 s): the host allows 190 s, the extension 195 s. A `timeout` on
  `sendPayment` means **outcome unknown** — the payment may still have been
  made; don't blindly retry.
* **Iframes** may use WebLN; the prompt names the embedding page, and the
  agent sees the iframe's own origin.

WebLN error codes (in addition to the NIP-07 ones below):

| code | meaning | from `org.nostr.Wallet1` |
|------|---------|--------------------------|
| `not_enabled` | call `webln.enable()` first (no valid grant) | — |
| `not_paired` | no wallet is paired with the desktop agent | `Error.NotPaired`, `GetInfo{paired:false}` |
| `wallet_unavailable` | agent not installed/running, or no relay reached the wallet | `ServiceUnknown`, `Error.RelayError` |
| `rejected` | you declined in the agent's dialog (or policy) | `Error.Denied`, NIP-47 `RESTRICTED` |
| `budget_exceeded` | over the site's daily budget and no dialog possible | `Error.BudgetExceeded` |
| `wallet_error` | the wallet refused (message starts with the NIP-47 code, e.g. `[INSUFFICIENT_BALANCE]`) | `Error.WalletError` |
| `unsupported` | not offered (keysend, signMessage, …), wallet lacks the method (`[NOT_IMPLEMENTED]`), or agent too old | `Error.Unsupported`, `UnknownMethod` |
| `invalid_request` / `too_large` | bad arguments / amount over 4 294 967 sats | `Error.InvalidArgs` (message passed on, e.g. "invoice has expired") |
| `timeout` | no answer in time (payments: outcome unknown) | `Error.Timeout` |
| `rate_limited` | too many pending wallet dialogs | `Error.RateLimited` |

Not supported yet: `keysend` (not exposed by the agent), LNURL / Lightning
addresses (agent: `nostrc-prqu.8`), `signMessage`/`verifyMessage` (not a
NIP-47 capability).

## Tests

* `node tests/policy.test.js` — origin derivation, validation, permission
  categories, page.js API shape and reply filtering.
* `node tests/webln.test.js` — sats parsing and the 4 294 967-sat limit,
  `makeInvoice` argument resolution, WebLN request validation / gates /
  timeouts, the `window.webln` shape, unsupported methods, error codes, and
  conditional injection (webln.js and content.js in a VM: nothing is
  injected unless the host reports a paired wallet).
* Host: `ctest -L nostr-signer-webext-host` — framing round-trip, origin
  policy, malformed events, D-Bus → NIP-07 error mapping, router, and
  `test_nmh_e2e` driving the real host against the real
  `nostr-signer-daemon` on a private bus, and `test_nmh_webln_e2e` driving
  it against the real `nostr-wallet-agent` (see
  `apps/grotto/native-host/README.md`).
