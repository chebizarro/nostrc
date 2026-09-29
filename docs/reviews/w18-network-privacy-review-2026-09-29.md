# W18 network-privacy review: nostrc-0d0d, nostrc-lpvj, nostrc-qi5e

- **Date:** 2026-09-29
- **Reviewer:** independent peer reviewer (AGENTS.md)
- **Branch reviewed:** `hardening/w18-network-privacy`, commits `252720cf`, `b7991e39`, `98a5328a`.
  These were cherry-picked onto `review/w18-network-privacy` at `3f55cb66` as `f434e367`, `ca681f1e` and `ac6fb637`.
- **Cherry-pick note:** both VERSION_MANIFEST.md conflicts were resolved additively. `b7991e39` rewrites the libnostr 1.0.11 row that `252720cf` added, so only the rewritten row is kept. The result matches the source branch: one libnostr 1.0.10 -> 1.0.11 row naming both beads, plus the "same | nostr-gobject, gnostr, groundhog" row.
- **Verdict:** **APPROVED.** There are no blocking findings. Two Medium items and several Low items are follow-ups.

## Build and test evidence

| Run | Result |
|---|---|
| macOS, `cmake -G Ninja -DBUILD_GROUNDHOG=ON`, `ninja`, `ctest -j6 --timeout 300` (Homebrew lws 4.5.8, OpenSSL 3.6.2, GLib 2.90, libsoup 3.6.6) | 428/428 pass, 5 skipped (pre-existing skips: nip5f_tcp, groundhog-launch, -store-key-keyring, -background-gui, -notifier-gui). `gnostr-profile-edit.ui` was rewritten by the build and restored. |
| Linux Docker (`groundhog-linux-ci` image: Ubuntu 24.04, lws 4.3.3, GLib 2.80, libsoup 3.4.4, OpenSSL 3.0.13), ASAN+UBSAN+LSan, CI sanitizer set plus `test_connection_{tls_no_resumption,writable_rearm,idle_writable,shutdown_order}` | 46/47 pass. The one failure, `groundhog-store-marmot`, predates this change (O1): it is a UBSAN report in libmarmot, which these commits do not touch. |
| Stress: `ctest -j8 --repeat until-fail:15` over `test_connection_{recv_drain,writable_rearm,idle_writable,shutdown_order}`, `test_relay_shutdown` and the nostr-gobject EOSE-order test (macOS) | 15/15 rounds, no failures or timeouts (no deadlock seen from the new lock nesting). |

### Does each test fail without its fix?

| Test | Fix removed | Result |
|---|---|---|
| `test_connection_tls_no_resumption` | Both context options removed from `connection.c` | **Fails** on macOS lws 4.5.8 and on Linux lws 4.3.3. `libnostr 2  pre_shared_key=yes resumed=yes`. With the fix, both libnostr connections show `pre_shared_key=no session_ticket=-1 resumed=no`. With only `SSL_OP_NO_TICKET` removed it still passes: the cache flag alone suffices; the ticket option removes the empty SessionTicket extension. |
| `test_connection_recv_drain` | `nostr_connection_recv_channel_free()` made not to drain | **Fails** under Linux LSan: `30041 byte(s) leaked in 1550 allocation(s)` from `websocket_callback` (connection.c:208/211). On macOS `leaks --atExit` reports 2590 leaks, against 0 with the fix. It passes in a plain macOS ctest either way, as the file header says (LSan-only detection). |
| `test_connection_recv_drain`, flood phase, against the post-detach enqueue race | Parent `connection.c` callback (ref under mutex, send after unlock) with the new drain | **Passes 30/30** under LSan: this phase does not catch the race it describes. See L4. |
| `groundhog-net /groundhog/net/public-only` | `gh_net_http_get_public_async()` passes `public_only = FALSE` | **Fails**: the TLS handshake to 127.0.0.1 happened, where PERMISSION_DENIED was expected. |
| `groundhog-blossom /groundhog/blossom/download-rebinding` | Download always uses `gh_net_http_get_accept_async()` | **Fails** the same way. |
| `groundhog-net /groundhog/net/public-address` | The old `address_private()` run over the test's refused list (probe harness) | The old classifier takes **21 of 43** refused entries as public, exactly as the commit says; the new one refuses all 43. |

All experiments were reverted and the tree rebuilt. The affected tests pass again on both platforms.

## Area checks

### libnostr concurrency (nostrc-lpvj)

- **One producer.** Only `recv_channel_enqueue()` sends to `conn->recv_channel` (connection.c:84). `grep` of connection.c/relay.c shows no other send. Each `try_send` runs under `priv->mutex`, and only while `conn->recv_channel` is the same live, open channel.
- **Detach before drain.** All three relay.c owners null the pointers under `conn->priv->mutex` before `nostr_connection_recv_channel_free()`: `relay_free_impl` (relay.c:607-617), `relay_discard_failed_connection` (685-695) and `nostr_relay_close` (1957-1969). Once detached, no frame can join, so no frame can be queued after the drain.
  - `nostr_relay_close` and `relay_free_impl` join the workers first (relay.c:1949, 592-597).
  - `relay_discard_failed_connection` runs before `write_operations`/`message_loop` start (relay.c:738/756 vs 775-777), so no reader competes with the drain.
  - `nostr_connection_new()`'s `fail:` path is reached only before any wsi exists (`ok == 0`), so no callback can race it.
- **Draining a closed channel is complete.** The MPMC `try_receive` returns `MPMC_BUSY -> -1` only for a slot claimed but not yet published. Every `try_send` finished under the mutex before the detach, so at drain time no slot is half-published.
- **No double free.** Every element is received once. The drain runs only for the owner that took the non-NULL pointer. The LWS `CLIENT_CLOSED` path (connection.c:445-455) still refs the channel under the mutex, but only closes it and never touches its elements. Its later unref just ends the channel's life.
- **Lock order.** The only new nesting is `priv->mutex` -> channel `NLOCK`, for the select-waiter signal inside `try_send`. No code takes `priv->mutex` while holding a channel or select-waiter lock: channel internals never call back into libnostr, and relay.c takes `r->priv->mutex` and `conn->priv->mutex` one after the other, never nested. The nostrc-75rv `CLIENT_WRITEABLE` re-arm (connection.c:~395-410) is unchanged; it nests `priv` -> send channel in the same order. No deadlock found, in review or in the stress loop.
- **Mock relay.** `send_response()` now queues a `WebSocketMessage`, the element type the drain frees. It still sends without `priv->mutex`, but only in test mode, where nothing else produces.

### TLS resumption (nostrc-0d0d)

- `LWS_SERVER_OPTION_DISABLE_TLS_SESSION_CACHE` is a `#define` (bit 39) in `lws-context-vhost.h:242` in both **4.3.3** and **4.5.8**, so the `#ifdef` is taken on both.
  - Both versions honour it in `openssl-client.c` (4.3.3 :290, 4.5.8 :294, skipping `lws_tls_reuse_session`) and in `openssl-session.c:230,346` (no new-session callback, no `SSL_SESS_CACHE_CLIENT`).
  - Without the define (lws < 4.1) lws has no session cache, so failing open there is harmless.
- `ssl_client_options_set` is applied to the vhost's client `SSL_CTX` (4.3.3 openssl-client.c:942-943). It is part of the client-context hash (:785), so a ticket-enabled `SSL_CTX` is never shared.
  - The default vhost takes `info->options`.
  - OpenSSL's own client cache is off by default (`SSL_SESS_CACHE_SERVER` mode), and clients never resume without `SSL_set_session`. No other resumption path is left.
- **Ubuntu's 4.3.3 was affected too.** `/usr/include/lws_config.h:229` defines `LWS_WITH_TLS_SESSIONS`, and the lws control resumes in Docker. The fix works there (see the table above).
- **Other lws contexts in the tree** (grelay, relayd, the mock server, tests) are servers or test controls. The only client context is libnostr's `g_lws_context`, so the fix covers every libnostr client connection.
- **gnostr and signet.** gnostr (through nostr-gobject) and signet (`signet/src/relay_pool.c`) reach relays through this context. The only effect is a full handshake on every reconnect: one RTT on TLS 1.3, as resumption without 0-RTT would be, plus ECDHE and certificate verification CPU. No API change. `signet_smoke` passes.
- The `LOAD_EXTRA_CLIENT_VERIFY_CERTS` hardening never running is real and already filed as nostrc-86xt.

### Rebinding defence (nostrc-qi5e)

- **TOCTOU.** The check and the dial use the same address objects. `GSocketClient` dials only addresses from `g_socket_connectable_proxy_enumerate(remote)`. `GhPublicAddress` inherits `proxy_enumerate` from `GNetworkAddress`, which builds a `GProxyAddressEnumerator` whose connectable is the subclass. On `direct://` it calls `g_socket_connectable_enumerate(priv->connectable)` (GLib 2.80 and main, `gproxyaddressenumerator.c:150-157`), which is the override that filters.
- **Happy eyeballs.** Every parallel attempt pulls from the same wrapped enumerator. The wrapper keeps at most one inner `next_async` outstanding per outer call, and GSocketClient issues the next outer call only after the previous one completes.
- **No reuse.** Each request gets its own session, aborted in `request_free`, so no connection can be reused across requests.
- **IPv6 scope and zone ids.** Link-local addresses are refused on their bytes, whatever the scope: `fe80::1%lo0` and `fe80::1%1` are refused. A zone id in a URL host is caught at enumerate time even if `host_public()` lets it through.
- **Bypass probe** (the classifier compiled out of the tree):
  - All 43 of the test's refused forms and my extra forms are refused, including:
    - SIIT `0:0:0:0:ffff:ffff:…`
    - the metadata forms 169.254.169.254, `::ffff:169.254.169.254` and `64:ff9b::a9fe:a9fe`
    - `fd00:ec2::254`
    - a non-zero u-octet
    - Teredo client/server private
  - Two gaps remain: L1 (local-use NAT64 with a non-zero suffix) and N2 (special-purpose ranges).
- **Proxy path.** With a real proxy, the destination name goes to the proxy, except for proxies without hostname support (L3). Tor mode is unchanged: SOCKS5, no local lookup, as the test checks.

### Versions

- libnostr: `declare_component_version(NOSTR 1 0 11)`, and the manifest component table and change row both say 1.0.11. **Consistent.**
- Groundhog stays at 0.10.0 (unreleased), with no manifest row for qi5e (N1).

## Findings

Severity scale: **Blocking** > **Medium** > **Low** > **Nit**. None is blocking.

### M1 (Medium, predates this change, not blocking). Auto-reconnect leaks the old connection's channels and any frames in them

- **Where:** `libnostr/src/relay.c:1002-1015` (`relay_attempt_reconnect`); `libnostr/src/connection-private.h:70`; `connection.c` `deferred_cleanup_process()`.
- **What:** `relay_attempt_reconnect()` calls `nostr_connection_close(old_conn)` and never detaches, drains or frees `old_conn->recv_channel`/`send_channel`. The service thread later frees `conn` and `priv` through deferred cleanup, but never the channels. The new header comment says "Every owner that releases a connection's recv_channel uses this", but this owner releases nothing.
- **Failure scenario:** A relay drops the WebSocket and `message_loop` auto-reconnects (relay.c:1438). Every successful reconnect leaks one 2048-slot recv `GoChannel`, one send `GoChannel`, and any `WebSocketMessage`s still queued (for example, a write that raced the drop). A flaky relay over a day of gnostr or Groundhog use grows memory without bound. LSan would report it as `go_channel_create` and `websocket_callback` or `nostr_connection_write_message`, but the sanitizer set does not exercise reconnects.
- **Fix:** Apply the same detach-under-`priv->mutex` + `nostr_connection_recv_channel_free()` + `go_channel_free(send)` sequence after `nostr_connection_close(old_conn)`. The caller is `message_loop`, the only reader. `write_operations` must first stop using `old_conn`'s `send_channel` before it is freed: it re-reads `r->connection` under `r->priv->mutex`. File a bead. This does not block, because the leak predates this change and lpvj's fix is correct on the paths it names.

### M2 (Medium, remaining risk, not blocking). "Public" includes the user's own public addresses

- **Where:** `gnome/groundhog/src/net/gh-net-http.c:190-229` (`gh_net_address_is_public`).
- **What:** The error text promises "leads to your own computer or local network … isn't used", but the check is only IP-class based. It accepts the host's own global IPv6 address (GUA), the GUAs of other LAN hosts, and the router's WAN IPv4 (hairpin NAT).
- **Failure scenario:** In System or No Proxy mode, a sender sends two attachments. Downloading the first from the sender's own server reveals the user's IPv6 /64 and public IPv4. The second URL's name resolves to `<victim /64>::1`, usually the home router's LAN-side admin interface, or to the user's WAN IPv4. It passes as "public", and GroundHog GETs it from inside the LAN.
- **Fix:** At enumerate time, also refuse addresses that are assigned to a local interface or fall in an on-link prefix (`getifaddrs`). Document the WAN-hairpin remainder in gh-net-http.h. File a bead.

### L1 (Low). Local-use NAT64: an address whose non-zero suffix fits only the /96 layout is judged by that layout alone

- **Where:** `gnome/groundhog/src/net/gh-net-http.c:160-186` (`nat64_local_public`, the `fits` test at :176).
- **What:** Only layouts whose suffix bytes are zero are decoded. An attacker sets one suffix bit so that only the /96 reading "fits", and makes that reading public.
- **Failure scenario:** `64:ff9b:1:abcd:c0:a801:100:1` is classified **PUBLIC** (probe run). Its /64 reading is 192.168.1.1 with suffix `00 00 01`, and its /96 reading is 1.0.0.1. On a network whose local-use NAT64 uses a /64 prefix `64:ff9b:1:abcd::/64`, a translator that extracts the embedded IPv4 without validating the SHOULD-be-zero suffix (RFC 6052 §2.2) delivers the connection to 192.168.1.1. This needs a rare network and knowledge of its prefix.
- **Fix:** For 64:ff9b:1::/48, decode all four layouts whatever the suffix and require every one to be public. This fails closed, which is acceptable for a rarely used prefix.

### L2 (Low, remaining risk). Network-specific NAT64 prefixes cannot be recognised

- **Where:** gh-net-http.c:229 (the global-unicast fallthrough).
- **What:** RFC 6052 NSPs are ordinary GUAs, so their synthesized addresses pass as public.
- **Failure scenario:** On an IPv6-only enterprise or ISP network with DNS64/NAT64 on a globally routed network-specific /96 (an ISP's or the enterprise's own prefix), a sender's name with AAAA `<NSP>::c0a8:0101` reaches 192.168.1.1 on the translator's IPv4 side, possibly the enterprise LAN.
- **Fix:** Document it in gh-net-http.h. Optionally, discover the local NSP with RFC 7050 (`ipv4only.arpa`) and judge the embedded IPv4.

### L3 (Low, documentation). "Through a desktop proxy the proxy does both" is not true for every proxy, and the defence is off in that mode

- **Where:** `gnome/groundhog/src/net/gh-net-http.h:77-78`; `gh-net-http.c:591`, where System mode with a proxy uses the filtered connectable only for `direct://`.
- **What:**
  - For a proxy without hostname support (an explicit `socks4://`; as far as I can tell, glib-networking also offers it as the last fallback for a GNOME "socks" setting), `GProxyAddressEnumerator` resolves the destination **locally** (`gproxyaddressenumerator.c:253-277`) and hands the unfiltered IP to the proxy.
  - With any local proxy on 127.0.0.1, a rebinding name reaches the user's own localhost services through that proxy.
- **Failure scenario:** In System mode with a local SOCKS4-only or HTTP proxy, a sender's name that resolves to 127.0.0.1 or 192.168.x has the proxy fetch it.
- **Fix:** Make the header precise (local resolution for SOCKS4; the proxy decides reachability). Consider refusing public-only downloads through non-Tor proxies, or at least `socks4`. This is a product decision; the current wording says the proxy is "trusted".

### L4 (Low, test coverage). The flood phase of `test_connection_recv_drain` does not detect the enqueue-after-detach race

- **Where:** `tests/test_connection_recv_drain.c:188-196`.
- **What:** The commit's second change (send under `priv->mutex`) is justified by reasoning only.
- **Failure scenario:** A future change that moves `try_send` back outside the mutex would reintroduce a leak, or a use-after-free once the channel is unreffed. This test would still pass: the parent callback with the new drain passed 30/30 under LSan.
- **Fix:** Add a test-only hook: a callback between the attach check and the send, or a `NOSTR_TEST` pause. With it, a test can detach and drain while a frame is pinned mid-enqueue and assert that the frame is dropped, not queued. Alternatively, record the limitation in the test's header.

### L5 (Low). Comments that no longer hold after lpvj

- **Where:** `libnostr/src/relay.c:1952-1955` ("A callback that acquired a channel before removal holds its own ref …"), `relay.c:1968` ("callbacks retain their own refs") and `relay.c:600-603`.
- **What:** The RECEIVE callback no longer takes a ref; only `CLIENT_CLOSED` does, and it never queues.
- **Failure scenario:** A maintainer relying on "callbacks retain their own refs" could reintroduce a send outside the mutex.
- **Fix:** Update the comments to the new invariant: queue only under the mutex to an attached channel.

### N1 (Nit). No VERSION_MANIFEST row for nostrc-qi5e

- **Where:** `VERSION_MANIFEST.md:66-67`.
- **What:** Groundhog 0.10.0 is unreleased, so no bump is needed, but the wave's Groundhog change (a new internal API and a privacy fix) is not recorded.
- **Failure scenario:** The wave coordinator misses the PATCH assessment when cutting 0.10.0.
- **Fix:** Add `| same | groundhog | 0.10.0 | No bump (unreleased): … nostrc-qi5e |`.

### N2 (Nit). Special-purpose ranges that are not globally reachable are judged public

- **Where:** `gh-net-http.c:190-229`.
- **What:** In IPv6, the probe finds 2001:2::/48 (benchmarking), 2001:10::/28 and 2001:20::/28 (ORCHID, ORCHIDv2) and 3fff::/20 (documentation, RFC 9637) PUBLIC. 198.18/15 and 2001:db8::/32 are refused, so this is inconsistent. In IPv4, TEST-NET-1/2/3 are also treated as public.
- **Failure scenario:** A lab or enterprise that numbers internal hosts from 2001:2::/48 is reachable.
- **Fix:** Add these ranges from the IANA special-purpose registries.

## Out-of-scope observations (not against these commits; beads recommended)

- **O1.** `groundhog-store-marmot` fails under Linux UBSAN: `libmarmot/src/commits.c:944` calls `memcpy(p.welcomes, welcomes /* NULL */, 0)`, reached from `marmot_update_group_metadata` via `test_store_marmot.c:2485`. It is from the W17b work. The `groundhog-sanitizers` CI job will fail on this branch and on its base until it is fixed (guard with `if (welcome_count)`). No bead exists.
- **O2.** Relay URLs chosen by other people (contacts' inbox relays, group relays) are dialled by libwebsockets with no public-address policy, literal or at connect time. If charter P-principles about "no local network" cover relays as well as downloads, this needs its own bead: lws resolves internally, so the qi5e technique does not apply.

## Verdict

**APPROVED.**

- 0d0d turns TLS resumption off for every libnostr client connection on lws 4.3.3 and 4.5.8.
- lpvj's drain is complete and race-free on the paths it names, with no double free and no new lock-order risk.
- qi5e's connect-time filter is truly TOCTOU-free for direct connections, including happy eyeballs.
- Every test fails without its fix, with the LSan/leaks caveat for the drain test.

Follow-ups: file beads for M1, M2 and O1, and consider L1-L5, N1 and N2.
