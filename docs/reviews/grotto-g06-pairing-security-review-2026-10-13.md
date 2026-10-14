# G06 pairing, live relay configuration, camera scan — focused security review

**Target:** `master` (`0d4dd1faf`) … `origin/grotto/g06-pairing` (`26550a440`, single commit "grotto: add consent-bound pairing and live relay configuration"), 47 files, +2008/−940. Line numbers refer to `26550a440`.
**Spec:** `docs/plans/grotto-signer-ux-2026-10-12.md` §5.9 (Pairing; Runtime state & configuration; `nostrconnect://` relays; Legacy allowlist; Relays; camera/OC-8), §5.10 client/bunker configuration, §9 `signer/bunker-e2e` pairing cases and `signer/qr-scanner`.
**Method:** source reading of the full diff and the touched call paths in `nips/nip46`, `libnostr/src/simplepool.c`, `nips/nip55l` and the GUI. Nothing was built or run for this review. **CONFIRMED** means the defect follows directly from the code paths cited. **PLAUSIBLE** means it depends on runtime behaviour or on how the spec is read.
**Scope:** limited to the seven areas in the request. Areas outside them, such as the NIP-5F comment in the unit file and G05's R1/R2, are not reviewed here.

## Verdict

**Approve with changes.** I found no path that grants signing authority from token possession, from client `perms`, or from legacy allowlisting. The token core is sound:

- 32 bytes come from `RAND_bytes` and are encoded as 43-character base64url with no padding.
- Matching is constant-time and limited to one authenticated peer. The reservation happens on the single-threaded main context.
- Approval consumes the token. Cancel, revoke, pause, endpoint reset, disable and config change all invalidate it.
- Tokens do not survive a restart.

The `nostrconnect://` path contacts no relay before the identity chooser and the review approval. The ack is a NIP-46 *response* whose result is the client's secret.

Two Medium issues need fixing before this ships:

- **F1:** every NIP-46 response, including pairing acks, goes to every relay in the shared pool. Relays approved for one client therefore see all other clients' traffic metadata.
- **F2:** approving a `nostrconnect://` pairing durably re-enables remote signing for everyone after the user explicitly disabled it.

The remaining findings are Low.

## Findings (most severe first)

### F1 — Medium, CONFIRMED: responses and acks are broadcast to every pool relay, so one client's approved relay sees all clients' traffic

- `nips/nip46/src/core/nip46_session.c:3145-3166` (`nip46_publish_response`): "Publish to all connected relays in the pool". This covers both deferred replies and the new `nostr_nip46_bunker_connect_response` ack (`:3206-3229`).
- The pool is the effective union: signer-wide relays plus every paired client's `relays` (`apps/grotto/daemon/grotto_control.c:323-336` `effective_relays`; `bunker_service.c:805-841`).
- **Scenario:** the user pairs app X through `nostrconnect://…?relay=wss://x-owned.example`. From then on, `x-owned.example` receives every kind-24133 response the signer publishes for *every* client. Each response carries the recipient client's pubkey in its `p` tag, plus timing and size, and the relay sees the transport key's REQ. The first ack for a new `nostrconnect://` client is also sent to the signer-wide relays and to other clients' relays.
- Contents stay NIP-44-encrypted, but this is cross-client metadata exfiltration through a relay the user approved for a single app. It also undercuts §5.9 "contact only user-confirmed relays" and `apps/grotto/docs/pairing.md`, which says the daemon does not advertise "another client's private relay list". The relays are not advertised, but they do receive the traffic.
- **Fix:** `NostrNip46BunkerRequest` now records the delivery relay (`nostr_nip46_bunker_request_relay`). Publish each reply to the relay(s) its request arrived on, falling back to that client's approved relays. Publish the `nostrconnect` ack only to `o->relays`.
- **Regression test:** two clients on disjoint relays; assert that `relay2` never stores an event p-tagged to client 1.

### F2 — Medium, CONFIRMED: approving a `nostrconnect://` pairing overrides an explicit "remote signing disabled"

- `grotto_control.c:386-391` (`pairing_commit`): `if (relays && !c->desired_bunker) { save_bunker_config(c, TRUE, …); c->desired_bunker = TRUE; } … apply_bunker(c)`.
- `AcceptConnectURI` does not check `desired_bunker` (`grotto_control.c:581-586`), and `offer_new` does not require a running listener (`bunker_service.c:701-711`).
- **Scenario:** the user runs `ConfigureBunker(enabled=false)` as a kill switch. Later, an approver accepts one `nostrconnect://` link. The daemon persists `enabled=true` and resubscribes the whole union: signer-wide relays and every *previously paired* client's relays. Those clients regain remote access with no further consent.
- The review only says "Pairing is not a signing permission". The behaviour is documented in `pairing.md` and is what `test-bunker-e2e.c` `uri_pairing_and_reconfigure` asserts, but it conflicts with §5.9: desired state belongs to `ConfigureBunker`, and "disable closes subscriptions and cancels pending remote ops".
- **Fix (pick one):**
  - Reject `AcceptConnectURI` with `NotReady` while disabled, and point the user to enable remote signing first.
  - Or, while disabled, start *only* the approved client's relays, without persisting a global enable.
  - Or disclose "this re-enables remote signing for N existing clients" in the review, and keep that consent separate.

### F3 — Low, CONFIRMED: pairing persistence is not atomic with relay activation or the ack

- `grotto_control.c:383` persists `paired=true` and the client relays through `grotto_clients_finish_pairing`. Only afterwards can `save_bunker_config` (`:387-388`) or `apply_bunker` (`:391`) fail.
- On failure, `outbound_delivered` marks the offer terminal (`bunker_service.c:770-772`) and sends no ack. The client stays durably paired, and its relays join the union on the next `apply_bunker`.
- The same happens if no client relay reaches EOSE before the 5-minute deadline: `deliver_offers` skips the expired offer (`:747`) after commit has already persisted.
- This is not an escalation, because the user approved. It does leave a paired client that never received an ack, which F1 makes noisier.
- **Fix:** validate and persist config before `finish_pairing`, and roll back admission on failure. Alternatively, mark the record pending until the ack is published.

### F4 — Low, CONFIRMED: consumed offers keep the token and a capacity slot until expiry

- `connect_delivered` sets only `offer->consumed = TRUE` (`bunker_service.c:174`). The 43-byte secret stays in secure memory, and the offer counts toward `>= 4` (`:706`) until `offer_expired` (`:698`) up to 5 minutes later.
- Terminal outbound offers (`:750`, `:758`, `:771`) also hold slots until expiry.
- The e2e test only demonstrates capacity being freed by *revoke* or cancel ("offer capacity released by cancellation/revoke").
- **Fix:** on consume or terminal, `secure_free` the secret and remove the offer. The completed `Rpc` cache already serves duplicate-ack retries.

### F5 — Low, CONFIRMED: avoidable unwiped token copies outside daemon secure memory

- `nostr-gobject/src/nostr_nip46_bunker.c:425-426`: `*out_uri = g_strdup(uri); free(uri);` frees the libnostr URI buffer, which contains the secret, without wiping it.
- `bunker_service.c:739` copies the URI into a GVariant that is never wiped.
- The D-Bus marshalling and GUI copies are inherent: the label, QR texture and clipboard are cleared on close (`sheet-qr-display.c` `pairing_closed`). The library buffer is not inherent.
- I found no logging of the token or of either full URI: the daemon logs only `BUNKER_STATE`, and nip46 `NOSTR_DEBUG` prints relay URLs only. However, no test asserts this (see §7).

### F6 — Low, PLAUSIBLE (spec reading): token lifetime is not tied to continued usability or to denial

- **"Shown only when LISTENING" is enforced only at issue time.** `create_pairing` and `get_bunker_uri` require `RUNNING` (`bunker_service.c:576`, `:725-726`). `relay_state` (`:412-421`) does not invalidate offers on OFFLINE or ERROR, and the QR sheet does not watch `StatusChanged`. §5.9 says "Pairing-token generations invalidate when their relay config is no longer usable". Only *config changes* invalidate today (`grotto_control.c:432-435`).
- **Deny does not invalidate.** `gate_reply`'s error path (`bunker_service.c:188`) completes the RPC but leaves the reserved offer live. The same client can raise fresh reviews with new RPC IDs until expiry. This is consistent with "release only for that client's retry", but a denial is not a retry.

### F7 — Low, CONFIRMED: `AcceptConnectURI` does not resolve the identity to a stored, signable account

- `CreatePairing` normalizes through `nostr_nip55l_normalize_selector` (`grotto_control.c:443-471`).
- `AcceptConnectURI` passes the raw string to `offer_new`, which only syntax-checks hex or npub and calls `accounts_store_is_watch_only(identity)` on the raw value (`bunker_service.c:708-710`). It can therefore bind a pairing to an npub that is not in the store.
- If the store is keyed by npub, a hex selector also skips the watch-only check (PLAUSIBLE).
- Impact is limited: only approvers can call it, and later signing fails. **Fix:** reuse the `PairingIdentity` worker.

### F8 — Low, PLAUSIBLE: a failed live reconfigure keeps the removed relays while the persisted config says they are gone

- `configure_bunker` persists first and ignores `apply_bunker`'s result (`grotto_control.c:431`, `:441`).
- If `nostr_nip46_bunker_reconfigure_relays` fails (allocation failure, >256 relays, `!listening`; `bunker_service.c:821`), runtime keeps the *old* relay set, including any relay the user just removed.
- This divergence is visible in `bunker_relay_states`, which matches §5.9's "report divergence". But for a removal, divergence means continuing to contact a relay the user distrusted. Prefer fail-closed: stop, or drop the removed relays, when the in-place diff fails.

### F9 — Low, CONFIRMED (camera): denied/busy detection matches English message substrings

- `qr-scanner.c:110-115` uses `strstr(message, "Permission denied" / "busy")`. GStreamer and `strerror` text is localized, so in non-English locales the result falls back to the generic message.
- The fallback is still honest and still offers paste, so this is a UX issue, not a security one. The test injects English strings (`test-qr-scanner.c:55-65`).
- **Fix:** classify by `GST_RESOURCE_ERROR_{OPEN_READ_WRITE,BUSY,NOT_FOUND}` and the errno in the debug string.

### F10 — Low, PLAUSIBLE (camera): a synchronous state-change failure leaves the pipeline allocated

- On `GST_STATE_CHANGE_FAILURE`, `start_pipeline` returns `TRUE` with `active=TRUE` and keeps `s->pipeline` (`qr-scanner.c:144-148`). It relies on a bus ERROR to tear down.
- If the element fails without posting one, the v4l2 element stays allocated, not in NULL state, until close or dispose. Both of those do tear down (`gn_qr_scanner_stop` `:181-192`; `sheet-qr-scanner.c` `scanner_closed`), so there is no lasting handle leak.

## Area checklist

### (1) Token security

| Requirement | Status | Evidence |
|---|---|---|
| 32 CSPRNG bytes, URL-safe | ✅ | `RAND_bytes` into secure buffer; base64url, 43 chars, no padding (`bunker_service.c:728-734`); e2e asserts length and alphabet |
| 5-min, single use | ✅ / F4 | Monotonic deadline and expiry source (`:719-722`); consumed only in `connect_delivered` after commit (`:174`); reuse → `invalid_pairing_token` (tested) |
| ≤4 outstanding | ✅ / F4 | `:706`; terminal and consumed offers occupy slots |
| Bound to account + generation | ✅ | `offer->identity` replaces the default identity (`:317`); generation checked at match and in `gate_reply` (`:305`, `:191`); invalidated on stop, reset and ConfigureBunker change |
| Secure memory only; never logged | ⚠️ F5 | No logging found; library buffer and GVariant copies are unwiped |
| Shown only when LISTENING | ⚠️ F6 | Checked at issue only |
| Atomic reservation to one authenticated client | ✅ | Constant-time compare; `pairing_reserved` for other peers (`:307-313`); reservation set on the main context (`:389`); tested |
| Consumed on explicit approval | ✅ | Manual review: `existing_pairing` is false for unpaired clients; no tier auto-allows CONNECT |
| Invalidated on expiry, cancel, revoke, reconfigure, pause | ✅ | `cancel_pairing` (`:656-676`), `disconnect_client` (`:587-592`), `grotto_control.c:434`, `:555`; cancel and revoke tested |
| Lost on restart | ✅ | In-memory hash only |
| Possession ⇒ review only | ✅ | Token never sets `paired`; tier staging is not admission (tested) |

### (2) `nostrconnect://`

- **Parse only before confirmation:** ✅. The GUI parses locally (`pairing-flow.c:70-86`, `sheet-qr-scanner.c` `show_result`). The daemon parses in `AcceptConnectURI` only after an identity is chosen, then validates pubkey, secret ≤1024, name ≤160, perms ≤2048, relays (`pairing_uri.c:662-684`).
- **No relay contact before chooser + approval:** ✅. Relays join the pool only through `pairing_commit` → `apply_bunker` after the gate allows. The e2e test asserts zero connections and zero REQs on the client relay before approval.
- **Ack uses the client secret:** ✅ (`nip46_session.c:3206-3229`; tested). Fan-out: F1.
- **Per-client relays persisted, resubscribed after restart, counted in readiness:** ✅ (`clients.ini` `relays`; `effective_relays`; `relay_state` counts the union; `client_relay_restored` test).
- **Legacy allowlist = admitted-not-paired:** ✅ (`bunker_service.c:315-316`). No token goes to review; tier `-1` keeps Low.
- **`perms` are hints only:** ✅. They feed `connect_hints` and are never persisted.
- **Pairing approval re-enables a disabled bunker:** ❌, see F2.

### (3) ConfigureBunker

- **Full-candidate validation:** ✅. All three keys required, unknown keys rejected, methods whitelisted, relays validated, effective union validated when enabled (`grotto_control.c:414-430`).
- **Atomic persist:** ✅. `g_file_set_contents_full` with CONSISTENT|DURABLE, mode 0600, directory 0700, profile guard (`:360-368`).
- **In-place diff:** ✅. Unchanged sockets and REQs are preserved (`nip46_session.c:3004-3040`, which relies on `pool_register_relay` registering synchronously so state and auth callbacks attach; verified in `simplepool.c:743-752`). RPCs whose only delivery relay was removed complete `relay_removed`. Others are untouched.
- **Disable cancels pending remote operations:** ✅ (`bunker_service_stop`).
- **Token invalidation on config change:** ✅. On usability loss: F6.
- **`ws://` loopback only, onion rejected:** ✅. Enforced in both `grotto_relays_validate` and `bunker_service_start`, so the unvalidated legacy-GSettings load path still fails closed.
- **No fallback or hardcoded public relays:** ✅. The sheet seeds relays only from status; the doc URL is marked as a placeholder.
- **Tor/Orbot controls absent:** ✅. `row_orbot` is hidden and `Suggests: tor` was dropped.
- **Failed diff keeps removed relays live:** F8.

### (4) Camera scanner

- **Frames reach the decoder:** ✅. appsink `new-sample` → RGB map → `gn_qr_scan_pixbuf`, with one frame in flight (`qr-scanner.c:87-109`). The gtksink path is gone.
- **Result marshalled to the owner context:** ✅. An idle source runs on the captured thread-default context with a generation guard (`:75-86`, `:107-108`). The test asserts the owner thread. zbar runs on the streaming thread, which is acceptable.
- **Teardown on close, cancel and dispose:** ✅. `set_state(NULL)` joins the streaming threads before the generation bump. See F10.
- **No-camera, denied and busy states:** ✅ with honest text and paste fallback. Classification caveat: F9.
- **Preview before any relay contact:** ✅. The scanned URI goes through `show_result` → `grotto_pairing_present` → chooser.
- **Fixture drives a real pipeline:** ✅. `filesrc ! pngdec ! videoconvert ! appsink` decodes a generated QR, and the CI image has plugins-good.
- **Required on Linux:** ✅. CMake and meson mark it REQUIRED, and deb/rpm dependencies are added.

### (5) systemd

- **`ExecReload` removed:** ✅. No SIGHUP handler exists, so the old reload would have killed the daemon.
- **Hardening and `IPAddressDeny=any` unchanged:** ✅.
- **Drop-in documents `IPAddressDeny=` and the `--no-approval-ui` ExecStart variant:** ✅. Covered in `bunker.conf.example:15-21` and `headless-bunker.conf.example`.
- **Note:** `bunker.ini enabled=true` now starts relay dialing without `--bunker`. Under the base unit that ends in honest OFFLINE, which is acceptable and documented.

### (6) Interaction with G05 and G07

- **G05 replay fencing:** no regression found.
  - The completed-cache revision fence now skips `connect` (`bunker_service.c:362-365`), so unrelated permission changes don't break duplicate-ack retries.
  - Revoke still purges every cached RPC for the client (`:593-601`), so a post-revoke replay gets `invalid_pairing_token` or `replayed_request` (tested).
  - Pause is still fenced by generation. Durable seen-IDs are unchanged.
  - `nip46_process_bunker_event` now holds `pool_mutex` while walking `relays`, which closes the race with reconfigure removal.
- **G07 connect preview row:** `gnostr_approval_info_can_approve` now requires a `nip46:` 64-hex principal, a non-empty identity and 1–64 relays (`approval_dialog.c:442-450`). Only NIP-46 submits `SIGNER_GATE_CONNECT` (`bunker_service.c:258`, `:799`), so local transports are unaffected. The preview renders strv hints and relays and never shows the secret (tested).
- **Minor edge (Low, CONFIRMED):** a legacy-allowlisted connect while the signer-wide relay list is empty submits an empty `relays` array, which makes the review impossible to approve. It fails closed, but the review gives no explanation.
- **Minor UX (Low, CONFIRMED):** `approval-controller.c:946` now binds the tier fetch to the request. If the review resolves first, `fetch_current` returns false and the success or failure message for the trust change is suppressed.

### (7) Coverage gaps against §9

**`signer/bunker-e2e`:**
- A token that actually expires. Only `expires_at` is asserted; a short-TTL test hook is needed.
- Token invalidated by a ConfigureBunker change, and by `SetPaused(true)`.
- An offer from before a restart is rejected after restart.
- `CreatePairing` returns `NotReady` in DEGRADED, OFFLINE and STARTING.
- Headless (`--no-approval-ui`) refusal of `CreatePairing` and `AcceptConnectURI`.
- Deny then retry with the same token (F6).
- A legacy-allowlisted connect without a token reaches review and holds zero permissions afterwards.
- No scan of the daemon log (`f->log_path`, already captured) for the token or the full URI.
- `nostrconnect` deny leaves the client relay uncontacted.
- `AcceptConnectURI` rejects invalid URIs (`ws://` remote, onion, >64 relays, oversized fields) at the daemon.
- F1 fan-out and F2 disabled-override are not asserted; F2 is asserted as *desired*.
- No `relay_removed` cancellation of an outstanding RPC. Only socket and REQ preservation is tested.
- No outstanding RPC surviving an unrelated diff.
- ConfigureBunker negatives over D-Bus: unknown or missing field, bad relay, persist failure → divergence.

**`signer/qr-scanner`:**
- §9's "no relay contact before confirmation" is not asserted in this suite. It is covered only indirectly by bunker-e2e for `AcceptConnectURI`.
- No sheet-level test that closing during streaming sets the pipeline to NULL.
- The no-device case does not assert the "No camera was found" text.
- Denied and busy use English synthetic errors (F9).

**systemd:** no static check (for example in `signer/desktop-contract`) that the unit lacks `ExecReload` and that the drop-ins contain `IPAddressDeny=` plus a single final `ExecStart`.

## Recommendations (priority order)

1. **F1:** route replies to their delivery relay or the client's approved relays, and the `nostrconnect` ack to `o->relays` only. Add the two-client disjoint-relay regression.
2. **F2:** do not let pairing approval flip global `enabled`. Either refuse while disabled, or scope activation to the approved client with explicit disclosure. Update `pairing.md` and the e2e expectation.
3. **F3 and F4:** order commit/persist/activate so that failure rolls back admission. Free the secret and the slot on consume or terminal.
4. **F5:** wipe the libnostr URI buffer in `gnostr_nip46_bunker_issue_uri`. **F7:** route `AcceptConnectURI` through the selector-normalization worker.
5. **F6 and F8:** decide the spec reading for "no longer usable" and for deny. Make a failed relay removal fail closed.
6. **F9 and F10:** classify camera errors by GStreamer error code, and stop the pipeline on a synchronous state-change failure.
7. Close the §7 coverage gaps. Prioritize the log-secret scan, real expiry, restart drop, deny-no-contact and the F1/F2 regressions.
