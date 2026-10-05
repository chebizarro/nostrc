# nostr-wallet-agent — `org.nostr.Wallet1`

A session D-Bus service that owns the desktop's one **Nostr Wallet Connect**
(NIP-47) pairing. Applications never see the pairing secret: they call
`org.nostr.Wallet1`, the agent identifies them from their bus credentials,
charges a per-application daily budget, and asks the user with a libadwaita
dialog whenever a request falls outside it. It also makes `lightning:`,
`bitcoin:` (BIP-21) and `nostr+walletconnect:` links work desktop-wide.

This is the same move the signer daemon (`org.nostr.Signer`) made for Nostr
identity keys, applied to the wallet (bead `nostrc-yka8`).

## Components

| Path | What |
|---|---|
| `src/nwa-nwc.c` | NIP-47 client: kind 23194 requests, 23195 responses, 13194 info, 23196/23197 notifications; NIP-44 v2 or NIP-04 per the wallet's `encryption` tag. Relay I/O via `libnostr-publish`'s `NostrPublishTransport` (libsoup-3 WebSocket, reconnect, NIP-42 AUTH). |
| `src/nwa-caller.c` | Caller identification from `GetConnectionCredentials` (+ pidfd) and `/proc`. |
| `src/nwa-policy.c` | Pure approval decision matrix. |
| `src/nwa-budget.c` | Per-app daily budgets + spend ledger (JSON). |
| `src/nwa-bolt11.c`, `src/nwa-uri.c` | BOLT-11 decoding; scheme URI parsing. |
| `src/nwa-service.c`, `src/nwa-ui.c`, `src/main.c` | D-Bus glue, dialogs, GApplication. |
| `../dbus/org.nostr.Wallet1.xml` | Interface definition (installed to `dbus-1/interfaces`). |
| `data/` | systemd user unit, D-Bus activation file, `.desktop` scheme handler, GSettings schema. |

## D-Bus API (`/org/nostr/Wallet1`, interface `org.nostr.Wallet1`)

Amounts are millisatoshis.

| Method | Signature | Policy class |
|---|---|---|
| `GetInfo()` | `→ a{sv}` | read (returns `{paired: false}` to anyone when unpaired) |
| `GetBalance()` | `→ t balance_msat` | read |
| `GetInfoNonInteractive()` / `GetBalanceNonInteractive()` | as the plain method | read, but **never prompts**: where the plain method would ask the user they fail with `InteractionRequired` (for panels/status indicators) |
| `MakeInvoice(u amount_msat, s description, u expiry)` | `→ s bolt11, s payment_hash` | receive |
| `PayInvoice(s bolt11, u amount_msat_or_0)` | `→ s preimage, t fees_paid_msat` | pay |
| `LookupInvoice(s payment_hash_or_bolt11)` | `→ a{sv}` | read |
| `ListTransactions(t from, t until, u limit, s type)` | `→ aa{sv}` | read |
| `Pair(s nwc_uri)` / `Unpair()` | | pair / unpair |
| `BeginWalletAuth(a{sv} options)` / `CancelWalletAuth()` | `→ s walletauth_uri` | pair by request with an agent-generated key; always confirmed (see [Pairing](#pairing)) |
| `OpenUri(s uri)` | | scheme-handler link (always confirmed) |
| `GetBudget(s app_id)` | `→ u msat_per_day, t spent_today_msat` | own: always; other app: trusted only |
| `SetBudget(s app_id, u msat_per_day)` | | lower own: always; otherwise confirm (a budget never grants read access) |
| `ListApps()` | `→ a{sa{sv}}` (`limit_msat_per_day` t, `spent_today_msat` t, `allow_read` b, `allow_receive` b per app id) | grant admin only |
| `SetReadAccess(s app_id, b allow)` | | grant: grant admin immediately, other apps only after the user agrees; revoke own: always; revoke another's: grant admin only |
| `SetReceiveAccess(s app_id, b allow)` | | the invoice grant (`MakeInvoice` without asking); same rules as `SetReadAccess` |
| `GetInfoFor` / `GetBalanceFor` / `MakeInvoiceFor` / `PayInvoiceFor` `(s origin, …)` | as the plain method | as the plain method, **with the web origin as the app** — browser bridge only, see [Web origins](#web-origins-browser-bridge) |

`app_id = ""` means "the caller". Signals: `PaymentReceived(a{sv})`,
`PaymentSent(a{sv})`, `BudgetExceeded(s app_id, t requested_msat, t remaining_msat)`,
`AppsChanged()` (a grant or limit changed; re-list with `ListApps`).
Properties: `Paired b`, `WalletPubkey s`, `Lud16 s`, `Relays as`.
Errors are `org.nostr.Wallet1.Error.{InvalidArgs, NotPaired, Denied,
BudgetExceeded, Timeout, WalletError, RelayError, Unsupported, RateLimited,
Keyring, InteractionRequired, Failed}`; `WalletError` messages start with the NIP-47 code, e.g.
`[INSUFFICIENT_BALANCE] …`. `Timeout` means **outcome unknown** — a payment
may still have happened.

```sh
gdbus call --session --dest org.nostr.Wallet1 --object-path /org/nostr/Wallet1 \
  --method org.nostr.Wallet1.GetInfo
```

## Pairing

**Recommended: connect by request (`nostr+walletauth`, `nostrc-prqu.12`).**
`BeginWalletAuth()` makes the agent generate the NWC client keypair and
return a link

    nostr+walletauth://<client-key>?relay=…&name=…&request_methods=…
        &notification_types=…&pubkey=<client-key>&state=<128-bit random>

(Alby Hub's form plus the NWC-08 draft's `pubkey`/`state`), which the caller
— Nostr Settings' *Connect with your wallet app* — opens in the user's
wallet app or shows for copying. It holds no secret. When the user approves
there, the wallet publishes its kind-13194 info event p-tagged to the client
key (NWC-08 wallets echo `state` in a `state` tag). The agent
(`src/nwa-walletauth.c`) takes an answer that verifies, p-tags the key,
lists NWC methods and is recent; with a matching `state` at once, without
one (Alby today) after a 3 s settle window. Two *different* wallets
answering one request fail it and new requests are refused for a minute
(someone else saw the link and is racing the wallet). Answers carrying
another request's `state`, malformed or stale ones are ignored; a relay hint
in the p tag is subscribed to in addition to the requested relays (≤ 5,
default: GSettings `wallet-auth-relays`).

The answer is then *staged*: a client on the new key must get a `get_info`
reply from the wallet before anything is shown, and the **Connect dialog is
always shown** (`state` travels inside the link, so it binds the answer to
the request but cannot prove the wallet's identity to someone who saw the
link): it names the requesting app, the wallet's name, the first 12 hex of
its key, the relay and encryption, asks the user to compare the key with
the wallet app, and warns louder when the wallet did not echo `state`. Only
on *Connect* is the pairing stored in the keyring and made active; decline,
timeout, conflict, cancel or a failed check leave the current pairing
untouched. The secret never leaves the agent. One request at a time: the
same app (or the settings app) may replace its pending request, others get
`RateLimited`; `CancelWalletAuth` likewise; the outcome is the
`WalletAuthFinished(b, s)` signal. Headless, `BeginWalletAuth` is refused.

**Pasted links.** Pair by clicking a `nostr+walletconnect://…` link (the agent is its scheme
handler) or by `Pair(uri)` from an application. Pairing and unpairing are
**always** confirmed in a dialog — trusted apps included — that names the
requesting application, the wallet (lud16 or pubkey) and relay, warns when it
replaces an existing wallet (a hostile pairing would redirect every app's
incoming payments), and points out that whoever created the link also holds
its secret.

> NIP-47 limitation: the `secret` in a `nostr+walletconnect://` URI is known
> to whoever produced or relayed the link, and they can use the wallet
> directly, outside the agent's budgets; `Unpair` cannot revoke that — only
> revoking the connection in the wallet can. Connecting by request (above)
> avoids it.

The URI is stored in the Secret Service under the dedicated schema
**`org.gnostr.WalletConnection`** (added to `gnome/seahorse/secret_store.[ch]`,
see `org.gnostr.secret.schema.txt`), not the identity schema: it is a
different secret class (a per-pairing client key, not a Nostr identity).
Seahorse shows it as `Nostr Wallet Connect: <lud16> (<pubkey prefix>…)`.
Exactly one pairing exists; pairing again replaces it, `Unpair` deletes it.

The NWC client keypair is the URI's `secret` (for a pairing by request, the
agent-generated key, stored in the same `nostr+walletconnect://` form). The agent signs NIP-47 requests
and NIP-42 AUTH with it and never involves the user's Nostr identity or the
signer daemon.

## Budgets

Each application has a daily limit (msat, default **0 = never pay without
asking**) and two separate flags: "allowed to read" (balance, history,
invoice lookup) and "allowed to receive" (create invoices). The limit is set
by ticking *Always allow up to N sats/day* in the payment dialog, by
`SetBudget`, or by a trusted settings app; the read flag by ticking *Always
allow this app* in the wallet-access dialog or by `SetReadAccess`; the
receive flag by ticking *Always let this app create invoices* in the invoice
dialog or by `SetReceiveAccess` (each the trusted settings app's per-app
switch; other apps only after the user agrees), and an app can give up its
own. None implies another: a read grant for `exe:/usr/bin/gnome-shell` does
not let every Shell extension create invoices that look like the user's own
(nostrc-muhk). `budgets.json` version 1 had only `allow_read`, which then
meant both; such records load with `allow_receive = allow_read` and are
rewritten as version 2.

* **Storage:** `$XDG_STATE_HOME/nostr-wallet/budgets.json` (dir 0700, file
  0600, atomic rewrite). Budgets are deliberately *not* in GSettings: they
  carry a spend ledger rewritten on every payment, which is state, not
  configuration. A corrupt file is moved to `budgets.json.corrupt` and the
  store starts empty (fails closed).
* **Day:** the local calendar day; spend resets at local midnight.
* **Conservative accounting:** the amount plus a routing-fee reserve (1 %,
  at least 1 sat) is reserved (and written to disk) *before* the request is
  sent; the budget check uses the same total. A definite failure (wallet error, relay
  refusal) refunds it; success replaces it with amount + `fees_paid`; a
  timeout keeps it (the payment may have gone through). A payment in flight
  across midnight is charged to the new day.
* A concurrent call that no longer fits when its reservation is taken fails
  with `BudgetExceeded` instead of overspending.

GSettings `org.nostr.Wallet` holds only preferences: `request-timeout` (60 s),
`approval-timeout` (120 s), `always-confirm-payments` (false),
`max-auto-pay-msat` (0 = no per-payment cap), `trusted-apps`
(`['org.nostr.Settings']`).

## Approval matrix

Evaluated by `nwa_policy_decide()`; every row is a case in `tests/test_policy.c`.
"Prompt" becomes **Deny** when no display is available. For the
`*NonInteractive` reads, both become `InteractionRequired` (the agent does
not even initialise GTK for them), so a status indicator can ask "may I
read?" without reimplementing caller identification or peeking at
`budgets.json`.

| Request | Condition | Decision |
|---|---|---|
| any | caller runs as another Unix user | Deny |
| read / receive / pay | not paired | Deny (`NotPaired`) |
| read | trusted app, scheme link, or identified app with a read grant | Allow |
| receive | trusted app, scheme link, or identified app with a receive grant | Allow |
| read / receive | otherwise | Prompt ("see your balance?" / "create an invoice?"; remembers only that grant, and only if *Always allow…* is ticked) |
| pay | identified app, amount + fee reserve ≤ remaining budget, ≤ `max-auto-pay-msat`, `always-confirm-payments` off | Allow |
| pay | over a configured budget | Prompt + `BudgetExceeded` signal |
| pay | no budget / unidentified caller / scheme link / always-confirm / above per-payment cap | Prompt |
| pair | always (trusted apps and clicked links included) | Prompt |
| unpair | not paired | Allow (no-op) |
| unpair | paired | Prompt |
| budget | lower own | Allow |
| budget | raise own, or change another app's | Trusted: Allow; identified (grant admin included): Prompt; unidentified: Deny |
| budget | read another app's, `ListApps` | Trusted or grant admin: Allow; otherwise Deny |
| read / receive grant | `SetReadAccess` / `SetReceiveAccess(app, true)` | Trusted or grant admin: Allow; identified: Prompt; unidentified: Deny |
| read / receive revoke | own | Allow (identified) |
| read / receive revoke | another app's | Trusted or grant admin: Allow; otherwise Deny |

"Trusted" means listed in `trusted-apps` **and** identified through Flatpak
(an id the caller cannot choose); unsandboxed callers are never trusted.

A **grant admin** may list apps and grant or revoke read and receive access without a
dialog, but not change spending limits: a trusted app, or the installed Nostr
Settings — an unsandboxed same-uid caller whose `/proc/<pid>/exe` is
`<bindir>/nostr-settings` by path and inode (the browser bridge's rule,
`nwa_caller_exe_is`, re-checked on every call; a Flatpak or snap that execs
the binary keeps its sandbox identity; test builds may replace the path with
`NOSTR_WALLET_AGENT_SETTINGS_APPS`). That keeps sandboxed apps from granting
themselves anything; like all unsandboxed identities it does not stop
malware already running as the user (which could edit `budgets.json`). An
unsandboxed binary is a softer identity than a Flatpak id (it can be driven
through its own D-Bus surface or the accessibility bus), so a budget raise
from Settings is still confirmed in the agent's dialog, and Settings exposes
no action that changes grants.
At most 2 outstanding prompts per application (6 in total); more are
refused with `RateLimited`. Dialogs default to *Deny*, time out to *Deny*,
and closing them is *Deny*.

## Security model

**Caller identification.** The agent never trusts an app id passed by the
caller. For each call it asks the bus for the sender's credentials
(`GetConnectionCredentials`: `UnixUserID`, `ProcessID`, and `ProcessFD` where
the bus provides it) and resolves, in order:

1. **Flatpak** — `/proc/<pid>/root/.flatpak-info` `[Application] name`. For a
   Flatpak app the bus peer is its `xdg-dbus-proxy`, whose root carries the
   app's `.flatpak-info`; this is the lookup xdg-desktop-portal performs and
   the only identity marked *verified* (it attests sandbox membership; a
Flatpak the user installed locally can still declare any app id).
2. **Snap** — cgroup `snap.<name>.*`.
3. **systemd app unit** — cgroup `app-[<launcher>-]<app-id>[-<rnd>].scope` /
   `app-…@….service` (the reverse-DNS component).
4. **Executable** — `exe:` + `readlink /proc/<pid>/exe`.

PID reuse: with a pidfd, an identity read from `/proc` is only accepted if
the process is still alive afterwards; without one (dbus-daemon < 1.15, e.g.
Ubuntu 24.04), the agent asks the bus for the sender's PID again after the
`/proc` reads and drops the identity unless the sender still exists with the
same PID. Identity is re-resolved on every call (no cache), so a process that
`exec()`s keeps no stale identity. Callers with a different UID are refused
outright.

**What budgets protect against.** Sandboxed (Flatpak) apps cannot pick their
identity, cannot read the keyring item and cannot edit the budget file
(unless granted `--filesystem=home`/`xdg-state`, which makes them effectively
unsandboxed); for them the budget is a real boundary. For unsandboxed apps
running as the same user, identities 2–4 are *unverified*: such a process can
choose its systemd scope name, edit `budgets.json`, or read the keyring
directly, so budgets and prompts guard against buggy or over-eager apps and
confused-deputy links — not against malware that already runs as you. The
dialog labels unverified identities as such, and none of them can be
"trusted". Flatpak apps need `--talk-name=org.nostr.Wallet1`.

**Budget bypasses considered.** Caller-chosen app ids (not accepted);
concurrent calls racing for the same remaining budget (reservation is
atomic in the single-threaded agent; losers get `BudgetExceeded`); fees
pushing a run of payments over the limit (fee reserve); setting a budget to
obtain read access (budgets never grant it); lying
about the amount (the invoice is decoded and checksummed by the agent; the
`amount` argument is only accepted for amount-less invoices and must match
otherwise); expired invoices (rejected); request replay after a timeout
(every request carries a NIP-40 `expiration` equal to the timeout so the
wallet refuses it later, and the timed-out amount stays charged); forged
wallet responses (every inbound event must have a valid id and signature
from the wallet pubkey and an `e` tag matching a pending request, and is
decrypted only with the request's scheme — no NIP-44 → NIP-04 downgrade);
raising one's own budget (always confirmed); dialog spam (rate-limited,
default-deny); spoofed "link" prompts (links arrive through `OpenUri`, whose
caller is identified and named as the link's opener).

<a id="web-origins-browser-bridge"></a>
**Web origins (browser bridge).** The WebLN browser extension reaches the
agent through its native-messaging host `nostr-signer-webext-host`
(`apps/grotto/native-host/`). The browser spawns that host, so it
inherits the browser's cgroup and would be identified as the browser: every
website would share one budget and one "Always allow". The `*For` methods
therefore take the page origin as first argument, under one rule — **only the
bridge may assert an origin**:

* the caller is same-uid, identified as a bare executable or a systemd app
  scope (never Flatpak, Snap or unidentified), and its `/proc/<pid>/exe`
  equals `<libexecdir>/nostr-signer-webext-host` both as a path and by
  device/inode. Both must match: a hard link or byte copy elsewhere, a
  replaced binary (`(deleted)`) or the same path naming a different file
  inside another mount namespace does not qualify. The exe is read
  under the same pidfd / PID-recheck liveness guard as the identity, and
  the gate only runs after that guard completed;
* the origin must be a browser-serialized secure origin (`https://host[:port]`,
  or `http://` on localhost / `*.localhost` / `127.0.0.1` / `[::1]`;
  lowercase, no default port, no path). Such ids contain `://`, so they can
  never collide with a reverse-DNS, `snap.` or `exe:` app id (they are
  opaque keys in `budgets.json`, never path components);
* anyone else calling a `*For` method gets `Denied` (never a silent fallback
  to their own identity); a malformed origin gets `InvalidArgs`.

The origin then *is* the application for that call: policy, "Always allow
this site", the daily budget, the spend ledger, the per-app prompt limit and
`PaymentSent.app_id` are all keyed on it, and the dialogs name the site ("The
website https://snort.social (in Firefox, via the Nostr browser extension)").
The payment dialog speaks of the *site* throughout: "The website snort.social
(in Firefox) wants to pay…", "exceeds the site's daily budget", "Always allow
this site".
Settings apps see and set site budgets with
`GetBudget`/`SetBudget("https://snort.social", …)`. The same origin string
is the same principal whichever browser it came from.

| Identity class | Set by | Can be trusted? |
|---|---|---|
| Flatpak | sandbox (`.flatpak-info`) | yes, if listed in `trusted-apps` |
| snap / systemd scope / `exe:` | the process itself (unverified) | never |
| web origin (`https://…`) | the browser, relayed by the exe-gated bridge | never (not attested) |

Trust assumptions, stated plainly: **the browser is a trusted attester** of
the origin (the extension takes it from the browser's message sender, never
from the page, and the host re-validates it); a compromised browser is a
compromised bridge, and the exe gate does not defend against it. Like every
unsandboxed identity this protects site budgets against over-eager sites
and sandboxed apps, not against malware running as you: any process of the
same user can exec the bridge binary and feed it frames (it could equally
edit `budgets.json`). A sandboxed app that can run commands on the host
(`org.freedesktop.Flatpak` talk-name, `flatpak-spawn --host`, or an
equivalent portal escape) is outside its sandbox and therefore out of scope.
The bridge is a single short-lived process per browser connection: it exits
on stdin EOF and never forks, so no other program inherits its bus
connection.

Test builds only (CMake `NOSTR_WALLET_AGENT_ORIGIN_BRIDGE_ENV`, default
`BUILD_TESTING`; packages are built with it off):
`NOSTR_WALLET_AGENT_ORIGIN_BRIDGES` (colon-separated absolute paths)
replaces the built-in bridge path so CTest can run the build-tree host, and
the agent logs that the override is active. With
`NOSTR_WALLET_AGENT_EPHEMERAL=1`, `NOSTR_WALLET_AGENT_TEST_PAIR_URI` starts
the agent paired with that URI (no dialog, no keyring) so tests can run it
against `tests/nwa-fixture-wallet`.

**Secret handling.** The pairing URI lives only in the Secret Service item
and in agent memory (wiped on drop); it is never returned over D-Bus, never
logged, and never written to the budget file. Preimages are returned only to
the paying caller; `LookupInvoice`/`ListTransactions` results omit them.
Signals are visible to every session-bus client, so `PaymentSent` /
`PaymentReceived` carry only type, amount, fees, timestamps, the paying
app id and — for zaps — the (already public) zap request; descriptions and
payment hashes stay behind the policy-gated methods.

**Sandboxing.** Caller identification needs ptrace-read access to the
callers' `/proc/<pid>/{exe,root}`, which a `systemd --user` unit loses as
soon as any mount-namespacing option puts it in a user namespace. The shipped
unit therefore uses only namespace-free hardening (`NoNewPrivileges`,
`SystemCallFilter=@system-service`, `RestrictNamespaces`,
`RestrictAddressFamilies`, …); if `/proc` access is denied the agent logs a
warning and treats every caller as unidentified (fails closed).

**Headless.** The agent is a plain `GApplication`; GTK/libadwaita are
initialised only when a dialog is needed. With no display (or
`NOSTR_WALLET_AGENT_HEADLESS=1`) everything that needs the user is denied.

## Scheme handlers

`org.nostr.Wallet.desktop` registers `x-scheme-handler/lightning`,
`x-scheme-handler/bitcoin` and `x-scheme-handler/nostr+walletconnect` with
`Exec=nostr-wallet-agent --open %u`, which calls `OpenUri` on the
(D-Bus-activated) agent. The agent identifies that process — normally
running in the scope of the application that opened the link — and names it
in the dialog as the link's opener. Link payments are always confirmed, never
drawn from the opener's automatic budget, and ledgered under
`org.nostr.Wallet`:

| Link | Action |
|---|---|
| `lightning:<bolt11>` (also `lightning://`, any case) | Payment dialog (always confirmed) → `pay_invoice` |
| `bitcoin:<addr>?…&lightning=<bolt11>` (BIP-21) | Same, using the Lightning invoice; `amount` must match it |
| `bitcoin:<addr>[?amount=…]` without `lightning=` | Window with a toast: *On-chain payments are not supported* |
| `nostr+walletconnect://…` (or `nostr+walletconnect:…`) | Pair dialog → keyring |
| `lightning:lnurl1…` (LUD-01), `lightning:user@host` (LUD-16 → `https://host/.well-known/lnurlp/user`) | LNURL-pay (LUD-06): the request is fetched, the **LNURL dialog** shows the recipient, its description (`text/plain`, `text/long-desc`), the server and an amount within `minSendable`..`maxSendable` (fixed when they are equal), plus a comment field when `commentAllowed` (LUD-12); on *Pay* the callback's invoice must be for exactly that amount and commit to the metadata (`description_hash` = sha256(metadata)), then it is paid like any link payment. LNURL-withdraw is refused |

LNURL fetches (`src/nwa-lnurl.c`, libsoup): HTTPS only (`http://` for
`.onion`; test builds also loopback), no redirects, 15 s, JSON bodies capped
at 64 KiB; an LNURL `{"status":"ERROR"}` answer is shown with its reason; a
LUD-16 server whose `text/identifier` names another address is refused.
The LNURL dialog is the payment's confirmation (not an extra dialog).

BIP-21 unknown `req-*` parameters make a link invalid, as the BIP requires.

## Notifications / zaps

Wallet `payment_received` / `payment_sent` notifications (kinds 23196/23197)
become `PaymentReceived` / `PaymentSent` signals. When the paid invoice's
description is a NIP-57 zap request (kind 9734) the details carry
`is_zap = true` and `zap_request`, which is the hook the notification daemon
can subscribe to for zap receipts. The agent itself does not post
notifications.

The gnostr `nip47-nwc` plugin is a thin client of this interface. The main
gnostr app's own NWC service (`apps/gnostr/src/util/nwc.c`, used by the zap
dialog) still holds its own pairing; moving it onto `org.nostr.Wallet1` is
`nostrc-prqu.13`.

## Running

```sh
systemctl --user start nostr-wallet-agent      # or just call it: D-Bus activation
gdbus introspect --session --dest org.nostr.Wallet1 --object-path /org/nostr/Wallet1
xdg-mime query default x-scheme-handler/lightning   # org.nostr.Wallet.desktop
```

`NOSTR_WALLET_AGENT_EPHEMERAL=1` keeps the pairing in memory only (tests /
lab machines without a keyring). `G_MESSAGES_DEBUG=all` shows per-call
caller identification.

## Tests

`ctest -R nostr-wallet-agent` (configure with `-DENABLE_NOSTR_WALLET_AGENT=ON
-DBUILD_TESTING=ON`). No network, display or keyring is used:

* `test_nwc` — NIP-47 against a fixture wallet over the in-process fixture
  transport: subscription filters, request queuing until the info event,
  NIP-44 v2 and NIP-04 (no `encryption` tag / no info event) round-trips,
  `expiration` tag, wallet errors, forged/tampered responses ignored,
  notification de-duplication, relay `OK false`, timeout and stop semantics.
  (Also caught a pre-existing double free in `nips/nip47` `nostr_nwc_uri_parse`
  for a URI with relays but no secret; fixed with a regression test.)
* `test_budget` — limits, reservations, commit/release, saturation, day
  rollover incl. a payment in flight across midnight, persistence + 0600
  mode, corrupt-file handling.
* `test_lnurl` — LUD-01 vector and bech32 round trip, LUD-16 resolution
  (onion → http, clearnet http refused, bad addresses, mixed case),
  payRequest parsing and refusals (withdraw, range, callback scheme,
  metadata, identifier mismatch), callback URL (comment trimmed/escaped),
  invoice checks (amount, `h` vs `d`, wrong commitment) and the fetch against
  an in-process libsoup server (LNURL errors, size cap, no redirects, 404).
  `test_dbus` pays `lightning:alice@127.0.0.1:<port>` end to end (dialog →
  callback with amount and comment → fixture wallet) and refuses an invoice
  committing to other metadata.
* `test_uri` — BOLT-11 spec vectors (amounts incl. pico, description hash,
  UTF-8, upper case, invalid checksum/multiplier/sub-msat), `lightning:`,
  BIP-21 `bitcoin:` (lightning fallback, amounts, `req-` params) and
  `nostr+walletconnect:` parsing.
* `test_policy` — the approval matrix above.
* `test_walletauth` — the pairing-by-request state machine over the fixture
  transport: link shape (no secret, deduplicated relays), subscription,
  `state` match / mismatch, settle window, same wallet via two relays, relay
  hint, conflicting wallets, malformed/stale/future answers never counting,
  a confirmed answer beating an unconfirmed one, stop and timeout.
* `test_dbus` — the real agent paired with `nwa-fixture-wallet` on a private
  bus (Linux): `*NonInteractive` reads, the settings app's grants, and
  pairing by request end to end (wallet approves → staged `get_info` on the
  new key → dialog → active; unconfirmed + declined → untouched; impostor
  race → conflict + cooldown; supersede/cancel rules; refused headless).
  Dialogs are answered by the test-build-only
  `NOSTR_WALLET_AGENT_TEST_ANSWER=accept|deny`, which also logs their text.
* `test_caller` — Flatpak info / snap / systemd-scope parsing; web-origin
  syntax, the browser-bridge gate (kinds, path + inode, replaced binary,
  foreign uid) and the web-origin principal.
* `test_budget` also covers web origins as opaque budget keys.
* The `*For` methods are exercised end to end against the real agent binary
  by `apps/grotto/native-host/tests/test_nm_webln_e2e.c` (browser
  bridge allowed, any other caller `Denied`) and, paired with a wallet, by
  `test_nm_webln_paired_e2e.c` (per-site budget auto-pay, `BudgetExceeded`,
  per-origin read grant, invoices).
* `nwa-fixture-wallet` (test helper, not a test): a NIP-47 wallet service
  and the relay it uses in one process on `ws://127.0.0.1:<port>`. It prints
  a pairing URI, answers `get_info`/`get_balance`/`make_invoice` (minting
  invoices the agent can decode — `tests/nwa-test-bolt11.h`, zero
  signature)/`pay_invoice`/`lookup_invoice`/`list_transactions`, and accepts
  `nostr+walletauth://` requests on stdin.
* `test_nwc` also checks that both spellings of the wallet's encryption tag
  (`nip44_v2`, older `nip44-v2`) select NIP-44.

## Not supported (yet)

* LNURL-withdraw, LUD-17 `lnurlp://` schemes, LUD-18 payer data.
* On-chain payments — out of scope for an NWC agent (toast only).
* `pay_keysend`, `multi_pay_*` — not exposed over D-Bus.
