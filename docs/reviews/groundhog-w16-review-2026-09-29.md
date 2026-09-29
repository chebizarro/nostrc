# Groundhog W16 review: G09 network modes and Tor, G20b relay-group UI, UX polish (2026-09-29)

## Context / scope

This is the independent peer review that AGENTS.md requires. It covers `5cfe3b94..8102ecf9` on branch `groundhog/w16-review`. File:line references are to `8102ecf9` unless a commit is named.

| Commit | Bead | What |
|---|---|---|
| `5872a157` | `nostrc-qp24.27` | G09: network modes and Tor, fail-closed (`gh-net-session`, `gh-relay-soup`, `gh-relay-net`, GhNetHttp/NIP-05/NIP-11 in Tor mode, Preferences, banner) |
| `293b318e` | `nostrc-3sh0` | G20b: NIP-29 group UI (Join, Group Info, New Group, composer delegate, row glyph, outbox `restricted:` retry) |
| `e1ba07bc` | — | The Join parser accepts `ws://` `.onion` group relays |
| `04d34c33` | — | Beads only in this range. Its code landed upstream as `5cfe3b94` (fixture id interning, `lsan.supp`), glanced at here |
| `d3976456` | `qp24.70/.71/.72/.74/.80/.66` | UX polish: Ctrl+I, Blocked Conversations, row context menu, onboarding [Start a Conversation], header rework plus the charter §7.4 amendment, contact display-name titles |
| `8102ecf9` | — | Menu test fix (the primary menu has a relay-group section) |

- **Ignored.** `chore(beads)` commits and `.beads/` hunks.
- **Normative references.** `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`, and `docs/reviews/groundhog-w15-review-2026-09-29.md` / `groundhog-w15f-rereview-2026-09-29.md`.
- **Changes.** I changed no code or beads. Scratch programs used for verification live in `/tmp` only.

## Summary

- **G09's routing is right; its isolation is not.**
  - In a Tor build I found **no code path that dials directly or resolves a relay, NIP-05 or NIP-11 host locally in Tor mode** (audit below). The dispatcher, SOCKS5 remote DNS, fail-closed behaviour, mode-switch teardown and URL policy all hold, and the tests for them are real.
  - Tor-mode connections still **resume TLS sessions across isolation credentials, across accounts, and from direct (non-Tor) connections**. glib-networking keeps one process-wide TLS session cache keyed by host name. A relay therefore links connections that the SOCKS credentials were meant to keep apart. After a mode switch, it can also link a Tor connection to the user's real IP. That is **B1 (blocking)**.
- **G20b is solid.** Nothing is contacted before Join or Create. Relay text is plain text everywhere. Admin gating follows the relay-signed lists, and the `restricted:` retry is manual-only and tested. The metainfo was not updated and still says relay groups are "Not in this version" (non-blocking #2). One required test is flaky (non-blocking #1).
- **d3976456 is clean.** PT-8 is enforced twice for titles (the directory returns NULL for requests, and `gh_conversation_get_title` ignores a contact title on a request). Blocks stay in the encrypted store. The menu and Blocked page are Blueprint, and focus and announcements are handled.

## Tor-mode egress audit

The question: can any code path make a direct connection or a local DNS lookup while in Tor mode?

| Path | Result |
|---|---|
| Relay scopes and publishes (inbox, discovery/self lists, contact directory, DM send/outbox, onboarding probe and publish, NIP-29 service and outbox) | Every production creation site calls `gh_relay_scope_new` or `gh_relay_publish_new`. The `*_new_with_transport` branches run only for an injected config transport, and `gh-app-services.c` injects none (grep: no `transport` in it). `network_init` installs the dispatcher before `accounts` (`gh-app-services.c:914`). In Tor mode `open_inner` (`gh-relay-net.c:80-115`) only ever builds the soup transport with a SOCKS5-only `GSimpleProxyResolver` that has no ignore list (`gh-net-session.c:150-162`). There is no fallback |
| Remote DNS | GIO's `socks5` proxy supports host names, so the destination name goes to the proxy (ATYP 0x03). `/groundhog/net/tor-routing` asserts ATYP 0x03 and zero lookups on a recording default `GResolver`. I also confirmed it independently with a SOCKS fixture (below) |
| GhNetHttp, NIP-05, NIP-11 | `gh_nip05_new(self->settings, …)` (`gh-app-services.c:817`). The NIP-29 service's GhNetHttp gets the app settings through `gh-app-outbox.c:113`. Each reads `network-mode` per request and builds a fresh SOCKS session in Tor mode. An unusable Tor address is an error, never "direct" |
| `GResolver` / `GSocketClient` use | Only the Tor probe (`gh-net-session.c:303-320`). It resolves the *proxy's own* address with `enable-proxy` FALSE, which is by design |
| libwebsockets (GNostrRelay) | Reached only from `open_inner` when the mode is not TOR (`gh-relay-net.c:86-94`) |
| Mode switch | Owner-context handles are closed synchronously in `on_session_changed`, and `rebuild` then reconnects scopes or fails publishes (`gh-relay-net.c:119-170`). No scope runs on a non-default context (grep: no `push_thread_default` in `src/`) |
| GNetworkMonitor, `g_network_address_parse`, `g_inet_address_new_from_string` | No lookups |
| Link clicks (`GtkUriLauncher`, `gh-conversation-view.c:1027-1034`) | They hand the URL to the browser, outside Groundhog's network. See non-blocking #4 (copy) |

Exceptions found:
- the TLS linkage in B1;
- a build without libsoup ignores a stored `network-mode=tor` (#6);
- a System-mode HTTP request already in flight when the mode switches still completes directly (#10).

## Findings

### Blocking

#### B1. High: Tor-mode connections resume TLS sessions from other circuits, other accounts and direct connections, so relays and NIP-05/NIP-11 hosts can link them and learn the real IP

- **Where.**
  - `gnome/groundhog/src/net/gh-relay-soup.c:111-120`: one `SoupSession` per handle, but nothing controls TLS resumption.
  - `gnome/groundhog/src/net/gh-net-http.c:48-58` and `:65-72`: the same for the per-request Tor session.
  - The header claims "no connection or TLS session shared with any other handle" (`gh-relay-soup.h:17-19`).
  - Charter PD-6 (`…charter…md:160`: "Prevents cross-account and cross-purpose linkage on a shared socket, **TLS session** or circuit") and §4.1 (`:514`: "…and a separate TLS session cache").
- **Root cause (glib-networking 2.80.1, the version installed here).**
  - A client connection's cache key is `server_hostname/cert_hash` (`tls/base/gtlsconnection-base.c`, `g_tls_connection_base_constructed`). It depends on neither the proxy nor its credentials nor the `SoupSession`.
  - The cache is process-global (`tls/base/gtlssessioncache.c`, 50 entries, `SESSION_CACHE_MAX_AGE` 10 minutes).
  - `session-resumption-enabled` defaults to `!g_test_initialized()`. It is **on in the app and off in every test binary**, which is why no Groundhog test can see this.
  - The flag gates only *storing* a ticket. `prepare_handshake` (`gtlsclientconnection-gnutls.c`) looks one up regardless.
- **Evidence.**
  1. **Raw libsoup.** Three fresh `SoupSession`s: the first is direct, the next two go through SOCKS5 with different credentials. `session-reused` is 0, then **1**, then **1**. The SOCKS log shows ATYP 3 for both proxied connections.
  2. **Groundhog's own `GhNetHttp`** (`src/net/gh-net-http.c` and `gh-net-session.c` compiled into a harness, app schema, memory backend). The harness makes one No Proxy fetch, then two Tor-mode fetches through a SOCKS5 fixture that parses each tunnelled ClientHello. Both Tor requests (users `f38073ea8d90bdde` and `cc9e509f99464e0d`) carried the TLS 1.3 **`pre_shared_key`** extension. The first presented the ticket issued to the *direct* connection, and the second presented the ticket from the first circuit.
- **Failure scenarios.**
  1. **Real IP linked to a Tor connection after a mode switch.**
     - In System mode the user has a relay group on `groups.example`, and the NIP-29 service fetches its NIP-11 document directly. glib-networking stores a ticket for `groups.example/`.
     - Within 10 minutes the user chooses Tor. `rebuild` reconnects the group scope at once through Tor, and its ClientHello presents the direct connection's ticket.
     - `groups.example` links the Tor circuit to the user's IP, contradicting "Tor hides your IP address from relays" (`gh-preferences-dialog.blp:244`).
     - The same happens for a NIP-05 domain looked up just before switching.
  2. **Sender identity of a gift wrap.**
     - In Tor mode the inbox scope for relay R authenticates as the account (circuit A), and R issues tickets on that connection.
     - The user sends a DM to someone whose 10050 inbox also includes R. The recipient-wrap publish (random isolation, ephemeral AUTH, circuit B) resumes a ticket from circuit A.
     - R links the wrap to the account, which is exactly what R1, S3 and PD-6 exist to prevent. The self-copy publish is linked the same way.
  3. **Cross-account.** After an account switch the next generation's scopes resume the previous account's sessions on shared relays (P7, PD-6 "per account generation").
- **Suggested fix.**
  - Clear resumption on **every** GIO-TLS client connection Groundhog makes, in all modes. Groundhog is then never the party that seeds the process cache. That covers:
    - the WebSocket `SoupMessage` in `gh-relay-soup.c` `dial()`;
    - every `GhNetHttp` message.
  - The mechanism: a `network-event` handler sets `session-resumption-enabled` to FALSE at `G_SOCKET_CLIENT_TLS_HANDSHAKING` on the `GTlsClientConnection` (guarded by `g_object_class_find_property`). I checked this here: with it, three consecutive sessions report `session-reused=0`.
  - Because lookups are not gated by that flag, note next to the code that no other in-process GIO-TLS user may exist. Today there is none: libwebsockets uses its own TLS.
  - Correct the claim at `gh-relay-soup.h:17-19`.
- **Test.** An in-process GLib test cannot catch this, because resumption is off under `g_test_init`.
  - Add a helper executable that does *not* call `g_test_init`. It makes two Tor-mode connections through the SOCKS5 fixture to a local TLS fixture.
  - The fixture asserts that the second ClientHello has no `pre_shared_key` (extension 41) and no TLS 1.2 session ticket or session id.
  - The helper is run from `test-groundhog-net`. It fails today.

### Non-blocking (follow-up suggested)

1. **Medium: `/groundhog/group-ui/admin-gating` is flaky in a CI-required suite** (`gnome/groundhog/tests/ui/test_group_ui.c:780-840`).
   - **Observed.** One abort in 16 full `ctest -j6/-j8` runs. It reproduced under `--repeat until-fail:25` alongside the network suites: `test_group_ui.c:840: assertion failed (relay.events_seen == seen): (4 == 3)`.
   - **Cause.**
     - `club`, `plain` and `mods` list Alice as a member before she joins (`nip29_set_member`). So `wait_join(…, MEMBER)` can be satisfied by the relay's 39002 snapshot on the subscription while her 9021 EVENT is still in flight on its own publish connection.
     - `seen` is then taken too early, and the late 9021 lands during `drain()`.
     - The same test file handles this race at `:606-608` ("Its 39002 may say "member" first"), but `test_admin_gating` does not.
   - **Suggestion.** `wait_op(gh_nip29_room_dup_request_op(room), GH_NIP29_OP_DUPLICATE)` for each pre-member room before `seen = relay.events_seen`. The sanitizer job (slower) is the most likely place to trip.

2. **Medium: the metainfo still says relay groups aren't in this version** (`gnome/groundhog/data/org.nostr.Groundhog.metainfo.xml:18`).
   - **Current text.** "Not in this version: relay groups (NIP-29) … replying in group conversations …".
   - **What ships.** G20b is enabled in the executable (`CMakeLists.txt:1613`, "Groundhog NIP-29 group UI: enabled"): Join, New Group and sending in relay groups all work.
   - **Scenario.** The store listing understates what ships, and it says nothing of what matters for relay groups: not end-to-end encrypted, and the relay operator reads everything.
   - **Suggestion.** Update it with the 0.9.0 release commit, using the row glyph's wording ("Relay group, not end-to-end encrypted").

3. **Medium: System mode keeps relays off the system proxy, but the charter still says otherwise** (`gh-relay-net.c:86-94`, `gh-net-session.h:13-17`).
   - **The charter.**
     - D3 (`charter:60`): libsoup "for **all** Groundhog relay traffic".
     - §4.1/§4.2 (`:510-514`, `:526`): `system` → `g_proxy_resolver_get_default()`.
     - NT-9 (`:1446`).
   - **What the code does.** G09 routes System and No Proxy relays through libwebsockets, which ignores the desktop proxy.
   - **The copy is honest.** "Groundhog connects to relays directly … Only web lookups … use the system's proxy settings" (`gh-preferences-dialog.c:583`); the gschema says the same.
   - **Scenario.** A user whose desktop proxy is a SOCKS proxy leaves "System Settings" selected. NIP-05 goes through the proxy; relays do not.
   - **Suggestion.** Amend the charter (D3, §4.2, NT-9) as d3976456 did for §7.4. Alternatively, since the soup transport now has NT-10 parity, route System mode through it with the default resolver.

4. **Medium (copy): "Tor hides your IP address from relays and websites"** (`gh-preferences-dialog.blp:244`).
   - **The problem.** A link clicked in a message opens in the default browser (`gh-conversation-view.c:1027-1034`), which does not use Groundhog's Tor connection.
   - **Scenario.** In Tor mode a contact sends `https://tracker.example/x` and the user clicks it. The site sees the real IP while Preferences promised otherwise. The link opens without confirmation for an ASCII https host.
   - **Suggestion.**
     - Scope the sentence to what Groundhog itself connects to ("…from relays and the address lookups Groundhog makes").
     - In Tor mode, confirm link opens with "Your browser doesn't use Groundhog's Tor connection".

5. **Low (copy): exposure copy is not mode-aware.** It overstates exposure in Tor mode.
   - **Where.**
     - Join: "It learns your public key and your IP address" (`gh-group-join-dialog.blp:73`).
     - New Group (`gh-new-group-dialog.blp:92`).
     - Onboarding "Check Privacy": "They see your IP address" (`gh-onboarding-view.blp:510`).
   - **A related gap.** "Can't reach Tor" is a sidebar banner (and only with an active account). During onboarding in Tor mode with Tor down, the relay checks fail with no Tor explanation.
   - **Suggestion.** Use the New Message dialog's pattern (`gh-new-message-dialog.c:651-658`) in these places too.

6. **Low: a build without libsoup ignores a stored `network-mode=tor`, so it fails open** (`CMakeLists.txt:1586`, `gh-app-services.c:166`, `gh-relay-scope.c:132`).
   - **Why.** Without `groundhog-net`, `GROUNDHOG_HAVE_TOR=0`, no dispatcher is installed, and every scope uses GNostrRelay directly. `.onion` URLs even reach local DNS there, because `gh_net_relay_url_allowed` is never called.
   - **Scenario.** The user chose Tor in a full build, and the dconf value persists. A distro or dev build without libsoup-3 then connects directly with no warning (P5).
   - **Suggestion.** In a non-Tor build, treat any `network-mode` other than `system`/`none` as "connect nowhere": install a refusing transport and show the Tor banner.

7. **Low: each Tor-mode GhNetHttp request disposes its session while its connection is still up** (`gh-net-http.c:25-34`).
   - **Why.** `request_free` unrefs the per-request `SoupSession` without `soup_session_abort`.
   - **Observed.** Against a keep-alive server, each Tor-mode fetch logs `soup_session_dispose: runtime check failed … num_conns == 0` and "Disposing connection … while still connected". It is invisible in the tests because the fixture server doesn't keep connections alive.
   - **Suggestion.** Abort a Tor-mode request's own session in `request_free`, as `gh-relay-soup.c:88-89` does.

8. **Low: a System-mode Join of a `ws://…onion` group retries forever with generic copy** (`gh-group-copy.c:43-53` after `e1ba07bc`; `gh-relay-net.c:84`).
   - **Why.** The parser accepts the address in every mode. The dispatcher then refuses it (".onion relays can only be reached through Tor"), and the NIP-29 outbox classes `CONNECTION_FAILED` as transient.
   - **Scenario.** The join shows asking or retrying indefinitely, and the reason (it needs Tor) is never shown.
   - **Suggestion.** Refuse `.onion` at parse time outside Tor mode with that sentence, as New Message does. Or map a policy refusal to NOT_SENT with its message.

9. **Low: soup reconnect backoff resets on every upgrade** (`gh-relay-soup.c:360` with `:306-313`).
   - **Scenario.** A relay (or a Tor exit it rate-limits) accepts the WebSocket and closes it at once. The scope then reconnects every 0.5–1.5 s indefinitely.
   - **Parity.** GNostrRelay's transport behaves the same, so this is not a regression.
   - **Suggestion.** Reset the backoff only after EOSE, or after the connection has lived for N seconds.

10. **Low: GhNetHttp does not tear down on a mode switch** (`gh-net-http.c:73-85`).
    - A System-mode request already in flight when the user picks Tor completes directly.
    - The kept System session is dropped only at the next non-Tor request.
    - This is not a *new* direct connection, but the relay dispatcher's "a mode change closes every old connection" is not true for HTTP.

11. **Nits.**
    - **Keyboard.** Charter §7.13 lists Ctrl+Shift+N for New Group, and G20b adds `win.new-group` without it.
    - **Stale comment.** `gh-group-copy.h:26-28` still says ws:// is loopback-only after `e1ba07bc`.
    - **`.onion` check.** `gh-group-copy.c:49` uses a case-sensitive `g_str_has_suffix(".onion")`, where `gh_net_host_is_onion` would handle case and a trailing dot.
    - **New Message copy.** "Connects to %s through Tor, which learns whom you looked up …" (`gh-new-message-dialog.c:653`) reads as if Tor learns it. Suggested: "…through Tor. %s learns whom you looked up, not your IP address."
    - **Icon.** `network-workgroup-symbolic` is both the relay-group row glyph and the Network preferences page icon.
    - **Status row flicker.** Every failed Tor-mode dial re-probes (`gh-net-session.c:469`), so Preferences' Tor Status flickers "Checking…" when a relay is merely down.
    - **`5cfe3b94` comment.** The `nip29-relay.h` comment says `nip29_event_id()` returns a fresh allocation; it means `nostr_event_get_id()`.
    - **`5cfe3b94` suppression.** `leak:^websocket_callback$` suppresses anything allocated beneath that frame, since LSan matches any stack frame. That is broader than "the only allocations in websocket_callback", but acceptable for a test-only entry tracked as `nostrc-lpvj`.

## Per-commit notes

### `5872a157` G09

- **SOCKS5 isolation.**
  - Username/password are the first and second 16 hex digits of SHA-256(label ‖ 0x00 ‖ 32-byte per-process salt). The salt comes from `getentropy`, with a random fallback and never a fixed value.
  - Labels are `<generation>/scope/<uuid>` or `<generation>/publish/<uuid>` (`gh-relay-net.c:96-103`), so no URL or key reaches Tor.
  - Scope reconnects keep their label, and publishes and HTTP requests get fresh ones.
  - `/groundhog/net/isolation` checks five distinct names plus reconnect stability.
  - The charter's `acct/inbox` token is realised as "per scope object", which satisfies NT-6.
- **Fail closed.**
  - Unknown mode strings map to TOR (`gh-net-session.c:21-29`).
  - An unparsable Tor address is an error, never direct.
  - With the proxy down, retries only ever dial the proxy; `/groundhog/net/tor-fail-closed` checks the tripwire and zero DNS across a backoff.
  - A publish with no dispatcher session installed is refused (`gh-relay-net.c:204-209`).
- **Probe.** It sends a SOCKS5 greeting only and never a CONNECT. Stale probes are cancelled on a mode change, and reports carry the serial.
- **gh-relay-soup, NIP-01 (protocol-smells lens).**
  - REQs get fresh alphanumeric ids and are framed by hand around libnostr's filter serializer (`nostrc-ptwq`).
  - EVENT and EOSE are matched to the live sub id, so late frames from a superseded REQ are dropped.
  - CLOSED clears the sub and goes to the scope's AUTH/resubscribe logic.
  - OK is forwarded on both sides, and AUTH challenges on both.
  - NOTICE is dropped unlogged (PD-10).
  - There is no polling and no EOSE timeout; the only timer is a 30 s dial deadline.
  - There are no `sleep`-based tests (bounded `g_timeout_add` safety nets and one 50 ms negative check).
  - Close stops every callback (the `closed` flag is checked in each handler, and signals are disconnected before the unref).
  - A lost connection before OK fails the publish immediately. `gh_relay_publish_failed` only touches pending endpoints (`gh-relay-publish.c:662-672`), so there is no clobbering after OK.
- **Dispatcher.**
  - The registry is under a mutex, and handles are ref'd across the idle.
  - `rebuild` rechecks `closed` after the DISCONNECTED notice, whose callback may close the handle.
  - A double switch collapses through the serial check.
  - NT-10: both wire suites run again under `/tor` with credentials asserted on every connection. `groundhog-net` is in CI build, required and sanitizer lists.
- **Copy and gates.**
  - `GH_FEATURE_TOR` equals `GROUNDHOG_HAVE_TOR`, which equals the dispatcher being installed. The gate matches reality.
  - The System and No Proxy notes, the Tor note (except #4), the gschema, the banner with [Network Settings] (`app.network-settings` → page `network`, which exists), and the accessible description on the Tor address row are all good.

### `293b318e` G20b and `e1ba07bc`

- **Consent.** Nothing is contacted before Join or Create: parsing is pure (`gh_group_parse_reference`), and `ask()` is the first network step (`gh-group-join-dialog.c:109-132`).
- **Plain text.** Relay-supplied text is plain everywhere: row and toast `use-markup` FALSE, and `AdwAlertDialog` bodies are plain by default.
- **Names.** Group Info names members from the accepted-contact cache only (`gh_contact_directory_get_display_name` → `accepted_contact`), so there is no fetch for strangers.
- **Admin gating.** It follows the relay-signed 39001/39003, with "The relay decides" when role rights are unknown. The service refuses hidden actions locally, which the test asserts.
- **Codes and ids.** Invite codes and new group ids are 64 bits from the OS CSPRNG (`gh_nip29_new_group_id` → `gh_store_new_op_id`).
- **Outbox.** A `restricted:` refusal is republished only on a user's Retry (`op->manual`, `gh-nip29-outbox.c:830-836`); `blocked:`, `invalid:` and auth stay final. `/groundhog/group-ui/send` would fail without the change (the retried op must reach ACCEPTED).
- **Composer delegate.** It is coherent: drafts are skipped, and reason, send, retry and report are the delegate's. `gh_group_find_message_op` is NULL-safe.
- **Row glyph.** It has a tooltip and an accessible label, and it joins the composed row label. `GhConversationBackend` and `GhPrivacyBackend` values match (1/2/3).
- **Info routing.** `win.conversation-info` reaches Group Info through the group handler, and the row menu is off for groups.

### `d3976456` UX polish and `8102ecf9`

- **Ctrl+I.** It is bound and listed. Shift+F10/Menu are listed, and the shortcuts test accounts for them.
- **Blocked Conversations.**
  - Rows show npubs only.
  - The query is NIP-17 `request_state = 2` from the encrypted store.
  - Unblock goes through `change_block` → `restore_room`, so the room is listed again, and the description says where it goes.
  - Each Unblock button has an accessible label naming whom it unblocks, and focus moves to the next row.
  - Empty and error states exist, and everything is in Blueprint.
- **Row menu.**
  - Right click, touch long press, and Shift+F10/Menu on the list.
  - Mute is hidden for requests.
  - Confirmations are Blueprint `AdwAlertDialog`s built per use.
  - Answers re-look up the room through a weak window ref.
  - Names come from accepted contacts only, else a short npub.
- **Contact titles.**
  - PT-8 is enforced in both `gh_contact_directory_dup_conversation_title` and `gh_conversation_get_title`.
  - Titles are refreshed on `notify::is-request` and `profile-changed`, which is now also emitted on store bind and unbind.
  - The notifier's `sender` level now shows the cached name, as charter §5.1 specifies, and requests stay forced hidden.
- **Header.** The §7.4 amendment is recorded. `8102ecf9` correctly counts four primary-menu sections.
- **Onboarding.** [Start a Conversation] shows only while `win.new-message` is enabled, and focus goes to it.

## Verification

- **macOS 15 (Darwin 24.6, arm64).** Submodules initialised (`third_party/nostrdb`, `third_party/nsync`).
- **Build.** `cmake -S . -B /tmp/w16rev -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w16rev` built all 2272 targets with no Groundhog warnings. `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui` was rewritten by the build and restored.
- **Tests.** `ctest --test-dir /tmp/w16rev -R 'groundhog-' -j6`:
  - The first run was **57/58**: `groundhog-group-ui` aborted, which is non-blocking #1.
  - The next 15 full runs (`-j6` ×5, `-j8` ×10) were **58/58**.
  - Stress over group-ui, net, nip29-service, relay-wire, relay-publish-wire and conversation-menu with `--repeat until-fail:25 -j8` reproduced the group-ui failure at `test_group_ui.c:840`.
  - Four platform skips: `groundhog-launch`, `-store-key-keyring`, `-background-gui`, `-notifier-gui`.
- **B1 reproduction.**
  - A raw libsoup program: fresh sessions, direct then SOCKS5 with two credential pairs. `session-reused` was 0 → 1 → 1.
  - A harness over `src/net/gh-net-http.c` and `gh-net-session.c`: one No Proxy fetch, then two Tor fetches through a Python SOCKS5 relay that parses ClientHello extensions. Both Tor ClientHellos carried `pre_shared_key`.
  - The fix, checked: setting `session-resumption-enabled` FALSE at `TLS_HANDSHAKING` on every connection gives `session-reused=0` three times.
  - The glib-networking 2.80.1 sources read: `gtlsconnection-base.c`, `gtlsclientconnection-gnutls.c`, `gtlssessioncache.c`.
- **Checks.** `git diff --check 5cfe3b94..8102ecf9 -- . ':!.beads'` is clean.
- **Code audits.**
  - Every scope and publish creation site and every custom-transport switch.
  - GResolver, `GSocketClient`, `soup_session_new` and GNostrRelay use in `src/`.
  - The NIP-29 service's and NIP-05's settings plumbing.
  - Every new `g_message` (static labels plus error text; no URLs, keys or content).
  - The `lsan.supp` entry against libnostr's `websocket_callback`.
- **Not run.** Linux, the ASAN job, a real Tor daemon, and real public relays.

**REQUEST CHANGES**

- **Blocking.**
  - **B1.** Tor-mode TLS connections resume sessions stored by other isolation credentials, other accounts, and direct connections. glib-networking's process-wide cache is keyed by host name, and resumption is on outside tests.
  - **Consequence.** A relay or NIP-05/NIP-11 host can link Tor circuits that PD-6 and R1 are meant to keep apart. After a switch to Tor, it can link a Tor connection to the user's real IP.
  - **Fix.** Disable resumption on every Groundhog GIO-TLS client connection, and add a non-`g_test_init` helper test that checks the ClientHello.
- **Everything else is non-blocking.** Highest priority first:
  - the flaky required `admin-gating` test (#1);
  - the stale metainfo, before the 0.9.0 release (#2);
  - the System-mode charter amendment (#3);
  - the Tor-note "websites" scope (#4).

---

## Addendum: W16f remediation confirmation pass (`cd70d443`)

**Scope.** This pass covers `cd70d443` on `groundhog/w16f-remediation` (bead `nostrc-qp24.87`), which answers this review. It checks two things: whether B1 is fully closed, and whether the new code adds defects. File:line references are to `cd70d443`. I changed no code or beads.

### B1: closed

- **The fix.** `gh_net_tls_no_resumption()` (`src/net/gh-net-tls.c`) sets glib-networking's `session-resumption-enabled` to FALSE at `G_SOCKET_CLIENT_TLS_HANDSHAKING`. It is applied to every `SoupMessage` Groundhog makes, in every mode:
  - the relay WebSocket (`gh-relay-soup.c` `ws_message`);
  - every `GhNetHttp` request, which covers NIP-05 and NIP-11.
  Turning resumption off in *all* modes is right: lookups ignore the flag, so the process must never store a ticket.
- **Remaining TLS clients.** I found none.
  - Only `gh-net-http.c` and `gh-relay-soup.c` create a `SoupSession` or GIO TLS client in `src/`.
  - Nothing Groundhog links (nostr-gobject, libnostr, the NIPs it uses) calls `g_tls_client_connection_new`, `soup_session_new` or `g_socket_client_set_tls`.
  - The executable's TLS stacks are libsoup/GIO and libwebsockets' OpenSSL (`otool -L`).
  - The Tor probe is plain TCP.
- **Remaining resumption paths inside libsoup.** Also none, checked against the installed libsoup 3.6.6 sources.
  - A connection's `event` signal reaches whatever message is attached to it (`soup_message_set_connection`).
  - Each new `SoupConnection` is created for, and handshaken under, the message that asked for it (`soup_connection_manager_get_connection_locked` → `soup_session_ensure_item_connection`).
  - A connection passes to another message only through `soup_session_steal_preconnection`, and only from a preconnect item. Groundhog never preconnects.
  - For HTTPS through an HTTP proxy (System mode), `tunnel_connect` leaves the user's message attached, so it also receives the tunnel's `TLS_HANDSHAKING`.
  - Reused keep-alive and HTTP/2 connections were set up by a covered message.
- **The test is genuine.**
  - `groundhog-tls-resumption` is a plain program (no `g_test_init`) with an OpenSSL server and a positive control. The control reports `pre_shared_key=yes resumed=yes`.
  - I relinked it against a no-op `gh_net_tls_no_resumption`. All four Tor connections (two GhNetHttp, two relay scopes) then offered and resumed the direct fetch's session, and the test fails at `test_tls_resumption.c:619` (`offered == 0`: 4 ≠ 0).
  - `check_privacy.py` `tls-resumption` has three mutations, `groundhog-privacy-static` passes, and CI installs `glib-networking` so the test cannot skip.
- **Paperwork.** `gh-relay-soup.h` and the charter (§4.1) no longer claim a per-session TLS cache.

### `nostrc-0d0d` (libwebsockets resumption, direct mode): acceptable as a follow-up

- **Tor mode is unaffected.**
  - libwebsockets 4.5.8 here has `LWS_WITH_TLS_SESSIONS`, and libnostr does not disable the client cache, so direct relay connections may resume one another.
  - But that cache is lws/OpenSSL's own and is never shared with glib-networking.
  - In Tor mode the dispatcher uses only the libsoup transport, so no lws ticket can ever be presented on a Tor connection.
- **The direct-mode gap is small.** In direct mode the IP address already links connections to a relay. The residual gap is linkage across accounts on one relay that survives an IP change or a shared NAT (PD-6, P7).
- **The bead is accurate.** It says what to verify (the same ClientHello fixture) and how to fix it (disable the lws client cache). P2 is proportionate, and it becomes moot with `nostrc-253z`.

### New blocking finding

#### N1. High: use-after-free in `GhNetHttp`'s `request_free` when an in-flight request outlives its owner (`gnome/groundhog/src/net/gh-net-http.c:19`, `:36-37`, `:268`)

- **The defect.**
  - `Request.owner` is a borrowed pointer, commented "the task's source object, so alive".
  - But `g_task_finalize` (GLib 2.90 `gio/gtask.c`) clears `source_object` *before* it calls the task-data destroy notify.
  - When the task holds the last reference, the `GhNetHttp` is disposed and finalized, which frees `requests` and the instance. `request_free` then reads `request->owner->requests` and calls `g_ptr_array_remove_fast` on it.
- **Reproduced.**
  - A harness built from the commit's `gh-net-http.c`, `gh-net-session.c` and `gh-net-tls.c` with `-fsanitize=address` runs these steps:
    1. start a request to a local listener;
    2. `g_object_unref` the `GhNetHttp`;
    3. cancel the request;
    4. run the loop.
  - Result: `AddressSanitizer: heap-use-after-free … READ of size 8 … in request_free gh-net-http.c:36`. The object was freed by `g_type_free_instance` ← `g_object_unref` ← `g_task_finalize`.
- **Production path.**
  - `GhNip29Service` dispose cancels every NIP-11 key fetch (`gh-nip29-service.c:2297`) and then drops `self->http` (`:2317`).
  - The cancelled fetch completes on a later iteration, because GTask defers a cancelled return to an idle. Its task then drops the last reference.
  - So an account switch, logout or store close while a group relay's NIP-11 fetch is in flight reads freed memory, and may write to it. Such fetches run at startup and on Join, with a timeout of up to 10 s.
  - `GhNip05` has the same shape at app teardown (`gh-nip05.c:190`) with a New Message lookup in flight.
- **Not caught.** No test drops a `GhNetHttp` with a request in flight, so the sanitizer job cannot see it.
- **Suggested fix.** Either:
  - `request->owner = g_object_ref(self)` and `g_object_unref(owner)` as the last step of `request_free`, after removing the request (the array doesn't own requests, so this makes no cycle); or
  - clear each queued request's `owner` in `gh_net_http_finalize` and guard `request_free`.
- **Suggested test.** A `groundhog-net` case: start a request, unref the `GhNetHttp`, cancel, and spin until the callback. It runs in the sanitizer job, which already lists `groundhog-net`.

### Other remediation items: verified

| Item | Status |
|---|---|
| #1 admin-gating flake | Fixed. The test waits for DUPLICATE on the two pre-member joins (`club`, `mods`). `plain` and `review` change only on the relay's answer. Stress over group-ui, net, nip29-service, relay-wire, relay-publish-wire, conversation-menu and tls-resumption with `--repeat until-fail:25 -j8` passed, and so did six full runs |
| #2 metainfo | Relay groups are described as shipped, not end-to-end encrypted, with the operator able to read everything. "Not in this version" is now MLS and attachments |
| #3 charter | Dated amendments to D3, §4.2 and NT-9 (System-mode relays ignore the desktop proxy; `nostrc-253z`) |
| #4 Tor note and links | The note names relays and address checks and says links go to the browser, outside Tor. In Tor mode every web link is confirmed first, with the browser sentence leading. `GH_LINK_ACTION_NOSTR` is untouched |
| #5 mode-aware copy | Join, New Group and onboarding Check Privacy |
| #7 session abort | A Tor request aborts its own session in `request_free` |
| #8 `.onion` outside Tor | Join and New Group refuse with the reason before queueing (`gh_group_relay_reachable`); the GUI test covers it |
| #10 mode-switch teardown | `on_mode_changed` marks, then cancels, requests of another mode, and they fail with `G_IO_ERROR_CONNECTION_CLOSED`, never CANCELLED. No deadlock is possible: a GTask with a cancelled cancellable always returns from an idle, so `request_free`'s `g_cancellable_disconnect` never runs inside the caller's `cancelled` emission (GLib 2.90 `g_cancellable_disconnect` would wait there) |
| #6, #9, onboarding Tor-down, nits | Filed as `nostrc-dod2`, `-81ad`, `-jpwe`, `-idz0`. Acceptable |

### New non-blocking notes

1. **Low: the reachability check only guards Join and Create** (`gh-group-join-dialog.c:119`, `gh-new-group-dialog.c:119`).
   - **Scenario.** A `.onion` group joined in Tor mode, after the user switches to System, still goes through `gh_nip29_service_send` and "ask again" without the check. It retries silently, the #8 case for existing rooms.
   - **Suggestion.** Map the dispatcher's policy refusal (`G_IO_ERROR_PERMISSION_DENIED`) to NOT_SENT with its message in the NIP-29 outbox.

2. **Nit: the network mode is read from two sources.**
   - The group dialogs read the installed `GhNetSession` (`gh_group_network_is_tor`).
   - Onboarding, New Message and links read the `network-mode` setting.
   - They differ only in a build without the dispatcher. There, onboarding would say "through Tor" while connecting directly. That case is `nostrc-dod2`'s, and should be fixed there.

3. **Nits (copy).**
   - "Groundhog connects through Tor, so it learns your public key …" (`gh-group-copy.c:85`, `:90`): "it" now reads as Groundhog. Suggested: "The relay learns your public key but not your IP address, because Groundhog connects through Tor."
   - "…so the website will see your IP address" (`gh-conversation-view.c:1057-1058`) is untrue when the default browser is Tor Browser. "may see" is exact.

### Verification

- **Build.** `cmake -S . -B /tmp/w16f -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w16f` (macOS 15 arm64, submodules initialised) is clean apart from the existing `ld` duplicate-library notices. `gnostr-profile-edit.ui` was not rewritten this time.
- **Tests.** `ctest --test-dir /tmp/w16f -R 'groundhog-'`:
  - **59/59 on each of six full runs** (`-j6` once, `-j8` five times), with four platform skips: `groundhog-launch`, `-store-key-keyring`, `-background-gui`, `-notifier-gui`;
  - the stress run above was clean.
- **Checks.** `git diff --check cd70d443~1 cd70d443` (excluding `.beads`) is clean.
- **Reproductions.**
  - The B1 mutant: `groundhog-tls-resumption` relinked with a no-op `gh_net_tls_no_resumption` fails with four offered sessions.
  - N1: ASAN harness as above.
- **Sources read.**
  - libsoup 3.6.6: `soup-session.c`, `soup-connection.c`, `soup-connection-manager.c`, `soup-message.c`.
  - GLib 2.90.0: `gtask.c`, `gcancellable.c`.
- **Not run.** Linux, the ASAN CI job, and a real Tor daemon.

**REQUEST CHANGES** (addendum)

- **B1 is closed.** Every Groundhog TLS connection has resumption off, in all modes. No other GIO TLS client or libsoup hand-off path remains. A genuine ClientHello-level test proves it and fails without the fix.
- **`nostrc-0d0d` is acceptable as a follow-up.** The lws cache never reaches Tor connections.
- **Blocking: N1.** The new `Request.owner` borrowed pointer is read after `g_task_finalize` has freed the `GhNetHttp`. That is a heap use-after-free on account switch, logout or store close during a NIP-11 fetch (and at quit during a NIP-05 lookup). The fix is one owned reference plus a sanitizer-visible test. Everything else in `cd70d443` is approved as is.

### N1 fix confirmation (`2e46e4b3` on local `master`) and final verdict

- **The fix.**
  - `Request.owner` is now `g_object_ref(self)` (`gh-net-http.c:271`).
  - `request_free` works in this order:
    1. removes the request from the non-owning `owner->requests`;
    2. disconnects the caller's cancellable;
    3. frees the rest, aborting a Tor request's own session;
    4. unrefs the owner last.
  - The owner therefore cannot be finalized while `request_free` still uses it, whatever order `g_task_finalize` uses.
- **ASAN reproduction.** The same harness, built from `master`'s `gh-net-http.c`, `gh-net-session.c` and `gh-net-tls.c` with `-fsanitize=address`, is clean on three runs; it reported the use-after-free at `gh-net-http.c:36` before the fix.
- **Other endings.** An extended harness against a server that accepts and never answers drops the `GhNetHttp` in flight, then ends the request three ways. All are ASAN-clean, and each checks with a weak pointer that the object stays alive while the request runs and is **finalized** after it completes:
  - by the caller's cancel (`G_IO_ERROR_CANCELLED`);
  - by a mode switch (`G_IO_ERROR_CONNECTION_CLOSED`, "The network setting changed…");
  - by the 10 s I/O timeout.
- **No reference cycle or leak.**
  - `owner->requests` does not own its entries.
  - While a request is in flight there is a loop: owner → kept System/No Proxy `SoupSession` → queued message → our `GTask` → `Request` → owner. Before the fix the same loop existed through the task's `source_object`, so it isn't new.
  - That loop ends on every completion (response, error, cancel, mode switch, timeout), as the finalization checks show.
  - `dispose` aborting the kept session only matters once no request holds the owner, as before.
- **The new test.** `/groundhog/net/http-owner-dropped-in-flight` checks the owner is kept alive and then released.
  - Without ASAN, a use-after-free need not crash, so the regression is caught by the CI sanitizer job, which lists `groundhog-net`.
  - It passed 10/10 in `--repeat until-fail:10`.
- **Other `GhNetHttp` users need nothing more.**
  - **`GhNip05`.** Its `on_fetched` calls `get_finish` on `transport_data`. That is the same `GhNetHttp` the request keeps alive until after the callback, so a lookup completing after `gh_nip05_dispose` (`gh-nip05.c:190`) is safe. It never reads `self->http`. A lookup at quit that nobody cancels ends at the 10 s timeout, then frees both objects.
  - **`GhNip29Service`.** It cancels its key fetches before dropping `self->http` (`gh-nip29-service.c:2297`, `:2317`). That now ends cleanly, as in the harness.
  - **NIP-11.** `gh_nip11_fetch_relay_key_async` uses only the `GhNetHttp` it was given, as the async source.
  - No other `src/` code creates a `GhNetHttp`.
- **Verification.**
  - `cmake -S . -B /tmp/w16m -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w16m` on the main checkout at `2e46e4b3` builds all 2280 targets. The rewritten `gnostr-profile-edit.ui` was restored.
  - `ctest -R 'groundhog-' -j8`: **59/59 on three runs**, with the same four platform skips.

**APPROVED**

- **Blocking findings.** B1 (closed by `cd70d443`) and N1 (closed by `2e46e4b3`) are both resolved.
- **Other W16 items.** Every remaining W16 and addendum item is either fixed or filed:
  - `nostrc-0d0d`, `-dod2`, `-81ad`, `-jpwe`, `-idz0`, `-253z`, `-f56o`;
  - the addendum's non-blocking notes on existing `.onion` rooms, the two network-mode sources, and two copy nits, which should be filed as follow-ups.
