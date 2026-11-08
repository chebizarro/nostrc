# Investigation: Real-time voice for Groundhog — RTP-over-QUIC vs a "nostr-native" path, and QUIC vs FIPS

## Summary
**RTP-over-QUIC is not the right first choice for Groundhog calls, and "nostr-native" does not mean media over relays.** Nostr's role is identity + encrypted ephemeral signalling to the peer's kind-10050 inbox relays (NIP-AC: kinds 25050–25054 inside 21059 gift wraps); the media path should be GStreamer WebRTC (`webrtcbin` + libnice, Opus/RTP, DTLS-SRTP) for 1:1, with SFU + SFrame keyed from the MLS exporter for groups. **QUIC and FIPS are not alternatives at the same layer**: QUIC is an in-process L4 transport between already-reachable endpoints; FIPS is a daemon-backed mesh overlay that makes npubs reachable (discovery, Noise encryption, NAT hole-punching) and exposes unreliable `npub:port` datagrams — QUIC/RTP can run *over* FIPS. Groundhog today has async voice notes only; none of its NIP-17 envelope/inbox/outbox code can carry 21059 call signalling without new modules, and no packaged build has proven the WebRTC plugin set.

## Question as asked
> I would like to add voice capabilities to Groundhog (`nostrc/gnome/groundhog`) and I am wondering if RTP over QUIC is a suitable choice or if there is something more "nostr native"? What is the difference between QUIC for example, and FIPS?

## Initial assessment (Phase 1)

### What already exists
- Groundhog already has **asynchronous voice messages** (W27 slice A, commit `e79ca7f51`): `src/media/gh-voice-recorder.{c,h}`, `gh-voice-player.{c,h}`, `gh-voice-meta.{c,h}`, `gh-voice-waveform.{c,h}`, `src/ui/gh-voice-bubble.{c,h}`; tests under `tests/media/test_voice.c`, fixtures `tests/media/fixtures/voice/groundhog-opus.ogg`. Audio is recorded via GStreamer, uploaded to Blossom, and referenced from NIP-17 / MLS messages. Review: `docs/reviews/w27-voice-review-2026-10-03.md`.
- Therefore "voice capabilities" in this question is read as **real-time voice (calls / rooms)**, not voice notes. Any recommendation must distinguish the two.

### Working hypotheses
- H1: RTP-over-QUIC (RoQ, IETF avtcore) is a *transport mapping*, not a signalling or discovery mechanism; adopting it still leaves "how do two npubs find each other and negotiate a call" unanswered — which is exactly the part Nostr is good at.
- H2: The "nostr-native" story is signalling-over-Nostr (encrypted call-offer/answer/ICE events to a peer's inbox relays) plus *some* media transport; existing ecosystem efforts (NIP-53 interactive rooms / Nests, WebRTC-signalling NIP drafts, Keychat, etc.) all end up using WebRTC/SRTP or a hosted SFU for the media path.
- H3: FIPS is not a competitor to QUIC at the same layer. QUIC is an L4 transport (UDP-based, TLS 1.3, streams + unreliable datagrams). FIPS is an L3-ish *mesh overlay* that gives every node a secp256k1/npub identity, Noise-encrypted hop-by-hop + end-to-end paths, routing, NAT traversal and a native datagram API. One could run RTP (or RoQ, or anything) *over* FIPS; FIPS is the "find and reach this npub" layer that QUIC lacks.
- H4: The practical constraints for a C/GTK4 app are decisive: GStreamer availability of QUIC/RoQ elements vs mature `webrtcbin`/`rtpbin`, Flatpak/AppImage packaging, and what the peer apps (White Noise iOS, other Marmot clients) can realistically speak.

### Areas to investigate
- Groundhog media/net stack: `src/media/*`, `src/net/gh-net-session.c`, `src/net/gh-relay-*.c`, MLS service (`src/mls/gh-mls-service.c`) for an E2E key source for call media (MLS exporter secret → SRTP keys / Noise PSK).
- FIPS docs: `fips/docs/design/fips-native-api.md`, `fips-nostr-discovery.md`, `fips-session-layer.md`, `fips-transport-layer.md`, `docs/reference/native-api.md`, `nostr-events.md`.
- Sibling projects in the workspace: `whitenoise-ios` (does it do calls?), `metadesk`, nostrc libs (`cascadia-nips` kind registry, any existing real-time/NIP-53 support).

## Background / Prior Research
<!-- Phase 1.5 explore-agent findings land here -->

### B1. RTP-over-QUIC (RoQ) and Media-over-QUIC (MoQ) status — web probe, 2026-11-07
- **RoQ is not an RFC.** Latest is `draft-ietf-avtcore-rtp-over-quic-14` (intended *Experimental*), **expired 21 Sep 2025**; Jan-2026 AVTCORE chair slides note WGLC did not reach consensus. No RFC number exists. ([history](https://datatracker.ietf.org/doc/draft-ietf-avtcore-rtp-over-quic/history/), [chair slides](https://datatracker.ietf.org/meeting/interim-2026-avtcore-01/materials/slides-interim-2026-avtcore-01-sessa-chair-slides-00))
- What RoQ defines: RTP packets carried either in unidirectional QUIC **streams** (reliable, ordered, length-prefixed) or in QUIC **DATAGRAM** frames (unreliable); a varint **flow ID** prefix maps each to an RTP stream; RTCP may be carried the same way; QUIC connection-level congestion control applies; media protection is **QUIC's TLS 1.3 only** (hop-to-hop to whatever terminates QUIC — *not* E2E through a relay/SFU). ([draft](https://datatracker.ietf.org/doc/html/draft-ietf-avtcore-rtp-over-quic), [RFC 9001](https://www.rfc-editor.org/rfc/rfc9001.html))
- **MoQ**: `draft-ietf-moq-transport-23` (2 Nov 2026), still a draft; pub/sub relay fan-out for live streaming (e.g. Meta's Instagram Live ingest). No evidence of production use for 1:1 messenger voice calls.
- **GStreamer**: the Rust `quinn` plugin in gst-plugins-rs ≥ 0.14.0 (July 2025) ships `quinnquicsink/src`, `quinnquicmux/demux`, **`quinnroqmux/quinnroqdemux`** and WebTransport elements ([docs](https://gstreamer.freedesktop.org/documentation/quinn/index.html)). Packaging: Arch has `gst-plugin-quinn`; **no Debian/Fedora package found and it is not in the GNOME Flatpak runtime** — Groundhog would have to bundle it (Flatpak + AppImage, cf. bead `nostrc-h3kh`).
- C QUIC libs (msquic, ngtcp2, lsquic, quiche-FFI) are bare transports: no RoQ mapping, no ICE, no signalling.
- **WebRTC baseline**: `webrtcbin` (+ `libnice ≥ 0.1.23` in GStreamer 1.28, DTLS-SRTP, `rtpbin`) is the mature call-oriented path in GStreamer. E2E on top of SRTP = **SFrame, RFC 9605** (transport-independent, needs its own key management; browser "encoded transform" is JS-only and not a GStreamer feature).
- QUIC **does not do NAT traversal**: RFC 9000 path validation is explicitly not a NAT traversal mechanism; RoQ assumes ICE/STUN/TURN or similar out of band.

| Concern | QUIC/RoQ provides | Still needed |
|---|---|---|
| Transport | encrypted QUIC conn, streams + DATAGRAMs, CC | codecs, capture/playback, rate adaptation, call lifecycle |
| Discovery | nothing | peer/relay address discovery |
| NAT traversal | nothing (UDP only) | ICE/STUN/TURN or an overlay |
| Identity | TLS cert of the QUIC peer | npub ↔ key binding, authorization |
| Signalling | flow IDs on an existing conn | offer/answer, codec params, out-of-band channel |

### B2. What "nostr-native" voice looks like in the ecosystem — web probe, 2026-11-07
Key point: in every deployed system **Nostr carries identity, discovery, room metadata and call signalling; audio never traverses relays.**

| Option | Kinds | Signalling | Media | E2E? | Shipped |
|---|---|---|---|---|---|
| NIP-53 Live Activities / rooms | 30311 stream, 30312 interactive room, 30313 meeting, 1311 chat, 10312 presence | room/activity events + service URL; NIP-53 defines no call negotiation | per service: HLS, **MoQ** (Amethyst Nests, nostrnests.com), WebRTC (Jam/Corny Chat, mediasoup/HiveTalk) | **no** E2E guarantee; SFU terminates SRTP | yes ([NIP-53](https://github.com/nostr-protocol/nips/blob/master/53.md)) |
| NIP-100 (old) WebRTC signalling | 25050 ephemeral, typed `connect/offer/answer/candidate` | SDP/ICE in event content, `p`/`r` tags | WebRTC P2P | NIP-44 variants | open PR [#363](https://github.com/nostr-protocol/nips/pull/363); 0xchat ships calls on its own variant |
| **NIP-AC call proposal** | 25050–25054, wrapped in ephemeral gift-wrap **21059** | offer/answer/ICE + reject/hangup/renegotiate, group mesh calls | WebRTC P2P/mesh (+TURN) | gift-wrapped signalling; media = DTLS-SRTP only | **yes**: Amethyst, Noscall, Nospeak — open PR [#2301](https://github.com/nostr-protocol/nips/pull/2301) |
| Generic WebRTC bootstrap | provisional kinds, PR [#2461](https://github.com/nostr-protocol/nips/pull/2461) | peer-directed ephemeral events + replaceable endpoint announcements; explicitly *not* a call state machine | P2P | NIP-44 content | proposal only |
| Media *through* relays | custom events | — | chunked audio in events (Coracle `nvid` POC) | none | POC: ~32 KB/s ceiling, 0.8–1.1 s latency, no FEC — **not viable** ([nvid](https://github.com/coracle-social/nvid), [issue #771](https://github.com/nostr-protocol/nips/issues/771)) |
| FIPS | none | optional Nostr adverts for peer discovery | any IPv6 app / native datagram API over the mesh | Noise IK + XK | shipped mesh; **no known voice app on it** |

- **MLS/Marmot + calls**: no Marmot/NIP-EE call spec or deployed Nostr app derives media keys from the MLS group. But the standard pattern exists: signalling inside the E2E group, media via SFU/P2P, frames encrypted with **SFrame (RFC 9605)**, which *explicitly defines an MLS-exporter-based key derivation*. (Element Call/MatrixRTC is the closest deployed analogue, LiveKit SFU + room-distributed SFrame keys — not MLS.)
- NIP-EE is now marked unrecommended/superseded by Marmot.

### B3. What already exists in the workspace (nostrc, whitenoise-ios, metadesk) — explore probe, 2026-11-07
- **Groundhog voice notes are async attachments, not calls.** Recorder pipeline `autoaudiosrc → audioconvert → audioresample → level → opusenc → oggmux → fdsink` (`nostrc/gnome/groundhog/src/media/gh-voice-recorder.h:12–18`); player `appsrc → decodebin → audioconvert → audioresample → scaletempo → autoaudiosink` (`gh-voice-player.h:12–25`). Outbound Ogg/Opus, inbound AAC/MP4 from White Noise (`docs/reviews/w27-voice-review-2026-10-03.md`).
- **MLS exporter is already public in libmarmot** — the strongest direct building block for E2E call keys: `marmot_export_secret()` (`nostrc/libmarmot/include/marmot/marmot.h:1484–1498`, impl `nostrc/libmarmot/src/exporter.c:9–66`) does labelled, context-bound RFC 9420 MLS-Exporter derivation over the current epoch, 1–8160 bytes. Groundhog owns a `Marmot` instance (`gnome/groundhog/src/mls/gh-mls-service.c:263`) but **has no exporter wrapper or media-key use yet**.
- **QUIC already appears in the Marmot ecosystem, but only for agent text streaming**: White Noise iOS `watchAgentTextStream()` (`whitenoise-ios/whitenoise-ios/Conversation/StreamWatcher.swift:257–270`); libmarmot knows the group component but explicitly opens no QUIC stream (`libmarmot/include/marmot/marmot-group-components.h:11–24`). White Noise voice notes: `AVAudioRecorder` AAC/.m4a (`VoiceMessageRecorder.swift:209–222`). **No call feature or roadmap in White Noise iOS.**
- **NIP-53 support is read-only discovery**: parser in `nostrc/nips/nip53/include/nip53.h:15–35`; GNostr discovers kind 30311 (`apps/gnostr/src/ui/page-discover-live.c:255,328`). Groundhog only preserves a NIP-29 `livekit` metadata flag (`gh-nip29-group.c:413`) — not a LiveKit integration.
- **metadesk has a working FIPS transport**: `md_stream_connect_fips()` after daemon health + peer readiness + session negotiation (`metadesk/src/client/main.c:285–310`); it is **framed TCP over the FIPS TUN** (`metadesk/src/core/stream.h:3–15`, npub resolution `stream.c:554–563`), H.264/FFmpeg video (`core/decode.c:162`); **audio explicitly deferred** (`metadesk/docs/metadesk-spec.md:38–40`). This is the closest in-house precedent for "real-time media between two npubs over FIPS".
- **Kind registry** `nostrc/libnostr/include/nostr-kinds.h`: ephemeral constants 21000, 22242, 23194, 23195, 24133, 24242, 27235 (`:130–137`); 20000–29999 classified ephemeral (`:247–248`); 30311 (`:161`), 1311 (`:56`), 39004 LiveKit participants (`:187`). **No 30312/30313, no 25050–25054 / 21059 call-signalling kinds.**
- Zero code hits in all three roots for `webrtc`, `webrtcbin`, `rtpbin`, `srtp`, `libnice`, `msquic`, `ngtcp2`, `sframe`, `CallKit`, `AVAudioEngine`. No open bead for calls (`bd search "voice call"` → none).

### B4. FIPS as a media substrate, and FIPS vs QUIC layering — explore probe over `fips/docs`, 2026-11-07
- **Native datagram API** (`fips/docs/reference/native-api.md`, `docs/design/fips-native-api.md`, `docs/how-to/write-a-native-api-client.md`): control channel is an `AF_UNIX` `SOCK_STREAM` socket speaking newline-delimited JSON; `connect`/`listen` hand back a flow FD via `SCM_RIGHTS` (`SOCK_SEQPACKET` on Linux, `SOCK_DGRAM` on macOS/FreeBSD); one `send` = one datagram (`native-api.md:575–644, 663–707`). Addressing is `npub:port` (x-only secp256k1 key + FSP port), no lookup/discovery in the API (`:66–89, 176–177, 582–584`). **Unreliable, unordered, no ACK/retransmit/flow control** (`:465–468`). `max_payload` ≈ transport MTU − FIPS encapsulation − 4-byte port header ≈ **1,247 bytes** on default UDP MTU (`docs/how-to/use-the-native-datagram-api.md:145–148`, `fips-mtu.md:16–29, 47–67`). No C client shipped, but the C how-to exists; the tricky bit is `recvmsg` + `SCM_RIGHTS` pairing with the JSON reply (`write-a-native-api-client.md:1–28`). API is **off by default, experimental, surface may change**; callers must be in the `fips` group and can then send *as the node* (`fips-native-api.md:93–118`, `reference/security.md:302–320`).
- **Identity**: node identity is a Nostr secp256k1 key; an existing personal nsec can be provisioned (`docs/how-to/persistent-identity.md:3–8, 172–191`), but the identity belongs to the **daemon**, not to each local API caller — if Groundhog's key ≠ node key, FIPS sessions authenticate the node (`fips-architecture.md:106–153`).
- **Encryption**: Noise IK per peer link (hop-by-hop) + Noise XK per FSP session (end-to-end across transit nodes) (`fips-architecture.md:164–190`, `fips-session-layer.md:289–317`).
- **Nostr discovery / NAT traversal** (`docs/reference/nostr-events.md:10–32, 37–67, 91–110, 162–191`): kind **37195** replaceable endpoint advert; **10050** inbox relay list; **21059** ephemeral NIP-44 + NIP-59 gift-wrapped traversal offer/answer. For `udp:nat` adverts peers exchange reflexive/local candidates via relays, STUN on the punch socket, then probe a candidate pair; on success the punched socket becomes a direct UDP link (`fips-nostr-discovery.md:200–223, 241–256, 281–315`). **Relays carry only signalling.** Symmetric NAT / UDP-blocked → no automatic FIPS relay fallback; design calls for an app-level relay or a non-UDP transport (`port-advertisement-and-nat-traversal.md:543–554`).
- **Real-time characteristics**: MMP measures RTT/loss/jitter/goodput as routing inputs, not a QoS guarantee (`fips-mmp.md:1–61`); UDP is the recommended Internet transport, TCP adds HoL blocking, Tor 200 ms–2 s RTT (`fips-transport-layer.md:118–153, 214–225, 329–340, 535–543`). Opus @ 50 pps × ~100 B ≈ 40 kbit/s fits easily; but **no retransmit, no throttle → app must do jitter buffer, sequencing, rate adaptation** (`fips-session-layer.md:68–76`).
- **No QUIC transport plugin in FIPS**; QUIC only appears as a spin-bit/measurement reference (`fips-prior-work.md:159–165, 314–334`). QUIC *could* run above the FIPS IPv6/TUN interface.
- **Platform**: `v0.6.0-dev`, unstable protocol/API; packaged for Debian/AUR/macOS/FreeBSD/Windows/OpenWrt, **no Flatpak**; daemon runs as root (TUN needs `CAP_NET_ADMIN`), so a Flatpak Groundhog would need host-service + Unix-socket access (`README.md:333–417`, `fips-ipv6-adapter.md:395–401`).

| Capability | FIPS | QUIC |
|---|---|---|
| Layer | overlay mesh (L3-ish) + session layer; ships as a daemon | L4 transport library over UDP/IP, in-process |
| Identity | npub = node key; Noise-authenticated | TLS 1.3 certs; npub binding is app's job |
| Discovery | Nostr adverts (37195/10050/21059), mDNS | none |
| NAT traversal | STUN-assisted UDP hole punching via Nostr signalling; mesh routing | none (needs ICE/TURN) |
| Encryption | Noise IK hop-by-hop + Noise XK end-to-end | TLS 1.3, endpoint-to-endpoint of the QUIC connection |
| Streams / datagrams | unreliable unordered datagrams (native API); or any IPv6 over TUN | reliable ordered streams + optional unreliable DATAGRAMs (RFC 9221) |
| Congestion / loss | none at native API; ECN relay + MMP metrics only | CC + loss recovery built in |

## Investigator Findings
<!-- Pair investigator appends here -->

### Scope and evidence

Read-only, source-grounded investigation of the eight requested claims, against the working files at nostrc HEAD `8553c908cfb6e72f53dc651df11e10a43e9536c6`. Three narrow explore probes covered envelope/inbox, packaging, and metadesk/FIPS; the investigator independently checked their load-bearing ranges. References below are relative to **nostrc** unless prefixed `metadesk/`, `fips/`, or `cascadia-nips/`. The latter registry was read at `/Users/bizarro/Documents/Projects/cascadia-nips` (HEAD `c8eab6ded26119f2b188cac4a0e9b638a44c660b`).

This is **static inspection plus the separately labelled host-only factory diagnostic below**, not executed call interoperability, Linux-image acceptance, or Flatpak-runtime proof. No builds, tests, package installs, hooks, tracker mutations, commits, or pushes were performed for this investigation. Only this section is added; the earlier assessment/log is preserved. Documentation-only: no shipped component version bump.

### 1. Outbound envelope and durable DM publication

**Verdict: 1059-specific is true; strictly kind-14-only is false; generic arbitrary-output-kind wrapping is absent.**

- `gnome/groundhog/src/app/gh-nip17-envelope.c:383–390` defines the actual ordinary-rumor allowlist, broader than the older kind-14 comments:
  ```c
  return kind == 14 || kind == GH_NIP17_FILE_KIND || kind == 7 || kind == 5 ||
         kind == GH_MESSAGE_MLS_POLL_KIND || kind == GH_MESSAGE_MLS_POLL_VOTE_KIND;
  ```
  `:824–837` also requires unsigned JSON, canonical ID, and the active sender. Both ordinary seal entrypoints converge on `seal_start()` (`:880–953`), which calls that recipient validator. Thus 25050–25054 are rejected, not transparently sealed. There is a separate **kind-444 Welcome** path (`:956–988`), not an arbitrary-rumor escape hatch.
- The seal is kind 13, account-signed through the account controller (`gh-nip17-envelope.c:218–258`). `draw_seal_time()` calls `nostr_nip59_randomize_timestamp()` (`:203–215`); `wrap_once()` calls `nostr_nip59_wrap()` (`:261–278`). Both seal and wrap times are randomized, not live-offer timestamps. The lower helper subtracts a random offset from now and fails if randomness fails (`nips/nip59/src/nip59.c:97–113`); its default window is two days (`:25`). Freshness must therefore come from authenticated call/rumor semantics, not the outer timestamp alone.
- `GhDmSendLeg.wrap_json` explicitly says **“signed kind-1059; republish this, never re-seal”** (`gnome/groundhog/src/app/gh-dm-send.h:126`). Recipient wraps are matched by receiver and the self-copy retained (`gh-dm-send.c:788–803`). Stored-send validation calls `nostr_nip59_validate_gift_wrap()` (`:1133–1153`), which requires kind 1059 (`nips/nip59/src/nip59.c:345–356`; `libnostr/include/nostr-kinds.h:45`).
- This is a real durable path, not just a planner: `gh-app-outbox.c:539–552` passes the active store into `gh_outbox_new()`. `gh-outbox.c:1060–1081` persists the sealed per-recipient/self events with `gh_store_seal()`; `:1838–1869` reloads unfinished NIP-17 entries on construction. Eligible sealed entries resume according to state, due targets and retry deadline (`:1619–1652`); `:1289–1305` publishes `leg->event->event_json` **byte-for-byte**. The automatic retry window is 72 hours (`:13`, `:185–187`), not a call-offer lifetime. This does **not** mean settled/accepted messages all replay on every startup. It means unfinished eligible sends can resume, which is sufficient to make this a wrong default for live offers/ICE/hangups.
- The persistence layer itself is not a 1059 enforcement boundary: `gnome/groundhog/src/store/gh-store.c:3509–3539` checks role, target/id/text bounds and targets; `:3570–3586` inserts `event_json`. Bypassing the DM validator would not make the durable queue ephemeral merely by choosing kind 21059.

**Lower-level reusable pieces:** `nostr_nip59_wrap()` documents **any inner event kind, signed or unsigned**, but exposes no output-kind parameter (`nips/nip59/include/nostr/nip59/nip59.h:71–109`). Implementation serializes the inner event, NIP-44-encrypts it, and unconditionally sets `NOSTR_KIND_GIFT_WRAP` before signing (`nips/nip59/src/nip59.c:145–183`). `nostr_nip17_create_seal()` likewise serializes the supplied rumor without an inner-kind allowlist (`nips/nip17/src/nip17.c:100–134`), but `create_rumor()` builds kind 14 (`:65–96`) and its gift-wrap constructor fixes the outer kind (`:247`). NIP-44 is a bytes/length crypto primitive, not an event-kind policy (`nips/nip44/src/core/nip44.c:24–48`).

**Conclusion:** reuse NIP-44, canonical event construction/validation, fresh ephemeral keys, and account-controller encryption/signing seams. A new call-envelope API (or an explicitly generalized NIP-59 API) is needed for 25050–25054 → seal → 21059. There is no existing single API accepting both arbitrary rumor kind and arbitrary wrap kind. Changing an already-signed wrap's kind without re-signing is not valid. Do not reuse the durable chat outbox unchanged for transient signalling.

### 2. Inbox subscription, dispatch and persistence

**Verdict: normal inbox subscriptions exclude 21059; an unsolicited 21059 can reach generic dispatch, but is rejected before decryption/admission.**

- `gnome/groundhog/src/app/gh-dm-inbox.c:14` defines `KIND_GIFT_WRAP 1059`. `inbox_filters()` (`:592–610`) requests only that kind, with `#p = active account`, `since`, optional `until`, and a limit:
  ```c
  const int kinds[] = { KIND_GIFT_WRAP };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_tags_append(filter, "p", pubkey_hex, NULL);
  ```
  `gh-inbox-setup.c:594–598` probes the same kind with a **random recipient** and limit 1; that is a capability probe, not a 21059 listener.
- Do not equate relay-side filtering with a local dispatcher guard: `gnome/groundhog/src/relay/gh-relay-scope.c:552–575` verifies signed-event structure/signature and dispatches valid events; it does not require a matching kind there. `deliver_event()` (`:532–550`) deduplicates and emits the update. Its `stored` flag identifies the pre-EOSE answer, **not a database write**.
- `gh-dm-inbox.c:563–585` queues received JSON for unwrap. The definitive outer-kind guard is `gh-nip17-inbox.c:226`:
  ```c
  NostrEvent *wrap = parse_signed(wrap_json, 1059, wrap_id);
  ```
  `parse_signed()` (`:169–179`) explicitly rejects `nostr_event_get_kind(event) != kind`. A 21059 thus fails **before** the NIP-44 signer call (`:247–249`). The inner rumor guard also excludes call kinds: 14/15/1068/1018, optionally 7/5, and an opt-in Welcome path (`:395–412`).
- Only successfully unwrapped ordinary messages reach `gh_conversation_store_admit()` (`gh-dm-inbox.c:416–429`). Protocol-invalid wraps are counted rejected; a rejected ID is recorded only if validation incurred a signer call (`:538–544`). A 21059 rejected at the outer-kind check incurs none, so this path does not persist its call payload or even that rejection record into `gh-store`.

**Conclusion:** add a distinct 21059 subscription/unwrap/call dispatcher, with authenticated freshness, call correlation and replay handling. Reusing the relay scope/dedup machinery is possible; simply broadening the REQ filter is insufficient. No current Groundhog call-store admission exists.

### 3. Recipient inbox resolution and the FIPS tag mismatch

**Verdict: the strict Groundhog claim and the FIPS-document `r`-tag claim are both true.**

- The interface contract says signed recipient-authored kind 10050 only, **“never from kind 10002, home relays or any default”** (`gnome/groundhog/src/app/gh-inbox-resolver.h:12–20`). The `.c` file implements interface/result plumbing, not a second fallback policy.
- `gh-inbox-lookup.c:8` defines kind 10050; `:289–309` checks signed-event validation, author, kind, future skew and newest-event ordering. `inbox_relays()` (`:259–281`) admits only `relay` tags, validates ws(s) URLs, deduplicates and caps at 16:
  ```c
  if (!tag || g_strcmp0(nostr_tag_get_key(tag), "relay") != 0 || ...)
    continue;
  ```
  `:186–201` returns FOUND/EMPTY/NO_SOURCES/NOT_FOUND/UNREACHABLE; it does not substitute default delivery relays. Discovery sources used to **find** a 10050 are not delivery fallback destinations.
- The real app uses the cached contact-directory resolver (as the interface documents). Its parser also requires `relay` (`gnome/groundhog/src/app/gh-contact-directory.c:216–238`); its result builders copy only the admitted inbox list or return explicit absence/failure (`:388–402`, `:776–787`). This confirms the contract is not just aspirational header text.
- **FIPS documentation evidence, not a claim of executed FIPS publication:** `fips/docs/reference/nostr-events.md:162–181` identifies 10050 as NIP-17 and describes falling back to local `dm_relays` on fetch failure/absent/empty lists. `:184–190` explicitly states:
  > FIPS emits each relay as an `r` tag whose single value is the relay URL.
  It then states this is not NIP-17's `relay` form, other NIP-17 clients do not read it, and FIPS reads both forms (ignoring NIP-65 read/write-marked `r` tags).

**Conclusion:** `GhInboxResolver` is directly reusable for peer call-inbox discovery, preserving its no-default-delivery policy. A FIPS-published `r`-only 10050 is not interoperable with that parser; do not silently import FIPS's fallback semantics or describe its published list as already compatible.

### 4. Network mode, Tor and live local-network policy

**Verdict: reusable policy state/hooks exist; automatic enforcement on ICE/media sockets does not.**

- `gnome/groundhog/src/net/gh-net-session.h:13–26,40–44` defines **SYSTEM**, **NONE**, **TOR** (`GhNetMode`). SYSTEM uses desktop proxies for web requests but direct GNostr relay sockets; NONE means direct/no proxy; TOR requires SOCKS5 with remote DNS and no direct fallback. “Direct/Tor” is therefore an incomplete description of a three-mode policy.
- `gh-relay-net.c:79–115` obtains `gh_net_session_get_mode()`, serial and URL permission; non-Tor uses the GNostr transport, Tor uses the session's resolver and Soup transport. `:117–129` starts rebuilding/closing old-mode connections after changes. `gh-net-session.c:334–354` increments the serial and emits `changed`; `:366–382` reloads `network-mode` and `tor-socks-address`. Thread-safe mode/resolver/serial accessors are public (`gh-net-session.h:100–107`), along with a default-session accessor (`:117–121`).
- Commit `865f98400` is present and does add a live preference. The current enforcement is in **GhNetHttp**, not in the `GhNetMode` enum. `gh-net-http.c:664–673` reads `allow-local-network` and returns FALSE in Tor mode. `:694–698` applies it to public-only HTTP requests; `:847–877` cancels old-mode requests and public-only requests after permission revocation; `:887–888` connects the settings listeners. It does not alter every relay/socket route.
  ```c
  if (!self->settings || gh_net_mode_from_string(mode) == GH_NET_MODE_TOR)
    return FALSE;
  ... g_settings_get_boolean(self->settings, "allow-local-network");
  ```

**Conclusion:** a new media stack must explicitly consume current mode, mode changes and local-network consent, and stop/reconfigure its own sockets. Relay signalling going through Tor does not proxy ICE UDP, STUN, TURN or FIPS traffic. No existing code wires a media/ICE stack to these hooks; treating it as automatically covered would violate the existing fail-closed intent. The HTTP-specific helper is a policy precedent, not a generic ICE candidate filter.

### 5. GStreamer build and packaging availability

**Verdict: voice-note GStreamer support is established; a complete packaged WebRTC or RoQ runtime is not established.**

- `gnome/groundhog/CMakeLists.txt:3338–3355` checks only `gstreamer-1.0`, `gstreamer-audio-1.0`, and `gstreamer-app-1.0 >= 1.20`, and builds the four voice-note sources. There is no WebRTC/SDP/RTP/libnice pkg-config gate in that voice section.
- Recorder factory checks are `audioconvert audioresample level opusenc oggmux fdsink` plus the selected source (`src/media/gh-voice-recorder.c:427–445`). Player checks are `appsrc decodebin audioconvert audioresample scaletempo autoaudiosink` (`gh-voice-player.c:422–441`). Neither checks any requested call element. Existing recorder output is an Ogg file pipeline, not RTP (`gh-voice-recorder.c:503–512`). The capture/Opus and playback techniques are reusable; the file-oriented pipeline is not an already-live media transport.
- **CI recipe:** `scripts/linux-ci.Dockerfile:10–25` starts from Ubuntu 24.04 and explicitly installs **base + good + bad + libav**, with `--no-install-recommends`. It does not explicitly install ugly, gst-plugins-rs or `gstreamer1.0-nice`. `:59–62` contains a build-time `gst-inspect` loop including `opusenc`, but no WebRTC/ICE/DTLS/SRTP/RTP/DSP elements. Reading that RUN instruction is **not evidence that this investigator built or ran the image**.
- **Flatpak recipe:** `gnome/groundhog/packaging/flatpak/org.nostr.Groundhog.yml:14–16` inherits `org.gnome.Platform//51`; `:24–27` grants network and PulseAudio. Its modules (`:48,65,75,89,105,121,145,161,179`) are secp256k1, sodium, qrencode, nsync, jansson, libwebsockets, tcl, sqlcipher and Groundhog. There are **no explicit GStreamer plugin-set, libnice, or Rust-plugin modules**. Consequently, base/good/bad/ugly/libav/rs contents are inherited-runtime questions, not answers encoded in this manifest. No Flatpak-runtime inventory was available/executed here; attempts to retrieve the matching upstream runtime build description did not establish that inventory. All Flatpak element-presence cells below remain deliberately unknown.

The following distinguishes **package/source expectation** from a loadable factory on an actual target. Ubuntu's official [Noble bad-package file list](https://packages.ubuntu.com/noble/amd64/gstreamer1.0-plugins-bad/filelist) lists the webrtc, dtls and srtp plugin libraries, but not webrtcdsp; [nice's file list](https://packages.ubuntu.com/noble/amd64/gstreamer1.0-nice/filelist) places `libgstnice.so` in a separate package. These external package facts supplement, not replace, repository evidence.

| Requested factories | Provider | CI recipe / package expectation (not executed) | Flatpak 51 |
|---|---|---|---|
| `opusenc` | base/opus | selected; also covered by the Dockerfile inspection loop | unknown, inherited |
| `rtpopuspay`, `rtpopusdepay` | good/rtp | good selected; not factory-checked by recipe | unknown, inherited |
| `webrtcbin` | bad/webrtc | plugin library supplied by selected Noble bad package; no factory/call check | unknown, inherited |
| `dtlssrtpenc`, `dtlssrtpdec` | bad/dtls | plugin library supplied by selected Noble bad package; not checked | unknown, inherited |
| `srtpenc` | bad/srtp | plugin library supplied by selected Noble bad package; not checked | unknown, inherited |
| `nicesrc`, `nicesink` | libnice GStreamer plugin | separate `gstreamer1.0-nice` is not selected; cannot count ICE as available | unknown, inherited |
| `webrtcdsp`, `webrtcechoprobe` | bad/webrtcdsp, optional build dependency | not in the inspected Noble bad-package payload; no explicit DSP provision/check | unknown, inherited |
| `quinn*` / RoQ | gst-plugins-rs / gst-plugin-quinn | no provider selected or checked | not app-bundled; inherited availability unknown |

Upstream identifies [opusenc as base](https://gstreamer.freedesktop.org/documentation/opus/opusenc.html), [webrtcbin as bad](https://gstreamer.freedesktop.org/documentation/webrtc/index.html), [webrtcdsp as bad](https://gstreamer.freedesktop.org/documentation/webrtcdsp/webrtcdsp.html), and [quinn as its separate plugin](https://gstreamer.freedesktop.org/documentation/quinn/index.html). Merely installing the bad set does not imply optional DSP/libnice features are loadable.

**Executed host-only diagnostic:** the investigator repeated `GST_REGISTRY_UPDATE=no gst-inspect-1.0 <factory>` against the already installed macOS/Homebrew GStreamer **1.28.7**, with no build or installation. `webrtcbin`, `dtlssrtpenc/dec`, `srtpenc`, `rtpopuspay/depay`, `opusenc`, and `quinnroqmux` returned success; `nicesrc/nicesink` and `webrtcdsp/webrtcechoprobe` returned failure. This confirms that the host has some call/RoQ plugins but lacks others; it does **not** establish Linux/Flatpak contents, full pipeline operation, or Groundhog bundling. No call or media pipeline was executed.

**Conclusion:** no gst-plugins-rs bundling head start is demonstrated by either recipe. The stronger prior statement “quinn is absent from the GNOME runtime” is **not independently established here**. Likewise, no target-runtime factory inventory can honestly be called passing. The verified gap is a call-specific dependency/factory gate plus any missing runtime providers, not the complete absence of GStreamer.

### 6. MLS exporter, serialization and conversation epoch identity

**Verdict: the exporter is public and directly reusable cryptography; Groundhog has group/epoch getters, but no exporter/media-key integration.**

`libmarmot/include/marmot/marmot.h:1484–1498` exposes:
```c
MarmotError marmot_export_secret(Marmot *m, const MarmotGroupId *mls_group_id,
    const uint8_t *label, size_t label_len,
    const uint8_t *context, size_t context_len,
    uint8_t *out, size_t out_len);
```
The documented bound is **1–8160 bytes** for SHA-256; label/context are raw bytes (NULL only with zero length), output must be writable/non-aliasing, failures wipe output and callers wipe success. **Serialize calls on the same Marmot instance**; this is not a thread-safe media-callback API.

- `libmarmot/src/exporter.c:9–19` enforces arguments and `out_len <= 255 * MLS_HASH_LEN`. `:23–54` loads an active group's canonical state, checks group ID/epoch agreement and refuses missing/stripped exporter secret. `:55–65` derives from `group.epoch_secrets.exporter_secret`, wipes loaded state and wipes output on error. There is no old-epoch selector or media key cache here.
- Exact `marmot_export_secret` search over Groundhog `src/` returned **zero** uses. Within libmarmot the only occurrences are the public declaration, implementation and 12 calls in `libmarmot/tests/test_exporter.c:68–117`. Those tests were inspected/search-located, **not executed** in this investigation. There is no existing production caller/media-key use in that scope.
- Groundhog **does expose identity and epoch**: `gh_mls_service_lookup()` accepts an MLS group hex ID or a conversation room ID (`gnome/groundhog/src/mls/gh-mls-service.h:826–827`; implementation `gh-mls-service.c:9788–9795`). `gh_mls_group_get_group_id()` returns `gid_hex` (`:737–742`); `gh_mls_group_get_epoch()` returns `guint64 epoch` (`:765–770`). `group_refresh()` reads Marmot and updates/notifies epoch (`:1902–1910`, `:1936–1939`). A borrowed `Marmot *` accessor exists but is explicitly **for tests/diagnostics, one thread** (`gh-mls-service.h:740–741`; implementation `:9780–9785`). That is not a production exporter wrapper.

**Conclusion:** the necessary group ID/current-epoch metadata is already accessible. A service-owned serialized exporter/key-lifecycle module and an actual SFrame/media implementation are still needed; fetch metadata and derive keys consistently with epoch changes, not independently from streaming threads. An epoch number alone is not globally unique, nor does this API define an SFrame key-ID format. A secret derived from the chat group authenticates/enables that group's membership, not automatically only the people who joined a call.

### 7. metadesk FIPS transport and C control-socket precedent

**Verdict: TCP over FIPS IPv6/TUN is confirmed; a C management-control client exists, but a C native datagram/FD-passing client was not found.**

- `metadesk/src/client/main.c:275–316` checks daemon health, waits for peer readiness, negotiates the application session and then calls `md_stream_connect_fips()`. `metadesk/src/core/stream.c:554–571` resolves the npub to IPv6 and calls the ordinary connector:
  ```c
  return md_stream_connect(ipv6_str, port, timeout_ms);
  ```
  That connector sets `hints.ai_socktype = SOCK_STREAM`, resolves and opens/connects a normal Internet socket (`stream.c:438–463`). This is TCP, not the FIPS native unreliable datagram FD. `fips_addr.c:342–371` tries `<npub>.fips` AF_INET6 DNS then deterministic address derivation; its comment explicitly warns that derivation does not prove a discovered peer or usable route.
- **Useful but different client:** `metadesk/src/core/fips_control.c:606–640` opens `AF_UNIX/SOCK_STREAM`. `:793–836` builds/sends a JSON command and reads a response line; `:714–746` uses ordinary `read()`. This is the daemon-management seam used for readiness, not the native flow socket protocol. It can contribute bounded socket/JSON/error-handling patterns, not a ready media datagram transport.
- Exact `SCM_RIGHTS`, `recvmsg`, `sendmsg`, `native_api`/`native-api`, and `SOCK_SEQPACKET` searches in `metadesk/src` C headers/sources returned no native flow client. In nostrc, the source-level `SCM_RIGHTS` hit is `gnome/nostr-homed/src/auth/auth_boundary.c:214–228`, which **closes received FDs and rejects the message**, not a reusable FIPS receiver. Searches for `fips`, `.fips` and `fd00:` in nostrc C-family source found private-address tests, not a FIPS integration. This is a scoped source-search absence, not a claim about every untracked/ignored build tree.
- The distinction matters: `fips/docs/how-to/write-a-native-api-client.md:1–17` directs Rust users to `fips::native::client` and describes what other languages must implement. `:19–29` requires **every setup read to use `recvmsg` with an ancillary buffer**; ordinary `read` loses the descriptor. Thus adapting metadesk's line reader unchanged would silently discard native flow FDs.

**Conclusion:** reuse the metadesk npub→IPv6/TCP precedent only if deliberately choosing the TUN/TCP route. Native FIPS media needs a new C AF_UNIX JSON + SCM_RIGHTS client and a datagram media pipeline; the existing management client is not that implementation.

### 8. Kind constants and the sibling registry

**Verdict: absent in `nostr-kinds.h` is true; absent everywhere in Cascadia is false for 21059.**

- Exact search for `21059|2505[0-4]|3031[23]` in `libnostr/include/nostr-kinds.h` returned zero matches. The ephemeral block (`:130–137`) contains the other transport kinds, but no call/gift-wrap variant constant; the addressable block has 30311 then 30315 (`:161–162`), not 30312/30313. Classification as an ephemeral range is not support for wrapping, filtering or dispatch.
- The sibling registry **already has 21059**: `cascadia-nips/registry/event-families.yaml:407–417` declares:
  ```yaml
  nip59_ephemeral_gift_wrap:
    kind: 21059
    symbol: NIP59_EPHEMERAL_GIFT_WRAP
  ```
  It describes ephemeral NIP-59 transport for no-persistence ContextVM payloads, not a call state machine. Generated Go already exports that constant (`cascadia-nips/generated/go/kinds.go:41–43`). Exact searches of the registry and generated Go found no 25050–25054 or 30312/30313 allocations/constants.

**Conclusion:** libnostr's C constants would belong in the ephemeral block (21059 and 25050–25054) and addressable block (30312/30313) of `libnostr/include/nostr-kinds.h`. For Cascadia, reuse the existing 21059 allocation rather than inventing another; new call/room allocations belong in `registry/event-families.yaml` and its generated bindings, if adopting that registry. Neither constants nor range classification supply call protocol behavior.

### Conclusions

- **Directly reusable:** recipient kind-10050 `relay` resolution (without delivery fallback), relay scope/validation/dedup infrastructure, account-controller encryption/signing, NIP-44 and ephemeral-key primitives; GStreamer capture/Opus/playback building blocks; Marmot exporter cryptography and Groundhog group/epoch metadata.
- **Needs a new module/integration:** 21059 call envelopes and inbox dispatch; live call lifecycle/freshness/replay policy instead of durable DM retry; real-time media pipeline, ICE/network-policy integration and call-specific packaged factory checks; service-owned serialized MLS-to-media key lifecycle if MLS/SFrame is chosen. Native FIPS additionally needs the FD-passing C client.
- **Absent in inspected Groundhog source:** a call state machine, WebRTC/RTP/RoQ/SFrame integration, exporter callers and call-kind constants. No explicit Rust GStreamer plugin bundling was found. metadesk supplies TCP/TUN and management-control precedents, **not** native datagram voice.
- **Do not overclaim:** 1059 is fixed but rumors are not only kind 14; 21059 already exists in the sibling Cascadia registry; FIPS's documented `r`-tag 10050 is incompatible with Groundhog's strict parser; Flatpak plugin presence and a working target call remain unverified.


## Investigation Log

### Phase 1.5 — External facts (four explore probes)
**Hypotheses:** H1 (RoQ is transport-only), H2 (nostr-native = signalling over relays), H3 (FIPS ≠ QUIC layer).
**Findings:** see B1–B4 above. All three hypotheses confirmed by external sources; H4 (packaging decides) strongly supported — RoQ GStreamer elements exist only in gst-plugins-rs `quinn`, unpackaged on Debian/Fedora/Flatpak.
**Conclusion:** Confirmed.

### Phase 2 — Context builder + oracle first pass (chat `investigate-groundhog-vo-6E0E5C`)
**Hypothesis:** Groundhog's existing NIP-17 envelope / inbox-resolver / relay-scope / MLS-exporter code gives most of a nostr-native *signalling* layer; the media path is a separate choice.
**Oracle assessment (to be verified by the pair):**
- Recommends: encrypted Nostr call signalling (NIP-AC-shaped) + GStreamer WebRTC (`webrtcbin`, ICE, DTLS-SRTP) for 1:1; SFU + SFrame for groups; FIPS optional transport experiment; RoQ "viable but no first-release advantage".
- Claims the DM send path is **not** a generic ephemeral signalling API: `GhDmSendLeg.wrap_json` is a kind-1059 wrap, text path builds kind-14 rumors, sealing validates rumor shapes, the durable outbox can republish stored wraps (would replay stale offers/hangups), room publication adds randomised spacing + self-copies.
- Claims `nostr-kinds.h` lacks 21059 / 25050–25054 (range classifier treats them as ephemeral, but no dispatch/filter support).
- Claims `gh-relay-net.c` network mode (Direct/Tor) does not constrain WebRTC ICE sockets → Tor-mode signalling must not silently leak UDP/STUN.
- Claims FIPS publishes 10050 with `r` tags while Groundhog's resolver reads `relay` tags, and FIPS has fallback-relay behaviour unlike Groundhog's strict inbox policy.
- Claims `marmot_export_secret()` has no media-key wrapper/use in Groundhog; warns exporter from the *chat* group keys all group members, not only call participants.
- Packaging: voice needs GStreamer ≥ 1.20 only; CI/Flatpak checks cover voice-note elements, not webrtc/nice/dtls/srtp; FIPS native API in Flatpak needs host Unix-socket access beyond `--share=network`.
**Conclusion:** Needs verification (Phase 3 pair).

### Phase 3 — Pair investigator (session `D1E3FF49`)
**Hypothesis:** the oracle's eight code-level claims.
**Findings:** see `## Investigator Findings` §1–§8. Corrections to the oracle: rumor allowlist is 14/15/7/5/polls (not kind-14-only); 21059 is already allocated in `cascadia-nips`; `GhNetMode` has three modes (SYSTEM/NONE/TOR); Flatpak plugin inventory is *unknown*, not proven absent.
**Evidence spot-checked by the orchestrator:** `gh-nip17-envelope.c:385–390`, `gh-nip17-inbox.c:169–179, 226`, `gh-dm-inbox.c:604–606`, `nips/nip59/src/nip59.c:165`, `fips/docs/reference/nostr-events.md:176–190`, `cascadia-nips/registry/event-families.yaml:407–417`, `scripts/linux-ci.Dockerfile:23–25, 62`, `grep marmot_export_secret` (only `libmarmot/src/exporter.c:9`).
**Conclusion:** Confirmed.

### Phase 4 — Oracle synthesis (same chat, after selection refresh)
**Question:** decisive answers to Q1–Q3 plus phased plan.
**Findings:** RoQ "yes technically, no as default — reachability"; NIP-AC #2301 in 21059 for signalling; `webrtcbin`+libnice 1:1, SFU+SFrame/MLS-exporter groups; FIPS optional; Tor-mode calls disabled unless relay-only path proven. Folded into Analysis/Recommendations below.
**Conclusion:** Confirmed; investigation closed.

## Analysis (answers to the three questions)

### Q1 — Is RTP-over-QUIC (RoQ) a suitable choice?
**Technically viable; not suitable as Groundhog's first media path.** Biggest reason: **reachability** — RoQ is only an RTP↔QUIC *mapping*; it provides no discovery, no NAT traversal (RFC 9000 path validation is explicitly not NAT traversal), no TURN-equivalent fallback, no identity binding, and no signalling. Everything that makes a call *connect* still has to be built. Supporting facts:
- Spec status: `draft-ietf-avtcore-rtp-over-quic-14`, Experimental track, **expired Sep 2025**, WGLC without consensus (B1). No RFC.
- Encryption: QUIC TLS 1.3 only, terminated at whoever terminates QUIC — not E2E through any relay/SFU (B1).
- Toolchain: `quinnroqmux/demux` exist only in the Rust `quinn` plugin (gst-plugins-rs ≥ 0.14); not in Debian/Fedora, not bundled in Groundhog's Flatpak manifest (`packaging/flatpak/org.nostr.Groundhog.yml` bundles no GStreamer modules at all) nor in `scripts/linux-ci.Dockerfile:23–25` (B1, Investigator §5).
- Ecosystem: no deployed messenger uses RoQ for calls; Nostr rooms that moved to QUIC use **MoQ** (pub/sub streaming), not RoQ (B2).
- **RoQ/QUIC over the FIPS TUN** is more plausible (FIPS supplies reachability + identity) but stacks a second encrypted transport on top of Noise, requires both peers to run the FIPS daemon, and is undemonstrated anywhere in the workspace (metadesk uses plain TCP over the TUN, `metadesk/src/core/stream.c:438–463, 554–571`). Treat it as a later experiment, not a baseline.
- If RoQ is ever used for voice, use QUIC **DATAGRAMs** (RFC 9221), not streams — retransmitted stale audio is worse than loss concealment.

### Q2 — What is the "nostr-native" design?
Honest definition: **Nostr identity (npub) + encrypted, gift-wrapped, ephemeral call signalling delivered to the callee's kind-10050 inbox relays + optional Nostr-mediated discovery/NAT traversal (as FIPS does).** Audio never goes through relays: the only attempt (Coracle `nvid`) hit ~32 KB/s and ~1 s latency with no FEC (B2).

**Signalling protocol:** adopt a pinned revision of **NIP-AC (PR #2301)** — rumors kinds 25050–25054 (offer/answer/ICE/state) wrapped in ephemeral **21059** — because it is already shipped by Amethyst, Noscall and Nospeak (B2). A Marmot-group-scoped private scheme would be cleaner for MLS groups but Groundhog-only; note **White Noise has no calls feature at all** (B3), so "Marmot compatibility" buys nothing today. Reuse the Cascadia `NIP59_EPHEMERAL_GIFT_WRAP` 21059 allocation (`cascadia-nips/registry/event-families.yaml:407–417`).

What Groundhog can and cannot reuse for signalling (verified, Investigator §1–§4):
| Piece | Status | Evidence |
|---|---|---|
| Recipient inbox discovery (strict 10050 `relay` tags, no default-relay fallback) | **reuse verbatim** | `gh-inbox-lookup.c:259–281`, `gh-contact-directory.c:216–238`, `gh-inbox-resolver.h:12–20` |
| Relay scope: signed-event validation, dedup, subscriptions, NIP-42 policy | **reuse** | `gh-relay-scope.c:532–575` |
| Account-controller NIP-44 encrypt / sign seams, ephemeral keys | **reuse** | `gh-nip17-envelope.c:203–278` |
| NIP-59 wrap | **extend** — `nostr_nip59_wrap()` takes any inner kind but hard-codes outer kind 1059 (`nips/nip59/src/nip59.c:165`); add `nostr_nip59_wrap_kind()` + matching validator (`nip59.c:345–356` requires 1059) | |
| Envelope rumor allowlist | **not reusable** — `dm_kind()` = 14/15/7/5/polls only (`gh-nip17-envelope.c:385–390`) | |
| Durable DM outbox | **must not reuse** — persists via `gh_store_seal()`, reloads on start, republishes byte-for-byte for 72 h (`gh-outbox.c:13, 1060–1081, 1289–1305, 1838–1869`) → would replay stale offers/hangups | |
| DM inbox | **not reusable** — subscribes kind 1059 + `#p` only (`gh-dm-inbox.c:604–606`); `parse_signed(wrap_json, 1059, …)` rejects 21059 before any signer call (`gh-nip17-inbox.c:169–179, 226`) | |
| Kind constants | **absent** — no 21059/25050–25054/30312/30313 in `libnostr/include/nostr-kinds.h` (ephemeral block `:130–137`, addressable `:161–162`) | |

Freshness must come from authenticated call IDs + expiry inside the rumor, because seal/wrap `created_at` is randomised over a 2-day window (`nip59.c:97–113`). Relay `OK` ≠ ringing; EOSE ≠ delivery.

**Media path:**
- **1:1:** `webrtcbin` + `libnice` (ICE/STUN/TURN) + DTLS-SRTP + Opus/RTP; bind the DTLS fingerprint to the npub inside the signed/encrypted signalling. TURN does not terminate SRTP, so 1:1 stays E2E.
- **Groups:** SFU + **SFrame (RFC 9605)** with keys from `marmot_export_secret()` (RFC 9605 defines an MLS-exporter derivation). The API exists and is tested (`libmarmot/include/marmot/marmot.h:1484–1498`, `src/exporter.c:9–66`, tests `tests/test_exporter.c:68–117`) but has **zero Groundhog callers**; Groundhog does expose `gh_mls_group_get_group_id()/get_epoch()` and `gh_mls_service_lookup()` (`gh-mls-service.h:737–770, 826–827`). Caveat: the chat group's exporter secret is available to all group members, not only call participants — scope the call key (separate call group or explicit key distribution). P2P mesh only for small capped groups (each peer uploads N−1 streams).
- **FIPS media (optional experiment):** raw RTP/RTCP over the native datagram API (`npub:port`, unreliable/unordered, `max_payload` ≈ 1,247 B — B4). Needs a new AF_UNIX JSON + `SCM_RIGHTS` client (metadesk's `fips_control.c:714–746` uses plain `read()` and would drop the FD), a jitter buffer and rate adaptation, and the account↔node identity binding (FIPS authenticates the daemon's key).

### Q3 — QUIC vs FIPS, plainly
> **QUIC** is a transport protocol (RFC 9000/9001/9002/9221): it runs *inside your process* over UDP between two endpoints that can already reach each other, and gives you TLS 1.3 encryption, multiplexed reliable streams, optional unreliable datagrams, congestion control and loss recovery. It knows nothing about who the peer *is* (beyond a TLS cert), how to *find* them, or how to get through NAT.
>
> **FIPS** is a network, not a transport: a daemon that joins your machine to a self-organising encrypted mesh where every node's address *is* a Nostr public key. It handles discovery (Nostr kinds 37195/10050/21059, mDNS), routing, NAT hole-punching, and encryption (Noise IK per hop + Noise XK end-to-end), and hands applications either an IPv6 TUN (`<npub>.fips`) or a native, unreliable `npub:port` datagram socket. It has no streams, no congestion control, no loss recovery — those are the application's (or QUIC's) job.
>
> They **compose**: QUIC (or RoQ, or SSH, or anything IPv6) can run *over* the FIPS TUN; or RTP can sit *directly* on FIPS datagrams. Pick QUIC when you need transport features between reachable hosts; pick FIPS when the problem is *reaching an npub at all*. For a Flatpak desktop app, FIPS also means a root daemon on the host plus sandbox socket access — a deployment dependency WebRTC does not have.

### Eliminated / downgraded options
| Option | Why not (now) | Evidence |
|---|---|---|
| Audio frames as relay events | 32 KB/s, ~1 s latency, no FEC, no auth | B2 (`nvid`, nips issue #771) |
| RoQ over Internet as baseline | expired draft, no NAT traversal, unpackaged plugin | B1, Investigator §5 |
| MoQ | pub/sub live-streaming shape, relay-centric; fine for Nests-style rooms later, not 1:1 | B1, B2 |
| Reuse DM outbox/inbox for signalling | 72 h replay, 1059-only filters/guards | Investigator §1–§2 |
| Treat FIPS 10050 as compatible inbox list | FIPS emits `r` tags + own-relay fallback; Groundhog parses only `relay` | `fips/docs/reference/nostr-events.md:176–190` |
| Assume packaged WebRTC works | CI lacks `gstreamer1.0-nice`; Flatpak inventory unknown; `nicesrc`/`webrtcdsp` missing on dev host | Investigator §5 |

## Recommendations
1. **Packaging gate first** (`scripts/linux-ci.Dockerfile:23–25, 59–62`, `gnome/groundhog/packaging/flatpak/org.nostr.Groundhog.yml`, AppImage bead `nostrc-h3kh`): add `gstreamer1.0-nice` (and decide on `webrtcdsp`) to CI, inventory the `org.gnome.Platform//51` runtime, and extend the existing factory-check pattern (`gh-voice-recorder.c:427–445`, `gh-voice-player.c:422–441`) to `webrtcbin nicesrc nicesink dtlssrtpenc dtlssrtpdec srtpenc rtpopuspay rtpopusdepay`. Run a real loopback call in each packaged form before choosing anything else.
2. **Kinds**: add `NOSTR_KIND_EPHEMERAL_GIFT_WRAP 21059` and `NOSTR_KIND_CALL_*` 25050–25054 to the ephemeral block of `libnostr/include/nostr-kinds.h:130–137`; register 25050–25054 in `cascadia-nips/registry/event-families.yaml` next to the existing 21059 entry.
3. **NIP-59**: add `nostr_nip59_wrap_kind()` / `nostr_nip59_validate_gift_wrap_kind()` in `nips/nip59/src/nip59.c` (generalise lines 145–183 and 345–356) rather than mutating a signed wrap.
4. **New call-control service** `gnome/groundhog/src/app/gh-call-service.{c,h}` + `gh-call-signalling.{c,h}`: NIP-AC state machine (offer/answer/ICE/accept/reject/busy/hangup), call-id + expiry + replay rejection, its own **in-memory** send queue with bounded retries (not `gh-outbox`), its own 21059 + `#p` subscription via `GhRelayScope`, reusing `GhInboxResolver` for the callee's relays and the account-controller encrypt/sign seams from `gh-nip17-envelope.c`.
5. **Media** `gnome/groundhog/src/media/gh-call-media.{c,h}`: `webrtcbin` pipeline (Opus, echo control, device changes, adaptation); DTLS fingerprint carried in the signed rumor and checked against the npub. Keep `gh-voice-recorder/player` untouched — they are voice notes.
6. **Network policy (hard requirement)**: consume `GhNetMode` SYSTEM/NONE/TOR and `allow-local-network` (`gh-net-session.h:13–26, 100–121`, `gh-net-http.c:664–673, 847–888`). In **TOR mode disable calls** (or prove a relay-only, Tor-routed TURN path); never emit host/srflx ICE candidates when local-network consent is off; tear down media on mode change like `GhNetHttp` does.
7. **Groups (phase 2)**: `gnome/groundhog/src/mls/gh-call-keys.{c,h}` — a service-owned, serialised wrapper over `marmot_export_secret()` keyed by `(group_id, epoch, call_id)` with sender-key/counter/epoch-rotation rules per RFC 9605; SFU selection and SFrame integration after that.
8. **FIPS (optional, phase 3)**: `gnome/groundhog/src/net/gh-fips-client.{c,h}` implementing the JSON-over-AF_UNIX + `recvmsg`/`SCM_RIGHTS` flow per `fips/docs/how-to/write-a-native-api-client.md:19–29`; RTP straight onto the datagram FD; account↔node identity attestation in signalling. Gate on FIPS reaching a stable native-API surface (README: "off by default, surface may change").
9. Track this as a bead epic (none exists: `bd search "voice call"` → none) with the packaging gate as the first child.

## Decision update (2026-11-07, after owner feedback)
Owner direction: **drop FIPS and RoQ from scope; implement NIP-AC voice/video calling compatible with the other clients.** Two further probes pinned the exact wire format and the shipped implementations.

### NIP-AC as written (PR #2301, branch `vitorpamplona/nips:NIP-ac/AC.md`, commit `31c7a41`, 2026-04-05; still open, last discussion 2026-09-24)
- Kinds: **25050 offer, 25051 answer, 25052 ICE candidate, 25053 hangup, 25054 reject, 25055 renegotiate** (new SDP offer, answered with 25051). No separate "state" kind.
- Inner events are **signed by the sender** (not rumors). Required tags: `p` recipient, `call-id` (UUID), `alt`; offers add `call-type` = `voice` | `video`.
- Content: offer/answer = **raw SDP string**; ICE = JSON string `{"candidate":…,"sdpMid":…,"sdpMLineIndex":…}`; hangup/reject = empty or a reason, `"busy"` prescribed for auto-reject.
- Wrap: NIP-44-encrypt the signed inner event with a random ephemeral key into a **kind 21059** event with a `p` tag. **No kind-13 seal.** No `expiration` tag; no mandated timestamp randomisation — inner `created_at` drives staleness.
- Delivery: publish to the **recipient's relay list** (spec doesn't name 10050; Amethyst/Nospeak use 10050 inbox relays with hint fallback).
- ICE: **trickle**, `GATHER_CONTINUALLY`, buffer candidates that arrive before the remote description.
- Media: no codec, DTLS, or fingerprint-binding requirements — plain WebRTC/SDP. STUN/TURN: "default public STUN, default TURN fallback, user-configurable TURN".
- Groups: full mesh, per-peer offers wrapped to each target; answers wrapped to all members (doubling as "joined"); lower pubkey initiates callee↔callee links.
- **Open issue**: spec says discard signalling older than **20 s** while ringing timeout is **60 s**; author has agreed to relax but hasn't pushed. TURN discovery unresolved.

### Shipped implementations
| Client | Kinds / wrap | Sends to | Stack / codecs | Video | Group | Shipped |
|---|---|---|---|---|---|---|
| Amethyst | 25050–**25055** in 21059 (quartz `nipACWebRtcCalls`) | recipient 10050, hint fallback | Android libwebrtc (stream-webrtc 1.3.10), default codec factories | yes | yes, mesh | v1.09.0, 2026-05-15 |
| Nospeak | 25050–**25055** in 21059; kind 1405 call-history via NIP-17 | recipient 10050 (prefers connected); self-wraps answer/reject | browser `RTCPeerConnection` / Android stream-webrtc 1.3.8; H.264 high profile disabled | 1:1 only | voice mesh ≤ 4, extra tags `group-call-id`/`conversation-id`/`initiator`/`participants` | v1.1.0–1.1.2, May 2026 |
| Noscall | 25050–25054 in 21059 (**no 25055**) | **sender's general relays** (not 10050) | flutter_webrtc 0.12; rewrites H.264 `640c1f`→`42e032`; legacy `DtlsSrtpKeyAgreement` | yes | none found | v0.5.2, 2026-05-06 |
| 0xchat | **NIP-100**: all inner kind 25050 + `type` tag, JSON `{description:{sdp,type},session_id,media}`, outer **1059** + `expiration` now+60 s | sender general/outbox/circle relays | flutter_webrtc 0.12; same H.264 rewrite; ICE trickled with 1 s delay | yes | no | v1.2.1-beta, 2023-09-27 |

Default ICE servers in the wild: Amethyst — Google + Cloudflare STUN, TURN `openrelay.metered.ca` (static creds); Nospeak — `turn.data.haus:3478` (`free/free`) + Cloudflare STUN; Noscall/0xchat — hard-coded TURN IPs with `0xchat:Prettyvs511`. No client exchanges a DTLS fingerprint outside the SDP. **No documented cross-client call test exists** (only "implemented" announcements in the PR thread).

### Decision
1. **Target NIP-AC (25050–25055 / 21059) exactly as Amethyst's quartz implementation does** — the PR states it was written to follow Amethyst, and Amethyst + Nospeak are the two clients routing to 10050 like Groundhog already does. Treat 0xchat's NIP-100 as **out of scope for v1** (spec says explicitly not backward-compatible; separate wrap kind, JSON schema, relay routing).
2. **Receive**: subscribe `kinds:[21059] #p:[me]` on the account's 10050 inbox relays via `GhRelayScope`; to catch Noscall (which publishes to *its* relays) optionally also subscribe on NIP-65 read relays — document it as best-effort.
3. **Send**: recipient 10050 relays via `GhInboxResolver` (verbatim), NIP-65 read-relay fallback only if no 10050 exists (Amethyst behaviour).
4. **Envelope**: `nostr_nip59_wrap_kind(inner_signed_event, recipient, NOSTR_KIND_EPHEMERAL_GIFT_WRAP)` — no seal, so `gh-nip17-envelope`'s seal path is *not* involved; only the account-controller NIP-44 + ephemeral-key signing seams are reused.
5. **Freshness**: accept inner `created_at` within ±60 s (be liberal; the 20 s rule is being relaxed), dedupe by (`call-id`, kind, event id), ignore events for unknown/ended `call-id`s, auto-`busy` on concurrent offer.
6. **Media**: GStreamer `webrtcbin` + libnice; Opus audio; **VP8 first, H.264 constrained-baseline (`42e01f`/`42e032`) second** for video so SDP survives Noscall/0xchat rewrites and Nospeak's high-profile ban; trickle ICE with candidate buffering; `bundle-policy=max-bundle`; DTLS-SRTP default.
7. **ICE servers**: ship Cloudflare/Google STUN defaults + a user-configurable TURN preference (Preferences → Calls); do not hard-code third-party static TURN credentials in a privacy-focused client — provide a field and, optionally, the `turn.data.haus` free server as an opt-in preset.
8. **Tor mode**: calls unavailable (ICE would leak); SYSTEM/NONE: respect `allow-local-network` by filtering host candidates when off.
9. **Interop test plan** (release gate): scripted Groundhog↔Amethyst and Groundhog↔Nospeak voice + video calls, both directions, direct and forced-TURN; log SDP both ways.

## Preventive Measures
- Keep "voice notes" and "calls" as distinct features/targets in CMake and docs so the async Ogg/Opus path is never mistaken for a live-call pipeline.
- Any new event kind must land in `nostr-kinds.h` **and** the Cascadia registry, with a dispatcher that owns its filter — range classification alone (`nostr-kinds.h:247–248`) is not support.
- Transient protocols (calls, presence) must never flow through `gh-outbox`/`gh-store`; document that boundary in `gh-outbox.h`.
- Every network-facing subsystem must subscribe to `GhNetSession::changed` and fail closed in TOR mode, following the `GhNetHttp` precedent.
- Packaging acceptance = executing the real pipeline inside the Flatpak/AppImage, not pkg-config or `gst-inspect` on a dev host.
