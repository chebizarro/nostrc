# NIP-46 remote signer: gnostr vs Groundhog (2026-10-10)

Read-only audit. Paths are relative to the repo root. `GH` means `gnome/groundhog/src`, `GN` means `apps/gnostr/src`, and `LIB` means `nips/nip46/src/core/nip46_session.c`.

Classes:
- **A**: port gnostr's behaviour.
- **B**: an owner decision rules out gnostr's approach (keyring storage, Groundhog network mode/Tor). Keep Groundhog's approach and make it as robust as gnostr.
- **C**: Groundhog is better. Keep it.

## Executive summary

gnostr works mainly because its happy path is short:
- It accepts any plausible connect reply.
- It persists synchronously to GSettings.
- It hands the live, already-connected session straight to the signer service.
- At startup it restores lazily with no network wait.

Groundhog's path is long and every step can stall:
1. It waits for an EOSE before sending anything.
2. It rejects a bare QR `ack`.
3. A modal confirmation follows.
4. It saves to the keyring asynchronously.
5. It tears down the live session.
6. It re-lists both Grotto and the keyring.
7. It waits for the controller's `changed` signal to select the account (with a 15 s timeout).
8. It loads the secret from the keyring again.
9. It opens new relay connections.
10. It waits for an EOSE again before the account is "ready".

Most of the stalls the owner has hit come from steps 5–10 or from the gating in steps 1–2. The protocol core (`gh-nip46-session.c`) is mostly sound and stricter than gnostr in several useful ways.

## 1. bunker:// flow

| Aspect | gnostr | Groundhog | Class |
|---|---|---|---|
| Parse | `nostr_nip46_uri_parse_bunker` `GN/ui/gnostr-login.c:1238-1250`; any scheme check `:1459` | Same parser, plus live validation while typing `GH/ui/gh-nip46-pair-dialog.c:593-622`; relays normalized and checked against network mode `:157-207` (max 4) | C |
| Client key | `nostr_key_generate_private` `gnostr-login.c:1257` | Same `GH/identity/gh-nip46-session.c:908` | = |
| When connect is sent | Pool `client_start` waits ≤5 s for the first CONNECTED, ≤3 s for the rest, then publishes. Late publish to relays that connect afterwards `LIB:1018-1165, 1786-1812` | Only after the **first EOSE** on the listening scope (`begin_bunker_connect` `gh-nip46-session.c:492-504`, called from `scope_update_inner :619-626`). A relay that never sends EOSE, or a slow Tor circuit, means connect is never sent, and the 300 s `pair_timeout` `:1094-1121` is the only exit | B (keep "listen before publish", but gate on REQ sent / socket open, add a short EOSE grace) |
| perms | `"sign_event"` only `gnostr-login.c:1300` | `get_public_key,sign_event,nip44_encrypt,nip44_decrypt,nip04_decrypt` `gh-nip46-session.c:23`. Correct for Groundhog's DM use. Some bunkers (nsec.app) show every perm and may reject or ask per method | C (keep; see §7 for a fallback) |
| Secret | Passed as connect param 2 `:1300` | Same `:498-500`; wiped after ack `:486`, `wipe_free` | C |
| Connect result | `ack` or the secret expected, but **any other value is accepted** with a warning `:1313-1317` | Must be `ack` or a constant-time match of the secret; anything else fails `:474-484` | C (matches spec; gnostr's "continue anyway" is lax). Consider accepting an empty-string result as `ack` (seen from some bunkers) |
| get_public_key | Synchronous in the same thread, same session `:1320-1331` | Async on the same session `:487-489`, hex64-checked `:456-460` | = / C |
| Timeouts | 30 s per RPC (`NOSTR_NIP46_DEFAULT_TIMEOUT_MS`, `nips/nip46/include/nostr/nip46/nip46_client.h:29`). Whole flow ≈ 60 s | Pair deadline 300 s `:786`. Per call: publish 30 s, then approval 330 s `:19-21`. The user waits up to 5 min with only "connecting…" | A (shorter, staged feedback: "relay unreachable" after ~15 s, "no answer from signer" after ~45 s) |
| Retries | Late publish to relays that connect later; rate-limited NACK backoff ×3; abort early when every relay NACKs `LIB:1786-1820` | `gh_relay_publish` deadline; failure only when no relay accepted `:355-373`. No republish after a reconnect, no rate-limit backoff | A (port the late-publish and rate-limit retry semantics into `gh_relay_publish` or the session pump) |

## 2. nostrconnect:// QR flow

| Aspect | gnostr | Groundhog | Class |
|---|---|---|---|
| URI params | `relay=` (all, escaped), `secret=`, `name=GNostr`. No `perms` `gnostr-login.c:1077-1095` | `nostr_nip46_uri_build_connect` with relays, secret, perms, `name=Groundhog` `gh-nip46-session.c:876-884` | = (Groundhog includes perms; Amber shows them, which is fine) |
| Key generation | `g_random_int_range` for the client secret, comment says "use a CSPRNG" `:987-991` | `nostr_key_generate_private` for the key and the token `:862-875` | **C** (gnostr uses a non-CSPRNG client key) |
| Relays | Configured pairing relays (GSettings) | Editable row, default Amber set `gh-nip46-pair-dialog.c:12-15`, validated under network mode | B/C |
| Listen before QR | QR drawn immediately, listener started in parallel `:1106-1124, 1804-1850` (race: a fast scan can be missed, though LIB's `since` window helps) | QR is hidden until the first EOSE `on_ready :242-260`; hidden again on `offline` `:262-272` | C in principle, **but** under Tor, or with a relay that is slow to send EOSE, the user sees "Connecting to pairing relays…" indefinitely. Show the QR once the REQ has been sent on ≥1 relay, with an EOSE timeout fallback |
| Subscription | `gnostr_pool_subscribe_multi` on all relays, no `since` `:1767-1785` | One scope over all relays, `since = now-600` `gh-nip46-session.c:939-943` | C |
| Accepting the response | First valid response wins (`nip46_connect_handled` latch `:1580, 1734`). Accepts `ack`, the matching secret, **or any 64-char string** `:1678-1693` | `result == secret` (constant-time) only. Also accepts a signer-initiated `connect` *request* carrying the secret and replies `ack` `:552-611`. Bare `ack` → **hard failure** `:572-575`. Undecryptable reply → hard failure `:650-659` | **Mixed.** Rejecting `ack` follows the spec but breaks older Amber builds and some other signers that reply `ack` to nostrconnect. gnostr's acceptance is too lax. Recommended (A-adapted): accept `ack` from a not-yet-bound author only when nothing better arrives within a short window, then rely on the existing confirmation dialog showing the npub (§3) as the human check. Never accept "any 64 chars" |
| Abort on a stray event | gnostr ignores non-matching events (`return`) | Groundhog fails the whole attempt on the first non-matching reply (`fail_pairing`). An injected or stale event on a busy relay kills the QR | A: ignore and keep waiting; log and show a non-fatal hint |
| Encryption | NIP-44 only (LIB default `transport_mode` NIP44 `LIB:443`) | NIP-44 only; tells the user if the reply looks like NIP-04 `:647-658` | = (neither falls back to NIP-04; see §7) |
| Error replies | Any `error` string → error status, but the listener keeps running `:1646-1658` | `classify_error` → fail pairing | = |

## 3. After pairing

| Aspect | gnostr | Groundhog | Class |
|---|---|---|---|
| Confirmation | None. Success closes the window `show_success :1523-1532` | Modal `AdwAlertDialog` "Connect X?" / "Replace signer…" `gh-nip46-pair-dialog.c:505-546`. Was the source of the Save bug (closed-before-response, fixed `:423-435` via an idle). Still connects to `closed` **and** `unmap`, so a dismissal can race a slow response | C for the human npub check (it is also what makes accepting `ack` safe), **but** move it inline onto the dialog page (a "Save" button in the dialog itself, not a nested alert) to remove the signal-ordering hazard entirely |
| Persistence | Synchronous GSettings write of client secret, signer pubkey and relays, then `current-npub` `:1360-1371, 1486-1521`. Done on the worker thread for bunker | Async keyring store with a 60 s timeout and cancel `:437-482, 357-366` | B (keyring is required). Keep it, but see the next row |
| How it becomes active | `signed-in` signal → main window takes the **live session** (`gnostr_login_take_nip46_session`) and installs it in the signer service `GN/ui/gnostr-main-window-auth.c:474-491`. No re-listing, no reconnect | After the save, `stop_attempt` **cancels the live session** `:388` → `gh_account_controller_refresh` re-lists Grotto **and** keyring `GH/app/gh-account-controller.c:431-462` → dialog waits for `changed` → `select_backend` `:303-329` (15 s timeout `:288-301`) → controller `bind_signer` looks the credential up in the keyring again `:212-247` → new `gh_nip46_session_new` → new relay sockets → wait for EOSE → READY | **A (most important port).** Add `gh_account_controller_adopt_remote(controller, npub, GhNip46Session *live, credential)`. It inserts the identity into the in-memory list, writes `current-npub` and `current-backend=nip46` in one transaction, and binds the signer to the live session. Re-list in the background only to reconcile |
| Listing dependency | n/a | `finish_listing` waits for **both** sources (`list_pending[0]` Grotto thread, `[1]` keyring) `:322-352`. A slow Grotto D-Bus list delays NIP-46 selection | A: never gate NIP-46 selection on the Grotto listing |
| Refresh side effect | n/a | Any refresh while a NIP-46 account is active tears down and rebuilds its session and re-reads the keyring `:339-348` | A: rebind only when the credential's (signer, relays, client key) tuple actually changed |
| UI completion | Window closes; app proceeds | Dialog closes on select. Onboarding listens to `changed` `GH/ui/gh-onboarding-view.c:1520-1526, 1683` | = |

## 4. Startup restore and reconnect

| Aspect | gnostr | Groundhog | Class |
|---|---|---|---|
| Source | GSettings `nip46-*`, synchronous, no prompt `GN/ipc/gnostr-signer-service.c:1115-1212`; called from `gnostr-main-window-auth.c:994` | Keyring lookup (async) after the account listing settles `gh-account-controller.c:186-247` | B. Keep the keyring. Store non-secret metadata (signer pubkey, relays) in GSettings or the attributes so the account can be listed and shown as active **before** the secret is read. Only signing needs the secret |
| Listing reads secrets | n/a | `kc_search` fetches `kSecReturnData` for **every** item even for `OP_LIST` `GH/identity/gh-nip46-credentials-keychain.c:137-155` (non-interactive, so no prompt, but the work is wasted and items come back marked `locked`). Secret Service `ss_search` similar | B: add an attributes-only list path |
| Store lock | n/a | One mutex around every backend op `GH/identity/gh-nip46-credentials.c:339`. An interactive lookup blocked on a Keychain prompt blocks listing too | B: list without the mutex, or on a separate lock |
| Relay pool | Lazy: started on the first RPC `LIB:1570-1592`. Persistent per-session pool, one subscription, `since now-60` `LIB:1140-1160` | Eager `gh_nip46_session_start` on bind `gh-account-controller.c:178`. One scope subscription. Each request is a separate `GhRelayPublish` `gh-nip46-session.c:396-427` | B (must use Groundhog's transport for Tor). Verify that publishes reuse the scope's sockets; if not, under Tor each sign opens a new circuit (slow) |
| Readiness | No "ready" concept; RPC waits ≤5 s for a connection | READY = first EOSE. A single RPC timeout sets `last_rpc_timed_out`, so `is_ready` is FALSE (`:925`) → UI says offline until the next success | C (honest), but surface "offline" in the composer, not just the status icon |
| Resubscribe | libnostr relay reconnect; the subscription is re-sent by the pool | Relies on `gh_relay_scope` reconnect behaviour. On DISCONNECTED the URL is dropped from `ready_urls` `:630-636` and comes back on the next EOSE | = (confirm with a test) |
| Network-mode change | n/a (no SOCKS) | Rebind on `network-mode` change `gh-account-controller.c:584-595` | B/C |
| auth_url | **Not handled**: LIB treats `result:"auth_url"` as an error string → DENIED (`LIB:1880-1890`) | Validated HTTPS, opened once, deadline extended to 600 s `gh-nip46-session.c:666-694`. Controller routes it by generation `gh-account-controller.c:139-153` | **C** |

## 5. sign_event / nip44 / nip04

| Aspect | gnostr | Groundhog | Class |
|---|---|---|---|
| Request building | Fills in a missing pubkey from the user; rejects a mismatched one `GN/ipc/gnostr-signer-service.c:712-792` | Requires the template pubkey == selected identity, `created_at>0`, no id/sig `GH/identity/gh-signer.c:286-296` | C |
| Response validation | Parses; pubkey == expected, kind == requested, signature valid `:850-905`. Does **not** check created_at, tags or content | Full validation **and** `created_at`, kind, content and tags must match the request exactly `gh-signer.c:185-204` | C for safety, but **interop risk**: signers that re-stamp `created_at` (seen with some web signers) or normalize tags fail as `INVALID_RESULT`. Recommended: allow a `created_at` drift of ≤ ±600 s and re-check that `id` matches the signed content; keep content and tags exact |
| Error mapping | LIB `nip46_classify_error_string` (protocol / denied) `LIB:1378-1408`; message "Signer did not respond…" | `classify_error` `gh-nip46-session.c:232-251`, then `map_error` **discards the specific message** and replaces it with generic text `GH/identity/gh-signer-nip46.c:36-63` | A: port the LIB keyword table (it is broader: "restrict", "permission", "reject", "policy") and keep the signer's own error text (sanitized, truncated) for the UI |
| Concurrency | LIB worker pool, interactive/bulk queues (`LIB:135`, `:1927-2100`), plus a rate-limit gate | Interactive/bulk queues, MAX_IN_FLIGHT 4, 150 ms pacing, 8 MiB cap `gh-nip46-session.c:15-18, 387-432` | = (Groundhog's is main-loop based, no threads: C) |
| nip04 | `nip04_encrypt/decrypt` RPCs available `LIB:2524-2541` | `nip04_decrypt` only (read old DMs) | C (deliberate) |

## 6. Errors and UI feedback

- gnostr: one status area with spinner/error/retry `GN/ui/gnostr-login.c:188-260`. Errors are generic but always visible on the page in use.
- Groundhog: `show_status` writes to both tabs `GH/ui/gh-nip46-pair-dialog.c:91-100` (fixes the hidden-tab bug). The error label is shared. Pair-failure messages are specific and good (`:1105-1116` names the relays).
- Remaining gaps:
  - (a) A 5-minute silent wait in bunker mode. Add staged messages: "relays reachable", "request delivered (relay OK)", "waiting for approval in your signer". `publish_update`/`publish_done` already know when a relay has accepted (`gh-nip46-session.c:355-385`); expose that as a signal.
  - (b) `map_error` flattening (§5).
  - (c) On a `SELECT_TIMEOUT` the account was saved but not selected (`:288-301`). With the adopt-live-session port this state disappears.

## 7. Interop details

| Detail | gnostr | Groundhog | Class |
|---|---|---|---|
| NIP-04 transport fallback | No (NIP-44 default; NIP-04 modes exist in LIB `:2343-2450` but are not enabled by gnostr) | No; explicit message | = (if needed, port LIB `transport_mode` negotiation as opt-in) |
| p-tag relay hint `["p",pk,"wss://"]` | `nostr_tag_get_value` index 1 `LIB:900-912` | `tag size >= 2` `gh-nip46-session.c:268-281` | = |
| created_at window on subscription | `since now-60` | `since now-600` | C |
| Signature check before decrypt | Yes `LIB:921-926` | `nostr_event_validate` `gh-nip46-session.c:253-266` | = |
| Author pinning | `remote_pubkey_hex` `LIB:884-890` | `:642-645` | = |
| ping | `nostr_nip46_client_ping` `LIB:1244` (not used by gnostr UI) | Not used. Recommended: send `ping` after restore and on reconnect to turn READY into "signer answered" rather than "relay EOSE'd" | A |
| switch_relays | Not implemented in LIB/gnostr | Not implemented | — (future: implement both; persist new relays to the keyring) |
| Bunker connect params | `[remote, secret, "sign_event"]` | `[remote, secret, full perms]` | If a bunker errors on the perms string, retry once with `"sign_event"` (A-adapted) |
| Amber QR `ack` | Accepted | Rejected | See §2 |
| Signer-initiated connect request | `nip46_try_deliver_unsolicited_connect` `LIB:3543` | Handled with `ack` reply `gh-nip46-session.c:580-606` | = |
| OK/NACK and `rate-limited:` | Handled `LIB:1422-1470` | Depends on `gh_relay_publish` | A |

## Port plan (ordered)

1. **Hand off the live session after pairing (A).**
   - New controller API `gh_account_controller_adopt_remote(ctrl, npub, GhNip46Session*, GError**)`. It inserts a `GhIdentityInfo{backend=NIP46}` into `identities`, sets `active_npub`/`backend` in one GSettings transaction (`own_pair_write`), and calls `activate_remote_session` with the live session instead of `bind_signer`'s keyring lookup.
   - Pair dialog `stored()` (`gh-nip46-pair-dialog.c:368-407`): after a successful save, steal `self->session` (do not `stop_attempt` it), call adopt, close. Remove `PAIR_SELECTING` and `select_timeout`.
   - Model: `gnostr-main-window-auth.c:474-491` and `gnostr_login_take_nip46_session` (`gnostr-login.c:709`).
2. **Decouple NIP-46 from the Grotto listing (A/B).**
   - `finish_listing` must publish keyring results without waiting on the Grotto thread.
   - Only rebind on a refresh when the credential tuple changed (`gh-account-controller.c:339-348`).
3. **Keyring robustness (B).**
   - Attributes-only list path (no `kSecReturnData` / no secret load in `OP_LIST`).
   - Separate locks for list and secret ops.
   - Store signer pubkey and relays as non-secret attributes so the UI can show the account before an unlock. Lookup stays non-interactive at startup; show "Unlock keyring" (already `GH_REMOTE_SIGNER_LOCKED`) with an explicit button that retries interactively.
4. **Readiness gating (B).**
   - Publish once the REQ is sent on a connected relay; treat EOSE as optional with a 5 s grace (mirrors `LIB:1018-1110`).
   - Show the QR after the REQ is sent.
   - Applies to `begin_bunker_connect`, `pump` and `on_ready`.
5. **QR acceptance (A-adapted).**
   - Stop failing on stray or non-matching events; ignore them (gnostr `:1580-1700`).
   - Accept a bare `ack` from the first author as a fallback after a short window with no secret match, relying on the confirmation step showing the npub.
6. **Deadlines and feedback (A).** Bunker pair deadline ~60–90 s with staged status from publish OK events. Keep the 330 s approval window for post-pair RPCs only.
7. **Publish retries (A).** Port late publish to relays that connect later, and `rate-limited:` backoff ×3 with early abort when every relay NACKs (`LIB:1722-1820`), into `gh_relay_publish` (it then also honours Tor).
8. **Error mapping (A).** Reuse the LIB keyword classifier directly (`nip46_classify_error_string` is static; expose it in `nip46_msg.h` or copy it). Preserve sanitized signer text through `gh-signer-nip46.c:map_error`.
9. **Sign validation tolerance (C, adjusted).** Allow bounded `created_at` drift in `signed_event_error` (`gh-signer.c:185-204`).
10. **Inline confirmation (C, restructured).** Replace the nested `AdwAlertDialog` with a confirmation page in the pair dialog's own stack. Delete the `closed`/`unmap` handlers (`:423-435, 540-543`).
11. **Health ping (A).** `ping` after restore and after reconnect; READY only on pong.

### Reuse vs adapt

- **Reuse directly:** `nostr_nip46_uri_*`, `nostr_nip46_request_*`/`response_*`, `nostr_nip46_build_*_event` (already used), and the LIB error classifier (expose it).
- **Adapt (do not link LIB's pool, because there is no SOCKS/Tor):** late-publish/rate-limit loop, connect-wait semantics, first-wins latch, and gnostr's session handoff pattern.
- **Do not port:**
  - GSettings secrets.
  - "Any 64-char result" acceptance.
  - Bunker "continue anyway" on a bad connect result.
  - `g_random_int_range` keygen.

### Tests to add

Real buttons, real keyring:
- Extend `gnome/groundhog/tests/ui/test_nip46_pair_nak.c`, which already drives the real alert "Save" button against `nak bunker` with a temporary Keychain or a private gnome-keyring.
  - **Restart restore:** after pairing, destroy the controller and dialog, create a new controller on the same keyring and GSettings, and assert `GH_ACCOUNT_STATE_ACTIVE` plus remote READY. Then click the composer Send and assert a signed event reaches the wire relay (`sign_event` via nak).
  - **No-Grotto:** run with Grotto absent on a bus whose list call hangs. NIP-46 must still become active within N seconds.
  - **Live handoff:** assert there is no second keyring lookup and no new relay connection between Save and the first sign (count wire-relay connections).
  - **Locked keyring at startup:** lock the temporary keychain, start, assert LOCKED. Click "Unlock" (interactive retry), unlock, assert READY.
- Protocol fixtures on wire-relay (`tests/relay`):
  - QR reply `ack` (Amber-style).
  - Stray non-matching event before the real reply.
  - Relay that never sends EOSE.
  - Relay answering `rate-limited:` then OK.
  - Relay disconnect mid-request → republish on reconnect.
  - `auth_url` flow (exists: `controller-auth-url`).
  - Signer re-stamping `created_at` by +5 s.
- **Tor mode:** run the nak e2e with `network-mode=tor` against a local SOCKS stub (or skip with 77 if none), asserting that all NIP-46 sockets go through the SOCKS transport.
- **Onboarding:** click `onboarding.add-remote-signer` → pair via nak → assert onboarding advances to "signer" with "Remote signer connected".
