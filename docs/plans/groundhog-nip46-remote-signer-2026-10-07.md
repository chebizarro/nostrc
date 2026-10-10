# Groundhog NIP-46 remote signers (Amber, nsec.app, any bunker): Plan

Status: proposed, 2026-10-07. Target: Groundhog 0.12.0 alpha 4. Bead: to be filed as an epic with one child per work item (§9).

## 1. Goal

A Groundhog account can be backed by any NIP-46 remote signer (Amber on Android, hosted bunkers such as nsec.app, self-hosted bunkers), alongside Grotto (NIP-55L D-Bus) accounts. Pairing works two ways: Groundhog **shows a `nostrconnect://` QR code** for the signer to scan, or the user **pastes a `bunker://` URI**. Every existing feature — NIP-17 DMs, read-only NIP-04, Marmot/MLS groups, NIP-29 groups, NIP-42 AUTH, attachments, setup flows — works for remote accounts, and a received message needs the phone only once.

## 2. Owner decisions (2026-10-07, up-front interview)

| # | Decision | Consequence in this plan |
|---|---|---|
| D1 | **Per account.** Each account remembers its signer backend; the switcher mixes Grotto keys and remote keys. | Identity key becomes `(backend, npub)` (§5.2); global `signer-method` retired (§5.2.4). |
| D2 | **Secrets in Secret Service / Keychain.** | New credential schema, never GSettings or files (§5.3). gnostr's plaintext GSettings approach is rejected. |
| D3 | **Everything, plus a local cache of decrypted messages.** | Remote accounts require the encrypted SQLCipher store; memory-only mode is refused for them (§5.6). |
| D4 | **auth_url: yes.** | Asked whether it can be done over HTTP: no — NIP-46 `auth_url` is an interactive login/approval page at the bunker (password, passkey, consent), so it is opened in the user's browser while Groundhog keeps waiting on the same request (§5.5). |

## 3. Background (evidence)

### 3.1 gnostr prior art (`apps/gnostr`)
- **QR flow** (`apps/gnostr/src/ui/gnostr-login.c`): `on_remote_signer_clicked` (:1106) → `generate_nostrconnect_uri` (:985) makes a random client keypair (secp256k1, :986–1071) and a separate `?secret=` (:999–1011); URI `nostrconnect://<clientpk>?relay=…&secret=…&name=GNostr` (:1073–1095), relays from GSettings `nip46-connect-relays` via `nip46_get_connect_relays()` (:1078). QR rendered with **libqrencode** `QRcode_encodeString` → texture in `generate_qr_texture` (:945–975), optional on `HAVE_QRENCODE` (`apps/gnostr/CMakeLists.txt:37,378–380`), shown in `qr_picture` in `qr_frame` (`data/ui/dialogs/gnostr-login.blp:333,345`). Listens on all pairing relays (`start_nip46_listener`; `on_nip46_sub_event` :1565), decrypts the reply itself with `nostr_nip44_decrypt_v2` (:1613); first valid connect wins (`nip46_connect_handled` :1580, :1734); `on_nip46_connect_success` (:477) builds the session and calls `get_public_key` on a worker (:380–406, :540–542).
- **Paste flow**: `entry_bunker_uri` (blp:378) with clipboard paste (:1186–1205); `on_connect_bunker_clicked` (:1445) → `bunker_connect_thread` (:1225) → `nostr_nip46_uri_parse_bunker` (:1239), sets client secret (:1287), sends `connect` requesting only `"sign_event"` (:1300), then `get_public_key` (:1323). A pasted `nostrconnect://` likely fails ("Invalid bunker URI format", :1241).
- **Routing**: `GnostrSignerService` singleton (`apps/gnostr/src/ipc/gnostr-signer-service.c:181`) holds either `nip46_session` or `nip55l_proxy` (:45–49), set by `set_nip46_session` (:357–394). `sign_event_async` (:935): NIP-46 via `nip46_sign_thread` (:795) which fills the expected pubkey, calls `nostr_nip46_client_sign_event`, and verifies pubkey/kind/signature (:860–900); NIP-55L via async D-Bus (:977–1011). Same split for NIP-44 (:1604, :1695; RPC threads :1554, :1561) and b64 variants (:1869–1910). No NIP-04 routing; no `auth_url` handling.
- **Persistence (rejected)**: plaintext GSettings `nip46-client-secret`, `nip46-signer-pubkey`, `nip46-relays` (`org.gnostr.gnostr.gschema.xml:135–158`), written by `save_nip46_credentials_to_settings` (`gnostr-login.c:1494`, called :360, :1370), restored by `gnostr_signer_service_restore_from_settings` (`gnostr-signer-service.c:1115`; relay fallback :1168–1207) from `gnostr-main-window-auth.c:994`; cleared by `clear_saved_credentials` (:1434). The connect `secret` is not stored (correct).

### 3.2 In-tree NIP-46 library (`nips/nip46`)
- **Codec and URI (pure, reusable)**: `nip46_msg.h` — `nostr_nip46_request_id_generate` (:13), `request_build/parse/free` (:26–31), `response_build_ok/err`, `response_parse/free` (:33–36); `nip46_envelope.h` — `nostr_nip46_build_request_event`, `build_response_event`, `build_encrypted_request_event`, `build_encrypted_response_event` (:14–32); `nip46_uri.h` — `nostr_nip46_uri_parse_bunker` (:25), `uri_parse_connect` (:26), `uri_build_connect` (:37), frees (:39–40).
- **Client session (`src/core/nip46_session.c`, 3,917 lines)**: blocking pthread RPC through `nip46_rpc_call` (:1479) over its **own libnostr relay pool** (`nostr_nip46_client_start` :1018, :1577–1589); default deadline 30 s (:47, :307); 3 workers (`NIP46_RPC_WORKERS` :133), queues 64 interactive / 256 bulk / 8 MiB retained (:134–136), max 4 in flight (`NIP46_DEFAULT_MAX_INFLIGHT` :226), 150 ms spacing (`NIP46_DEFAULT_MIN_INTERVAL_MS` :227, applied :1323), priority for sign/connect/get_public_key over encrypt/decrypt (:1264–1300); async queue (:1969–2041); refcounted sessions (:623), `session_free` blocks for in-flight RPCs. QR helpers `nostr_nip46_client_new_qr_session` and `nostr_nip46_client_await_connect` (`nip46_client.h:325–376`). NIP-04 RPCs `nostr_nip46_client_nip04_{encrypt,decrypt}_rpc` (:2524, :2534). GLib wrapper `src/glib/nip46_client_g.c` covers sign/connect/get_public_key only.
- **Envelope builders** return **unsigned** events (`src/core/nip46_envelope.c:16-32`); the *encrypted* builders take an opaque `NostrNip46Session *` and use session-owned crypto (`nip46_envelope.h:25-33`, `nip46_envelope.c:55-69`). `nostr_nip46_core` packages session, URI, envelope and message sources in one target (`nips/nip46/CMakeLists.txt:6-19`).
- **Gaps confirmed by grep**: no `auth_url` handling anywhere in `nips/nip46/src` (only a parse test, `tests/test_nip46_c1_hardening.c:182–186`); **no SOCKS/proxy support** in `nips/nip46` or `libnostr/src`.

### 3.3 Groundhog today (`gnome/groundhog`)
- **GhSigner** (`src/identity/gh-signer.{c,h}`): API `gh_signer_new/free/select`, `sign_async/sign_finish`, `nip44_encrypt/decrypt_async` + `nip44_finish`, `nip04_decrypt_async` (finished by `nip44_finish`) (`gh-signer.h:31-60`); ops `OP_SIGN/OP_ENCRYPT/OP_DECRYPT/OP_NIP04_DECRYPT` (`gh-signer.c:15`). D-Bus hard-wired, no vtable: one private session-bus connection per op, 330 s call timeout, typed approval errors, methods `SignEvent/NIP44Encrypt/NIP44Decrypt/NIP04Decrypt`; cancellation closes the op's sender; `select`/`free` revoke pending calls (`gh-signer.c:7-35,81-124,321-349`). Input validation (npub; ≤1 MiB UTF-8; 64-hex peer; unsigned event matches selected pubkey, no id/sig; canonical NIP-44 v2 b64; NIP-04 `?iv=`) at `gh-signer.c:88-175,397-444`; reply validation (signature/id/pubkey/unchanged fields; canonical NIP-44; bounded UTF-8) at `:177-196,254-288,464-507`. Errors `GH_SIGNER_ERROR_{INVALID_INPUT,UNAVAILABLE,DENIED,TIMED_OUT,CANCELLED,KEY_MISMATCH,INVALID_RESULT,NO_APPROVER}` (`gh-signer.h:9-28`), mapping at `gh-signer.c:221-252`.
- **GhAccountController** (`src/app/gh-account-controller.{c,h}`): owns the active-generation GhSigner; binds only for active npub + bus + supported method; rebinds on account/method change. States `DISCOVERING, STORE_UNAVAILABLE, NO_IDENTITIES, UNSELECTED, SELECTED_MISSING, ACTIVE`; signer availability separate (`.h:8-23`; `.c:7-27,79-119,247-257`). Listing in a worker, stale serials dropped, `changed` emitted; injectable `gh_account_controller_new_full(settings, bus, list, list_data)` (tests' `fake_list`) (`.c:123-178,261-289`). Default list: macOS D-Bus `ListIdentities`, elsewhere signer-owned Secret Store attributes (`src/identity/gh-identity.c:36-103`); `GhIdentityInfo` has `npub`, `label`. Wrappers `.h:63-108`, `.c:365-542`. `signer_method_supported` (`.c:69-85`) accepts `auto/local/nip55l`; `nip46` reported unsupported (`.c:551-568`).
- **`signer-method` key** (`data/org.nostr.Groundhog.gschema.xml:113-117`, description already says "nip46 remains unsupported"); read by `src/app/gh-send-ui.c:485-498` (+ `changed::signer-method` at :829), `src/app/gh-account-ui.c:188-218`; bound in `src/ui/gh-preferences-dialog.c:1508`; set in `src/app/gh-test-control.h:296`.
- **Signer callers** (all via the controller; none touch GhSigner directly):
  - sign — NIP-42 AUTH `src/app/gh-account-auth.c:103`; attachment consent `src/app/gh-app-services.c:841`; inbox setup `src/app/gh-inbox-setup.c:1041`; relay-list setup `src/app/gh-relay-list-setup.c:298`; NIP-17 seal `src/app/gh-nip17-envelope.c:247`; MLS proof / KeyPackage publish / republish / old-relay cleanup / withdraw `src/mls/gh-mls-service.c:6860,7971,8011,8222,8431`; NIP-29 outbox `src/nip29/gh-nip29-outbox.c:662`; issue dialog `src/ui/gh-issue-dialog.c:139`; onboarding signer test `src/ui/gh-onboarding-view.c:726`.
  - NIP-44 — seal encrypt `gh-nip17-envelope.c:192`; unwrap `gh-nip17-inbox.c:247,277`; onboarding test `gh-onboarding-view.c:660,676`.
  - NIP-04 decrypt — `src/app/gh-nip04-inbox.c:240`. Groundhog never NIP-04-encrypts.
- **Concurrency**: no global cap. NIP-17 unwraps 1 in flight by default (1–4 configurable), 2 sequential decrypts each (`gh-dm-inbox.c:282-301,1284-1288,1480-1482`; `gh-dm-inbox.h:77-91`; `gh-nip17-inbox.c:247-280`). NIP-04 one at a time (`gh-nip04-inbox.c:214-241`). MLS proof gated by `identity_busy`; a KeyPackage round may run two format slots concurrently (`gh-mls-service.c:6837-6863,7030-7036,8454-8460,8518-8528,8627-8642`). AUTH one per relay URL, different relays concurrent (`gh-account-auth.c:10-16,99-139,247-279`).
- **Relay stack with Tor**: `GhRelayScope` (`src/relay/gh-relay-scope.{c,h}`; injectable `GhRelayTransport`, used by every inbox test) and `GhRelayPublish` (`src/relay/gh-relay-publish.h:147-172`, `gh_relay_publish_new[_with_transport]`, `add_url`, `set_deadline`). The installed network-mode dispatcher (`src/net/gh-relay-net.c:78-107`) uses GNostrRelay for `system`/`none` and libsoup with `gh_net_proxy_resolver_new(mode, tor_address, isolation, …)` for `tor` — SOCKS5 with per-isolation credentials (`src/net/gh-net-session.c:140-160`), SOCKS probe (:178); relay URL policy accepts `ws://` only for `.onion` under Tor and loopback (`gh-net-session.c:54-78`). GSettings `network-mode` ∈ `system|none|tor` (`gschema.xml:89-95`). Scope/publish take a caller-supplied generation and need no account signer; per-URL AUTH defaults to `NONE` (`gh-relay-scope.c:155-175`, `gh-relay-scope.h:11-24`; `gh-relay-publish.c:147-177`, `gh-relay-publish.h:28-45`). `GhRelayPublish` sends one **signed** event per relay on private connections, does not retry, reports relay-local OK, and clamps its deadline to 300 s (`gh-relay-publish.h:11-27,144-171`).
- **Storage is keyed by user pubkey only** (`src/store/gh-store.c:325-340,1656,1726`); `gh_account_store_continue_without_saving()` exists (`gh-account-store.c:769-784`), invoked from `gh-app-services.c:509`; `bind_store()` starts inbox/outbox before `OPEN` and can report `OPEN` after a failed restore (`gh-account-store.c:291-320`). `gh_account_describe_limits()` is at `gh-account-controller.c:546-580`; the controller reacts to individual setting changes (`gh-account-controller.c:247-259`) and starts discovery from its constructor (`:261-289`).
- **Keyring**: `GhStoreKey` (`src/store/gh-store-key.h:3-20,29-36,101-141,148-218`; `gh-store-key.c:718-736`; Keychain `gh-store-key-keychain.c:1-18`) — async per-account lookup / lookup-or-create / destroy over `GhStoreKeyBackend` (libsecret / SecItem), storing a fixed 32-byte SQLCipher key with `store-id`/version attributes. Real-keyring test `test-groundhog-store-key-keyring` (`CMakeLists.txt:834-842`; `tests/store/test_store_key_keyring.c`).
- **Decrypted-message cache already exists**: per-account SQLCipher store `$XDG_DATA_HOME/groundhog/accounts/<hash>/store.db` (`src/store/gh-store.h:9-24`; `gh-store.c:325-388`); `GhConversationStore`'s persistence delegate commits decrypted rumor JSON, conversation state and seen ids before the model changes, so stored messages are never re-decrypted (`src/app/gh-conversation-store.c:159-170`; `src/store/gh-store-conversations.c:349-395,667-724,737-779`); the DM inbox skips known wraps before any signer call and persists a cursor with skew overlap. **NIP-04 messages use the same path** (verified: `gh-nip04-inbox.c:199` admits through `gh_conversation_store_admit` keyed by the kind-4 id). Memory-only mode loses messages/seen ids and re-unwraps (`src/app/gh-account-store.c:172-198`; `gh-dm-inbox.c:225-237,304-316,905-912`); rejected-wrap set at `$XDG_STATE_HOME/groundhog/nip17/<hash>.seen` (`gh-dm-inbox.c:1046-1069,1176-1195`; `gh-nip17-inbox.c:788-805`).
- **Onboarding** (`src/ui/gh-onboarding-view.{c,h}`; `data/ui/gh-onboarding-view.blp:3-11`): welcome → account → signer → inbox relays → confirm → publish → done (`.h:22-51`). Account page lists Grotto identities; select → `gh_account_controller_select`; Refresh relists (`blp:156-249`; `.c:497-544`). Seam account-continue → signer page → inbox (`.c:1418-1440`). No QR library in Groundhog's CMake (deps near `CMakeLists.txt:775-791`).
- **Account switcher**: Account submenu in the sidebar main menu, identities then "No Account (Read-Only)" and "Refresh Accounts" (`data/ui/gh-account-ui.blp:102-129`; `src/app/gh-account-ui.c:319-341`); labels = signer label or short npub; sidebar title prefers the cached profile name (`gh-account-ui.c:67-75,226-253`).
- Grotto keeps the `org.nostr.Signer` wire contract and serves all `--instance`s (`docs/designs/grotto-rename-plan-2026-10-05.md`); no Grotto change is needed here.

### 3.4 NIP-46 spec and Amber (external)
- Spec https://github.com/nostr-protocol/nips/blob/master/46.md:
  - `nostrconnect://<client-pubkey>?relay=…(repeatable, required)&secret=…(required)&perms=…&name=…&url=…&image=…`; `perms` = comma list of `method[:param]`. The signer sends a `connect` **response** to the client pubkey; the client learns the remote-signer pubkey from the event **author** and **MUST** check the returned secret.
  - `bunker://<remote-signer-pubkey>?relay=…&secret=<optional>`: client sends `connect [remote-signer-pubkey, secret?, perms?]`, expects `"ack"`; secrets single-use.
  - Methods: `connect, sign_event, ping, get_public_key, nip04_encrypt/decrypt, nip44_encrypt/decrypt ([third_party_pubkey, text]), switch_relays, logout`. `switch_relays` returns the new list or null.
  - Kind 24133 both ways, `p`-tagged, JSON-RPC content; transport encryption NIP-44 (signers may accept NIP-04; Amber answers in whichever the request used).
  - `auth_url`: `{"id":…, "result":"auth_url", "error":"<URL>"}`; client opens it and keeps listening for a second response with the same id.
  - Remote-signer pubkey ≠ user pubkey in general: always `get_public_key` after connect; store both.
- Amber https://github.com/greenart7c3/Amber: scans/pastes `nostrconnect://` (`ui/NewApplicationScreen.kt:58`), can show its own `bunker://`; echoes the secret (`EventNotificationConsumer.kt:347`); deletes bunker secrets after use (`BunkerRequestUtils.kt:316-328`); default relays `auth.nostr1.com, bucket.coracle.social, nrs.primal.net, relay.nip46.com` (`models/AmberSettings.kt:8`), editable; replies on the URI's relays. Permissions: default policy "approve basic actions" or manual; per app "manually approve each permission" or "fully trust"; per permission "remember for…/always"; `perms` prefilled. Background: sticky foreground `ConnectivityService`, battery-optimisation exemption request, subscription refresh every 5 min; Android 12+ may not start the service until the app opens; manual approvals wait for a notification tap; responses retried with backoff (5 s relay ack). **Expect tens of seconds.** Its 5 req/30 s limiter covers only NIP-55 intents. No handlers for `switch_relays`, `auth_url`, `logout` (expect error/null).

## 4. Design overview

```
callers (AUTH, NIP-17, NIP-04, MLS, NIP-29, setup, onboarding)
   └─ GhAccountController  (unchanged wrappers; selects (backend,npub))
        └─ GhSigner  (public API + shared input/reply validation + error domain)
             ├─ backend: NIP-55L D-Bus → Grotto        (existing code, moved)
             └─ backend: NIP-46 → GhNip46Session        (new)
                    ├─ nips/nip46 codec + envelope + URI (reused, pure)
                    └─ GhRelayScope (kind 24133 #p=<client pk>) + GhRelayPublish
                           └─ libsoup + GhNet proxy resolver (system / none / tor)
GhNip46CredentialStore (Secret Service / Keychain)  ── lists remote accounts, holds client key
GhAccountStore (SQLCipher)  ── required OPEN before a remote account may sign/decrypt
```

### 4.1 Transport decision (corrected from the draft)
The planning draft chose to drive `nips/nip46`'s own client session and relay pool behind GTask/GLib bridges and made "add SOCKS5 remote-DNS to libnostr's relay dialer" a release gate. Grep shows **no proxy support at all** in `nips/nip46` or `libnostr/src`, while Groundhog's `GhRelayScope`/`GhRelayPublish` already honour `network-mode` including Tor with stream isolation. **Decision: build `GhNip46Session` on Groundhog's relay stack and reuse only the pure parts of `nips/nip46`** — msg codec (`nostr_nip46_request_build`, `response_parse`), the **plain** (unsigned) envelope builder, URI parse/build — plus libnostr NIP-44 v2 and event signing done in Groundhog with the in-memory client key. The session-bound *encrypted* builders are not used. Groundhog links the existing `nostr_nip46_core` target as a whole (the unused session code is inert); no `nips/nip46` change or target split is required.

Why: Tor correctness comes from the installed network-mode dispatcher (still proven by a real destination test, §10.1); no blocking threads or `session_free` stalls on the main loop; the injectable `GhRelayTransport` makes a fake bunker as easy as today's DM-inbox recorder tests; one relay/network code path in Groundhog. Cost: Groundhog re-implements the session's pending-request table and queue policy (~600–900 lines). The core library stays untouched for gnostr; no second libnostr network path is introduced. Rejected alternative: the library client session — needs a new proxy layer in libnostr (unknown size, release-gating), blocking pthreads, and its 30 s default.

The queue policy copies the library's proven numbers (§3.2): ≤4 in flight, 150 ms publish spacing, 64 interactive / 256 bulk queued, 8 MiB retained input; interactive = `sign_event`, `connect`, `get_public_key`, `ping`; bulk = NIP-44/NIP-04 content ops.

## 5. Detailed design

### 5.1 GhSigner backend seam
Split `gh-signer.c`:
- `gh-signer.{c,h}` — public API unchanged (`sign_async/finish`, `nip44_encrypt/decrypt_async`, `nip44_finish`, `nip04_decrypt_async`, `select`, `free`), all current **input validation before dispatch** and **reply validation after** (signed event id/sig/pubkey/unchanged fields; canonical NIP-44; bounded UTF-8), error domain, generation revocation.
- `gh-signer-private.h` — `GhSignerBackendOps { sign, nip44_encrypt, nip44_decrypt, nip04_decrypt (each async → gchar* | GError), cancel_all, destroy }`.
- `gh-signer-nip55l.c` — today's D-Bus code moved verbatim (private connection per op, 330 s, typed approval errors).
- `gh-signer-nip46.c` — adapter onto `GhNip46Session`; calls remote `sign_event`, `nip44_encrypt`, `nip44_decrypt`, `nip04_decrypt` RPCs (never local transport crypto with the client key).

Constructors: keep `gh_signer_new(bus, npub, error)` (Grotto); add `gh_signer_new_nip46(GhNip46Session *session, const gchar *npub, GError **error)` — controller is the only production owner.

**Error mapping** (existing codes only): bunker `error` reply → classified by a small case-insensitive matcher on the error string (the text itself is never copied into the `GError`): unsupported/unknown method → `INVALID_RESULT` with "Your signer doesn't support this action"; login/authorization required (other than `auth_url`) → `UNAVAILABLE` with "Sign in to your signer and try again"; anything else (rejected, denied, unknown wording) → `DENIED`; deadline expiry → `TIMED_OUT`; no relay reachable / queue full / storage gate closed → `UNAVAILABLE` (queue full message: "Your signer is busy"); caller or generation cancellation → `CANCELLED`; `get_public_key` or signed-event author ≠ account → `KEY_MISMATCH`; malformed, unparseable, wrong-author or mismatched response, bad `auth_url` → `INVALID_RESULT`. `NO_APPROVER` stays Grotto-only (an offline phone is `UNAVAILABLE`/`TIMED_OUT`). Error text never contains URIs, secrets, client keys, RPC plaintext or decrypted content.

### 5.2 Accounts: identity, selection, migration
1. `GhSignerBackend { GH_SIGNER_BACKEND_GROTTO = 0, GH_SIGNER_BACKEND_NIP46 }` added to `gh-identity.h` and to `GhIdentityInfo` (zero default keeps `fake_list` tests valid).
2. Identity key = **`(backend, npub)`**. The same npub may appear twice ("Grotto" / "Remote signer" subtitle); dedupe only within a backend; sort by npub then backend.
3. `gh_account_controller_select_backend(self, backend, npub, error)` verifies the pair is listed, writes `current-backend` + `current-npub` in one `g_settings_delay/apply`, bumps the generation **once**. Because GSettings may notify the two keys separately, the controller stops reacting per key (`gh-account-controller.c:247-259`): any `changed::current-*` schedules one idle **pair reconciliation** that reads both keys and rebinds only if the pair differs, so no transient wrong-backend binding exists. Read-only selection writes `current-npub=''` and leaves `current-backend` as is (ignored when npub is empty). `gh_account_controller_select(self, npub, error)` remains as a Grotto wrapper; empty npub = read-only.
4. **GSettings migration**: add `current-backend` (`'grotto'|'nip46'`, default `'grotto'`) and `backend-migration-version` (int, default 0). Runs in `gh_account_controller_new_full` **before** constructor-triggered discovery (`:261-289`). On first alpha-4 start: if `current-npub` set → `current-backend='grotto'`; reset `signer-method` to `'auto'`; set version 1 (idempotent if interrupted). `signer-method` stays in the schema as deprecated (rollback), its Preferences row (`gh-preferences-dialog.c:1508`) and every runtime read (`gh-send-ui.c:485-498,829`, `gh-account-ui.c:188-218`, `gh-account-controller.c:69-85,254`) are removed; `gh-test-control.h:296` updated. Update `tests/check_privacy.py` allowlist for the new keys.
5. **Merged listing**: the controller runs the existing Grotto list (injectable seam kept) and `GhNip46CredentialStore` listing; one source failing does not hide the other. A selected identity is `SELECTED_MISSING` only when its own source enumerated successfully and omitted it; if its source is locked/unavailable → `STORE_UNAVAILABLE` with retry, selection kept. Stale results dropped by the existing serial.
6. **Readiness**: remote signer sub-states `LOADING_CREDENTIAL, CONNECTING, READY, OFFLINE, LOCKED, ERROR`, separate from Grotto bus availability, reported via `changed`. Replace production use of `gh_account_describe_limits(state, availability, requested_method, network)` with a controller-based description that considers backend + remote state; update `gh-account-ui.c`, `gh-send-ui.c`, onboarding copy and `GhStatus` mapping. The old helper may stay for source compatibility only.
7. **Same npub under both backends shares one store (decided).** Storage is keyed by user pubkey (`gh-store.c:325-340`); the identity — and therefore its messages, seen ids, cursors, MLS state — is the same whichever device signs. Both rows open the same SQLCipher store; the store key is unchanged (changing it would strand existing Grotto data). Only one backend can be selected at a time per instance.
8. **Removal**: switcher "Remove Remote Signer…" (confirmation) → revoke generation → delete credential → refresh. Never deletes the encrypted store or its key; the existing "Forget account" storage action stays separate and says what it deletes.
9. **Rollback**: an alpha-3 binary ignores `current-backend` and works for Grotto selections; a remote-only selection is unusable there. Release notes say: switch to a Grotto account or read-only before downgrading.

### 5.3 Credential store
New `GhNip46CredentialStore` (`src/identity/gh-nip46-credentials.{c,h}`, `-secret-service.c`, `-keychain.c`), final GObject owned by the app service container (`gh-app-services.c`). **Separate from `GhStoreKey`**, whose backend validates a fixed 32-byte key with store-id attributes. Async API: `list_async/finish` (public remote `GhIdentityInfo`s, including locked items where the platform exposes the attribute), `lookup_async/finish(user_pubkey_hex)` (typed locked/unavailable/not-found/invalid/newer-version), `store_async/finish(credential, interactive)`, `delete_async/finish(user_pubkey_hex, interactive)`.

- One item per **user** pubkey (decided: one remote signer per identity; pairing a second signer for the same user pubkey asks to replace the first). Secret Service schema `org.nostr.Groundhog.Nip46Credential`, attributes `account` (lowercase 64-hex user pubkey), `version` (`"1"`). Keychain: service = same name, account = user pubkey, version in comment. Label "Groundhog remote signer" (no identifier in the label).
- Secret = bounded UTF-8 JSON v1:

| Field | Rule |
|---|---|
| `client_secret_hex` | 64 lowercase hex — the NIP-46 transport key |
| `remote_signer_pubkey_hex` | 64 lowercase hex — kind-24133 response author |
| `user_pubkey_hex` | 64 lowercase hex — must equal attribute `account` |
| `relays` | 1–4 unique normalized relay URLs accepted by Groundhog's relay URL policy (`gh-net-session.c:54-78`), user order |
| `transport` | `"nip44-v2"` |

- Never persist the QR pairing secret or a `bunker://` `secret=` (single-use). Reject unknown newer versions without overwrite/delete; conflicting/partial item → "repair: remove and pair again" action, never silent replace. Wipe client-secret buffers on release. No credential, relay list or URI in GSettings, logs, crash reports or SQLCipher.
- `--instance`: Groundhog instances share the keyring; each instance's controller lists all remote credentials (as they list all Grotto identities). Each running instance uses its own session (§5.5).

### 5.4 Pairing UI
`GhNip46PairDialog` (`src/ui/gh-nip46-pair-dialog.{c,h}`, `data/ui/gh-nip46-pair-dialog.blp`), an `AdwDialog` opened from the onboarding account page (new "Use a signer on your phone or a bunker…" row) and the switcher ("Add Remote Signer…"). Two modes in an `AdwViewStack`/toggle:

1. **Scan with your signer** (default): QR, "Waiting for your signer…", selectable/copyable URI (copy warns the clipboard holds pairing access), collapsible pairing-relay editor, Regenerate, Cancel.
2. **Paste a bunker link**: entry + paste button, parsed signer pubkey and relays shown, Connect, progress, Cancel. A pasted `nostrconnect://` gets "That link is for your signer to scan — use the QR tab."

Pairing relays: default Amber's four (`wss://auth.nostr1.com`, `wss://bucket.coracle.social`, `wss://nrs.primal.net`, `wss://relay.nip46.com`); editable; 1–4, normalized/deduped and validated by the existing relay URL policy (`gh-net-session.c:54-78`: `wss://`, plus `ws://` only for `.onion` under Tor and loopback); `.onion` only in Tor mode; note that pairing contacts these relays (IP visible unless Tor). These are signer-control relays, distinct from NIP-65/NIP-17 relays. A bunker URI's relays are used as given (≥1 required; never substitute defaults).

**QR rendering**: libqrencode → `GdkMemoryTexture` in a `GtkPicture`, locally (no web QR service). **Required dependency** when the GUI is built (not optional like gnostr's `HAVE_QRENCODE`): the QR is the primary Amber flow.

**QR state machine**: `EDITING → STARTING_LISTENER → WAITING (QR shown) → VERIFYING → SAVING → COMPLETE`, terminal `FAILED`/`CANCELLED`.
- Fresh client key + 16-byte pairing secret per attempt; `perms` and `name=Groundhog` in the URI.
- Open the `GhRelayScope` for kind 24133 `#p=<client pk>` first and **show the QR only after ≥1 relay reaches EOSE** (truthful "ready to scan"; closes the start/await race).
- Accept an inbound event only after: kind 24133, valid signature, matching `p` tag, NIP-44 transport decrypt, constant-time secret match (response `result` = secret, or `"ack"` only if the spec form with secret param matched), author recorded as remote-signer pubkey. First valid signer wins; duplicates/later signers ignored. If it arrives as a `connect` **request**, reply `ack`.
- Then `get_public_key` → user pubkey (defines the account; may differ from signer key). Optional `switch_relays` (null/error tolerated — Amber lacks it).
- **Confirm before saving (decided).** The QR secret is a bearer token, so a leaked/photographed QR could let another signer win the race. The dialog shows "Connect <display name or short npub>?" (kind-0 name via the contact directory when available, otherwise npub) with the signer's relays, and saves only on the user's confirmation; Cancel at this step closes the listener and discards the session. Same confirmation for the bunker flow.
- **Save credential before** listing/selecting the account. Save failure → nothing registered, session wiped, retry needs a fresh QR. A completed save is not rolled back if selection fails afterwards (it stays listed).

**Bunker state machine**: `EDITING → PARSED → CONNECTING → VERIFYING → SAVING → COMPLETE`. Parse with `nostr_nip46_uri_parse_bunker`; generate a fresh client key; open the subscription **before** publishing `connect [signer pk, secret?, perms]`; pin response author to the URI key; require `"ack"`; then `get_public_key`, save. Never persist or automatically resend the consumed token; on timeout tell the user to get a **new** link from their bunker.

Both: five-minute pairing deadline (covers listener start through verification). Cancel/dialog destruction cancels the scope/publish/pending ids immediately and drops late responses; URI, QR and pairing secret are cleared on cancel, regenerate, success and dispose; never logged or saved.

**Permissions requested** (QR `perms` and bunker `connect`): `get_public_key,sign_event,nip44_encrypt,nip44_decrypt,nip04_decrypt`. (No `ping`: Groundhog never sends it — §5.5 Liveness.) `sign_event` **unqualified** — Groundhog signs kind 22242 AUTH, NIP-17 seals, 10050/10002 setup, MLS proof/KeyPackages, NIP-29 events, attachment consent, issue reports; a kind list would break as features grow. No `nip04_encrypt` (never used). A signer may grant less: missing methods fail at use (`DENIED`/`INVALID_RESULT`); a partial grant is saved, and the onboarding test result plus per-feature errors tell the user what is limited. After pairing, the existing onboarding signer test (`gh-onboarding-view.c:660,676,726`: sign + NIP-44 round trip) runs and the UI says "permissions verified" only if it passed; skipping is allowed but not claimed as verified.

### 5.5 GhNip46Session (protocol engine)
New `src/identity/gh-nip46-session.{c,h}`, a GObject per active remote account per process (and one per pairing attempt).
- **Inputs**: credential (or pairing keys), relay URLs, generation, optional `GhRelayTransport` (tests), `auth-url` callback.
- **Transport**: one `GhRelayScope` subscription (kind 24133, `#p=<client pk>`, `since=now-600s`; widened from 60 s in nostrc-p15n5.3 for phone clock skew, dedup stays by request id) across the credential relays; each request is one `GhRelayPublish` to the same relays (one **signed** event, same request id to every relay → idempotent). Session/pairing attempts allocate their own generation (no account generation or account signer needed). Both inherit `network-mode`; a mode change revokes the generation and recreates the session (no direct socket survives a switch to Tor).
- **Signer-relay NIP-42 (decided)**: URLs are added with AUTH mode `NONE`; if a signer relay demands AUTH (`auth-required` CLOSED/OK), the session answers it signed by the **client transport key**, never the account key and never via the account AUTH path. If that is refused, the relay is marked failed for this session (others continue); all relays failed → `UNAVAILABLE`.
- **Codec**: `nostr_nip46_request_build` / `response_parse`; plain envelope builder → set NIP-44 v2 content encrypted with the client secret ↔ remote-signer pubkey → `nostr_event_sign` with the client secret (all in Groundhog; the session-bound encrypted builders are not used). Replies are decrypted the same way; NIP-04 transport replies are rejected (Amber mirrors the request's scheme, so NIP-44 requests get NIP-44 replies).
- **Request lifecycle**: a new publish waits until the scope has reached EOSE on ≥1 relay (initially and after a full reconnect) so the reply cannot be missed. Publish phase: `GhRelayPublish` deadline 30 s; if **no** relay accepts (OK true) → `UNAVAILABLE`, request removed. The approval timer (330 s) starts at the first relay OK. Some relays accepting but no answer → `TIMED_OUT` at the deadline. The pending entry owns its publish object and cancels/unrefs it on completion, cancellation or teardown. The 300 s publish clamp (`gh-relay-publish.h:163-171`) is irrelevant because the publish phase is short; the long wait is on the subscription.
- **Pending table**: request id → op, deadline, GTask, cancellable handler. Responses accepted only from the remote-signer pubkey, `p`=client pk, known id; first terminal response wins; duplicates dropped; unknown ids ignored.
- **Queue**: §4.1 numbers; interactive before bulk; per-op cancellation removes a queued job without waiting, or drops an in-flight id (no session-wide cancel for one op). Queue full → `UNAVAILABLE` (must not advance any inbox checkpoint — the inbox already holds checkpoints on decrypt failure).
- **Deadlines**: 330 s per request (matches Grotto approval window); after an `auth_url`, total 600 s from request start.
- **auth_url**: recognize exactly `result:"auth_url"` with URL in `error`; keep the pending entry; validate `https://`, ≤2,048 bytes, no userinfo, else `INVALID_RESULT`; on the main context launch with `GtkUriLauncher` (active window, portal-capable; works in Flatpak) once per request — identical repeats don't reopen, a different second URL is a protocol error; launch failure cancels that request with a retry action. Groundhog never fetches the URL itself. Cancellation, account change, timeout or shutdown removes the id.
- **Reconnect**: relay drops are handled by the scope; on reconnect the subscription is re-established before further publishes; in-flight requests keep their ids and deadlines (Amber retries replies; the scope's `since` window catches replies sent during a short drop).
- **`--instance`**: independent sessions from the same credential, same client pubkey; responses are routed by request id, so both instances work concurrently. No shared daemon in alpha 4.
- **Liveness (decided: no ping)**: `READY` = subscription live on ≥1 relay and the last RPC (if any) did not time out; `OFFLINE` = no relay connected or the last RPC timed out; it returns to `READY` on the next successful reply. No periodic signer call (it could prompt Amber). Stored history never depends on liveness.
- **Key binding after pairing**: `get_public_key` runs only at pairing. Later, every signed event's author is validated against the account (`KEY_MISMATCH` otherwise); there is no startup re-handshake (it would prompt on every launch).
- **Signer compatibility (nostrc-p15n5.3)**: reply `p` tags may carry a relay hint (`["p", <hex>, "wss://..."]`) and are accepted. QR (`nostrconnect://`) pairing never waits silently: a reply addressed to the attempt's single-use client key that is not the pairing secret fails the attempt at once with a specific error (secret mismatch; bare `"ack"`; signer error/denial; undecryptable or legacy NIP-04 content); the deadline error names the relays ("No reply from signer on ..." or "Could not reach the signer relays"). **Bare `"ack"` is rejected, not accepted**: the event binds to this attempt (kind 24133, NIP-44 to our fresh key, on the listening subscription), but anyone watching the relay sees that key; only the secret, carried solely in the QR, proves the replier scanned it, and NIP-46 says the client MUST validate it. Cost: a party able to inject on the pairing relay can abort (never hijack) an attempt; the user regenerates the QR. In the `bunker://` flow a `connect` result echoing the URI secret is accepted like `"ack"`.
- **Ownership and teardown** (all on the main context): the controller owns the account session; the pair dialog owns its attempt session. A session owns its scope, pending table and each pending publish; every async completion checks the session generation and a per-request cancellable before touching state. Teardown order on account switch, network-mode change, dialog dispose or shutdown: bump generation → cancel queued and pending requests (complete `CANCELLED`) → cancel publishes → cancel scope → wipe keys → unref. Late responses after teardown are dropped by the generation check; a late pairing reply after save/cancel can neither select an identity nor complete a new operation (tested, §10.1).

### 5.6 Cache, storage gate and offline behaviour
- A remote account must have a **usable durable cache** before account signer work is enabled: new generation-scoped `gh_account_controller_set_remote_storage_ready(self, generation, ready)`. `GhAccountStore` sets it `TRUE` only after the conversation delegate is attached **and** restore succeeded (not merely `state == OPEN`, since `bind_store()` can reach `OPEN` after a failed restore and starts inbox/outbox earlier — `gh-account-store.c:291-320`); for remote accounts the inbox/outbox start moves after that point. It sets `FALSE` **before** unbind/cancellation. A partial restore keeps it `FALSE` and shows the store-recovery UI. While false, account ops fail `UNAVAILABLE` without RPCs (pairing is outside the controller and still works).
- **`gh_account_store_continue_without_saving()` is refused for remote accounts** with an explanation, and its caller/action in `gh-app-services.c:509` is hidden or disabled for remote accounts so the UI never offers it; keyring locked / key missing / corruption → keep selection and files, show unlock/retry/recovery, do not run inbox/outbox/MLS. Grotto accounts keep the memory-only option.
- Cache semantics are the existing ones (§3.3): each new NIP-17 wrap needs two remote NIP-44 decrypts once; after commit, display, restart, duplicates and backfill overlap use SQLCipher. Failure (decrypt, denial, timeout, queue full, DB commit) leaves the wrap unseen and holds the checkpoint. NIP-04 already uses the same admission/seen path. No plaintext cache; unverified replies never cached. MLS/NIP-29 keep their existing encrypted persistence.
- **Phone offline**: stored conversations stay readable; new decrypts/signs show "Waiting for your signer…" then a retryable `UNAVAILABLE`/`TIMED_OUT`; outbound items stay in the existing outbox; denials are not auto-retried; the inbox's deferred-id behaviour gives "Try again" without re-prompting the same wrap within a session.
- **Backlog UX**: when a remote account first opens with a large backlog, the inbox status shows "Decrypting N messages with your signer" (count from deferred/pending wraps) and suggests enabling auto-approve for Groundhog in Amber.

## 6. File-by-file impact

| File(s) | Change | Depends on |
|---|---|---|
| `gnome/groundhog/src/identity/gh-signer.{c,h}`; new `gh-signer-private.h`, `gh-signer-nip55l.c`, `gh-signer-nip46.c` | Backend split (§5.1); public API unchanged; `gh_signer_new_nip46`. | WI-3 |
| new `gnome/groundhog/src/identity/gh-nip46-session.{c,h}` | Protocol engine (§5.5). | WI-1 |
| `gnome/groundhog/src/identity/gh-identity.{c,h}` | `GhSignerBackend` in `GhIdentityInfo`; pair comparison. | — |
| new `gnome/groundhog/src/identity/gh-nip46-credentials.{c,h}`, `-secret-service.c`, `-keychain.c` | Credential store (§5.3). | — |
| `gnome/groundhog/src/app/gh-account-controller.{c,h}` | Merged listing, `select_backend`, signer binding per backend, remote states, storage gate, limits description. | WI-2,3,4 |
| `gnome/groundhog/src/app/gh-app-services.c` | Own `GhNip46CredentialStore`; pass to controller; network-mode → session recreation. | WI-2 |
| `gnome/groundhog/src/app/gh-account-store.c` (+ `.h`) | Call storage gate; refuse continue-without-saving for remote. | WI-4 |
| `gnome/groundhog/data/org.nostr.Groundhog.gschema.xml`; `src/ui/gh-preferences-dialog.c`; `src/app/gh-send-ui.c`; `src/app/gh-account-ui.c`; `src/app/gh-test-control.h`; `tests/check_privacy.py` | New keys + migration; retire `signer-method` reads/row; controller-based limits. | WI-4 |
| `gnome/groundhog/src/app/gh-account-ui.{c,h}`; `data/ui/gh-account-ui.blp` | Backend subtitles, duplicate-npub rows, Add/Remove Remote Signer, remote status. | WI-4,5 |
| `gnome/groundhog/src/ui/gh-onboarding-view.{c,h}`; `data/ui/gh-onboarding-view.blp`; new `src/ui/gh-nip46-pair-dialog.{c,h}`, `data/ui/gh-nip46-pair-dialog.blp` (+ generated `.ui`) | Pair dialog (§5.4); onboarding entry; backend-neutral copy (replace "your key stays in Grotto"). | WI-5 |
| `gnome/groundhog/src/app/gh-dm-inbox.c` (status text only) | Backlog "Decrypting N messages with your signer". | WI-6 |
| `gnome/groundhog/CMakeLists.txt` | New sources/resources; link `nostr_nip46_core` and `libqrencode` (pkg-config `libqrencode`); new tests. | all |
| `gnome/groundhog/tests/app/gh-test-signer.h` | Unchanged Grotto mock. | — |
| new `gnome/groundhog/tests/app/gh-test-bunker.h`, `tests/app/test_nip46_session.c`, `test_nip46_pairing.c`, `test_account_backends.c` | Fake bunker over the recording `GhRelayTransport` (pattern of `tests/app/test_dm_inbox.c`): answers/denies/delays/duplicates/auth_url/wrong-author/wrong-secret. | WI-1… |
| new `gnome/groundhog/tests/store/test_nip46_credentials_keyring.c` | Real private Secret Service + temporary Keychain (pattern of `test_store_key_keyring.c`). | WI-2 |
| `packaging/` (Flatpak manifest, `debian-desktop/control`, `rpm/groundhog.spec`, `archlinux/groundhog/PKGBUILD`), `flake.nix` | Add libqrencode build/runtime deps. | WI-8 |
| `gnome/groundhog/README.md`, `apps/grotto/README.md`, `docs/designs/groundhog-packaging-plan-2026-10-05.md` | Remote signers, relay privacy, phone-offline behaviour, Grotto needed only for Grotto accounts. | WI-8 |

`nips/nip46` itself: **no changes required** — Groundhog uses its msg codec, plain envelope builder and URI functions, and does NIP-44 v2 + signing itself with libnostr (§4.1); it links the existing `nostr_nip46_core` target.

## 7. Errors and edge cases (checklist)
- Wrong secret / wrong `p` / wrong author / bad signature / undecryptable → ignored during pairing (no state change); after pairing → `INVALID_RESULT` only for a matching id from the right author with bad content.
- Signer starts signing with a different key → signed-event author check gives `KEY_MISMATCH`, account `ERROR`, suggest re-pairing.
- Signer grants fewer perms → per-op `DENIED`; MLS/AUTH surface their existing denial UI.
- Phone asleep (Android doze) → requests wait up to 330 s; UI shows waiting; no duplicate publishes.
- All signer relays refuse the publish → `UNAVAILABLE` immediately (not a 330 s wait).
- Leaked QR answered by an unexpected signer → user sees the wrong identity at confirmation and cancels; nothing saved.
- Relay accepts publish but signer never answers → `TIMED_OUT`; checkpoint held.
- Two Groundhog instances on one remote account → both work; Amber may show two approvals for the same op if both instances need it.
- Same npub in Grotto and remote → two rows; selection pair persisted; each backend's state independent.
- Keyring locked at startup → `STORE_UNAVAILABLE` for remote sources, Grotto accounts unaffected.
- Network mode switched to `none` → session stops; remote account `OFFLINE`; nothing queued is lost (outbox semantics).
- `auth_url` while the window is hidden (background service) → present a notification that opens the URL on click instead of launching silently.
- Credential deleted externally → `SELECTED_MISSING` after a successful listing; messages remain; re-pair restores.

## 8. Tradeoffs and risks
- **Own engine vs library session**: chose own engine for Tor and testability (§4.1). Risk: protocol bugs in new code → mitigated by reusing the library codec and by the fake-bunker matrix plus manual Amber/nsec.app checks.
- **Phone latency at volume**: first open of a large backlog can mean hundreds of approvals unless the user trusts Groundhog in Amber. Mitigation: bounded queue, backlog status text, perms requested up front so Amber's "remember" applies; cache guarantees once-only.
- **Same npub, two backends**: confusing UI. Mitigation: explicit subtitles; selection by pair; one shared store (§5.2.7).
- **Rollback**: remote-only selection unusable in alpha 3 (documented).
- **Security**: the client key in the keyring is as powerful as a session token for this signer; deleting the credential (Remove Remote Signer) plus revoking the app in Amber ends access. Remote `nip44_decrypt` replies carry no attestation of the user key — trust rests on the authenticated session (same as Grotto).
- **Relay privacy**: signer relays learn timing/IP (unless Tor) and the client↔signer pubkey link; documented in the pairing dialog.

## 9. Work items (execution index)

| WI | Goal | Done when | Key files | Deps | Size |
|---|---|---|---|---|---|
| WI-0 | Pin current behaviour | Tests cover GhSigner validation, cancellation, generation revocation for Grotto; all 90 `^groundhog` tests green | `tests/app/test_account_controller.c`, new signer test | — | S |
| WI-1 | `GhNip46Session` engine + fake bunker | Session tests pass: request/response, duplicates, wrong author/p/id, queue priority & bounds, per-op cancel, publish-phase failure vs approval timeout, wait-for-EOSE, deadlines, auth_url (one launch, same id, 600 s, bad URL), signer-relay AUTH with the client key, error-string classes, late reply after teardown, reconnect; **plus** one wire-level test against a real local relay (`tests/relay/wire-relay.h`: REQ/EOSE, signed EVENT/OK, disconnect) and a loopback-SOCKS test proving Tor mode dials only the proxy, fails closed, and leaves no connection after a mode switch | `gh-nip46-session.{c,h}`, `tests/app/gh-test-bunker.h`, `test_nip46_session.c`, CMake | — | L |
| WI-2 | Credential store | Real-keyring tests: round trip, list, locked, **non-interactive (no-prompt) lookup while locked**, newer version untouched, conflict, **interrupted store and interrupted delete**, replace-on-second-pairing, delete, no secret in label/attributes | `gh-nip46-credentials*`, `test_nip46_credentials_keyring.c`, `gh-app-services.c` | — | M |
| WI-3 | GhSigner backend split (atomic) | D-Bus code moved unchanged; NIP-46 adapter; shared validation applies to both; full suite green | `gh-signer*.c/h` | WI-1 | M |
| WI-4 | Account model + migration (atomic) | `(backend,npub)` listing/selection with pair reconciliation (one generation bump, no transient binding), migration idempotent and before discovery, `signer-method` retired, remote states, storage gate, limits text; tests for dual bindings sharing one store, one source failing, stale lists, switch mid-op, upgrade from alpha-3 settings | controller, identity, gschema, prefs, send-ui, account-ui, account-store, test-control, check_privacy | WI-2, WI-3 | L |
| WI-5 | Pairing UI | QR shown only after EOSE; identity confirmation before save; bunker token never stored or resent; save before select; cancel (including at confirmation) leaves nothing; onboarding + switcher entry; backend-neutral copy | pair dialog, onboarding, account-ui, CMake (libqrencode) | WI-1, WI-4 | L |
| WI-6 | Cache gate + backlog UX | Gate set only after delegate attach + successful restore, cleared before unbind; remote memory-only refused and not offered (`gh-app-services.c:509`); with fake bunker offline after first commit, restart shows **NIP-17 and NIP-04** messages with **zero** RPCs; new wrap stays deferred, checkpoint held; backlog status text | account-store, dm-inbox, tests | WI-4 | M |
| WI-7 | Full-feature matrix on remote | AUTH, NIP-17 send/receive, NIP-04 read, MLS proof/KeyPackage publish/receive, NIP-29 outbox, onboarding test all pass against a permission-checking fake bunker; two-instance acceptance with a remote account | new acceptance tests, `test_two_instance_acceptance.c` variant | WI-5, WI-6 | M |
| WI-8 | Packaging, docs, alpha 4 | libqrencode in all recipes; release workflow green for the alpha gates: deb ×3 (Ubuntu 24.04, 26.04, Debian 13), RPM, Arch, Flatpak, Nix — AppImage and any unfinished target are not shipped and not claimed; package descriptions say Grotto is needed only for Grotto-backed accounts (remote-only users can skip it); the Flatpak still relies on a native Grotto for Grotto accounts and no Grotto Flatpak is claimed; manual checks (§10.2) done; release notes cover offline behaviour, relay privacy, encrypted storage requirement, rollback | packaging, flake, READMEs, changelogs | WI-7 | M |

Order: WI-0 → (WI-1 ∥ WI-2) → WI-3 → WI-4 → (WI-5 ∥ WI-6) → WI-7 → WI-8. WI-3 and WI-4 each land as one commit (no half-split signer, no half-migrated settings).

## 10. Verification

### 10.1 Automated
- `ctest -R "^groundhog"` (all existing + new) on macOS; Linux and sanitizer gates via `scripts/pre-push` (`linux-gate.sh`, `sanitizer-gate-ci.py`).
- New: `groundhog-nip46-session` (fake transport), `groundhog-nip46-wire` (real local relay + loopback SOCKS), `groundhog-nip46-pairing`, `groundhog-account-backends`, `groundhog-nip46-credentials-keyring` (real gnome-keyring on a private bus, like `groundhog-store-key-keyring`), remote variants of DM inbox/NIP-04/MLS/NIP-29/AUTH tests, two-instance acceptance with a remote account.
- `tests/check_privacy.py`: no secrets/URIs in GSettings or logs; new keys allowlisted.

### 10.2 Manual (Ubuntu 26.04 + macOS, before the cut)
1. Amber (current release): scan QR from onboarding → account appears with display name; send/receive NIP-17; join a Marmot group; NIP-42 AUTH relay; close Amber, background the phone → "Waiting for your signer…", then success on wake; restart Groundhog with phone off → history visible, no prompts.
2. nsec.app: paste `bunker://` → `auth_url` opens browser → approve → request completes; second identical `auth_url` does not reopen.
3. Tor mode: pairing and signing work; packet capture/`ss` shows only the Tor SOCKS port.
4. Same npub in Grotto and Amber: both rows, switching works, Remove Remote Signer leaves messages.
5. Flatpak: QR renders, browser opens via portal, keyring works.
6. Downgrade to alpha 3 with a Grotto selection: works.

### 10.3 Release cut (alpha 4)
Same process as alpha 3: bump `flake.nix desktopVersion`, `packaging/archlinux/groundhog/PKGBUILD`, `packaging/rpm/groundhog.spec` (`upstream_version`, `Release: 0.4.alpha4`, changelog), prepend `packaging/debian-desktop/changelog`; commit, annotated tag `groundhog-v0.12.0-alpha4`, push through the gate; `groundhog-release.yml` publishes the pre-release. Gating artifacts are the alpha-3 set (debs for Ubuntu 24.04/26.04 and Debian 13, Fedora RPMs, Arch, Flatpak, source tarball, SHA256SUMS; Nix via the flake); AppImage is not part of the gate.

## 11. Decisions taken from the critique (no owner input needed)
Recorded from `docs/reviews/groundhog-nip46-remote-signer-plan-critique-2026-10-07.md`; each can be revisited by the owner.
- Same npub in Grotto and remote shares one store (§5.2.7); one remote signer per user pubkey (§5.3).
- Signer relays demanding NIP-42 get AUTH signed by the client transport key (§5.5).
- Human confirmation of the paired identity before saving (§5.4).
- No `ping`; readiness from subscription state and real RPC outcomes (§5.5).
- Codec use is plain envelope + Groundhog-side NIP-44/signing; no `nips/nip46` change (§4.1).

## 12. Open questions
None blocking.

## 13. References
- NIP-46: https://github.com/nostr-protocol/nips/blob/master/46.md
- Amber: https://github.com/greenart7c3/Amber (`models/AmberSettings.kt`, `service/ConnectivityService.kt`)
- gnostr: `apps/gnostr/src/ui/gnostr-login.c`, `apps/gnostr/src/ipc/gnostr-signer-service.c`
- `nips/nip46/include/nostr/nip46/{nip46_msg,nip46_envelope,nip46_uri,nip46_client}.h`, `nips/nip46/src/core/nip46_session.c`
- `docs/designs/grotto-rename-plan-2026-10-05.md`, `docs/designs/groundhog-packaging-plan-2026-10-05.md`
- Critique: `docs/reviews/groundhog-nip46-remote-signer-plan-critique-2026-10-07.md`
