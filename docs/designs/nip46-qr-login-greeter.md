# Design — NIP-46 QR login at the GDM greeter (`nostrconnect://`), plus pre-paired-bunker login at the greeter

Status: **proposed — ready for implementation**
Date: 2026-09-22
Beads: `nostrc-z1fb` (implementation), `nostrc-zcll.6` (B5 greeter UX), `nostrc-zcll.7` (B6 real-GDM acceptance), `nostrc-ot2c.7` (C6/C7 real-signer proof)
Component: `gnome/nostr-homed` (broker, PAM module, providers) + `nips/nip46` (client/bunker library)

---

## 1. Context and scope

### 1.1 What already exists (do not re-derive)

| Piece | Where | State |
|---|---|---|
| Immutable kind-1 challenge bound to the account pubkey; strict verify | `gnome/nostr-homed/src/auth/auth_challenge.{c,h}`, `auth_broker.c:404-480` | Shipped |
| Pre-paired `bunker://` NIP-46 provider, proven live against `wss://bunker.sharegap.net` (`result=ok`) | `src/auth/provider_nip46.c` | Shipped |
| PAM conversation: provider choice (`local`/`remote`), `PAM_TEXT_INFO` approval message, 3-attempt budget, strict-deny stacking | `src/pam/pam_nostr_broker.c`, `packaging/pam/nostr` | Shipped |
| Real `gdm-password` stack proven via `pamtester` | `docs/reviews/b6-gdm-login-2026-09-22.md` | Shipped |
| `nostrconnect://` URI **parser** + session plumbing | `nips/nip46/src/core/nip46_uri.c:174`, `nip46_session.c:705,764,2745` | Shipped (parser only) |
| Headless bunker (signer stand-in) | `nips/nip46/src/core/nip46_session.c:2582-2900`, `include/nostr/nip46/nip46_bunker.h` | Shipped (bunker-URI issuance only) |
| QR encoder | — | **Does not exist in-tree** |
| Live infra | `wss://relay.sharegap.net`, `wss://bunker.sharegap.net` | Available |
| Lab | amd64 GNOME VM, real GDM, `ssh gnome-dev`, SPICE console | Available |

### 1.2 Gaps this design closes

1. **Library**: the NIP-46 client can *parse* a `nostrconnect://` URI but cannot **build** one, and cannot **await an unsolicited `connect` from a signer**. The persistent client subscription (`nip46_session.c:1070-1090`) filters kind-24133 `#p=<client pubkey>` and dispatches strictly by pending-request id — a signer-initiated `connect` carries an id the client never issued and is therefore dropped today. Similarly the bunker side can only *issue* a `bunker://` URI; it cannot *consume* a `nostrconnect://` URI. Both are required.
2. **Broker/provider**: no client-initiated mode; no way to hand a display string (the URI) back to the PAM module while the broker waits.
3. **PAM/greeter**: no QR rendering, no long-wait UX.

### 1.3 Non-goals for v1

- No custom greeter, no gnome-shell extension, no browser launch, no image/pixmap channel (explicitly out of scope per `nostrc-zcll.6`).
- No persistence of the ad-hoc pairing as a durable bunker provider (see D5).
- No pubkey-first / "scan to identify" login (see §2.2).
- No change to the shipped pre-paired `bunker://` path — it stays byte-for-byte intact and keeps its own acceptance evidence.

---

## 2. Flow shape at the greeter

### 2.1 Decision: **username-first**, ship it first

```
GDM greeter
  │  user picks / types a username           (GDM opens the PAM tx for THAT user)
  ▼
pam_nostr.so  BEGIN_LOGIN(user, service=gdm-password)
  │  broker: account lookup → enabled providers ["local","nip46","nip46qr"]
  ▼
provider choice                              (silent if exactly one; `provider=` arg pins)
  │  chosen = "nip46qr"
  ▼
SELECT_PROVIDER{provider:"nip46qr"}
  │  broker: build challenge (kind 1, bound to ACCOUNT pubkey)
  │          provider.prepare(): ephemeral keypair + secret, relay pool START
  │                              + SUBSCRIBE (before any publish), build URI
  │  ← reply: INTERACTION_REQUIRED + display{ uri, hint, expires_in_ms }
  ▼
PAM renders: [unicode half-block QR]  +  nostrconnect://…  +  "Scan with your
             Nostr signer app, then approve."      (PAM_TEXT_INFO)
  ▼
SUBMIT_UNLOCK{secret:"qr"}                   (blocking; broker waits)
  │  broker/provider:
  │    1. await signer `connect` whose result == our secret   → learn signer pubkey
  │    2. get_public_key  → MUST equal account.pubkey_hex     → else DENIED
  │    3. sign_event(challenge)                                → signed kind-1
  │  ← broker: nh_auth_challenge_verify(...) → OK / INVALID_PROOF / …
  ▼
PAM_SUCCESS → GDM session
```

**Why username-first wins for v1.** GDM constructs the PAM transaction *after* a username is chosen, and `pam_get_user()` is the first thing our module does. A pubkey-first flow would require the module to (a) show a QR before it knows which account to bind the challenge to, (b) mint a challenge bound to *no* account, (c) discover the account from the signer's pubkey, and (d) mutate `PAM_USER` mid-conversation. (c) and (d) are the killers:

- The broker's whole security model is "challenge is immutable and bound to the account's pubkey at issue time" (`auth_challenge.c`, `auth_broker.c:435-450`). Pubkey-first inverts that: you would have to accept a signature first and *derive* the identity from it, which is a different (and weaker-to-reason-about) transaction shape and a new attack surface for account enumeration.
- `pam_set_item(PAM_USER, …)` mid-`pam_sm_authenticate` is legal PAM but GDM has already selected the user in its own model (`gdm-session-worker` runs the PAM conversation with a preselected user; the session/`logind` handoff and the greeter's own user-list state assume it does not change). Divergence here is exactly the kind of thing `nostrc-zcll.6` forbids ("unmodified pinned GDM PAM conversation").
- Pubkey-first also needs a "Not listed? / Scan to sign in" entry point in the greeter UI, i.e. a gnome-shell change. Out of scope.

**Verdict: ship username-first.** Pubkey-first is deferred to a later phase and would require the greeter-side helper described in §3.5; record it as a follow-up issue, not v1 scope.

### 2.2 Pre-paired bunker at the greeter

No flow change: the existing `nip46` provider already works end-to-end. The greeter work for it is UX + proof only:

- Replace the current one-shot `pam_info("Approve the sign-in request on your Nostr signer (bunker).")` with the same three-part message shape used by QR mode, minus the QR: `"Approve the sign-in request on your Nostr signer."` + `"Waiting up to 90 s…"`.
- Re-prove it at the *graphical* greeter with the maintainer's single-use re-pairing bundle for agent pubkey `e2a4…7240` (see §7.4). That bundle is a secret on the maintainer's machine; the implementer consumes it **once**, never copies it into the repo, and records only the redacted outcome.

---

## 3. Rendering a QR inside GDM

### 3.1 The only channel

`PAM_TEXT_INFO`. gnome-shell's login dialog renders PAM info messages as a plain text label (`AuthPrompt`/`MessageList`). Consequences:

- **No markup, no images, no font control.** The label inherits the theme font (Cantarell — *proportional*).
- The label wraps at the dialog width (gnome-shell's `.login-dialog` prompt column is ~`23em`, roughly 320–400 px depending on scaling).

### 3.2 Risk assessment — this is the #1 project risk

A unicode half-block QR is only scannable if rendered in a **monospace** font with **no line wrapping** and **line-height ≈ character cell height** (half-blocks must tile vertically with no gaps).

- Proportional font ⇒ columns do not align ⇒ **unscannable**.
- Wrapping at ~40 visual columns ⇒ **unscannable**.
- Extra line leading between rows ⇒ half-blocks separate into stripes ⇒ **marginal-to-unscannable**.

We cannot influence any of these from inside the PAM conversation. **Therefore the QR is treated as best-effort, and the URI + hint are the guaranteed path.**

### 3.3 Mandatory Phase 0 spike (go/no-go, half a day)

Before any provider work, run on `gnome-dev` with SPICE watching:

1. Add a throwaway `pam_info()` to the module (or use `pam_exec`/a scratch PAM service on the real `gdm-password` stack) that emits:
   - a 3-line monospace ruler `0123456789…` ×3 identical lines,
   - a 21×21 (version 1) half-block QR encoding `test`,
   - a 53×53 (version 9) half-block QR encoding a representative 190-byte URI.
2. Photograph the SPICE console; attempt a scan with a phone.
3. Record: does the ruler align? At what column does wrapping occur? Are half-block rows contiguous?

**Go**: QR ships in v1 as designed below.
**No-go**: v1 ships **URI + hint only at the graphical greeter**; the QR text is still generated and still emitted (it is correct and scannable on a text console — `pamtester`, `tty1` `login`, ssh), and the in-dialog QR moves to Phase 2 (§3.5). Either way the rest of this design is unchanged — this only flips one config default (`nip46_qr_render=auto|qr|uri`).

### 3.4 QR specification (v1)

**Encoding.** Byte mode (the URI contains `:`, `/`, `?`, `&`, `=`, `%` and lowercase hex — alphanumeric mode is not usable). Error correction **L** (the display is pristine — no print damage; L maximises data per module and minimises size). Version **capped at 9** (53×53 modules).

**URI byte budget.** Target ≤ 230 bytes (version 9-L capacity). Representative minimal URI:

```
nostrconnect://<64-hex>?relay=wss%3A%2F%2Fbunker.sharegap.net&secret=<32-hex>&perms=sign_event%3A1&name=GNOME
```
≈ **190 bytes** → version 8–9 at ECC L. Budget enforcement, applied in order until the URI fits:

1. one relay only (never two in the QR — a second relay costs ~35 bytes and one version step),
2. `secret` is 16 random bytes → 32 hex chars (128 bits; §8),
3. `perms=sign_event:1` (URL-encoded) — not a blanket `sign_event`,
4. drop `name=GNOME`,
5. drop `perms` entirely,
6. if still > 230 bytes: **no QR**, emit URI + hint only, log `qr_skipped=uri_too_long`.

**Rendering.** Half-blocks: `▀` (U+2580, top module dark), `▄` (U+2584), `█` (U+2588), space. Two module-rows per text row; odd final row pads with light. Quiet zone **2 modules** on every side (spec says 4; 2 is the pragmatic screen minimum and scanners tolerate it — plus the dialog background is already light). Version 9 + quiet 2 ⇒ **57 columns × 29 text rows**.

**Message layout** (single `PAM_TEXT_INFO`, in this order, always all three parts present):

```
Scan this with your Nostr signer app:

<57 cols × 29 rows of half-blocks>

Or open this link on your phone:
nostrconnect://a1b2…?relay=wss%3A%2F%2Fbunker.sharegap.net&secret=…&perms=sign_event%3A1&name=GNOME

Pairing code: A1B2-C3D4   ·   expires in 90 s
```

- The **URI** is the always-works fallback (a user can type/photograph it; a desktop signer can take it from a text console).
- The **pairing code** is the first 8 hex chars of the ephemeral client pubkey, upper-cased and hyphenated. It is *not* a secret and *not* a second auth factor — it is a human cross-check so the user can confirm the signer app is showing the same session before approving. Never derive it from the `secret`.

**Emission rule.** One `PAM_TEXT_INFO` for the whole block. Do not split across multiple info messages: gnome-shell shows only the most recent message in the prompt area, so a split would hide the QR behind the hint.

### 3.5 Phase 2 (explicitly not v1): richer rendering

Sketch only, to be filed as a follow-up issue:

- Broker writes `/run/nostr-auth/greeter/<tx-id>.png` (mode 0644, owned root, directory readable by `gdm`, unlinked on transaction retire) and includes the path in the display payload.
- A small gnome-shell extension (or a patched `gdm` greeter session) watches the path and renders the image in the login dialog.
- This is also the enabler for pubkey-first login (§2.2), since it gives us a UI surface before a username exists.

---

## 4. Protocol: the exact `nostrconnect` flow

### 4.1 Steps, with timeouts

All budgets are subordinate to the broker's existing `NH_AUTH_CHALLENGE_LIFETIME_SEC = 120` and `NH_AUTH_TRANSACTION_LIFETIME_SEC = 180`.

| # | Step | Where | Budget | On failure |
|---|---|---|---|---|
| 0 | Generate ephemeral secp256k1 client keypair (`RAND_bytes`) + 16-byte `secret` | `provider_nip46_qr.prepare()` | — | `PROVIDER_UNAVAILABLE` |
| 1 | `nostr_nip46_client_set_secret(ephemeral_priv)` | provider | — | `PROVIDER_UNAVAILABLE` |
| 2 | `nostr_nip46_client_start()` — pool connect **and** `#p=<client pubkey>` subscription established **before** anything is published (the C3 subscription-before-publish lifecycle already in the lib, `nip46_session.c:1070-1090`) | provider | 10 s | `NETWORK_UNAVAILABLE` |
| 3 | Build `nostrconnect://…` URI (§3.4) and return it to the PAM module in the `SELECT_PROVIDER` reply | provider → broker → PAM | — | — |
| 4 | **Await signer `connect`**: an inbound kind-24133 event `#p=<client pubkey>`, NIP-44 (fallback NIP-04) decryptable by us, whose payload is `{"id":…, "result":"<secret>"}` (or `{"method":"connect","params":[…,"<secret>",…]}` — accept both shapes, spec implementations differ). Constant-time compare against our `secret`. On match, adopt the event's author as the signer pubkey (`nostr_nip46_client_set_signer_pubkey`). | provider | **90 s** (or challenge deadline, whichever is sooner) | `INTERACTION_REQUIRED` → PAM shows "timed out" |
| 5 | `get_public_key` RPC | provider | 15 s | timeout → `INTERACTION_REQUIRED`; mismatch → **`DENIED`** (§8.3) |
| 6 | `sign_event(challenge_json)` RPC | provider | 20 s | denied → `DENIED`; timeout → `INTERACTION_REQUIRED` |
| 7 | Local strict verify (recompute id == `expected_id`, `pubkey == account.pubkey_hex`, signature valid) — identical code path to the existing bunker provider (`provider_nip46.c:328-354`) | provider | — | `INVALID_PROOF` |
| 8 | `nh_auth_challenge_verify()` in the broker | `auth_broker.c:524` | — | per existing mapping |

Total worst case ≈ 10 + 90 + 15 + 20 = **135 s**, which exceeds the 120 s challenge lifetime. **Clamp**: step 4's budget is `min(90 s, challenge_deadline − now − 40 s)`. With a 120 s challenge and ~2 s of setup that yields a ~78 s scan window. If the maintainer wants a full 90 s scan window, raise `NH_AUTH_CHALLENGE_LIFETIME_SEC` to 150 for the QR purpose only (see D10).

`unknown` / unsolicited inbound events that do **not** carry our secret are dropped silently and counted (`qr_connect_rejected`), never surfaced as a login outcome, so a relay-side spammer cannot break the wait.

### 4.2 Cancel / retry

- **Cancel (Escape / "Cancel" in GDM)**: GDM aborts the conversation and tears down `gdm-session-worker`, which closes the broker socket. The broker's connection-close path must call `provider->ops->cancel` + `destroy` (→ `nostr_nip46_client_cancel_all` + `_stop`), releasing the relay pool. **This must be explicitly tested** (§7.2, T-7) — today the bunker provider's blocking `sign_event` is short (20 s), so a leaked pool was never observable; a 90 s wait makes it observable.
- **Retry**: the broker retires the transaction after any verification outcome and `pam_nostr_broker.c` already opens a fresh connection per attempt. Each retry therefore mints a **fresh ephemeral keypair, fresh secret, fresh challenge, fresh QR**. That is the correct behaviour — never re-display a consumed QR. The existing 3-attempt budget applies, but as with the bunker provider, a `DENIED` from the signer returns immediately rather than looping (`pam_nostr_broker.c:261-264`).
- **Timeout at step 4** returns `INTERACTION_REQUIRED` → `PAM_AUTH_ERR` today. Recommend the PAM module special-case it with `pam_error(pamh, "Sign-in request expired. Press Enter for a new code.")` and consume one retry slot.

### 4.3 Relay configuration

Source, in precedence order:

1. The account's provider record `public_config_json`: `{"mode":"nostrconnect","relays":["wss://bunker.sharegap.net"]}` — lets a site pin a private relay per account.
2. `auth.conf`: new key `nip46_qr_relays=wss://bunker.sharegap.net` (comma-separated, max 4 parsed, **only the first goes into the QR**; the rest are used for the subscription only). Also new: `nip46_qr_wait_ms` (default 90000), `nip46_qr_render=auto|qr|uri` (default `auto`).
3. Compiled-in default: `wss://bunker.sharegap.net`.

**Default relay: `wss://bunker.sharegap.net`** — it is ours, it is the relay the live NIP-46 path is already proven against (`ot2c.7` milestone 2026-09-21), and it is purpose-named so a NIP-46-only firewall rule is trivial. `wss://relay.sharegap.net` is the general-purpose relay and is *not* used for signer traffic.

`auth.conf` is currently documentation-only (the daemon takes paths on argv). This design requires the broker to actually **parse** `auth.conf` — that is new work (§6, step 2) and the first real config file the broker reads. Keep the parser trivial: `key=value`, `#` comments, no sections, reject unknown keys with a warning (not a fatal error).

---

## 5. Where it lives

### 5.1 New provider mode

**Decision: a separate provider type and a separate source file.** `NH_IDENTITY_PROVIDER_NIP46_QR = 3` in `nh_identity_provider_type`, canonical protocol name `"nip46qr"`, PAM choice token `"qr"`.

Rationale: the existing `NH_IDENTITY_PROVIDER_NIP46_BUNKER` record is defined by a `bunker_uri` + a persistent transport `secret_blob`; the QR mode has neither (its keypair is per-login and ephemeral, its record carries only relay config and no secret at all). Overloading the type with a mode flag would mean `prepare()` branching on JSON content and a record whose `secret_blob` is conditionally meaningless — and it would make it impossible for one account to enable *both* modes (which is exactly what we want: pre-paired bunker at the desk, QR from a phone when travelling). The bitmask `NH_IDENTITY_PROVIDER_BIT` already has room.

Cost of the new type (all mechanical): identity schema/enum, `enabled_providers` mapping in `providers_json()` and `pick_provider_type()`, `homectl` enrollment, NSS unaffected. Alternative (mode flag) is cheaper but is the wrong shape — see D1.

Files:

- **New** `gnome/nostr-homed/src/auth/provider_nip46_qr.c` — the client-initiated provider. Shares the strict-verify block with `provider_nip46.c`; factor that into **new** `src/auth/provider_nip46_verify.{c,h}` (`nh_nip46_verify_signed_challenge(signed_json, expected_id, account_pubkey)`) rather than copy-pasting.
- **Unchanged** `src/auth/provider_nip46.c` — pre-paired bunker mode. Only edit: pull the verify block out into the shared helper (no behaviour change; existing tests must pass untouched).

### 5.2 Provider event contract addition

New event type so a provider can hand the broker something to display *before* the blocking wait:

```c
NH_AUTH_PROVIDER_DISPLAY_REQUIRED   /* data = JSON: {"uri":"…","hint":"…","expires_in_ms":90000} */
```

Emitted by `provider_nip46_qr.prepare()` (after the subscription is up and the URI is built), captured by `provider_emit()` into `conn_state`, and serialised by the broker into the `SELECT_PROVIDER` reply. Never carries the `secret` as a separate field (it is inside the URI, which is by design public-to-the-scanner) and never carries private key material.

### 5.3 Broker protocol additions

`SELECT_PROVIDER` response payload gains an optional object:

```json
{ "result": "interaction_required",
  "display": { "kind": "nostrconnect",
               "uri": "nostrconnect://…",
               "hint": "Scan this with your Nostr signer app",
               "pairing_code": "A1B2-C3D4",
               "expires_in_ms": 78000 } }
```

- Backwards compatible: absent for `local` and `nip46`; old clients ignore it.
- Size: bounded at 1 KiB, well inside `NH_AUTH_PACKET_MAX`.
- `NH_AUTH_PROTOCOL_VERSION` stays `1` (additive optional field). Bump only if a maintainer prefers strictness (D8).

`auth_client.h` gains:

```c
typedef struct nh_auth_display {
  char kind[16];          /* "nostrconnect" */
  char uri[512];
  char hint[128];
  char pairing_code[16];
  uint32_t expires_in_ms;
} nh_auth_display;

int nh_auth_client_submit_selection_display(int fd, const char *provider,
                                            const char *passphrase,
                                            nh_auth_display *display_out,  /* may be NULL */
                                            nh_auth_result *result_out);
```

Split the existing `nh_auth_client_login_with()` into `begin_login` → `select_provider(+display)` → `submit_unlock` for the QR path, so the PAM module can render between the two. The existing one-shot wrappers stay for the local/bunker paths and for the SMB client.

### 5.4 PAM module changes (`src/pam/pam_nostr_broker.c`)

1. Provider prompt becomes `"Sign in with (local/remote/qr): "` when >1 provider is enabled; `nh_auth_provider_choice_parse()` learns `"qr"`/`"nip46qr"` → canonical `"nip46qr"`.
2. New `provider=nip46qr` module-arg value (lets the maintainer pin QR-only on the GDM stack without touching the prompt).
3. For `nip46qr`: `select_provider` with display → render → single `pam_info()` → `submit_unlock("qr")`.
4. Rendering helper: **new** `src/pam/pam_qr_render.{c,h}` — `char *nh_qr_render_halfblock(const char *text, int max_version, char **err)`. This is the only new code that links the QR encoder.
5. Honour `nip46_qr_render` by observing the broker's `display.kind`: the broker decides `qr` vs `uri`-only (it owns `auth.conf`); the module renders whatever it is told. Keeps policy in one place.

### 5.5 Library additions (`nips/nip46`)

These are prerequisites; nothing else can be built first.

- **New** `nostr_nip46_uri_build_connect(const NostrNip46ConnectURI *in, char **out_uri)` in `nip46_uri.c` — the mirror of the existing parser, with correct percent-encoding of `relay`/`perms`.
- **New** `int nostr_nip46_client_await_connect(NostrNip46Session *s, const char *expected_secret, uint32_t timeout_ms, char **out_signer_pubkey_hex)` in `nip46_session.c` — registers an "unsolicited inbound" waiter alongside the pending-request table so `nip46_persistent_client_cb` can route a signer-initiated `connect` to it instead of dropping it; validates the secret in constant time; on success sets `s->remote_pubkey_hex` from the event author. Must be safe to call with the pool already started (it is: the subscription is established in `client_start`).
- **New** `int nostr_nip46_bunker_connect_to_client(NostrNip46Session *s, const char *nostrconnect_uri)` in the bunker half — parses the URI, listens on its relays, and publishes the `connect` ack carrying the URI's `secret` to the client pubkey. **Required for the headless test harness** (§7.1); also makes our own bunker a complete NIP-46 signer.

---

## 6. QR encoder dependency

**Recommendation: vendor `qrcodegen` (Nayuki, MIT), C version, into `third_party/qrcodegen/`.**

| | libqrencode (`libqrencode-dev` / `qrencode-devel`) | vendored qrcodegen (MIT) |
|---|---|---|
| Availability | Debian, Ubuntu, Fedora, Arch | n/a — in-tree |
| Size | ~120 KB shared lib + dev package | 2 files, ~1.2 kLOC, ~15 KB object |
| New runtime dep on the headless stack | **Yes** — `pam_nostr.so` would gain a `.so` dep pulled into every PAM-using process (sshd, login, gdm, cron) | **No** |
| Dependency-purity gate | Fails: the headless broker/PAM stack currently depends only on libc + OpenSSL + jansson + our own libs | Passes |
| Packaging churn | `debian/control` Build-Depends + Depends; `packaging/rpm/*.spec` BuildRequires/Requires; Arch + Homebrew too | None |
| API | Mature, more modes | Exactly what we need: byte mode, ECC level, version cap, `getModule()` |
| Licence | LGPL-2.1+ | MIT (compatible, no copyleft obligation on a statically linked PAM module) |
| Version skew | Encoder behaviour must be identical on every distro we ship to | Pinned |

The deciding factor is the PAM module: linking an extra shared library into a module that `sshd` and `login` `dlopen()` is a real blast-radius and a real packaging cost, for ~1 kLOC of pure arithmetic with no I/O and no parsing of untrusted input (we encode a URI **we** generated). Vendor it, pin the upstream commit in `third_party/qrcodegen/VERSION`, add the licence to `debian/copyright`, and wire it into both `meson.build` and `CMakeLists.txt` as a static convenience library linked **only** into `pam_nostr.so`.

**The broker does not link the encoder.** It emits the URI; the PAM module renders. This keeps the root daemon's dependency surface unchanged and puts the (memory-safe, bounded) rendering in the unprivileged greeter process. See D3.

---

## 7. Test strategy (no phone required for CI)

### 7.1 Headless signer stand-in

**New** `gnome/nostr-homed/tests/integration/qr_signer_standin.c` — a small binary built on `nostr_nip46_bunker_*`:

```
qr_signer_standin --uri <nostrconnect://…> --nsec-file <path> [--deny] [--wrong-key] [--delay-ms N]
```

It parses the URI, connects to the URI's relay(s), sends the `connect` ack with the URI's secret, answers `get_public_key` and `sign_event`. Modes:

- default: full success;
- `--deny`: reply `error: user rejected` to `sign_event` → expect `DENIED`;
- `--wrong-key`: answer `get_public_key` with a *different* pubkey → expect `DENIED`, **no** signing attempted (§8.3);
- `--delay-ms`: exercise timeouts;
- `--no-connect`: never ack → expect step-4 timeout.

### 7.2 Automated tests (ordered by cheapness)

| ID | Test | Kind | File |
|---|---|---|---|
| T-1 | `nostrconnect` URI build/parse round-trip incl. percent-encoding, multi-relay, byte-budget trimming | unit | `nips/nip46/tests/test_uri_comprehensive.c` (extend) |
| T-2 | `await_connect` accepts a matching secret, rejects a wrong secret, rejects a replay, times out cleanly | unit + mock relay | **new** `nips/nip46/tests/test_client_await_connect.c` |
| T-3 | Bunker consumes a `nostrconnect://` URI and completes `connect`/`get_public_key`/`sign_event` | mock relay | `nips/nip46/tests/test_nip46_e2e_mock.c` (extend §2) |
| T-4 | QR half-block renderer: known-answer vectors (encode `"HELLO"` → compare module matrix against the spec's version-1 reference), version cap, quiet zone, odd-row padding, oversize input → error | unit | **new** `gnome/nostr-homed/tests/unit/test_qr_render.c` |
| T-5 | Provider `nip46qr` happy path against the stand-in on a **mock relay** — asserts subscribe-before-publish ordering | integration | **new** `tests/integration/test_provider_nip46_qr.c` |
| T-6 | Negative matrix: wrong signer pubkey → `DENIED`; denied sign → `DENIED`; no connect → `INTERACTION_REQUIRED`; tampered signed event → `INVALID_PROOF`; expired challenge → `EXPIRED` | integration | same |
| T-7 | **Cancel/leak**: close the broker connection mid-wait → provider destroyed, relay pool released, no lingering thread (assert via `/proc/self/task` count or a pool refcount hook) | integration | **new** `tests/integration/test_broker_qr_cancel.c` |
| T-8 | Broker protocol: `SELECT_PROVIDER` reply carries `display`, bounded; old-client compatibility (ignores unknown field) | integration | `tests/integration/test_broker_login_providers.c` (extend) |
| T-9 | Rate limit: N failed QR attempts trip the existing account cooldown | integration | `tests/integration/test_broker_hardening.c` (extend) |
| T-10 | **Live relay** QR login against `wss://bunker.sharegap.net` with the stand-in (opt-in, `NOSTR_LIVE=1`, mirrors `test_broker_login_nip46_live.c`) | live | **new** `tests/integration/test_broker_login_nip46_qr_live.c` |

### 7.3 `pamtester` through the real `gdm-password` stack (`nostrc-zcll.7`)

On `gnome-dev`, same procedure as `docs/reviews/b6-gdm-login-2026-09-22.md`:

1. Enable the `nostr` pam-auth-update profile; seed an account with `nip46qr` enabled.
2. `pamtester gdm-password <user> authenticate` with the stand-in launched from the URI the module prints. Since `pamtester` runs on a **text terminal**, the QR renders in real monospace — this is also our functional proof that the QR encodes correctly and is scannable **by a real phone**, independent of gnome-shell's font.
3. Assert: correct → RC 0; `--wrong-key` → RC 1 with `default=die` (no Unix downgrade); `--no-connect` → RC 1, bounded within the challenge lifetime.
4. Restore the stack to stock.

### 7.4 Pre-paired bunker at the greeter

Consume the maintainer's single-use re-pairing bundle for `e2a4…7240` **once**, enrol it as a `nip46` provider on the lab account, and drive a real graphical GDM login (SPICE click-through). Record redacted evidence into `docs/reviews/`. Do not commit the bundle, its path, or any derived secret.

### 7.5 Manual UX (documented, not CI)

- **Real phone**: Amber (or another maintained NIP-46 signer, per `nostrc-ot2c.7`'s "named signer" requirement). Scan from the SPICE console with a physical phone; record approve, deny, and ignore-until-timeout. ≥20 observed approvals are required by `nostrc-zcll.7`'s acceptance criteria.
- **SPICE visual check**: screenshot the greeter at each step; attach to the B5/B6 evidence bundle. This is the record of whether §3.3's go/no-go held in practice.

---

## 8. Security

### 8.1 The `secret` in the URI

- 16 bytes from `RAND_bytes`, hex-encoded. It is a **one-time pairing token**, not a credential: it authorises nothing by itself, it only proves that whoever sends the `connect` ack saw the QR we just displayed on this screen.
- It is single-use and dies with the transaction (fresh one per attempt, per §4.2).
- It is compared in constant time.
- **Never logged.** Logging rule: log the ephemeral *client pubkey* (public), the transaction id, the relay, the signer pubkey once learned, and the outcome. Never the secret, never the ephemeral private key, never the full URI (the URI contains the secret). The `pairing_code` (§3.4) is derived from the *pubkey*, precisely so it is loggable.
- The ephemeral private key lives in a `secure_alloc()` buffer and is wiped in `destroy()`, mirroring `provider_nip46.c:377`.

### 8.2 Replay / expiry

- The challenge is already immutable, nonce-bearing, deadline-bound (`NH_AUTH_CHALLENGE_LIFETIME_SEC`), and verified by `nh_auth_challenge_verify()` against the current account record. Nothing in the QR path weakens that.
- A `connect` ack replayed after the wait window finds no waiter and is dropped; replayed *within* the window it is idempotent (we already have a signer) — accept the first, ignore the rest, count `qr_connect_duplicate`.
- The subscription filter's `since = now − 60 s` (`nip46_session.c:1080`) bounds how old an inbound event may be. Keep it.

### 8.3 Binding the signer to the account — no takeover via a different signer

Three independent checks, all of which must pass:

1. **`get_public_key` gate (step 5)**: if the signer's user pubkey ≠ `account->pubkey_hex`, fail **immediately with `DENIED`** and **never send `sign_event`**. This prevents an attacker's signer from even being asked to sign a challenge for someone else's account, and it fails fast with a distinguishable outcome.
2. **Signed-event gate (step 7)**: recompute the event id and require `id == expected_id` **and** `event.pubkey == account.pubkey_hex` **and** a valid signature (existing code, `provider_nip46.c:328-354`).
3. **Broker gate (step 8)**: `nh_auth_challenge_verify()` re-checks the full binding (account, key generation, deadline) against the live account record.

Note the failure-mode asymmetry that makes this safe: since the challenge is minted from the *account's* pubkey before the QR is shown, a signer holding a different key simply cannot produce a passing proof — the worst it can do is waste the window. Scanning a QR displayed on someone else's greeter therefore cannot grant access to *their* account, only fail.

The `DENIED` for a pubkey mismatch must be logged at `LOG_NOTICE` with **both** pubkeys (both are public) so a misconfigured enrolment is diagnosable: `nostr: qr signer pubkey mismatch user=%s account=%.16s signer=%.16s`.

### 8.4 Rate limiting and DoS

- `nh_auth_ratelimit_check()` already gates `BEGIN_LOGIN` and `SUBMIT_UNLOCK` by username (`auth_broker.c:504`). QR inherits it unchanged.
- **New exposure**: each QR attempt holds a relay connection for up to ~90 s. Add a broker-wide cap `nip46_qr_max_concurrent` (default **4**) on simultaneous QR waits; beyond it, return `PROVIDER_UNAVAILABLE` (→ `PAM_AUTHINFO_UNAVAIL` → falls through, never accepts). Without this, N greeter tabs × 90 s is a cheap resource exhaustion against the root daemon.
- Inbound-event flood on the subscription is bounded by the existing NIP-46 rate-limit gate (`nip46_rpc_gate_acquire`) plus the drop-and-count rule in §4.1.

### 8.5 What the greeter leaks on screen

The QR/URI is displayed on an unattended lock/login screen and is visible to anyone in the room, and to anyone watching the SPICE/VNC console. That is acceptable **only** because of §8.3: the URI grants a scanner the ability to *attempt* a pairing for an account whose key it does not hold. Still, do not extend the display to include the username's pubkey or any account metadata beyond what is already on the greeter.

---

## 9. Implementation checklist (ordered)

Each step should be an independently reviewable commit with its tests green.

**Phase 0 — spike (blocking, do first)**
1. `gnome-dev` font/wrapping spike per §3.3; record result in `docs/reviews/`. Set the `nip46_qr_render` default accordingly.

**Phase 1 — library (`nips/nip46`)**
2. `nip46_uri.c` + `nip46_uri.h`: `nostr_nip46_uri_build_connect()`. Tests T-1.
3. `nip46_session.c` + `nip46_client.h`: `nostr_nip46_client_await_connect()` — unsolicited-inbound waiter wired into `nip46_persistent_client_cb`. Tests T-2.
4. `nip46_bunker.h` + session bunker half: `nostr_nip46_bunker_connect_to_client()`. Tests T-3.

**Phase 2 — QR renderer**
5. `third_party/qrcodegen/` vendored (MIT), pinned, wired into meson + cmake as a static lib; `debian/copyright` updated.
6. `gnome/nostr-homed/src/pam/pam_qr_render.{c,h}` — half-block renderer with version cap, quiet zone, budget trimming. Tests T-4.

**Phase 3 — broker / provider**
7. `include/nostr_identity.h`: `NH_IDENTITY_PROVIDER_NIP46_QR = 3`; identity store + `homectl` enrolment for a QR provider record (relays only, no secret).
8. `src/auth/provider_nip46_verify.{c,h}`: extract the strict-verify block from `provider_nip46.c` (pure refactor; existing tests must pass untouched).
9. `src/auth/auth_provider.h`: `NH_AUTH_PROVIDER_DISPLAY_REQUIRED` event type.
10. `src/auth/provider_nip46_qr.c`: the new provider (§4.1).
11. `src/auth/auth_broker.c`: `auth.conf` parsing (`nip46_qr_relays`, `nip46_qr_wait_ms`, `nip46_qr_render`, `nip46_qr_max_concurrent`); `providers_json()` + `pick_provider_type()` learn `"nip46qr"`; `provider_emit()` captures the display payload; `do_select_provider()` serialises it; concurrency cap; connection-close teardown path (T-7).
12. `config/auth.conf.sample`: document the new keys.
13. Tests T-5, T-6, T-7, T-8, T-9 + the stand-in binary (§7.1).

**Phase 4 — PAM / greeter**
14. `src/auth/auth_client.{c,h}`: `nh_auth_display`, `nh_auth_client_submit_selection_display()`.
15. `src/pam/pam_nostr_broker.c`: `"qr"` choice token, `provider=nip46qr` arg, the render-then-wait sequence, the expiry message, and the improved bunker-mode wait text (§2.2).
16. `packaging/pam/gdm-password.sample` + `packaging/pam/nostr`: no stacking change expected — re-verify and document.

**Phase 5 — proof**
17. T-10 live-relay test.
18. `pamtester` on the real `gdm-password` stack (§7.3).
19. Pre-paired bunker at the graphical greeter with the single-use bundle (§7.4).
20. Real-phone (Amber) manual UX + SPICE screenshots (§7.5); write `docs/reviews/qr-greeter-<date>.md`.
21. Update `docs/ACCEPTANCE_MATRIX.md` — **D owns this file**; hand evidence paths/hashes over rather than editing it.

---

## 10. Maintainer decisions

| # | Decision | Options | **Recommendation** |
|---|---|---|---|
| **D1** | Provider modelling | (a) new `NH_IDENTITY_PROVIDER_NIP46_QR = 3` type + own record; (b) `mode` flag inside the existing bunker record's `public_config_json` | **(a)**. Cleaner record semantics (no vestigial `secret_blob`), and an account can enable bunker *and* QR simultaneously. (b) is ~1 day cheaper but is the wrong shape. |
| **D2** | QR encoder | (a) vendor Nayuki `qrcodegen` (MIT); (b) `libqrencode` | **(a)** — §6. Keeps the headless stack dependency-pure and avoids adding a `.so` dep to a module `sshd`/`login` load. |
| **D3** | Who renders the QR | (a) PAM module; (b) broker (root) | **(a)**. Keeps the encoder out of the root daemon and the URI is public data anyway. Broker still decides *whether* to offer a QR. |
| **D4** | Default relay for the QR | `wss://bunker.sharegap.net` / `wss://relay.sharegap.net` / a public default (`relay.nsec.app`) | **`wss://bunker.sharegap.net`** — ours, already proven for NIP-46, purpose-named. Overridable per-account and via `auth.conf`. |
| **D5** | Persist the pairing as a durable bunker provider after a successful QR login? | (a) no, ephemeral only; (b) yes, offer to save | **(a) for v1.** Persisting means writing a signer-supplied `bunker://` into the authority DB from an unauthenticated greeter context — a new trust edge that deserves its own design. File as a follow-up. |
| **D6** | PAM choice token | `"qr"` (canonical `nip46qr`) vs reusing `"remote"` with a sub-prompt | **`"qr"`**. Prompt becomes `Sign in with (local/remote/qr):`. One flat level; no nested prompt in the greeter. |
| **D7** | In-dialog QR go/no-go | Decided by the Phase 0 spike (§3.3) | **Run the spike before anything else.** If no-go, v1 ships URI + hint at the graphical greeter and the QR text still ships for text consoles. Do not let this block Phases 1–4. |
| **D8** | Protocol version | Keep `NH_AUTH_PROTOCOL_VERSION = 1` (additive optional field) vs bump to 2 | **Keep 1.** The `display` object is optional and ignorable; nothing on the wire changed meaning. Bump only if you want lockstep client/broker upgrades enforced. |
| **D9** | Long blocking `SUBMIT_UNLOCK` (up to ~90 s) | (a) single blocking call; (b) chunked `WAIT_RESULT` polling so PAM can update the message | **(a) for v1.** The bunker path already blocks (20 s) through the real GDM stack with no worker timeout; (b) buys only a nicer countdown and costs a protocol loop. Revisit if the spike or the lab shows GDM giving up. **Verify in Phase 5 that `gdm-session-worker` tolerates a 90 s conversation.** |
| **D10** | Challenge lifetime for QR | keep 120 s (→ ~78 s scan window) vs raise to 150 s for the QR purpose only | **Keep 120 s for v1**; ~78 s is enough to scan and approve. Raise only if the real-phone runs (§7.5) show users running out of time. |
| **D11** | ECC level / version cap | L + cap 9 vs M + cap 11 | **L + cap 9.** Screen display is undamaged; L gives the smallest module count, which is the scarce resource in a narrow dialog. |
| **D12** | `auth.conf` parsing in the broker | new (this design) vs keep argv-only and pass relays on the command line | **Parse `auth.conf`.** Relay lists do not belong in a systemd unit's `ExecStart`. Keep the parser trivial and non-fatal on unknown keys. |

---

## 11. Open risks

1. **gnome-shell font/wrapping (§3.2)** — highest. Mitigated by the Phase 0 spike and by the URI/hint fallbacks being mandatory, not optional.
2. **Phone-signer interop on the `connect` ack shape** — implementations differ on whether the signer sends `{"result":"<secret>"}` or a `connect` *request* carrying the secret. §4.1 step 4 accepts both. Confirm empirically against Amber in Phase 5.
3. **Relay reachability from a phone on mobile data** while the greeter is on a LAN — both must reach `wss://bunker.sharegap.net`. A site using a LAN-only relay breaks phone pairing; document this in the enrolment guide.
4. **90 s relay pool held by the root daemon** — bounded by `nip46_qr_max_concurrent` (§8.4) and by the cancel/teardown test T-7.
5. **`gdm-session-worker` conversation timeout** at 90 s — unverified; D9 flags it for Phase 5 verification.
