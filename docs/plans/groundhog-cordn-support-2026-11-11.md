# Groundhog Cordn Support: Plan

Status: plan only (no code) · Date: 2026-11-11 · Target: Groundhog (`gnome/groundhog`) plus new/extracted lower-layer libraries · Beads: none yet — create one per phase in §9 before work starts

## 1. Goal

Add Cordn (coordinator-assisted MLS over Nostr/ContextVM, https://github.com/Cordn-msg/cordn) as Groundhog's fourth conversation backend (`GH_CONVERSATION_BACKEND_CORDN = 4`) without changing the identity or behavior of existing backends. Implement each protocol concern in the lowest viable layer — `libmls` (extracted RFC 9420 engine), `libcontextvm` (generic MCP-over-Nostr), `libcordn` (Cordn protocol), `nostr-gobject` async wrappers — so Groundhog owns only account wiring, encrypted persistence, application policy, models and GTK UI.

## 2. Owner decisions (fixed, from the up-front interview)

1. **MLS engine:** extract `libmarmot/src/mls/` into a standalone `libmls` with a public RFC 9420 API used by both `libmarmot` and `libcordn`. Accept the libmarmot refactor and version bump. Existing persisted MLSG bytes stay byte-compatible.
2. **ContextVM:** build a generic, protocol-neutral ContextVM client+server library (core C + `nostr-gobject` async wrapper) **and** migrate Signet's management transport onto it.
3. **Scope:** core Cordn plus **all** optional features — `0xC04D` group metadata; invitations, join requests and `cordn1…` group refs; encrypted media via Blossom (`cordn-em-v1`); edits/deletes/pins/polls (kinds 1010/5/1011/1068/1018); multi-device sync (kinds 30078/178).
4. **Coordinator:** Groundhog is a **client only** of existing coordinators (TS `@cordn/server`, Rust `cordn-rs`); no coordinator implementation. **Gift-wrapped client↔coordinator transport by default.**

## 3. Summary

The work is a bounded foundational refactor (extract RFC 9420 machinery into `libmls`, keeping Marmot policy and byte-compatible MLSG persistence in `libmarmot` behind an explicit policy hook), followed by new protocol-neutral `libcontextvm`, protocol-specific `libcordn`, async GObject wrappers, Signet's dual-wire migration, and Groundhog storage/service/UI integration. Fifteen phases (§9), each landing independently with existing Marmot/Signet tests green.

This document was produced from curated code/spec reconnaissance and a planning pass; **no builds, tests, protocol audits or interoperability runs were executed**. File:line references were spot-checked on 2026-11-11 where marked "verified"; others come from explore-agent reads and must be re-checked in Phase 1.

---

## 4. Background

### 4.1 Cordn protocol (external; all specs Draft)

- **Architecture:** clients perform MLS; a self-hostable **coordinator** stores KeyPackages, Welcomes, join requests and per-group ordered message streams. It never decrypts and does not adjudicate Commits (first accepted post = lower cursor = stream order). Clients reach it through ContextVM over ordinary relays; it connects outbound (works behind NAT). One active coordinator per group. [why-cordn](https://github.com/Cordn-msg/cordn-web/blob/master/docs/why-cordn.md)
- **Reference versions** inspected 2026-11-11: repo 0.5.1, server 0.5.8, CLI 0.8.0, web 0.6.1, cordn-rs 0.5.1. MIT for core/CLI/web/rs; **no license declaration found for the TS coordinator/server package**.
- **RPC methods** ([contracts.ts](https://github.com/Cordn-msg/cordn/blob/master/packages/core/src/contracts.ts), [handlers](https://github.com/Cordn-msg/cordn/blob/master/packages/server/src/coordinatorMethods.ts)) — 11 tools; there is no single `join_request_take`:

  | Method | Input → result |
  |---|---|
  | `kp_publish` | `{kp_ref,kp_64}` → `{kp_ref,last_resort,at}` |
  | `kp_list` | `{}` → `{keyPackages:[{pk,kp_ref,last_resort,at}]}` |
  | `kp_take` | `{id}` → `{keyPackage: null \| {pk,kp_ref,last_resort,at,event:NostrEvent}}` |
  | `kp_remove` | `{kp_refs:[…]}` (non-empty) → `{kp_refs}` |
  | `welcome_store` | `{target_pk,kp_ref,welcome_64,after?}` → `{at}` |
  | `welcome_take` | `{consumed?:[{kp_ref,at}]}` → `{welcomes:[{kp_ref,welcome_64,at,after?}]}` |
  | `join_request_store` | `{gid,kp_ref}` → `{at}` |
  | `join_request_take_many` | `{groups:[{gid}],consumed?:[{gid,pk,at}]}` → `{requests:[{gid,pk,kp_ref,at}]}` |
  | `msg_post` | `{msg_64,gid}` → `{cursor,gid,at}` |
  | `msg_fetch_many` | `{groups:[{gid,after?}]}` → `{messages:[{cursor,gid,msg_64,at}]}` (input-group order, then ascending cursor) |
  | `msg_sub_many` | same input as fetch → `{subscribed:true,groups}` + streamed message objects (CEP-41) |

  `NostrEvent` = `{id,pubkey,created_at,kind,tags,content,sig}`. String params are non-empty; `after` is a positive integer. Cursors: per-group, monotonic from 1, `after` exclusive (`cursor > after`), omission = from start, **not message IDs**. Coordinator `at` is **milliseconds**. `msg_sub_many` registers before replay, buffers live arrivals during replay, suppresses already-replayed cursors. Acks (`consumed`) retire Welcomes/join requests before the fetch; observation alone does not delete. **No error-code enum** — text errors (`Rate limit exceeded`, `Key package quota exceeded`, `Invalid kp_64`, `Missing publication event`, identity mismatch, `Unknown key package ref`, `Unauthorized key package ref`, `Invalid welcome_64`, `Invalid msg_64`, `Too many pending join requests for this group`). Server defaults ([runtimeConfig](https://github.com/Cordn-msg/cordn/blob/master/packages/server/src/runtimeConfig.ts)): rate bucket 160 burst / 500 per min / 1 h idle expiry; 50 KPs per identity; 1 last-resort (republish replaces); 100 pending join requests per group; Welcome/join-request expiry 30 days with 6 h cleanup. No max array/payload sizes in schemas.
- **Identity/KeyPackages** ([spec 00](https://github.com/Cordn-msg/cordn/blob/master/spec/00.md)): MLS `BasicCredential` identity = lowercase 64-char hex pubkey as **UTF-8 text** (reference convention; spec leaves encoding unsettled — [mlsIdentity.ts](https://github.com/Cordn-msg/cordn/blob/master/packages/cli/src/utils/mlsIdentity.ts)). `kp_ref` = hex `MakeKeyPackageRef`; `kp_64` = base64 KeyPackage. The KP "publication" is the **signed ContextVM `kp_publish` request event itself** (no dedicated kind); its `content` JSON holds `params.arguments.kp_64` (legacy alias `keyPackageBase64`). `kp_take` returns that event; clients re-verify signature, decode the KP and compare credential identity to signer ([publishedKeyPackage.ts](https://github.com/Cordn-msg/cordn/blob/master/packages/cli/src/utils/publishedKeyPackage.ts)). Last-resort KPs carry the MLS last_resort extension; identity lookup prefers the oldest ordinary KP, then the retained last-resort.
- **gid:** opaque and distinct from MLS `group_id` in the spec; the reference client uses UTF-8 `randomUUID()` bytes as MLS group ID and the same string as `gid`.
- **Envelope** ([spec 02](https://github.com/Cordn-msg/cordn/blob/master/spec/02.md)): UTF-8 JSON `{id,pubkey,created_at,kind,tags,content}` without `sig`; no separate canonicalization beyond NIP-01 id; receiver recomputes the NIP-01 id over `[0,pubkey,created_at,kind,tags,content]` and requires `pubkey` == MLS-authenticated sender. Kinds 9 chat, 1111 NIP-22 replies (root `E/K/P`, parent `e/k/p`; preferred over NIP-C7 `q`), 7 reactions (`e`, normally `p`, optional `k`). Routing uses RPC `gid`, not an `h` tag.
- **Sealing** ([spec 03](https://github.com/Cordn-msg/cordn/blob/master/spec/03.md), [web codec](https://github.com/Cordn-msg/cordn-web/blob/master/src/lib/services/chatGroupPayloadCrypto.ts)): serialized MLS message sealed with ChaCha20-Poly1305; key = MLS exporter(label `"cordn"`, context UTF-8 `"group-payload"`, 32 bytes); empty AAD; random unique 12-byte nonce; wire = padded std base64 of `nonce ‖ ciphertext ‖ 16-byte tag` (≥ 28 decoded bytes); no extra plaintext padding. Application messages use the current epoch's key; **Commits use the epoch they transition from**. Reference suite `MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519` (0x0001) ([mlsBase.ts](https://github.com/Cordn-msg/cordn/blob/master/packages/cli/src/utils/mlsBase.ts)).
- **Metadata extension `0xC04D`** ([spec 01](https://github.com/Cordn-msg/cordn/blob/master/spec/01.md)): `uint16 version` (=1; 0 rejected) then five length-prefixed opaque fields in order — Name, Description, AdminPubkeys (concatenated raw 32-byte keys, multiple of 32, no duplicates), Icon, ImageUrl; UTF-8 text fields. Empty admin list = egalitarian mode. A group using it requires all members to advertise support. The scout reported 16-bit length prefixes; **lock the exact prefix encoding against the reference fixture** ([groupMetadata.test.ts](https://github.com/Cordn-msg/cordn/blob/master/packages/cli/src/groupMetadata.test.ts)) — see §6.4.
- **Group ref `cordn1…`** ([group-ref](https://github.com/Cordn-msg/cordn/blob/master/spec/applications/group-ref.md), [groupRef.test.ts](https://github.com/Cordn-msg/cordn/blob/master/packages/core/src/groupRef.test.ts)): bech32 (not bech32m), TLV `type(1)|len(1)|value`: 0 = gid UTF-8 (exactly one, ≤ 255 B); 1 = raw 32-byte coordinator pubkey (0 or 1); 2 = relay URL (repeatable, requires type 1). Unknown TLVs ignored; descending TLV order recommended; encoded string ≤ 5000 chars.
- **Join requests / Welcome delivery** ([join-requests](https://github.com/Cordn-msg/cordn/blob/master/spec/applications/join-requests.md), [welcome-delivery](https://github.com/Cordn-msg/cordn/blob/master/spec/applications/welcome-delivery.md)): requester must own an existing coordinator KP (last-resort allowed); group existence not checked; one pending request per (group, requester), re-request refreshes KP ref/time. Welcome `after` = admitting Commit cursor; invitee fetches immediately after it. Consumed refs: Welcome `(kp_ref, at)`, join request `(gid, pk, at)`.
- **Encrypted media** ([spec](https://github.com/Cordn-msg/cordn/blob/master/spec/applications/encrypted-media.md)): exporter(`"cordn"`, `"encrypted-media"`, 32); ChaCha20-Poly1305; AAD = `utf8(m) ‖ 0x00 ‖ utf8(filename) ‖ 0x00 ‖ sha256(plaintext)` (exact bytes, no normalization); random 12-byte nonce as hex `imeta n` (not prepended); stored blob = ciphertext+tag. Required `imeta` fields `url,m,filename,x,n,v=cordn-em-v1`; optional `dim,blurhash,thumbhash,alt`. Blossom upload authorized by kind 24242 ([chatBlossomClient.ts](https://github.com/Cordn-msg/cordn-web/blob/master/src/lib/services/chatBlossomClient.ts)).
- **Multi-device** ([spec](https://github.com/Cordn-msg/cordn/blob/master/spec/applications/multi-device.md)): devices **share one MLS leaf/ClientState per group**. Group docs carry base64 library-specific `clientState` + matching cursor (metadata already in state); meta doc carries tombstones + optional last-resort KP (public and private). Outer kind **30078** replaceable event signed by an independent ephemeral key, stable random `d`, content NIP-44 sealed to owner; inner owner-signed kind **178** with `x` tags (doc hashes), `dek` (64-hex private DEK), ordered `server` tags. DEK fresh per identity and reused; docs NIP-44 v2 self-sealed; address = SHA-256 of the sealed blob. Same-leaf Commit race precedence: lower stream cursor → authenticated third-party post-fork message → lower commit-point cursor via `prev` chain → higher live-document cursor → lexicographically greater address; the loser discards its staged transition and associated Welcomes.
- **Web-client kinds** ([kinds.ts](https://github.com/Cordn-msg/cordn-web/blob/master/src/lib/chat/kinds.ts), [references.ts](https://github.com/Cordn-msg/cordn-web/blob/master/src/lib/chat/references.ts), [polls.ts](https://github.com/Cordn-msg/cordn-web/blob/master/src/lib/chat/polls.ts)): 1010 edit (trimmed replacement content; `e/p/k`); 5 deletion (empty; `e/k`); 1011 pin (empty; `e/p/k`, `op=add|remove`); 1068 poll (question content; indexed `option` tags; `polltype=singlechoice|multiplechoice`; optional `endsAt`; `alt`); 1018 vote (readable fallback content; `e/p/k`; one `response` per selected option; `alt`). The protocol has no typing indicators or transmitted read receipts (web read state is local).
- **Spec ambiguities this plan works around:** credential encoding unsettled (follow hex-UTF-8 reference); gid derivation client-defined; suite not pinned by core text; spec 03 "no previous-epoch sealing keys retained" vs multi-device §14 "retention parity" contradiction (§6.5); README examples predate the 0.5.0 multi-group calls; multi-device text still mentions mixed payload modes; coordinator migration outside baseline; **no cross-client conformance vectors** (only TS unit tests).

### 4.2 ContextVM (external)

- **Base** ([draft spec](https://contextvm.github.io/contextvm-docs/reference/spec/ctxvm-draft-spec/), Draft): signed kind **25910**, `content` = stringified MCP JSON-RPC; `p` tag = peer; response `e` tag = request's **Nostr event id**; JSON-RPC `id` also pairs request/response. Notifications have no id and get no response. Legacy flow: `initialize` → response → client MUST send `notifications/initialized`; servers may also be stateless ([CEP-35](https://contextvm.github.io/contextvm-docs/reference/ceps/informational/cep-35/)). Docs show `protocolVersion "2025-07-02"`; MCP 2026-07-28 drops `initialize` (per-request `_meta`, optional `server/discover`) and ContextVM does not specify that mapping.
- **Encryption** [CEP-4](https://contextvm.github.io/contextvm-docs/reference/ceps/cep-4/) (Final): serialize the **signed inner 25910 event**, NIP-44 encrypt to recipient, place in kind **1059** addressed by `p` — **no kind-13 seal**; server responses use the same pattern. `support_encryption` advertised in the init response or announcement; encrypted init attempts allowed. [CEP-19](https://contextvm.github.io/contextvm-docs/reference/ceps/cep-19/): ephemeral kind **21059** with `support_encryption_ephemeral`; prefer 21059 when both sides support it, else 1059.
- **[CEP-22](https://contextvm.github.io/contextvm-docs/reference/ceps/cep-22/) oversized transfer** (Draft): requires a request `progressToken`; frames are `notifications/progress` with `params.cvm.type:"oversized-transfer"`, `frameType` start/accept/chunk/end/abort; `start` carries `completionMode:"render"`, `digest` (SHA-256 of the UTF-8 serialized message), `totalBytes`, `totalChunks`; `chunk.data` is a fragment of the exact serialized JSON-RPC message; reassemble by `progress` order; `end` required, `abort` terminal; never expose partial content. No protocol cap; ~64 KiB relay events are the practical ceiling. TS SDK: 48,000-byte threshold/chunk, 100 MiB receive cap, enabled by default ([guide](https://contextvm.github.io/contextvm-docs/reference/ts-sdk/transports/oversized-transfer/)).
- **[CEP-41](https://contextvm.github.io/contextvm-docs/reference/ceps/cep-41/) open streams:** `cvm.type:"open-stream"`, stream id = request `progressToken`; frames start/accept/chunk(`data`, contiguous `chunkIndex` from 0)/ping/pong/close/abort; wait for `accept` when receiver support is unknown; `close` does not replace the final JSON-RPC response; `abort` terminal; no replay/resume or stream-id reuse. TS SDK requires explicit enablement on both transports ([guide](https://contextvm.github.io/contextvm-docs/reference/ts-sdk/transports/open-stream/)).
- **Discovery** [CEP-6](https://contextvm.github.io/contextvm-docs/reference/ceps/cep-6/) (Final): replaceable 11316 (server metadata: `protocolVersion`, `serverInfo`; tags `name/about/picture/website/support_encryption`), 11317 (JSON tools list), range 11316–11320; relays via NIP-65 10002 with `r` tags ([CEP-17](https://contextvm.github.io/contextvm-docs/reference/ceps/cep-17/), Draft). All optional for direct connections. Minimal exchange = signed 25910 + peer addressing/correlation + known relays/keys.

### 4.3 In-repo seams

**libmarmot MLS engine (extraction source)** — paths under `libmarmot/src/mls/`:
- Ten `.c` files compiled straight into `marmot`; no separate target. Public link closure includes OpenSSL, sodium, secp256k1, nostr/NIP-44, optional SQLite/LMDB (`libmarmot/CMakeLists.txt:80–134,182–208`). Crypto: libsodium (X25519/Ed25519/random/memzero) + OpenSSL EVP (SHA-256/HMAC/AES-128-GCM) (`mls_crypto.c:10–20,27–60,257–345,493–505,736–754`). No libnostr/NIP/secp256k1 includes in `mls/`.
- **Marmot coupling:** `mls-internal.h:13–19` includes `marmot-types.h`/`marmot-error.h`/visibility; `MARMOT_ERR_*` returns and `MARMOT_CIPHERSUITE` (`mls_key_package.c:339–372`); default capabilities hardcode `0xF2EE` group data, app-data dictionary, SelfRemove; adopted add AppDataUpdate/agent-stream receive (`mls_tree.c:1347–1422`); KP creation installs Marmot lifetime policy (`mls_key_package.c:169–183`); `MlsGroup` embeds `MarmotGroupProfile` (`mls_group.h:231–239`); adopted admission requires 32-byte Basic identities + 104-byte account proof, admins must be members (`mls_app_components.c:768–890`); legacy creation accepts caller identity bytes (`mls_group.h:290–307`); engine calls `marmot_group_*_{decode,clear}` and `marmot_agent_text_stream_policy_decode()` (`mls_app_components.c:592–640`); AppDataUpdate account-based admin auth (`mls_app_data_update.c:80–162`); legacy markers `0xF2EE/0xF2F1`, roles `0xF2D1/2/4` (`mls_app_components.h:31–64`). `MlsSecretTree` is not thread-safe; ratchet state includes consumed generations and bounded skipped-key caches.
- **API:** `mls-internal.h:25–331` (fixed-size crypto/HPKE + `MlsTlsBuf`/`MlsTlsReader`); `mls_group.h:175–700` (create, add/replace/remove, self-update, extension commits, proposals, by-reference commits, PSKs, process, app encrypt/decrypt; `mls_group.c:4997–5165`); KeyPackage `mls_key_package.c:132–417`; Welcome `mls_welcome.c:389–452`; exporter `mls_key_schedule.c:773–849`; framing `mls_framing.c:761–1277`; app-data update `mls_app_data_update.h:24–90`. Only MLS 1.0, suite 0x0001, Basic credentials (`mls-internal.h:4–40`). Admission is profile-constrained (`mls_group.c:3214–3302`). Test-hook exports via `MARMOT_TEST_API` (`libmarmot/src/marmot-internal-visibility.h:4–12`). TLS `opaque8/16/32` helpers use **VLI (QUIC-varint) prefixes**, not fixed 16-bit (verified `mls-internal.h:270–282`).
- **Serialization:** MLS owns private `MLSG` format v3 (legacy) / v4 (adopted): trees, private keys, retained epoch secrets, transcript hashes, extensions, caches, sender ratchets, profile (`mls_group.c:5901–6045`). Marmot persists through `MarmotStorage.mls_store/mls_load` keyed `"mls_group"` + group ID (`libmarmot/src/messages.c:217–299`, `groups.c:829–855`).
- **Tests:** direct MLS tests (TLS, crypto, tree, schedule, framing, KP, group, Welcome, AppDataUpdate, vectors) all link `marmot` (`libmarmot/CMakeLists.txt:270–354,425–455`); MDK vectors `libmarmot/tests/vectors/mdk` (non-suite-1 skippable, `tests/mdk_vector_stubs.c:235–239`); ASan lanes `tests/selection/inventory.json:732–808`.
- **Consumers:** `marmot-gobject/CMakeLists.txt:97–108`; `libvault-marmot/CMakeLists.txt:38`; Groundhog (`gnome/groundhog/CMakeLists.txt:1267,2272–2287`; tests include engine internals `#include "mls/mls_group.h"`, `"mls/mls_key_package.h"` — verified `gnome/groundhog/tests/mls/mls-forge.h:14–15`); `apps/gnostr/plugins/mls-groups/CMakeLists.txt:88–95`, `apps/gnostr/CMakeLists.txt:1015–1016`.

**Signet "ContextVM" (migration target)**
- Constants `CAS_INTENT=25910`, `NIP59_GIFT_WRAP=1059` (`signet/include/signet/cascadia.h:24,51`); handler entry points `signet/include/signet/mgmt_protocol.h:427–468`.
- **Not CEP-4 compatible:** `signetctl_build_intent()` builds JSON-RPC with json-glib (`signet/src/signetctl_main.c:197–288`) then calls `nostr_nip17_wrap_dm()` → kind-14 rumor / kind-13 seal / 1059 (`signetctl_main.c:305–316`; `nips/nip17/src/nip17.c:298–325`). Correlation = JSON-RPC string id + authenticated bunker sender (`signetctl_main.c:464–527`), not `e`. Daemon subscribes 24133/25910/1059 by bunker `#p`, 49 h wrap backfill (`signet/src/signetd_main.c:52–78`); relay middleware verifies signatures (`signet/src/relay_pool.c:125–163`); live/EOSE boundary + authenticated rumor-time skew (`mgmt_protocol.c:1452–1471`); unwrap `nostr_nip17_decrypt_dm_ex()` (`nips/nip17/src/nip17.c:490–546`); dispatch (`mgmt_protocol.c:1392–1441`); method table (`:611–630`); request copy (`:632–735`); reply normalization + wrap (`:1090–1135,900–946`; legacy-ack→JSON-RPC `:467–503`); legacy 28090 ACK uses `e` (`:987–1027`). Signet policy (stays in Signet): provisioners, self-service reissue, delivery grants, deny lists, rate limits (`:1062–1087,1271–1346,1805–1827`); replay caches per sender class keyed by delivered event id, refuse-not-evict (`:1989–2047`; enabled `signetd_main.c:946–959`). Wire contract `signet/INTEGRATION.md:197` (direct 25910 only for read-only methods; mutations require 1059; timeouts are ambiguous; ≥ 270 s stop before restart).
- Go SDK: `MgmtClient` publishes raw JSON and returns without receive/verify/correlate (`signet/sdks/go/signetclient/contextvm.go:8–45`); `signet/sdks/go/cmd/signet-mcp/main.go:15–46` is scaffolding. Prior investigation: `docs/investigations/signet-mcp-server-2026-08-19.md`.
- Tests: `signet/tests/test_mgmt_replay.c:634–639`, `test_relay_pool_protocol.c:698–746`, `test_signetctl_intent.c:1–44`; registered `signet/tests/CMakeLists.txt:272–305,355–363` and `signet/tests/meson.build:19,342–360`; run with `ctest --test-dir <build> -R '<name>'`.

**Low-level building blocks**
- NIP-44 `nips/nip44/include/nostr/nip44/nip44.h:23–55`. NIP-59 `nostr_nip59_wrap()` wraps **any** (signed or unsigned) event directly in a kind-1059 wrap with no seal, randomized timestamp, ephemeral key (verified `nips/nip59/include/nostr/nip59/nip59.h:71–97`) — exactly CEP-4's shape for 1059; `nostr_nip59_unwrap()` requires the recipient secret key hex (`:112+`), unusable with remote signers. NIP-17 `nips/nip17/include/nostr/nip17/nip17.h:24–142`; NIP-19 bech32/TLV `nips/nip19/include/nostr/nip19/nip19.h`; `nips/nip22`, `nips/nip25`; BUD-02 `nips/bud02` → `libblossom` → `nostr-gobject/src/gn-blossom-client.c:145–157`.
- libnostr relay publish/OK/AUTH `libnostr/include/nostr-relay.h:130–181,498–528`; subscriptions EVENT/EOSE/CLOSED `libnostr/include/nostr-subscription.h:19–74`; `NostrSimplePool` dedup/middleware `libnostr/include/nostr-simple-pool.h:220–293`. nostr-gobject `GNostrRelay` signals `event-received/eose/ok/closed/auth-challenge` (`nostr-gobject/src/nostr_relay.c:762–872`); `GNostrPool` (`nostr-gobject/include/nostr-gobject-1.0/nostr_pool.h:21–40,234–257`).
- JSON: core C uses `libjson` (Jansson, `libjson/CMakeLists.txt:4–24`); GObject code uses json-glib/`GNostrJsonBuilder` (`nostr-gobject/src/nostr_json.c:4–26`); `nson` is event serialization only.
- Build: `nostrc_link_archives()` (verified root `CMakeLists.txt:459`); `nostr_gobject_executable` INTERFACE target / `nostrc::gobject_executable` (verified `nostr-gobject/CMakeLists.txt:317–319`); nostr-gobject source lists `:105–175,272–279`, GIR `:427–490`; NIP package discovery `nips/CMakeLists.txt:3–12`, toggles `NipOptions.cmake` (root `CMakeLists.txt:404–421`), scaffold `nips/create_nip.sh:23–62`, exemplar `nips/nip59/CMakeLists.txt:8–92`.

**Groundhog MLS backend (template)** — paths under `gnome/groundhog/`:
- `GhConversationBackend {NIP17=1,NIP29=2,MLS=3}` (`src/app/gh-conversation.h:9–13`, GType `gh-conversation.c:109–116`); `gh_conversation_new_mls` / `set_mls_peers` (`src/app/gh-conversation-private.h:22,39`); backend delegate `gh_conversation_store_set_backend_delegate` (`:165`, used by `gh-store-mls.c:505`, `gh-store-nip29.c:744`); backend inferred from message `gh-conversation-store.c:154–156`.
- **Every backend switch/branch site:** `src/app/gh-app-outbox.c:432,475,485`; `gh-app-services.c:814,857–937,2200–2230`; `gh-attachment-ui.c:161,191,203,660`; `gh-contact-directory.c:1153`; `gh-contact-titles.c:18–20`; `gh-notifier.c:460`; `gh-send-ui.c:123,435,448,545,548`; `src/mls/gh-mls-invitee.c:166`; MLS-only guard `gh-conversation.c:1268`; `is_direct` `:173,1335`.
- **Service:** `gh_mls_service_new(GhMlsServiceConfig*)` (`src/mls/gh-mls-service.h:655–686`, `.c:9667–9770`): a `GListModel` of `GhMlsGroup` with `invite-received/group-added/invites-changed`; no explicit start/stop — `update_activity()` `.c:9534` gates on account-active + online (`on_accounts_changed :9568`, `on_network_changed :9578`, `resume_all :9406`). KP publish `key_package_maybe_publish()` `.c:8782`; subscriptions `group_subscribe()` `.c:3496` via `gh_relay_scope_new` with backfill paging and `GH_AUTH_PURPOSE_MLS_ROUTING`; receive `process_event()` `.c:2720` inside `gh_store_begin/commit`; send `send_inner()` `.c:5626` (inner event → save room → `gh_store_enqueue` → create → save → `gh_store_seal` role `GH_STORE_OUTBOX_ROLE_MLS_MESSAGE` → commit → send); outcomes `delivery_update/done` `.c:5397–5452`; restart `send_resume()` `.c:6080` over `gh_store_outbox_list_unfinished(GH_STORE_BACKEND_MLS)`; retry `.c:3602,3613`; reactions/deletion `.c:5912,5957`; Welcome sink `.c:6530`; invites `.c:6603–6787`. Instantiated by `gh-app-outbox.c:bind_mls()` `:217` (after `bind_reactions`/`bind_nip29`, `:569–571`; config `:226–235`; `unbind_mls :255`; getter `:266`).
- **Store:** append-only `migrations[]` (`src/store/gh-store-schema.c:430–442`); `GH_STORE_SCHEMA_VERSION 10` and `GhStoreMigration {version, description, sql}` with **no flags field** (verified `src/store/gh-store.h:818–831`); runner `store_migrate()` wraps **each migration in its own `store_begin` (BEGIN IMMEDIATE)** transaction, records `schema_migrations`, sets `user_version` (verified `src/store/gh-store.c:1378–1416`); `PRAGMA foreign_keys=ON` at connection configure (verified `gh-store.c:1305,1319`); newer `user_version` refused with `GH_STORE_ERROR_NEWER_SCHEMA` (verified `gh-store.c:1537–1543`). `conversations.backend CHECK IN (1,2,3)` (`gh-store-schema.c:25–40`); tables referencing `conversations(id)` (verified `gh-store-schema.c:43,50,75,128,143,345,379`); triggers `messages_admit_seq` and `messages_keep_read_marker` update `conversations` by name (verified `gh-store-schema.c:309–320`). Enums `GH_STORE_BACKEND_*` (`gh-store.h:295–297`), `GH_STORE_SEEN_*` (`:301–313`), `GH_STORE_OUTBOX_ROLE_*` (`:340–344`). MLS tables v1/v2/v5/v6 (`gh-store-schema.c:142–344`). `MarmotStorage` adapter `src/store/gh-store-marmot.{h,c}` (txn/savepoint semantics `.h:24–54`); Marmot room layer `src/store/gh-store-mls.{h,c}` (room id `"mls:"+hex`, backend 3).
- **UI:** `src/ui/gh-mls-ui.{h,c}` (`gh_mls_ui_attach_if_enabled :619`, `gh_mls_ui_attach :676–724`, delegate table `:422`, send delegate `:713`) wired in `src/app/gh-app-services.c:2548–2578`; pages `gh-mls-new-group-page.c`, `gh-mls-invites-page.c`, `gh-mls-invitee-picker.c`, `gh-mls-group-info-dialog.c`; blueprints `data/ui/gh-mls-*.blp`; list badge `gh-conversation-row.c:296–310`; room key `gh-conversation-list.c:238` (`"%d:%s"`); delete wording `gh-conversation-view.c:3036–3046`; privacy summary `gh-conversation-info-dialog.c:134–136,909–942`; message rendering owner `src/ui/gh-message-row.{h,c}` (verified); preferences `src/ui/gh-preferences-dialog.{h,c}` (verified). Resources `data/groundhog.gresource.xml`; translations `po/POTFILES` (both verified). `GhSendUiDelegate` `src/app/gh-send-ui.h:84–110` (`set_delegate`/`add_delegate`).
- **Media:** `src/mls/gh-mls-media.h:6–215` (read → seal → Blossom upload → epoch check in send txn, enforced `gh-mls-service.c:5700–5712`); `gh-mls-attachments.h:71–116`; shared `src/media/gh-blossom-client.c`, `gh-attachments.c`. Replies via `gh_message_get_reply_to_id` (`src/app/gh-message.h:128`); **no edit API in any backend**. Marmot KP discovery is kind 30443 (verified `src/mls/gh-mls-key-packages.h:9`).
- **Tests:** CMake `CMakeLists.txt:1277–1285,2305–2327,2994–3010`; `TestInventory.cmake:99–110,163–179,232`; `tests/mls/test_mls_service.c` (63 cases), `test_mls_media.c`, `test_mls_files.c`, `test_mls_kp_lifecycle.c`, `test_mdk_interop.c`, `tests/store/test_store_marmot.c`, `tests/ui/test_mls_ui.c`; harness `tests/mls/mls-world.h` (≤ 3 in-process accounts, local relays from `tests/relay/wire-relay.h`, mock signer on private bus `gh-test-signer.h`, no sleeps), `mls-forge.h`, `mdk-peer.h`.

### 4.4 Prior art and house rules

- `docs/MLS_INTEGRATION.md`, `docs/MARMOT_API.md` (libmarmot; MLS_INTEGRATION describes an older gnostr plugin and older Marmot conventions — it does not override current Groundhog interfaces or the current kind-30443 KP behavior); `docs/plans/groundhog-gnome-messaging-2026-09-25.md` (Groundhog protocol-flow authority); `docs/plans/groundhog-nip46-remote-signer-2026-10-07.md` (house style); `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md` (MLS state in encrypted store; per-protocol privacy boundaries); `docs/plans/nip-29-gnostr-plugin-2026-05-18.md` (protocol normalization in lower libs, app owns UI/subscriptions/persistence; rejected a shared group-provider abstraction because authority/state models differ).
- `ARCHITECTURE.md`: libnostr → nips → nostr-gobject → apps; libmarmot is a separate protocol library with `marmot-gobject`; GObject async = `GTask` async/finish. `CODING_STANDARDS.md`: public headers in `include/`, `create_*`/`free_*` pairs, documented ownership, `static` helpers.
- `AGENTS.md`: event-driven relay behavior (REQ/EOSE boundary; no polling/sleeps/timed closes); handle `OK false`, `CLOSED`, NIP-42 AUTH; dedupe by event id; NIP-11 probing; run `nostr-protocol-smells` before merging protocol changes; per-component SemVer in `VERSION_MANIFEST.md`; never add the banned relay named in `AGENTS.md` (use `wss://nos.lol` / `wss://relay.nostr.band` in examples and tests); fix Groundhog leaks rather than suppressing; `scripts/test-gate.sh` affected selection.
- Versions (verified `VERSION_MANIFEST.md`): nostr-gobject 3.0.2 (`:25`), libmarmot 0.14.0 (`:27`), marmot-gobject 1.5.1 (`:28`), groundhog 0.12.1 with published prerelease `groundhog-v0.12.1-alpha8` (`:30`), nip59 0.2.1 (authoritative source `nips/nip59/CMakeLists.txt` only; no Meson) (`:49`), signet 0.9.0 / released 0.8.0-rc.1 (sources `signet/meson.build`, `signetd_main.c SIGNET_VERSION`, `signetctl_main.c SIGNETCTL_VERSION`, `signet/CMakeLists.txt`) (`:55`). Meson files exist for libmarmot, nostr-gobject, signet; **Groundhog and nip59 are CMake-only** (verified).
- No existing Cordn, libmls or generic ContextVM code or beads in the repo.

---

## 5. Current-state analysis

### 5.1 Responsibilities and dependencies

| Layer | Current responsibility | Constraint for this work |
|---|---|---|
| `libnostr` | Events, signatures, filters, relays, subscriptions, publish acks, reconnect, AUTH | Reuse; no parallel WebSocket or relay framing. |
| `nips/*` | Individual Nostr protocol modules | ContextVM is not a NIP — do not place it here. |
| `libmarmot` | Marmot protocol **and embedded MLS engine** | Extraction must preserve Marmot behavior and persisted MLSG bytes. |
| `nostr-gobject` | GLib/GObject wrappers, main-context integration, async APIs | New wrappers follow async/finish and the executable/archive contract. |
| Groundhog | Account/signer wiring, encrypted persistence, app policy, models, GTK UI | No protocol parsing, MLS policy or Cordn conflict resolution here. |
| Signet | Management ops, provisioner authorization, capability/rate/replay policy | Share transport, not authorization or management semantics. |

Target dependency graph:

```text
libsodium + OpenSSL
        │
      libmls ───────────────► libmarmot ─► marmot-gobject
        │
libnostr + libjson + NIP-44/NIP-59/NIP-19/NIP-22/NIP-25
        │
   libcontextvm
        │
     libcordn
        │
   nostr-gobject
        │
     Groundhog

Signet ─► libcontextvm / its GObject wrapper
```

`libmls` must not depend on Nostr, GLib, Marmot, SQLCipher or Groundhog. `libcontextvm` must not depend on Cordn, Marmot, Signet or GTK.

### 5.2 MLS extraction seam

Moving files alone is insufficient: `mls-internal.h` imports Marmot types/errors/visibility; KP creation embeds Marmot lifetime defaults; `mls_app_data_update.h` mixes generic proposal encoding with Marmot profile restrictions and admin authorization; `MarmotGroupProfile`, account proofs, profile admission, capability defaults and app-component decoding sit inside the engine (§4.3). Exporter, KP validation and Welcome processing are reusable engine capabilities — not reasons to write another MLS implementation. Existing Groundhog data depends on **MLSG v3/v4** compatibility including tree and ratchet persistence; redesigning serialization during extraction would force an unnecessary migration.

### 5.3 Groundhog lifecycle and ownership

Production lifecycle (`gh-app-outbox.c`): account store opens → `gh_app_outbox_create()` → durable `GhOutbox` → `bind_reactions()` → `bind_nip29()` → `bind_mls()` → services run while the store is open; services are disposed before store close. Cordn follows this ordering including account-generation cancellation.

Conversation integration assumes three backends: enum values 1–3; group constructors/backend delegates/group-room recognition cover NIP-29 and Marmot; `GhSendUiDelegate` already separates availability, enqueue, retry and delivery reports and supports multiple delegates; `GhStoreMls` is a **Marmot** adapter, not generic MLS; `conversations.backend` has a released `CHECK (backend IN (1,2,3))`; incoming persistence → model admission is already transactional-then-notify; reactions, media cache/linkage, read markers and bounded windows are reusable.

Cordn must **not** reuse `GhMlsService`, `GhStoreMls`, Marmot KP lookup (kind 30443), MIP-04 media encoding or the `"mls:"` room namespace — their transport, trust, state and persistence assumptions differ.

### 5.4 Receive/send transformation boundaries

Marmot today: relay event → validate/unseal → libmarmot process → SQL txn (state + message) → models.

Cordn:

```text
relay EVENT
  → verified ContextVM outer envelope (1059/21059)
  → NIP-44 unwrap (via signer)
  → verified signed kind-25910 inner event
  → JSON-RPC / CEP frame
  → coordinator record {gid,cursor,msg_64,at}
  → cursor ordering and duplicate detection
  → Cordn payload unseal
  → MLS authentication/process
  → unsigned envelope id + sender verification
  → transactional protocol state + application projections
  → post-commit GObject/model notifications
```

Coordinator timestamps are ms; Nostr `created_at` is seconds. Coordinator cursor order, sender display order and Groundhog arrival/read order are distinct and stay distinct.

### 5.5 Signet incompatibility

Signet's path is `JSON-RPC → NIP-17 kind-14 rumor → kind-13 seal → kind-1059`. ContextVM requires a **signed kind-25910 event directly inside the wrap**, no seal, and replies carrying the request-event `e`. The Go SDK's `MgmtClient.call()` publishes and returns without receiving/verifying/correlating, so a successful publish can look like successful management execution.

---

## 6. Design

### 6.1 Placement and fixed defaults

Three new **top-level** libraries:
- **`libmls/`** — RFC 9420 machinery and neutral extension hooks.
- **`libcontextvm/`** — ContextVM client/server, MCP lifecycle, CEP framing, discovery, relay transport. Top-level (not `nips/`) because ContextVM is not a NIP.
- **`libcordn/`** — Cordn coordinator contract, MLS profile, sealing, envelopes, optional features, sync. Peer of `libmarmot`.

Defaults and application policy:
1. Groundhog Cordn transport is **encryption-required**.
2. Use CEP-4 kind **1059** for all Groundhog request/response traffic. **Groundhog never uses 21059 for RPC requests or responses**: relays don't store ephemeral events, so a response sent during a client reconnect would be lost and every reconnect would become an ambiguous retry. 21059 is allowed only for CEP-41 stream frames, and only after authenticated negotiation shows both sides support it; a lost frame already triggers cursor catch-up. libcontextvm keeps full 21059 support for other hosts (Signet, CLI).
3. **Never fall back to plaintext** after encryption, initialization or decryption failure.
4. Newly created groups: lowercase 64-hex UTF-8 MLS identities, suite `0x0001`, UUID-text group IDs (MLS group ID = `gid`).
5. Incoming group IDs are opaque valid UTF-8; do not require UUIDs.
6. **No bundled coordinator defaults.** Users add coordinator pubkey + relay set per account.
7. A group reference may supply coordinator and relays; importing it requires confirmation before contacting an unfamiliar endpoint.
8. One active coordinator per group; coordinator replacement/automatic failover is out of scope.
9. Groundhog↔Groundhog multi-device interop via a versioned libcordn/libmls `clientState`; **do not advertise state interop with ts-mls** (cordn-web/CLI).
10. No transmitted typing indicators or read receipts; local read/verification/profile-consent state stays local.

**External baseline pinning (Phase 1):** record immutable commits for Cordn core/CLI/web/Rust and the ContextVM SDK/spec revision; use them for fixtures and interop jobs. Where the fact base lacks complete normative schemas (multi-device document JSON, CEP control-frame fields), use the pinned upstream definitions verbatim — **do not infer field names or encodings from this plan**.

### 6.2 `libmls`: neutral engine and Marmot compatibility

**Public opaque, caller-owned handles:**

| Type | Purpose |
|---|---|
| `MlsClient` | Engine config, application policy registration, supported capabilities |
| `MlsGroup` | One group's authenticated state, ratchets, pending proposals |
| `MlsKeyPackage` | Validated public KeyPackage |
| `MlsKeyPackageSecrets` | Associated private material; securely cleared on free |
| `MlsTransition` | Uncommitted candidate state + generated wire objects |
| `MlsProcessResult` | Authenticated application/proposal/commit/removal result |
| `MlsStateSnapshot` | Structural snapshot for persistence adapters (not policy) |

`create_*`/`free_*` pairs, negative error returns, explicit byte lengths, documented allocator ownership; freeing secret-bearing objects wipes buffers. Raw Ed25519 keys, secret-tree arrays and `MlsEpochSecrets` are not public mutable fields; engine internals stay reachable to engine tests through **non-installed** headers.

**API families** (all synchronous; cover every existing Marmot consumer, not just Cordn):

| Family | Inputs → outputs |
|---|---|
| Client creation | Suite, capabilities, lifetime policy, extension policy → client (fallible) |
| KeyPackage create/import | Arbitrary BasicCredential bytes, lifetime, extensions → public package + secrets |
| KeyPackage validate/ref | Serialized package → validated package/reference |
| Group creation | Group ID, creator package/secrets, extensions → group |
| Admission | Welcome, package/secrets, optional tree/PSKs → group + authenticated GroupInfo signer |
| Membership/proposals | Add/replace/remove/update/extensions/PSKs → transition + protocol outputs |
| Message protection | Group + plaintext → protected MLS message + advanced candidate state |
| Message processing | Group + wire bytes → process result + candidate state |
| Exporter | Group/explicit eligible epoch, label/context/length → secret bytes |
| State access | Epoch, members, credentials, extensions, authenticated sender → immutable views/copies |
| Persistence | Group ↔ neutral snapshot or versioned generic state (fallible) |

**In-place API kept; transitions layered on top.** Today the engine mutates in place (`mls_group_process_commit(MlsGroup *group, …)` is documented "modified on success", verified `libmarmot/src/mls/mls_group.h:583–591`, as are the `_ex`, `_ex_with_psks` and `_by_ref` variants), and `libmarmot/src/mls/` has no clone/copy function. Decisions:
- (a) libmls keeps the in-place `mls_group_*` family as public API, so Marmot's call sites move without semantic change (lowest extraction risk).
- (b) Transitions are an additional layer: `snapshot → load into a fresh group → mutate → accept (swap pointers) / discard (wipe)`. No new deep-copy code is written, and the snapshot API also produces `cordn_operations.candidate_state`.
- (c) Invariant: at most one candidate derived from a given committed state is ever accepted, and `discard` wipes all of its secrets. This addresses the risk that copied ratchet state causes key reuse.
- If snapshot/load proves too slow for large groups, treat it as a measured Phase 8 problem, not a Phase 2 redesign.

Transition boundary:

```text
prepare_process(group, bytes)        → transition(result, candidate)
prepare_application(group, plaintext) → transition(wire, candidate)
accept_transition(group, transition) → replaces state; no I/O
discard_transition(transition)       → securely destroys candidate
```

Preparing never mutates the committed group; the caller persists the candidate and all dependent outputs before accepting in memory.

**`MlsError`** cases: invalid argument; allocation failure; malformed encoding; unsupported version/suite/credential/required extension; invalid signature/authentication; invalid proposal/commit; epoch mismatch; unavailable/consumed message key; exhausted generation; policy rejection; invalid persisted state. `libmarmot` maps these to existing `MARMOT_ERR_*` at its boundary; `libmls` never exposes Marmot error numbers.

**Policy hook — `MlsPolicy`**, an immutable descriptor retained for the client's lifetime, with callbacks for: credential admission and app identity validation; required/default capabilities and leaf extensions; GroupContext validation at create, Welcome, load and commit; app-defined proposal decoding/application; proposal/commit authorization using authenticated parent/result views; cloning, destroying and snapshotting app-owned policy state. Callbacks are synchronous, deterministic, non-publishing, must not mutate the active group or start I/O, and receive authenticated sender/committer indices and parent/result state explicitly.

- **Stays in `libmarmot`:** profile identity and profile-transition restrictions; 32-byte identity/account-proof rules; capabilities and SelfRemove defaults; KP lifetime defaults; application components and AppDataUpdate authorization; profile-specific extension restrictions.
- **Lives in `libcordn`:** hex-text identity convention; `0xC04D` advertisement/validation; Cordn KP requirements and last-resort handling.
- An unknown optional extension retains its bytes; an unsupported required capability fails admission; generic MLS never silently acquires Marmot defaults.

*Why a hook rather than copying or forking the engine:* one implementation keeps RFC 9420 fixes, vectors and sanitizer coverage shared; the rejected alternative (exposing a Marmot-flavored "marmot-mls" sub-API) would make Cordn carry Marmot defaults and dependencies, contrary to decision 1.

**MLSG compatibility** — keep the existing MLSG encoder/decoder **in `libmarmot`** (`libmarmot/src/mlsg_state.{h,c}`) over the neutral snapshot API:
- v3 and v4 states remain readable; re-encoding an unchanged loaded v3/v4 state produces identical bytes.
- Profile-specific field order, presence rules, retained state and ratchet encoding unchanged; legacy migration behavior (incl. legacy own-ratchet advancement) unchanged; exact legacy scalar discriminants preserved even where the state becomes policy-owned.
- Loading requires the correct policy — never infer Cordn from a Marmot blob or vice versa.
- No database blob rewrite; no "upgrade on open".
- New generic libmls state uses a distinct magic/version and never masquerades as MLSG.

**Concurrency/rollback:** one serialized owner per engine object; no internal synchronization promised. A DB failure discards the prepared candidate and keeps the old committed object. Once wire bytes are durable or published, retries reuse those bytes — never regenerate encryption from an earlier ratchet state.

### 6.3 `libcontextvm`: shared client/server transport

**Core model:** serialized, event-driven state machine driven by host interfaces. Public handles `ContextVmClient`, `ContextVmServer`, `ContextVmCall`, `ContextVmStream`, plus immutable request/response/notification/peer-capability records. The host interface supplies:
- scheduling onto one owning context;
- **signing and NIP-44 encrypt/decrypt via async completion callbacks** (required because Groundhog's signers are remote NIP-46/NIP-55L and `nostr_nip59_unwrap()` needs a raw secret key). Decrypt failures come in two classes: **transient** (signer offline, denied, cancelled on account switch) keeps the wrap for retry; **permanent** (malformed or wrong recipient) rejects it. This mirrors the fail/reject split in `gnome/groundhog/src/app/gh-nip17-inbox.c:255–262`;
- **relay transport**: `subscribe(filters) → event/eose/closed callbacks`, `publish(event) → ok/failed`, `auth-challenge → policy`, plus an optional **inbound-wrap source** so a host that already holds the account's 1059 REQ can feed wraps in instead of a second subscription;
- monotonic deadlines and cancellation;
- server replay-store callbacks.

**Relay ownership:** libcontextvm ships a default relay driver over libnostr `NostrRelay`/`NostrSubscription` for tests, the CLI and the Go binding. It copies worker-thread input before posting to the owner executor and never invokes application code on relay reader threads. Groundhog supplies its own relay scope, so its consent-gated AUTH identities (`gnome/groundhog/src/app/gh-auth-policy.h:17`) and Tor/proxy policy apply to Cordn traffic. Signet supplies `relay_pool`, keeping its signature-verifying middleware (`signet/src/relay_pool.c:125–163`). Neither host opens a second socket per relay URL.

**GLib host adapter:** an optional `contextvm-glib` sub-library inside `libcontextvm` (behind a GLib build option) provides the `GMainContext` executor, `GCancellable`/deadline bridge and a local-key signer. `GNostrContextVm*` wraps it, and Signet uses it directly: `signetd` runs a `GMainLoop` (`signet/src/signetd_main.c:122`) but does not link nostr-gobject, so Signet gains no nostr-gobject dependency. Tests use a deterministic executor. Core JSON is `libjson`/Jansson; JSON-GLib conversion happens only at the GObject/GLib boundary.

**Test harness:** `libcontextvm/tests/fake_host.{h,c}` provides a deterministic executor, a scriptable fake signer (completion ordering, mid-decrypt cancellation, transient/permanent failures) and an in-memory relay driver. It is shared by `test_client_server.c`, `test_stream.c` and the Cordn fake coordinator (§6.4).

| Interface | Contract |
|---|---|
| Create client/server | Config, host callbacks, relay set, identity → owned handle |
| Connect | Establish subscriptions, negotiate configured peer; async |
| Call | Method, JSON params, typed JSON-RPC ID/options → call handle; async completion |
| Notify | Method/params, no ID; no response expected |
| Open stream | Method/params, progress token, callbacks → independent stream handle |
| Cancel | Terminate local wait; request remote cancellation where supported |
| Register server method | Name, parameter validator, handler, execution policy |
| Server complete | Result or JSON-RPC error tied to the authenticated request |
| Discover | Explicit server pubkey or consented discovery filter → verified announcements/relay info |
| Close/free | Stop subscriptions, cancel unfinished work, detach callbacks, release resources |

JSON-RPC ID **type and presence** are preserved: string, integer and null are explicit; absence alone means notification. Library-generated IDs are unpredictable strings. Batch requests are rejected (no partial processing).

**Outgoing encrypted request:** (1) build JSON-RPC payload; (2) build kind 25910 with recipient `p`; (3) sign via configured signer; (4) persist the signed inner event when the application's durable operation asks; (5) gift-wrap the signed event directly (no kind-13 seal) through the single new kind-parameterized NIP-59 family below, for both 1059 and 21059. libcontextvm never calls the old `nostr_nip59_wrap()`/`_unwrap()`, which become 1059-fixed delegates of the new family; (6) publish via relay driver and observe `OK`. Responses carry an inner `e` = request's signed event id and preserve the JSON-RPC id; stream/control notifications keep their token/request association.

**Incoming validation:** outer kind, signature, size, exact recipient → decrypt → require a complete signed kind-25910 inner event → recompute id and verify signature → validate inner recipient and permitted peer → for responses require both request-event association and matching typed JSON-RPC id → for CEP frames validate peer, token, request association and frame state → reject unexpected encrypted/plaintext mode transitions. Unknown ephemeral outer authors are normal; identity comes from the verified inner event.

**NIP-59 extension for 21059:** add kind-parameterized wrap/unwrap/validate functions accepting **only 1059 or 21059**, including an unwrap/validate variant that takes an already-decrypted payload (so the host signer performs NIP-44). Existing `nostr_nip59_*` functions remain unchanged and keep meaning 1059. The new helper constructs and signs the chosen kind — never mutate a signed 1059 into 21059 or pass a fabricated 1059 to the old validator. Inner-event signing stays mandatory ContextVM behavior even though generic NIP-59 permits unsigned payloads.

**Handshake and encryption negotiation.** Client states:

```text
disconnected → connecting → negotiating → ready
                         ↘ failed
ready → reconnecting → negotiating/ready
any active state → closing → closed
```

Initialization policy `AUTO | LEGACY_REQUIRED | STATELESS`; Groundhog uses `AUTO`:
- Begin in encrypted 1059 mode and attempt the legacy `initialize`/`notifications/initialized` sequence using the ContextVM-supported revision in the pinned baseline.
- An authenticated method-not-found / explicit stateless response permits stateless operation; **a timeout does not imply stateless and never permits plaintext fallback**.
- A peer requiring initialization gets it before application methods; unsupported protocol-version negotiation is a visible incompatibility.
- After verified capabilities advertise `support_encryption_ephemeral`, later traffic may use 21059 if enabled locally. Groundhog enables it only for CEP-41 stream frames, never for RPC requests or responses (§6.1 item 2).
- Encryption-required peers that reject CEP-4 are unusable, not downgraded.
- Do not adopt the newer MCP lifecycle merely because it is newer; follow the pinned ContextVM mapping.

**Publish outcomes, retries, cancellation.** Keep distinct: (1) signed and durable; (2) sent to socket; (3) relay accepted; (4) server responded; (5) stream/protocol operation applied. Relay `OK true` is not RPC success; a valid authenticated response is sufficient application evidence even if `OK` was lost. Ambiguous retry: reuse the same signed inner event and JSON-RPC id; preferably reuse the original outer event while valid; if rewrapping, server replay identity remains the inner event id. Cancellation completes the caller exactly once; a late valid response may update durable operation evidence but must not resurrect a disposed UI object or complete twice.

**Server replay semantics.** `ContextVmServer` requires a replay-store implementation for mutating methods: key = authenticated sender + signed inner event id; value = `running | completed(response bytes) | rejected`. Duplicate delivery: completed → resend recorded response without rerunning policy/effects; running → attach to existing execution; same identity with different bytes → reject; outside the configured admission window → reject. Request-ID reuse with a **different** signed event is not automatically the same operation. Signet keeps its operation-level replay/authorization rules. Record a completed mutation and its reusable response durably with the mutation wherever the owning subsystem can transact them together.

**CEP-22 oversized transfer** (`contextvm_transfer.c`, shared): defaults 48,000-byte threshold/chunk; 100 MiB aggregate receive cap; ≤ 4 concurrent transfers per peer; validate declared chunk count/total bytes before allocating; index/order validation, digest verification and complete-message validation before dispatch; reject conflicting duplicate chunks; never expose partial content. Control-frame size, timeout and memory limits configurable downward; deadline expiry/cancellation sends `abort` where possible and wipes/frees buffers. **`kp_publish` must stay a single directly signed request** usable as returned KP evidence; if a package cannot fit, fail explicitly instead of substituting an oversized-transfer control event.

**CEP-41 streams:** states `opening → awaiting_accept → open → closing → closed` (`↘ aborted`). Start/accept negotiation when support unknown; ordered `chunkIndex`, exact duplicates accepted once; missing/conflicting chunk aborts and reports recovery-required; ping/pong is protocol liveness, not app polling; close and final JSON-RPC response tracked independently; an initial subscription result must not destroy a still-open stream; no invented generic resume (Cordn reopens from its durable cursor); bounded buffering — pause transport consumption where supported, otherwise abort and recover, never silently drop.

**Relay behavior:** subscribe before publishing requests; filters scoped by addressed identity and configured kinds (plaintext filters also constrain peer authors); encrypted outer events cannot be filtered by inner author or hidden `e`; EOSE is the relay catch-up boundary, not proof the coordinator stream is caught up; use the existing randomized-wrap backfill allowance (default 49 h for stored wraps); dedupe by outer event id and logical inner request/frame identity; reconnect with existing backoff and reissue subscriptions; handle `CLOSED` reasons, `OK false`, AUTH explicitly; connection-generation-aware callback ownership (never free callback data reachable by relay workers); probe NIP-11 with existing facilities and respect limits — capability discovery is never permission to downgrade encryption.

### 6.4 `libcordn`: protocol state and coordinator operations

**Types:**

| Type | Owned state |
|---|---|
| `CordnClient` | Account identity, coordinator connections, KP inventory, groups, persistence callbacks |
| `CordnCoordinator` | Pubkey, canonical relay set, verified transport capabilities, connection status |
| `CordnGroup` | Opaque `gid`, coordinator identity, committed MLS state, cursor, metadata, revision |
| `CordnOperation` | Durable operation identity, intent, immutable generated bytes, progress/error state |
| `CordnEnvelope` | Validated unsigned Nostr event + MLS-authenticated source epoch/sender |
| `CordnGroupRef` | Parsed `gid`, optional coordinator and relay hints |
| `CordnAttachment` | Validated `cordn-em-v1` reference + authenticated source epoch |
| `CordnSyncDocument` | Verified, decrypted document with provenance and compatibility status |

Operation states: `queued`, `waiting-for-signer`, `prepared`, `posting`, `posted`, `applying`, `completed`, `retryable-failure`, `blocked`, `cancelled`, `superseded`. Persist retryable error text only after redaction; record structured local error categories rather than parsing coordinator prose into authoritative enums.

**`CordnPersistence` callbacks:** load an account snapshot; atomically apply a change batch with an expected group revision; stage normalized application effects in the same transaction. A batch includes group-state replacement, inbox disposition, operation changes, KP changes and application effects. A storage conflict/failure leaves the committed in-memory group unchanged. **No SQLite in `libcordn`.** `cordn-store.h` enumerates the neutral effect records libcordn emits, so no Groundhog table knowledge leaks in: `message`, `reaction`, `action(kind, target, payload)`, `membership-change`, `metadata-change`, `media-link`, `rejection`. `GhStoreCordn` maps each record to SQL.

**Operation families** (network ops async via callbacks; pure codecs/crypto sync):

| Family | Operations |
|---|---|
| Coordinator | Configure/connect/disconnect/query status |
| KeyPackages | Create/publish/list/take/remove; verify returned publication |
| Groups | Create, inspect members/metadata, self-update, add/remove member, leave |
| Messaging | Enqueue chat/reply/reaction; retry; inspect operation/delivery state |
| Invitations | Store/take Welcome, accept/decline, list/request/approve join requests |
| References | Parse/encode `cordn1` |
| Metadata | Decode/encode/set `0xC04D` |
| Actions | Edit/delete/pin/unpin/create poll/vote |
| Media | Parse reference, seal/open, exporter-key lifecycle |
| Sync | Publish/discover/adopt documents, resolve competing state, remove group/tombstone |
| Teardown | Leave, forget, remove coordinator, account deletion (below) |

**Teardown semantics:**
- **Leave group:** post a self-remove Commit through the normal send pipeline, then wipe locally (`client_state`, `cordn_media_keys`, inbox, pending operations) and tombstone the group for sync.
- **Forget group:** local wipe plus tombstone, with no network.
- **Remove coordinator:** allowed only once no groups remain (§6.8); `kp_remove` all published refs, then wipe the KP inventory.
- **Account deletion:** all of the above, with network steps best-effort and the local wipe guaranteed.

Every path cancels in-flight signer calls through account-generation cancellation, and marks unsent operations `cancelled` or, after a posted Commit, `superseded`. Wipes rely on the store's `PRAGMA secure_delete=ON` (verified `gnome/groundhog/src/store/gh-store.c:1306`).

Typed Cordn functions build exactly the RPC shapes in §4.1; generic ContextVM calls remain underneath but are not Groundhog's protocol interface.

**KeyPackage lifecycle:** (1) create with Cordn identity/capabilities and explicit lifetime; (2) persist public/private material **before** `kp_publish`; (3) sign and retain the actual `kp_publish` inner request event; (4) publish encrypted by default; (5) confirm RPC result and update inventory; (6) when taking a package verify: publication event signature and canonical id; request method and public package bytes; legacy `keyPackageBase64` only in the documented compatibility parser; `kp_ref`; credential identity == signed publisher; expected intended identity; lifetime/suite/capabilities/signatures. Never substitute Marmot's kind-30443 discovery. Target inventory: **five ordinary + one last-resort package per configured coordinator**, replenished on connection, consumption and known expiry — not by polling. Last-resort private material is retained while published; ordinary private material is removed only after successful durable Welcome admission and ack bookkeeping.

**Ordered receive algorithm.** Per group: `committed_cursor` (last durably processed contiguous cursor); durable inbox keyed by cursor; sealed-wire digest for duplicate/replay recognition; committed MLS state + revision; optional blocked cursor/reason. Steps:
1. Validate record types, expected group and positive safe-integer cursor.
2. Reject values outside the baseline's exact-integer range; never round JSON numbers.
3. Persist the raw record before consuming it.
4. Same cursor + same bytes → idempotent.
5. Same cursor + different bytes → **coordinator equivocation: halt that group**.
6. Buffer later cursors; process only `committed_cursor + 1`.
7. Recognize previously processed identical sealed bytes before invoking MLS again.
8. Unseal using the eligible current or explicitly staged branch.
9. Prepare MLS processing without mutating committed state.
10. For application results, parse the unsigned event, recompute id, require `pubkey` == MLS-authenticated credential identity.
11. Stage normalized effects and candidate state.
12. Atomically persist state, cursor, replay evidence and projections.
13. Accept candidate in memory.
14. Emit callbacks/signals only after commit.

Exact sealed-byte duplicates at later cursors advance stream position without replaying MLS or effects. Malformed content authenticated by MLS consumes its generation and advances the cursor but creates no trusted message (rejection recorded). An undecryptable record may mean unavailable branch/state: reconcile pending branches and fetch missing history once; if unresolved, **block the group** with a repair/rejoin path — never silently skip a potentially necessary Commit. This block rule applies only to MLS-level unseal/process failures. Transient outer-wrap decrypt failures (signer offline, denied or cancelled) keep the wrap for retry and never block the group.

**Catch-up/live boundary** per coordinator connection (fetch first, then subscribe):
1. Install relay reply subscriptions (the "subscribe before publishing" rule applies to these).
2. Call `msg_fetch_many` with the durable exclusive `after` cursors.
3. Call `msg_sub_many` with `after = max(highest fetched cursor, committed_cursor)` per group.
4. Wait for the authenticated `{subscribed:true}` and stream negotiation.
5. Process the fetched records, then the stream records, in contiguous cursor order.
6. Mark the group live once the fetch boundary is processed.

This is gap-free because the coordinator registers `msg_sub_many` before replay and replays from `after` (§4.1). It also delivers each record **once**, which matters because every delivery costs a remote-signer NIP-44 decrypt before `(gid,cursor)` dedup is possible. The fetch result is the finite boundary, so no undocumented replay-complete frame is needed. The `(gid,cursor)` merge and buffering stay in place for reconnects and for out-of-order decrypt completions. `CORDN-ORDER` exercises both this order and the subscribe-first-plus-merge order against the pinned coordinator. If the pinned server does not replay from `after` (Q4), switch to subscribe-first-plus-merge.

On cursor gaps, reconnect, stream abort or detected local delivery loss, run a new cursor-based catch-up — **no periodic `msg_fetch_many` loop, no sleep-based close**.

**Fake coordinator for tests:** `libcordn/tests/fake_coordinator.{h,c}` implements the 11 methods on `ContextVmServer` over the libcontextvm fake host (deterministic executor, in-memory relay driver), with per-method fault injection: equivocation (same cursor, different bytes), cursor gaps, delayed `{subscribed:true}`, lost `msg_post` responses, duplicate deliveries and stream aborts. Real coordinators can't produce these faults, and unit tests use no external network. It is also the first unit-level exercise of `ContextVmServer` replay semantics, before Signet.

**Ratchet-rewind guard (backend-wide, independent of multi-device):** an external restore of the data directory (Groundhog has no in-app backup/restore) or a crash-recovered older state can rewind `client_state` and lead to sender key/nonce reuse. On connect, if the coordinator stream is ahead of `committed_cursor` or the own leaf's epoch is behind, catch up and then force a self-update Commit before any application send. Multi-device write eligibility (§6.6) reuses this rule. Marmot has the same latent hazard; that is out of scope here and should be filed as a separate bead.

**Sending and ambiguous outcomes:**

```text
UI intent → persist queued operation → generate envelope/MLS candidate/sealed bytes
  → persist immutable bytes + candidate → msg_post → receive {cursor}
  → reconcile ordered stream through that cursor → apply confirmed transition/effect → complete
```

- Local echo after durable enqueue, with honest pending status.
- Commit sealed with the **parent epoch** key; application message with the current eligible epoch.
- `msg_post` result proves posting, not successful MLS application.
- Never activate a locally staged Commit before earlier cursors are processed.
- Lost RPC response: first catch up and match the immutable sealed bytes; if reposting is necessary reuse identical bytes and tolerate a different cursor; never claim exactly-once.
- An already-posted operation cannot be cancelled; cancellation only stops local waiting or unsent work.

**Group creation, Adds, Welcomes.** New groups use UUID-text IDs and advertise `0xC04D`. Add: (1) take and verify invitee package; (2) stage Add Commit + Welcome; (3) persist both plus parent state; (4) post the Commit; (5) resolve stream ordering/conflicts; (6) only for the winning applied Add call `welcome_store`; (7) with `after` = admitting Commit cursor; (8) retry Welcome storage independently without redoing the Add. A losing Commit's Welcomes are destroyed and never sent. Welcome observation does not consume it. Accept: process Welcome into candidate state → verify group identity and metadata → persist group/state/cursor from `after` → create ack receipt in the same transaction → ack later via `welcome_take(consumed)`. Decline: durable local decision + queued consumed receipt. Missing ordinary private material → visible "invitation cannot be opened", not acceptance. Join requests use the same durable observe/ack separation, deduped by `(coordinator,gid,requester,at)`, a newer re-request refreshing the displayed request.

**Metadata `0xC04D`:** version 1; five fields in order; strict lengths and UTF-8; admin bytes divisible by 32, no duplicates; preserve exact canonical encoding; reject trailing garbage; advertise support in every local package/leaf. **Do not reuse `mls_tls_write/read_opaque16()`** without proof — they emit VLI prefixes (verified `mls-internal.h:270–282`); lock the length encoding against the pinned reference fixture first. Admins drive UI roles and local management-action availability; do not invent extra incoming-Commit rejection rules from "admin" beyond what the pinned spec requires (avoids a local-policy-induced MLS fork).

**Group refs:** standard bech32 (reuse NIP-19 bech32 primitives where they expose generic HRP encode/decode; otherwise add a minimal generic helper there); exactly one UTF-8 gid TLV ≤ 255 B; at most one 32-byte coordinator key; relay TLVs require a coordinator; ignore unknown well-formed TLVs; reject malformed lengths, mixed/invalid encoding and inputs > 5,000 chars; canonicalize relay URLs without changing the gid.

**Edits, deletions, pins, polls** — same authenticated envelope, send operation and cursor pipeline:

| Kind | Validation and projection |
|---|---|
| `9` | Chat message |
| `1111` | NIP-22 root/parent; keep references even when target absent |
| `7` | Reaction with validated target/author tags; existing reaction projection |
| `1010` | Trimmed replacement text; target in same room **and by same author**; original message id preserved |
| `5` | Author-scoped deletion; nobody deletes another member's event |
| `1011` | Pin `add`/`remove`, target in same room |
| `1068` | Question, unique indexed options, valid poll type, optional `endsAt` |
| `1018` | Vote targeting a poll, valid option refs and cardinality |

Projection rules: process in coordinator cursor order; latest valid non-deleted edit sets displayed body; edits change text, not original attachment metadata; latest valid pin op sets pin state; latest valid non-deleted vote per voter sets that voter's choices; single-choice = exactly one response, multiple-choice = non-empty unique set; evaluate poll closing against the vote record's **coordinator `at`**, explicitly converted (`floor(at_ms/1000)` compared with `endsAt` seconds), because the unsigned envelope's `created_at` is sender-chosen and can be backdated; `created_at` is display-only (cordn-web parity is open question Q5); deleting an edit/vote/pin recomputes from surviving actions; deleting a deletion does not resurrect content; actions arriving before targets are deferred until target admission; unsupported/malformed actions do not invalidate other messages. Store original immutable event data separately from projected display state — **never rewrite `messages.raw_json` with edited text** (breaks canonical-id verification on restore). Local "Delete for me" suppresses locally only; it emits no kind 5 and is not shared state.

### 6.5 Encrypted media

`libcordn` owns `cordn-em-v1` imeta parse/encode, exporter derivation, ChaCha20-Poly1305 seal/open, AAD construction, nonce/hash validation and retained-media-key state. Groundhog owns local-file selection and metadata stripping, upload/download consent, Blossom config and account signing, network/Tor routing, cache and UI. Reuse `gh_attachment_prepare`, local-only file checks, `GhBlossomClient`, `GhNetHttp`, cache linkage and download UI; **do not call `gh_mls_media_seal/open()` or serialize MIP-04 tags**.

Send: (1) validate/read a local regular file under existing GVfs/Tor-safe policy; (2) strip JPEG/PNG metadata, keep the existing notice for other formats; (3) record MIME and filename exactly as transmitted; (4) derive exporter key (`"cordn"`, `"encrypted-media"`, 32); (5) fresh 12-byte nonce; (6) encrypt with AAD `m ‖ 0 ‖ filename ‖ 0 ‖ SHA256(plaintext)`; (7) upload **ciphertext+tag** without the nonce; (8) authorize Blossom upload (kind 24242) via account signer; (9) build `url,m,filename,x,n,v`; (10) before queueing, re-check source epoch — if changed, reseal and re-upload. Cancellation never publishes a partial attachment message; uploaded-but-unreferenced ciphertext may remain on the server (documented limitation; never delete unrelated blobs).

Receive: source epoch comes from authenticated MLS processing, not imeta; attachments parsed independently (a bad one does not reject the message); downloads are user-triggered via `GhNetHttp`; validate blob bounds → AEAD → plaintext hash; refuse malformed nonce/hash/MIME/filename; show missing-key and damaged-file errors separately; never auto-render unauthenticated thumbnails or remote icons.

**Previous-epoch retention decision** (resolves the spec 03 vs multi-device §14 contradiction conservatively):
- **No historical group-payload sealing-key archive.**
- Retain only the separately derived `encrypted-media` key for epochs referenced by retained attachments.
- Keep parent/branch state only while a staged Commit or unresolved sync fork needs it.
- Wipe retired media keys after the last retained attachment/pending upload reference disappears (`DELETE FROM cordn_media_keys` under the store's `secure_delete=ON`; no extra overwrite logic).
- Include the same retained-media-key map in compatible libcordn sync state.

This supports old attachments without a general past-message decryption capability; it chooses security over the broad "retention parity" reading. Document it and request upstream clarification (open question Q1); do not silently widen retention later.

### 6.6 Multi-device synchronization

Implement the full outer/inner/document protocol: replaceable kind 30078, stable random `d`, independent ephemeral signer; owner-directed NIP-44 sealing; inner owner-signed kind 178; ordered `server`, `x`, `dek` tags; documents addressed by SHA-256 of sealed bytes; DEK self-sealing exactly per pinned baseline. Use existing Blossom transport for documents, with explicit account server configuration.

**Versioned libcordn client-state container:** format identifier/version; libmls state format/version; committed cursor; pending transition/commit-point ancestry required by the protocol; retained media-key map; compatibility metadata. It wraps libmls state without moving Cordn concepts into the engine. An unsupported `clientState` (e.g. ts-mls) is preserved as verified opaque remote evidence and shown as incompatible — never imported into libmls, never overwrites local state.

**Triggers:** account connection/unlock; verified remote 30078 update; locally committed group/state change; group removal/tombstone; explicit "Sync now". Coalesce local changes while a publish is in flight, then publish the latest committed snapshot; no polling. Upload all referenced sealed documents before publishing the index; keep the previous usable index until relay acceptance of the new one; a failed upload leaves local state usable and sync visibly pending.

**Validation order:** (1) verify outer event and replacement address; (2) decrypt to owner; (3) verify inner 178 is owner-signed; (4) validate DEK/hash/server tags and bounds; (5) fetch only explicitly permitted document endpoints under app network policy; (6) verify sealed-blob hash before decrypting; (7) validate document schema and client-state compatibility; (8) resolve lineage/conflicts; (9) atomically adopt winning compatible state and required tombstones; (10) publish model changes after commit. The outer ephemeral identity is not authorization; the inner owner signature is.

**Conflict resolution** in `libcordn/src/sync.c` (not Groundhog): lower accepted competing Commit cursor → authenticated third-party post-fork messages as branch evidence → `prev` ancestry commit-point cursors → higher eligible live-document cursor → lexicographically greater document address. Compare only stages whose evidence exists; incomplete comparisons stay pending and fetch referenced evidence (never guess). Losing branch: discard staged transition; suppress/destroy unsent Welcomes; mark dependent unsent operations `superseded`; atomically adopt the winning state; retain only bounded evidence needed to finish reconciliation.

**Shared-leaf send safety** (local policy, no wire change): two devices must never encrypt application messages from the same restored sender-ratchet position.
- A device gains application-write eligibility only after its own fresh self-update Commit wins the ordered stream.
- The claim uses an MLS **PublicMessage Commit** under Cordn's outer sealing, avoiding shared parent handshake-ratchet reuse.
- Eligibility is never transferred through a downloaded state document; it is revoked on state import, app restart/restore, or any intervening epoch transition; another device must win its own self-update before writing.
- Persist each eligible application ratchet advancement and immutable ciphertext before posting.
- A losing claim emits no application message and no Welcome.
- Validate PublicMessage Commit support against both coordinators and the pinned MLS peers before exposing multi-device writes; lack of it is a **concrete blocker** to multi-device writes, not a reason to allow unsafe concurrent ratchet use (open question Q3).

**History and tombstones:** a state document is not a message archive. Existing devices replay missing coordinator history before adopting a newer snapshot where state permits; a newly linked device starts at the snapshot cursor; undecryptable earlier history shows an explicit history boundary (no claim of full history); never rewind an already-used local sender state to an older downloaded snapshot; group tombstones prevent stale documents resurrecting removed groups; do not sync Groundhog read markers, verification marks, drafts or profile-image consent.

### 6.7 Groundhog persistence and migrations

Append migrations; never edit released SQL (`schema_v1`…`v10`). Three landable migrations: **v11** backend 4 + Cordn core; **v12** action projections; **v13** multi-device. `GH_STORE_SCHEMA_VERSION` advances with each; final 13 if nothing intervenes — otherwise append at the actual next numbers without renumbering released migrations. All data lives in the existing encrypted account database.

**Migration runner change (prerequisite for v11).** `store_migrate()` runs each migration inside `store_begin` (BEGIN IMMEDIATE) (`gh-store.c:1378–1416`) and FKs are enabled at configure time (`gh-store.c:1305`); SQLite ignores `PRAGMA foreign_keys` inside a transaction. Add a field to `GhStoreMigration` (`gh-store.h:825–829`), e.g. `GhStoreMigrationFlags flags` with `GH_STORE_MIGRATION_REBUILDS_TABLES`, and in `store_migrate()` for such migrations: confirm no active transaction, record FK state, set `PRAGMA foreign_keys=OFF` **before** `store_begin`, run, `PRAGMA foreign_key_check` before commit, and restore `foreign_keys=ON` on both success and failure paths. Existing migrations default to no flag.

**v11 `conversations` rebuild** (CHECK cannot be altered in place):
1. Ensure no active transaction; record FK enforcement state.
2. Disable FK enforcement before the migration transaction (runner flag above).
3. Create `conversations_new` with the **cumulative** v10 shape unchanged except `backend IN (1,2,3,4)`: the v1 columns and inline `UNIQUE (backend, backend_key)` (`gh-store-schema.c:25–40`) **plus** the five v4 `ALTER TABLE conversations ADD COLUMN` columns `admit_seq`, `read_seq`, `reply_read_at`, `reply_read_id`, `timer_changed_at` with their defaults (verified `gh-store-schema.c:300–306`). Do not hand-copy the v1 literal alone.
4. Copy every column explicitly, preserving ids and NULLs.
5. Drop old `conversations`; rename `conversations_new` → `conversations`.
6. Recreate `conversations_by_backend_key` (a v8 index, verified `gh-store-schema.c:392`); the inline UNIQUE's auto-index comes back with the new DDL.
7. Preserve/revalidate the `messages` triggers (`messages_admit_seq`, `messages_keep_read_marker`, `gh-store-schema.c:309–320`) — they reference `conversations` by name and must still resolve.
8. `foreign_key_check` before commit (child tables at `gh-store-schema.c:43,50,75,128,143,345,379`).
9. Record migration + `user_version` transactionally (existing runner behavior).
10. Restore FK enforcement on success and failure.

Do **not** rename the old table first (SQLite may rewrite dependent FK definitions to the temporary name); do not use `writable_schema`. The v11 migration test diffs `PRAGMA table_info(conversations)` and the `sqlite_master` rows for `conversations` (table, indexes, and triggers referencing it) before and after, and asserts that only the CHECK text changed.

**Core tables (v11).** `?` = nullable; otherwise `NOT NULL`; no default unless shown; `_s` seconds, `_ms` milliseconds; FKs to conversations use `ON DELETE CASCADE`.

| Table | Columns and keys |
|---|---|
| `cordn_coordinators` | `pubkey TEXT PRIMARY KEY`, `name TEXT?`, `is_default INTEGER DEFAULT 0`, `created_at_s INTEGER`, `capabilities_json TEXT?`, `capabilities_at_s INTEGER DEFAULT 0` |
| `cordn_coordinator_relays` | `coordinator_pk TEXT`, `relay_url TEXT`, `position INTEGER`; PK `(coordinator_pk,relay_url)`; FK coordinator |
| `cordn_groups` | `conversation_id INTEGER PRIMARY KEY`, `coordinator_pk TEXT`, `gid TEXT`, `client_state BLOB`, `state_format INTEGER`, `revision INTEGER DEFAULT 0`, `cursor INTEGER DEFAULT 0`, `epoch INTEGER`, `metadata BLOB?`, `status TEXT DEFAULT 'active'`, `blocked_cursor INTEGER?`, `last_error TEXT?`, `history_start_cursor INTEGER DEFAULT 0`; UNIQUE `(coordinator_pk,gid)` |
| `cordn_inbox` | `conversation_id INTEGER`, `cursor INTEGER`, `msg_64 TEXT`, `wire_hash BLOB`, `at_ms INTEGER`, `disposition TEXT DEFAULT 'pending'`, `error_code TEXT?`; PK `(conversation_id,cursor)` |
| `cordn_wire_seen` | `conversation_id INTEGER`, `wire_hash BLOB`, `first_cursor INTEGER`; PK `(conversation_id,wire_hash)` |
| `cordn_key_packages` | `coordinator_pk TEXT`, `kp_ref TEXT`, `kp_bytes BLOB`, `private_state BLOB`, `publication_json TEXT?`, `last_resort INTEGER DEFAULT 0`, `not_before_s INTEGER`, `not_after_s INTEGER`, `status TEXT DEFAULT 'created'`; PK `(coordinator_pk,kp_ref)` |
| `cordn_welcomes` | `coordinator_pk TEXT`, `kp_ref TEXT`, `at_ms INTEGER`, `welcome_64 TEXT`, `after_cursor INTEGER?`, `status TEXT DEFAULT 'pending'`, `error_code TEXT?`; PK `(coordinator_pk,kp_ref,at_ms)` |
| `cordn_join_requests` | `coordinator_pk TEXT`, `gid TEXT`, `requester_pk TEXT`, `at_ms INTEGER`, `kp_ref TEXT`, `status TEXT DEFAULT 'pending'`; PK `(coordinator_pk,gid,requester_pk,at_ms)` |
| `cordn_receipts` | `coordinator_pk TEXT`, `receipt_type TEXT`, `receipt_key TEXT`, `payload_json TEXT`, `created_at_s INTEGER`; PK `(coordinator_pk,receipt_type,receipt_key)` |
| `cordn_operations` | `op_id TEXT PRIMARY KEY`, `outbox_id INTEGER UNIQUE`, `method TEXT`, `intent_json TEXT`, `request_event_json TEXT?`, `request_event_id TEXT?`, `sealed_bytes BLOB?`, `wire_hash BLOB?`, `base_revision INTEGER?`, `candidate_state BLOB?`, `accepted_cursor INTEGER?`, `dependency_op_id TEXT?`, `last_error_code TEXT?`; FK outbox cascades |
| `cordn_media_keys` | `conversation_id INTEGER`, `epoch INTEGER`, `media_key BLOB`; PK `(conversation_id,epoch)` |
| `cordn_local_hidden` | `conversation_id INTEGER`, `event_id TEXT`, `hidden_at_s INTEGER`; PK `(conversation_id,event_id)` |

Use existing `outbox` state/error/attempt fields for scheduling (no independent retry queue); backend 4 distinguishes Cordn entries. Allowed values — group status: `active`, `removed`, `blocked`, `incompatible`; inbox disposition: `pending`, `applied`, `duplicate`, `rejected`, `blocked`; Welcome/join status: `pending`, `accepted`, `declined`, `ack-pending`, `acknowledged`, `invalid`. Indexes: pending inbox by conversation/cursor; pending operations via outbox state; Welcome/join status; package expiry/status; request-event id and wire-hash lookup. Keep accepted-record hashes after pruning large inbox payloads so replay cannot consume MLS state again.

**Existing-table integration:**
- `conversations.backend_key` = canonical encoding of `(coordinator_pk,gid)`; model room id = `"cordn:" + coordinator_pk + ":" + lowercase_hex(UTF8(gid))`; backend key = same suffix without `"cordn:"` — injective and independent of relay spelling.
- `messages.backend_msg_id` = inner envelope id; `messages.mls_epoch` = authenticated Cordn source epoch (broaden its documentation to both MLS-based backends); `messages.raw_json` = original immutable envelope.
- `message_media`/`media` reused with a Cordn-domain-separated file identity.
- `seen`: append a Cordn message namespace to `GH_STORE_SEEN_*`; dedupe key includes room identity and envelope id.
- `outbox_events`: don't mislabel coordinator RPC as a Marmot carrier; add a Cordn role to `GH_STORE_OUTBOX_ROLE_*` only where an actual signed request/wrap is stored.
- Group-less operations (e.g. KP publication) persist in Cordn protocol state, not a fabricated conversation; their retry state is part of the account snapshot/change batches.

**v12 action state:**

| Table | Columns |
|---|---|
| `cordn_actions` | `conversation_id INTEGER`, `event_id TEXT`, `cursor INTEGER`, `kind INTEGER`, `sender_pk TEXT`, `target_id TEXT?`, `created_at_s INTEGER`, `event_json TEXT?`, `deleted INTEGER DEFAULT 0`; PK `(conversation_id,event_id)`; index target/cursor |
| `cordn_message_state` | `conversation_id INTEGER`, `event_id TEXT`, `edit_event_id TEXT?`, `edited_body TEXT?`, `deleted INTEGER DEFAULT 0`; PK `(conversation_id,event_id)` |
| `cordn_pins` | `conversation_id INTEGER`, `target_id TEXT`, `action_event_id TEXT`, `cursor INTEGER`; PK `(conversation_id,target_id)` |
| `cordn_polls` | `conversation_id INTEGER`, `event_id TEXT`, `poll_type TEXT`, `options_json TEXT`, `ends_at_s INTEGER?`; PK `(conversation_id,event_id)` |
| `cordn_votes` | `conversation_id INTEGER`, `poll_id TEXT`, `voter_pk TEXT`, `vote_event_id TEXT`, `responses_json TEXT`, `cursor INTEGER`; PK `(conversation_id,poll_id,voter_pk)` |
| `cordn_event_tombstones` | `conversation_id INTEGER`, `target_id TEXT`, `author_pk TEXT`, `deletion_id TEXT`, `cursor INTEGER`; PK `(conversation_id,target_id,author_pk)` |

Projection updates share the receive transaction. Chat content is stored once in `messages`, not duplicated into `cordn_actions`. On deletion, erase removed plaintext and keep only minimal evidence/tombstone to prevent resurrection.

**v13 sync:**

| Table | Columns |
|---|---|
| `cordn_sync_identity` | `id INTEGER PRIMARY KEY` constrained to 1, `outer_private_state BLOB`, `dtag TEXT`, `dek BLOB`, `last_created_at_s INTEGER DEFAULT 0`, `last_event_id TEXT?` |
| `cordn_sync_documents` | `address TEXT PRIMARY KEY`, `doc_type TEXT`, `coordinator_pk TEXT?`, `gid TEXT?`, `cursor INTEGER?`, `prev_address TEXT?`, `sealed_blob BLOB`, `compatible INTEGER`, `status TEXT`, `stored_at_s INTEGER` |
| `cordn_sync_heads` | `doc_key TEXT PRIMARY KEY`, `address TEXT`, `revision INTEGER DEFAULT 0` |
| `cordn_sync_tombstones` | `coordinator_pk TEXT`, `gid TEXT`, `document_address TEXT`, `removed_at_s INTEGER`; PK `(coordinator_pk,gid)` |
| `cordn_sync_publications` | `event_id TEXT PRIMARY KEY`, `event_json TEXT`, `status TEXT`, `created_at_s INTEGER`, `last_error TEXT?` |

The group-document key is the canonical coordinator/gid identity; the meta-document key is a reserved non-group key. Sync key material stays in the encrypted store and is wiped from temporary buffers.

**Transaction and UI observation.** `GhStoreCordn` is GTK-free and main-context-owned (like `GhStoreMls`). Receive/send/sync adoption:

```text
libcordn candidate batch → GhStore begin → protocol tables
  → messages/reactions/actions/media links → read/unread/window metadata
  → commit → libcordn accepts candidate → conversation/model notifications
```

No model signal before commit; failure rolls back SQL and discards candidates. Account switch cancels callbacks, detaches delegates and closes adapters before store close.

### 6.8 GObject, service and UI integration

**GObject wrappers** (nostr-gobject): `GNostrContextVmClient`, `GNostrContextVmServer`, `GNostrContextVmStream`, `GNostrCordnClient`, `GNostrCordnGroup`. Immutable boxed records for results, references, attachments and reports; async methods use `GTask`/`GCancellable`; pure accessors are sync.

```text
gnostr_context_vm_client_call_async(method, params_json, options, cancellable, callback)
gnostr_context_vm_client_call_finish(result) → response or GError
gnostr_cordn_client_send_async(group, intent, cancellable, callback)
gnostr_cordn_client_send_finish(result) → durable operation ID or GError
```

`send_finish` = durable acceptance of the intent, not delivery; delivery follows operation/group properties and signals. Signals — ContextVM: `state-changed`, `notification`, `stream-opened`, `stream-frame`, `stream-closed`; Cordn: `group-added`, `group-changed`, `invitation-added`, `join-requests-changed`, `operation-changed`, `sync-state-changed`. All emissions on the captured owning main context. Never run mutable client/group operations on arbitrary GTask worker threads; offload only isolated file/network work or immutable crypto whose result returns with an account-generation check.

**`GhCordnService`** — `src/cordn/gh-cordn-service.{h,c}`. Config mirrors `GhMlsServiceConfig`: borrowed open `GhStore`, account controller, conversation store, account relays/settings, reaction store, signer/network bridges. Owns `GNostrCordnClient`, `GhStoreCordn`, coordinator/group list models, activity/network gating (same pattern as `update_activity()`), pending operation/sync subscriptions. Does not own the store and is disposed first. Its `GListModel` holds `GNostrCordnGroup` — no second `GhCordnGroup` model.

**Application binding:** add `bind_cordn`, `unbind_cordn`, `gh_app_outbox_get_cordn_service` in `gh-app-outbox.c`. Requires: backend compiled in (`GROUNDHOG_HAVE_CORDN`); encrypted-groups feature enabled; durable non-ephemeral store; conversation account == store account. Created after reactions, beside existing group services; disposed before reaction storage and before store close; disconnect model callbacks and cancel account-generation work before releasing borrowed objects. A Cordn init failure leaves NIP-17/NIP-29/Marmot usable and exposes a Cordn-specific error state.

**Gift-wrap router (Phase 9, touches the NIP-17 path).** `GhDmInbox` already holds the account's only `{kinds:[1059], #p:[account]}` live REQ on its own 10050 relays (verified `src/app/gh-dm-inbox.h:117–122`). `gh-nip17-inbox.c` decrypts each wrap with one signer call, then **requires a signed kind-13 seal** inside and rejects anything else with `GH_NIP17_INBOX_ERROR_INVALID_SEAL` (verified `gh-nip17-inbox.c:268–274`). CEP-4 puts a signed kind-25910 event directly in the wrap. Without routing, whenever coordinator relays overlap the 10050 set, every coordinator wrap costs a wasted signer round-trip in `GhDmInbox` before being rejected, every DM wrap costs one in the ContextVM client, and each relay carries two identical REQs. Design:
- Verify the wrap, then decrypt it **once**, then dispatch on the inner shape: a signed kind 13 goes to the existing NIP-17 path; a signed kind 25910 goes to a new **ContextVM sink** on `GhDmInbox`, following the existing Welcome-sink pattern (`gh-dm-inbox.h:203–217`); anything else is rejected.
- Groundhog's ContextVM host feeds the client from that sink as its inbound-wrap source (§6.3) for relays the inbox already covers. It opens its own `#p` REQ only on coordinator relays outside the 10050 set.
- Async decrypt completions may reorder frames; cursor buffering (§6.4) absorbs this. Account-generation cancellation drains in-flight decrypts.
- Per-wrap signer latency and call counts are measured with the mock signer in `GH-ENTRYPOINTS`, and NIP-17 receive regressions are covered there (open question Q6 covers signer UX/throughput).

**Conversation/dispatch changes:**

| Existing | After |
|---|---|
| `GhConversationBackend {1,2,3}` | Add `GH_CONVERSATION_BACKEND_CORDN = 4`; existing values unchanged |
| `GH_STORE_BACKEND_*` | Add value 4 |
| `gh_conversation_new_mls(account,room_id)` | Unchanged; add `gh_conversation_new_cordn(account,room_id)` |
| `gh_conversation_set_mls_peers(...)` | Keep as compatibility wrapper; add neutral `gh_conversation_set_group_peers(...)` |
| Backend delegate acceptance | Extend to Cordn |
| `ensure_group()` room recognition | Accept Cordn canonical room ids |
| `GhSendUiDelegate` | No signature change |

Cordn directness uses the existing two-member/unnamed-group presentation convention (remains a group in protocol/storage). Update **every** switch site listed in §4.3, including negative conditions that treat "not MLS" as NIP-17. No generic group-provider abstraction (consistent with the NIP-29 plan's rejection).

**Coordinator UX** — account-level **Cordn** preferences page (in `gh-preferences-dialog`): coordinator name/pubkey, relay list, default selection, connection/encryption compatibility status, add/edit/remove. Opening the page causes no network access; "Check connection" is explicit. Removing a coordinator with joined groups is refused with an explanation (leave/forget groups first). Editing relays reconnects the same coordinator identity; changing its pubkey creates a different entry.

New conversation: "Cordn group" and "Cordn direct message" choices; choose a configured coordinator; members/title; explain coordinator metadata visibility; offer coordinator setup when none exists. Reference import: preview group/coordinator/relays → confirm unfamiliar endpoints → issue a join request or process a received invitation.

**Group and message UI** — Cordn-specific new-group page; invitations/join-requests page; group-info dialog (members/admins, metadata, shareable reference); sync status/link-device/compatibility page. Reuse composer, message list, reply UI, reactions, delivery indicator, attachment chooser, confirmation dialogs. Extend shared message presentation additively (in `gh-message-row`): nullable edited indicator/current display body; pinned-message list/row; poll model and vote controls; pending/retry/failed action state. Actions offered only when the conversation is Cordn and the service supports them: edit own text; delete for me / delete for everyone; pin/unpin; create poll/vote; view/edit metadata where locally authorized. Don't make other backends appear editable. Persistent conditions (blocked group, incompatible coordinator, missing invitation keys, sync incompatibility) use banners/inline status; transient success uses existing toasts; controls keyboard-accessible with explanatory unavailable states (apply the `gnome-hig` skill when building these). Privacy text: the coordinator sees group identifiers, request identity, timing and stream ordering but cannot decrypt content; "Sent" never implies read or delivered to every member.

### 6.9 Signet migration

**Keep in Signet:** management method normalization and handlers; provisioner/target authorization; mutation encryption requirements; grants, deny lists, rate limits; redacted audit; replay policy and durable management effects. **Replace** ContextVM envelope handling with the core `ContextVmServer` through libcontextvm's `contextvm-glib` adapter (§6.3). Signet supplies `relay_pool` as the relay host transport and takes no nostr-gobject dependency. Handlers receive an immutable authenticated request context: sender pubkey; signed inner request event id; outer carrier id; typed JSON-RPC id; transport family; encrypted flag; live/backfill provenance. Never derive authorization from the outer ephemeral key or caller-supplied JSON identity.

Correlation/handlers: new replies carry the request-event `e`; preserve string/integer/null JSON-RPC ids at the transport boundary; legacy normalization may keep its internal string id but must not overwrite the typed wire id; notifications execute only methods explicitly safe for notification semantics and never fabricate a response; apply mutation freshness/replay policy before handlers; shared transport validation does not replace Signet authorization. Signet's "direct 25910 only for read-only methods" rule (`INTEGRATION.md:197`) is preserved by the shared server's per-method execution policy.

**Bounded dual-accept rollout** (from declared 0.9.0):

| Release | Behavior |
|---|---|
| **0.10.0** | Server accepts CEP-4 and legacy NIP-17 management; new clients default CEP-4; legacy use emits redacted deprecation telemetry |
| **0.11.0** | Same dual acceptance; packaging/docs state the removal deadline; explicit legacy client flag remains |
| **0.12.0** | Remove the legacy NIP-17 JSON management path; preserve unrelated NIP-46/legacy protocols |

Removal requires: official `signetctl` and Go SDK CEP-4 releases; published migration instructions; new/old client-server compatibility tests passing; configured external provisioner owners notified. If unmet, explicitly revise the release plan before cutting 0.12.0 — no undocumented permanent dual path. Legacy replies stay legacy-shaped during the transition; never send CEP-4 replies to an old NIP-17 client.

**`signetctl`:** default to the shared ContextVM client with encryption required; during the window expose an explicit legacy transport option; **never automatically retry a failed mutation over the old path**. Command output contracts stay stable except for honest failure when no authenticated application result arrives.

**Go SDK:** replace raw `Relay.Publish()` management with a thin cgo binding to `libcontextvm` (no second ContextVM state machine): configured constructor supplying signer and bidirectional relay transport; preserve public operation signatures; return success only after verified matching RPC success; propagate JSON-RPC error code/message and context cancellation; existing public fields may remain deprecated for source compatibility but publish-only configuration returns a descriptive configuration error; `runtime/cgo.Handle`-style callback lifetime, handles released after callback drainage; document the native-library dependency. Turning the Go MCP scaffold into a production sidecar is a separate deliverable.

### 6.10 Build, packaging and versions

Root options `BUILD_LIBMLS`, `BUILD_LIBCONTEXTVM`, `BUILD_LIBCORDN`; Groundhog availability definition `GROUNDHOG_HAVE_CORDN` (following the existing `GROUNDHOG_HAVE_*` pattern, e.g. `gnome/groundhog/CMakeLists.txt:296,440,604`). Explicit dependencies: Marmot requires libmls; Cordn requires libmls, ContextVM and its NIP deps, plus **libsodium** directly (ChaCha20-Poly1305 IETF AEAD and CSPRNG nonces for payload/media sealing) and SHA-256 via libmls/OpenSSL. UUID text for new group IDs is generated from `randombytes_buf` plus formatting, with no libuuid dependency; `contextvm-glib` requires GLib only when its option is enabled; GObject ContextVM/Cordn sources register conditionally with installed availability metadata; selecting Groundhog Cordn without required components is a **configuration error**, not a silently missing backend. Each new library gets CMake and standalone Meson support (Meson needed because libmarmot, nostr-gobject and signet have Meson builds that will link them), installed public headers, generated visibility/version headers, CMake export/component registration, pkg-config metadata, static dependency closure and focused tests. Use `nostrc_link_archives` (root `CMakeLists.txt:459`) and extend `nostr_gobject_executable` (`nostr-gobject/CMakeLists.txt:317–319`) and the build-tree `.pc` closure; never embed another copy of libnostr or MLS into a shared wrapper.

| Component | Decision |
|---|---|
| `libmls` | New `0.1.0` |
| `libcontextvm` | New `0.1.0` |
| `libcordn` | New `0.1.0` |
| `libmarmot` | `0.14.0 → 0.15.0` (MINOR on 0.x): extraction/build dependency and internal/test-symbol changes; public behavior and persisted bytes preserved |
| `nostr-gobject` | `3.0.2 → 3.1.0`: additive public wrappers |
| `nip59` | `0.2.1 → 0.3.0`: additive 21059-capable API (source `nips/nip59/CMakeLists.txt`) |
| `signet` | `0.9.0 → 0.10.0`: transport migration and completed client behavior |
| `groundhog` | `0.12.1 → 0.13.0`: 0.12.1 already has a published prerelease tag (`groundhog-v0.12.1-alpha8`), so new backend + storage functionality takes the next MINOR |
| `libnostr`, NIP-44 | No bump planned (existing APIs reused, no shipped change) |
| `marmot-gobject`, `libvault-marmot`, gnostr | No bump solely for transitive extraction; assess actual shipped packaging changes under the PATCH rule |

Update every authoritative source listed in `VERSION_MANIFEST.md` and the Declared version in the same component change; latest-release/tag columns only after publication. Record each decision in the phase bead.

---

## 7. File-by-file impact

Paths from repo root; `{h,c}` = both. Grouped rows land together.

### 7.1 Foundation and extraction

| File(s) | Change and reason | Dependency |
|---|---|---|
| `CMakeLists.txt` | Options, dependency ordering, component exports, new subdirectories | New library builds |
| `ARCHITECTURE.md` | Document libmls/ContextVM/Cordn layers and Groundhog ownership | Architecture agreed |
| `VERSION_MANIFEST.md` | Add new components; update affected declarations | Each release-bearing phase |
| `libmls/CMakeLists.txt`, `libmls/meson.build`, `libmls/mls.pc.in` | New standalone/installable engine target and tests | Extraction |
| `libmls/include/mls/mls.h`, `mls-types.h`, `mls-error.h`, `mls-policy.h`, `mls-state.h`, `mls-version.h.in` | Public opaque API, policy/snapshot contracts, versions | Engine boundary |
| `libmls/src/mls.c`, `mls_policy.c`, `mls_state.c`, `mls_error.c` | Public lifecycle/transition adapters, neutral policy and state | Public API |
| `libmarmot/src/mls/mls_crypto.c`, `mls_tls.c`, `mls_tree.c`, `mls_key_schedule.c`, `mls_framing.c`, `mls_key_package.c`, `mls_group.c`, `mls_welcome.c` | Move generic implementation to `libmls/src/`; remove Marmot dependencies | Atomic extraction |
| `mls-internal.h`, `mls_tree.h`, `mls_key_schedule.h`, `mls_framing.h`, `mls_key_package.h`, `mls_group.h`, `mls_welcome.h` | Move generic internals to `libmls/src/` (non-installed); replace Marmot visibility/errors/types | Atomic extraction |
| `libmarmot/src/mls/mls_app_components.{h,c}`, `mls_app_data_update.{h,c}` | Stay in Marmot; adapt to neutral views/hooks; separate generic wire facts from Marmot authorization | Policy API |
| `libmarmot/src/mls_policy.{h,c}` (new) | Marmot `MlsPolicy` implementation | Hook extraction |
| `libmarmot/src/mlsg_state.{h,c}` (new) | Byte-compatible MLSG encode/decode | Snapshot API |
| `libmarmot/src/marmot.c`, `credentials.c`, `kp_profile.c`, `kp_lifecycle.c`, `adopted.c`, `commits.c`, `convergence.c`, `proposals.c`, `members.c`, `groups.c`, `welcome.c`, `messages.c`, `exporter.c`, `media.c`, `group_image.c` | Replace direct engine/policy coupling with public engine + policy boundary; public Marmot signatures preserved | Atomic extraction |
| `libmarmot/CMakeLists.txt`, `libmarmot/meson.build`, `libmarmot/marmot.pc.in` | Stop compiling generic MLS sources; link/export libmls; build-tree closure, version 0.15.0, test linkage | Atomic extraction |
| `libmarmot/tests/test_mls_*.c`, `test_rfc9420_vectors.c`, `test_ratchet_persist.c`, `test_app_data_update.c`, `mdk_vector_stubs.c` | Move neutral tests to libmls or relink; keep policy/compat tests in Marmot. **Enumerate actual matched filenames in the bead before editing.** | Extraction |
| `libmls/tests/test_policy.c`, `test_state.c`, `test_transitions.c` | Neutral-policy, persistence and transactional-state tests | Public API |
| `libmarmot/tests/test_mlsg_compat.c` | Exact-byte compatibility fixtures and failure tests | Legacy codec |
| `tests/selection/inventory.json` | Enroll libmls tests in existing ASan lanes (`:732–808` pattern) | Build |

### 7.2 NIP-59 and ContextVM

| File(s) | Change | Dependency |
|---|---|---|
| `nips/nip59/include/nostr/nip59/nip59.h` | Restricted kind-aware wrap/unwrap/validate (1059/21059), decrypted-payload validation for signer-side NIP-44; old signatures preserved | CEP-19 |
| `nips/nip59/src/nip59.c` | Chosen-kind signing/validation; old functions delegate | Header |
| `nips/nip59/CMakeLists.txt` | Tests; version 0.3.0 (sole authoritative source) | API |
| `nips/nip59/tests/test_wrap_kinds.c` | 1059/21059 signatures, rejection, compatibility | Helpers |
| `libcontextvm/CMakeLists.txt`, `meson.build`, `contextvm.pc.in` | Build/install/export | Root |
| `libcontextvm/include/contextvm/contextvm.h`, `contextvm-host.h`, `contextvm-version.h.in` | Client/server/call/stream; executor/signer/replay contracts | §6.3 |
| `libcontextvm/src/contextvm_client.c`, `contextvm_server.c`, `contextvm_wire.c`, `contextvm_session.c` | Lifecycle, dispatch, envelope, negotiation | Public API |
| `libcontextvm/src/contextvm_transfer.c`, `contextvm_stream.c` | CEP-22/41 state machines | Wire/session |
| `libcontextvm/src/contextvm_discovery.c`, `contextvm_relay_driver.c` | Explicit discovery; default libnostr relay-transport implementation of the host interface | NIP/relay APIs |
| `libcontextvm/glib/contextvm-glib.{h,c}` (optional sub-library) | `GMainContext` executor, `GCancellable`/deadline bridge, local-key signer; consumed by `GNostrContextVm*` and Signet | GLib option |
| `libcontextvm/tests/fake_host.{h,c}` | Deterministic executor, scriptable fake signer, in-memory relay driver | Host interfaces |
| `libcontextvm/tests/test_wire.c`, `test_client_server.c`, `test_transfer.c`, `test_stream.c`, `test_relay_driver.c` | ContextVM gates | Implementation |

No production change to `libnostr/include/nostr-relay.h`, `nostr-subscription.h` or the NIP-44 header is planned.

### 7.3 Cordn core

| File(s) | Change | Dependency |
|---|---|---|
| `libcordn/CMakeLists.txt`, `meson.build`, `cordn.pc.in` | Build/install/export and dependency closure | libmls/ContextVM |
| `libcordn/include/cordn/cordn.h`, `cordn-types.h`, `cordn-store.h`, `cordn-version.h.in` | Protocol APIs, owned records, atomic persistence interface | Core contracts |
| `libcordn/src/cordn.c`, `coordinator.c`, `state.c` | Client lifecycle, RPC contract, durable batches | ContextVM |
| `libcordn/src/group.c`, `key_package.c`, `welcome.c` | MLS profile/policy, membership, invitation lifecycle | libmls |
| `libcordn/src/message.c` | Sealing, cursor ordering, envelope verification, immutable retries | Group/coordinator |
| `libcordn/src/metadata.c`, `group_ref.c` | Strict optional codecs | Pinned fixtures |
| `libcordn/src/media.c` | Media crypto/reference/key retention | Exporter |
| `libcordn/src/actions.c` | Edit/delete/pin/poll/vote validation, deterministic effects | Envelope pipeline |
| `libcordn/src/sync.c` | Documents, ancestry, conflict resolution, write ownership | State/ordering |
| `libcordn/tests/test_profile.c`, `test_coordinator.c`, `test_order.c`, `test_recovery.c`, `test_metadata.c`, `test_group_ref.c`, `test_media.c`, `test_actions.c`, `test_sync.c` | Focused unit/integration coverage | Modules |
| `libcordn/tests/fake_coordinator.{h,c}` | In-process 11-method coordinator on `ContextVmServer` with per-method fault injection | libcontextvm fake host |
| `libcordn/tests/interop/` | Pinned peer adapters, fixtures, coordinator launch definitions | External baseline |

### 7.4 GObject wrappers

| File(s) | Change | Dependency |
|---|---|---|
| `nostr-gobject/include/nostr-gobject-1.0/gn-contextvm-client.h`, `gn-contextvm-server.h`, `gn-contextvm-stream.h` + `nostr-gobject/src/` counterparts | Async ContextVM types; main-context executor, signer bridge, lifecycle, error mapping | libcontextvm |
| `nostr-gobject/include/nostr-gobject-1.0/gn-cordn-client.h`, `gn-cordn-group.h` + `src/` counterparts | Thin Cordn client/group models and immutable records | libcordn |
| `nostr-gobject/include/nostr-gobject-1.0/nostr-gobject.h` | Availability-guarded umbrella includes | Headers |
| `nostr-gobject/CMakeLists.txt`, `meson.build`, version sources | Sources/headers/deps (lists `:105–175,272–279`), GIR (`:427–490`), executable/archive/pkg-config contracts; 3.1.0 | Core libs |
| `nostr-gobject/link_contract.c` | Reference new enabled wrapper types in link probe | Build |
| `nostr-gobject/tests/test_contextvm.c`, `test_cordn.c` | Async cancellation, context affinity, disposal, result ownership | Wrappers |

### 7.5 Groundhog model, storage, application, UI

All under `gnome/groundhog/`.

| File(s) | Change | Dependency |
|---|---|---|
| `src/app/gh-conversation.{h,c}` | Enum/GType value 4, Cordn constructor, directness/title rules (`:173,1268,1335`) | Backend 4 |
| `src/app/gh-conversation-private.h` | `gh_conversation_new_cordn`, `gh_conversation_set_group_peers`, delegate contracts | Model |
| `src/app/gh-conversation-store.{h,c}` | Delegate routing (`:154–156`), Cordn room recognition, lifecycle | Model |
| `src/app/gh-message.{h,c}` | Cordn envelope constructor/room ids, authenticated epoch, edited/poll presentation | Envelope/store |
| `src/store/gh-store.h` | `GH_STORE_BACKEND_CORDN`, Cordn seen namespace, outbox role, migration flags field, schema version | Migrations |
| `src/store/gh-store.c` | `store_migrate()` FK-off rebuild support + FK restoration | v11 |
| `src/store/gh-store-schema.c` | Append v11/v12/v13; never alter released SQL | Schema |
| `src/store/gh-store-cordn.{h,c}` (new) | Typed snapshot/batch persistence (implements `CordnPersistence`), conversation delegate, action/sync projections | Core/store APIs |
| `src/store/gh-store-mls.h` | Document Marmot-only boundary | Docs |
| `src/store/gh-store-reactions.{h,c}` | Backend-4 reconciliation where SQL/routing restricts backends | Actions |
| `src/cordn/gh-cordn-service.{h,c}` (new) | Account-scoped service, activity gating, ContextVM host (relay scope, AUTH consent, inbound-wrap sink), teardown entry points | Wrapper/store |
| `src/cordn/gh-cordn-media.{h,c}` (new) | File/upload/download adapter, epoch check, cache linkage | Core media |
| `src/cordn/gh-cordn-sync.{h,c}` (new) | Account server/network/signer integration only (no duplicate conflict algorithm) | Core sync |
| `src/app/gh-dm-inbox.{h,c}`, `src/app/gh-nip17-inbox.{h,c}` | Gift-wrap router: decrypt once, dispatch signed-13 → NIP-17, signed-25910 → new ContextVM sink; NIP-17 behavior otherwise unchanged | Phase 9 |
| `src/app/gh-app-outbox.{h,c}` | `bind_cordn`/`unbind_cordn`/getter; switch sites `:432,475,485`; deletion dispatch, lifetime | Service |
| `src/app/gh-app-services.c` | Service/UI wiring (`:2548–2578` pattern), reaction dispatch (`:814,857–937`), `:2200–2230` | Service/UI |
| `src/app/gh-send-ui.{h,c}` | Cordn delegate path; switch sites `:123,435,448,545,548`; audit fallback/draft assumptions | Cordn UI |
| `src/app/gh-attachment-ui.c` | Backend-4 send/open dispatch (`:161,191,203,660`) | Media adapter |
| `src/app/gh-contact-directory.c` (`:1153`), `gh-contact-titles.c` (`:18–20`) | Cordn peer/title behavior without request-driven profile fetches | Model |
| `src/app/gh-notifier.c` (`:460`) | Backend-aware preview/privacy, committed-message notifications | Service |
| `src/mls/gh-mls-invitee.c` (`:166`) | Audit backend assumptions; keep Marmot invitation path separate | Enum |
| `src/media/gh-attachments.{h,c}` | Cordn attachment dispatch/reference/cache identity if centralized here | Media |
| `src/media/gh-blossom-client.{h,c}` | Reuse unchanged unless sync needs a missing document operation; no new HTTP stack | Media/sync |
| `src/ui/gh-conversation-row.c` (`:296–310`), `gh-conversation-list.c` (`:238`), `gh-conversation-view.c` (`:3036–3046`), `gh-conversation-info-dialog.c` (`:134–136,909–942`) | Badge, identity, delete/edit/pin/poll actions, privacy wording | Model/service |
| `src/ui/gh-message-row.{h,c}` | Edited/poll/pin presentation at the existing owner | Message properties |
| `src/ui/gh-preferences-dialog.{h,c}` | Host the Cordn coordinators page | Coordinators page |
| `src/ui/gh-cordn-ui.{h,c}` | Send delegate/action attachment, refresh | Service |
| `src/ui/gh-cordn-new-group-page.{h,c}` | Group/DM creation, coordinator selection | Service |
| `src/ui/gh-cordn-invites-page.{h,c}` | Welcome/join-request UI | Invitation APIs |
| `src/ui/gh-cordn-group-info-dialog.{h,c}` | Metadata, members, refs, group actions | Group APIs |
| `src/ui/gh-cordn-coordinators-page.{h,c}` | Account coordinator configuration | Store/service |
| `src/ui/gh-cordn-sync-page.{h,c}` | Sync/linking/status/incompatibility/history boundary | Sync |
| `src/ui/gh-cordn-poll-row.{h,c}` | Poll choices/results/closing state | Action model |
| `data/ui/gh-cordn-*.blp` | Templates for the seven new UI components | UI classes |
| `data/groundhog.gresource.xml`, `po/POTFILES` | Register templates and user-facing strings | UI |
| `CMakeLists.txt`, `TestInventory.cmake` | `GROUNDHOG_HAVE_CORDN`, sources, resources, tests, version 0.13.0 | All phases |
| `tests/store/test_store_cordn.c`, `test_store_cordn_migrations.c` | Atomic batches, window/read state, rebuild integrity | Store |
| `tests/cordn/cordn-world.{h,c}`, `test_cordn_service.c`, `test_cordn_media.c`, `test_cordn_sync.c` | Real service/relay/signer entrypoints, restart, account switch (modeled on `tests/mls/mls-world.h`) | Service |
| `tests/ui/test_cordn_ui.c` | Delegate/actions, absent coordinator, blocked/sync states | UI |
| `tests/mls/mls-forge.h` (`:14–15`) and other internal-MLS include sites | Update extracted test-only include/link paths | Extraction |

### 7.6 Signet and cross-component

| File(s) | Change | Dependency |
|---|---|---|
| `signet/include/signet/mgmt_protocol.h`, `signet/src/mgmt_protocol.c` | Authenticated shared request context, typed response mapping, bounded dual acceptance | ContextVM server |
| `signet/src/signetctl_main.c` | Shared client/default CEP-4; explicit transitional legacy option; `SIGNETCTL_VERSION` | ContextVM client |
| `signet/src/signetd_main.c` | Server lifecycle, transport mode, deprecated-use telemetry; `SIGNET_VERSION` | Server adapter |
| `signet/src/relay_pool.c` | Keep Signet relay/security integration; expose it as libcontextvm's relay host transport; remove only duplicated ContextVM envelope handling | Relay host interface |
| `signet/include/signet/cascadia.h` | Reuse shared ContextVM constants where appropriate; keep unrelated constants | Transport |
| `signet/sdks/go/signetclient/contextvm.go`, `contextvm_native.go` (new), `contextvm_test.go` (new) | Awaiting authenticated RPC client via cgo; signatures preserved; real signed response/error/cancellation tests | Core C binding |
| `signet/CMakeLists.txt`, `signet/meson.build`, `signet/meson.options` | libcontextvm + `contextvm-glib` dependency (no nostr-gobject), migration config, version 0.10.0 | Migration |
| `signet/tests/test_mgmt_replay.c`, `test_signetctl_intent.c`, `test_relay_pool_protocol.c` | Keep legacy cases; add CEP-4, replay and correlation coverage | Migration |
| `signet/tests/test_contextvm_transition.c` (new) | Old/new compatibility and removal criteria | Dual path |
| `signet/tests/CMakeLists.txt`, `signet/tests/meson.build` | Register new tests in both builds | Tests |
| `signet/INTEGRATION.md` | New default wire, compatibility window, SDK requirements | Migration |
| `tests/selection/inventory.json` + owning selection metadata | Register new components/downstream deps and sanitizer lanes | Build/tests |
| `docs/MLS_INTEGRATION.md`, `docs/MARMOT_API.md` | Describe extracted dependency; flag historical plugin conventions | Extraction |
| `docs/CORDN_INTEGRATION.md`, `docs/CONTEXTVM.md` (new) | Public API/wire/security/interop profile, sync limitations, retention profile | Implemented APIs |

---

## 8. Risks and migration

1. **Persisted Marmot state.** Extraction could change profile admission, ratchet persistence or field order. *Mitigation:* legacy codec stays in Marmot; `MLSG-COMPAT` is a prerequisite for all downstream phases; no bulk rewrite or upgrade-on-open.
2. **Groundhog downgrade.** v11 widens a table constraint; v12/v13 add state. New code migrates from v10. Old code rejects a newer `user_version` with `GH_STORE_ERROR_NEWER_SCHEMA` (verified). No supported downgrade rewrites Cordn groups into another backend. **Groundhog has no in-app store backup** (verified: no backup/export code in `src/`), so safety comes from per-migration transactional atomicity: a failure rolls back, the FK state is restored and the store stays at the prior version. A pre-migration backup feature would be a separate bead, not a prerequisite. Migration tests prove unchanged conversation ids, FK relationships, unread state and outbox entries, plus the cumulative-shape diff in §6.7.
3. **Signet wire break.** CEP-4 and current Signet wrapping are incompatible. Keep the bounded dual-accept window and legacy-shaped replies; never retry an ambiguous mutation by switching transport family.
4. **Draft protocol / coordinator behavior.** Require direct interop evidence before advertising support for: encrypted `kp_publish` retaining usable signed publication evidence; initialization/stateless behavior; actual 21059 support; `msg_sub_many` registration-ack behavior; CEP-41 response/stream lifetime; PublicMessage Commit processing; exact optional-action/document encodings. Coordinator failures are reported as incompatibility or a concrete external blocker — never compensated with plaintext downgrade, polling or invented RPCs.
5. **TS coordinator license.** No license declaration → test against an externally obtained, pinned installation; do not vendor or ship its source/container image without license resolution.
6. **Multi-device security/history.** Shared-leaf state is riskier than independent leaves; the write-ownership/self-update policy adds Commit traffic but prevents imported sender-state reuse. State sync is not chat-history backup. Both limits are visible in docs and UI.
7. **Remote-signer decrypt cost.** Under the gift-wrap default, every RPC reply, fetch result and stream frame is a separate wrap needing one NIP-44 decrypt. With NIP-46/NIP-55L signers that's a network or IPC round-trip and possibly a permission prompt (`nip44_decrypt` is a granted permission, `src/identity/gh-nip46-session.c:49`). Mitigations: fetch-first catch-up (single delivery), the wrap router (single decrypt), and measured budgets in `GH-ENTRYPOINTS`. If signer UX is unacceptable, availability may need to be gated by signer type (Q6).
8. **Unverified references.** Complete serialization code, sync schemas and some UI owners were not read in full during planning; Phase 1 validates them. Curated file references are not executed verification.

---

## 9. Implementation order

Each phase gets a bead with scope, version decision, verification results and landing boundary. Phases land independently and keep existing Marmot/Signet tests green.

1. **Lock baseline and inventory.** Pin external revisions (Cordn core/CLI/web/rs, ContextVM SDK/spec); locate any remaining owners/version sources; confirm migration-runner behavior (§6.7); capture real MLSG v3/v4 fixtures (incl. from a Groundhog encrypted store); record exact test/source lists for §7.1 globs; validate `0xC04D` length encoding and sync-document schemas against reference fixtures. No production behavior change.
2. **Extract libmls + Marmot policy boundary — atomic.** Engine move, public API, Marmot hooks, legacy codec, all affected call sites (incl. Groundhog/gnostr test includes), build closure, libmarmot 0.15.0 together. Gates: `MLS-EXTRACT`, `MLSG-COMPAT`, `MARMOT-REGRESSION`.
3. **Kind-aware NIP-59 helpers.** Additive API, nip59 0.3.0. Gates: 1059 compatibility + 21059 signature/validation tests.
4. **ContextVM base client/server.** Signed envelopes, encryption-required negotiation, typed correlation, replay store, host interfaces (signer with transient/permanent errors, relay transport, inbound-wrap source), default libnostr relay driver, `contextvm-glib` adapter, `fake_host` harness, cancellation. Gates: `CTX-WIRE`, `CTX-LIFECYCLE`.
5. **CEP-22/41 and discovery.** Extends the base. Gates: `CTX-CEP22/41` + pinned TS SDK interop.
6. **ContextVM GObject wrappers + build contracts.** Gates: context affinity, disposal, GIR, static/shared link tests. Usable by Signet before any Cordn UI.
7. **Signet dual-accept migration — atomic within Signet.** Server adapter, `signetctl`, Go binding, compatibility tests, docs, 0.10.0. Gate: `SIGNET-TRANSITION`. Legacy removal is a later explicit release.
8. **libcordn core + pure codecs.** Profile, KP evidence, group/Welcome flows, ordering (fetch-first catch-up, ratchet-rewind guard), sealing, metadata, refs, teardown family, neutral effect records; `fake_coordinator`; Cordn GObject wrappers. Tested against the fake coordinator and both real coordinators before Groundhog exposure. Gates: `CORDN-COORDINATORS`, `CORDN-MLS-INTEROP`, `CORDN-ORDER`.
9. **Groundhog backend 4 + schema v11 — atomic integration.** Enum/GType, delegates, room identity, migration-runner flag, cumulative-shape v11 rebuild, store adapter, service (ContextVM host on Groundhog's relay scope), gift-wrap router in `GhDmInbox`, teardown entry points, outbox lifecycle, every switch site, behind `GROUNDHOG_HAVE_CORDN`/feature gate. Gates: `GH-STORE` (v10→v11), `GH-ENTRYPOINTS`.
10. **Coordinator, group, invitation and basic messaging UI.** Real service through send/retry/report and reference/join flows. Verify empty-account, offline, signer-denied and incompatible-coordinator behavior.
11. **Encrypted media end-to-end.** Core crypto + Groundhog adapter/cache/attachment dispatch together. Verify real upload, receive/download, epoch-change retry, cancellation, missing-key.
12. **Actions + schema v12.** Persistence/projections land before edit/pin/poll controls. Gate: `CORDN-OPTIONALS` (out-of-order targets, deletion authorization, restart reconstruction).
13. **Multi-device core + schema v13 + UI — atomic feature exposure.** Document transport, compatibility checks, conflict resolution, write-ownership policy, tombstones, history boundary before enabling device linking. Gates: `CORDN-SYNC` + crash-recovery checks. If PublicMessage Commit support is absent (Q3), ship read-only sync/linking and keep multi-device **writes** disabled with a visible reason.
14. **Affected integration gate + protocol review.** New libraries and real downstream consumers, relevant Marmot compatibility, Groundhog UI/store/service tests, Signet transition tests; run the `nostr-protocol-smells` skill (or `nostr-protocol-review` workflow). Report executed results, unavailable capabilities and external blockers separately.
15. **Enable the backend; release notes.** Only after all scoped features are wired through production entrypoints. Document coordinator compatibility, encrypted default, sync/retention profile, migration restrictions, Signet deprecation dates. Completion is never "isolated helpers pass" or unfinished feature flags.

### 9.1 Execution index

| # | Goal | Done when | Key files | Depends on | Size |
|---|---|---|---|---|---|
| 1 | Pin external baseline and close inventory gaps | Pinned commits + fixtures recorded in bead; MLSG fixtures captured; runner/encoding/schema facts confirmed | bead notes, `libcordn/tests/interop/` fixtures, `libmarmot/tests/` fixtures | — | S |
| 2 | Standalone libmls with Marmot policy hook; in-place API kept, snapshot-based transitions added | `MLS-EXTRACT`, `MLSG-COMPAT`, `MARMOT-REGRESSION` pass; libmarmot 0.15.0 | `libmls/**`, `libmarmot/src/{mls_policy,mlsg_state}.*`, `libmarmot/CMakeLists.txt`, `meson.build` | 1 | XL |
| 3 | 21059-capable NIP-59 | 1059 unchanged + 21059 tests pass; nip59 0.3.0 | `nips/nip59/**` | — | S |
| 4 | ContextVM base + host interfaces + GLib adapter + fake host | `CTX-WIRE`, `CTX-LIFECYCLE` pass | `libcontextvm/src/contextvm_{client,server,wire,session,relay_driver}.c`, `libcontextvm/glib/*`, `libcontextvm/tests/fake_host.*` | 3 | L |
| 5 | CEP-22/41 + discovery | `CTX-CEP22/41` + TS SDK interop pass | `contextvm_{transfer,stream,discovery}.c` | 4 | M |
| 6 | ContextVM GObject wrappers | Affinity/disposal/GIR/link tests pass; nostr-gobject 3.1.0 | `nostr-gobject/{include,src}/gn-contextvm-*`, `nostr-gobject/CMakeLists.txt` | 4 (5 for streams) | M |
| 7 | Signet on shared ContextVM (dual-accept) | `SIGNET-TRANSITION` passes; signet 0.10.0; INTEGRATION.md updated | `signet/src/{mgmt_protocol,signetctl_main,signetd_main,relay_pool}.c`, `signet/sdks/go/signetclient/*` | 4 (`contextvm-glib`), 5 | L |
| 8 | libcordn core + codecs + fake coordinator + Cordn GObject | `CORDN-COORDINATORS`, `CORDN-MLS-INTEROP`, `CORDN-ORDER` pass | `libcordn/**`, `libcordn/tests/fake_coordinator.*`, `nostr-gobject/*/gn-cordn-*` | 2, 5, 6 | XL |
| 9 | Groundhog backend 4 + v11 + gift-wrap router | `GH-STORE` (v11 incl. shape diff), `GH-ENTRYPOINTS` (incl. router, decrypt budget, teardown, NIP-17 regression) pass; all §4.3 switch sites handled | `src/store/gh-store{,-schema,-cordn}.*`, `src/cordn/gh-cordn-service.*`, `src/app/gh-dm-inbox.*`, `gh-nip17-inbox.*`, `src/app/gh-app-outbox.c`, `gh-conversation*.{h,c}` | 8 | XL |
| 10 | Core Cordn UI | UI tests for create/invite/join/chat/reply/react incl. empty/offline/signer-denied/incompatible | `src/ui/gh-cordn-{ui,new-group-page,invites-page,group-info-dialog,coordinators-page}.*`, `data/ui/gh-cordn-*.blp` | 9 | L |
| 11 | Encrypted media | Upload/download, epoch-change reseal, cancellation, missing-key tests pass | `libcordn/src/media.c`, `src/cordn/gh-cordn-media.*`, `src/app/gh-attachment-ui.c` | 9 | M |
| 12 | Edits/deletes/pins/polls + v12 | `CORDN-OPTIONALS` passes | `libcordn/src/actions.c`, v12 SQL, `gh-message-row.*`, `gh-cordn-poll-row.*` | 9 (10 for UI) | L |
| 13 | Multi-device + v13 | `CORDN-SYNC` + crash recovery pass; writes gated on Q3 | `libcordn/src/sync.c`, `src/cordn/gh-cordn-sync.*`, `gh-cordn-sync-page.*`, v13 SQL | 11, 12 | XL |
| 14 | Integration + protocol review | Affected test-gate run recorded; protocol-smells report resolved | — | 2–13 | M |
| 15 | Enable + release notes | Feature on in production entrypoints; groundhog 0.13.0; docs published | `docs/CORDN_INTEGRATION.md`, `docs/CONTEXTVM.md`, `VERSION_MANIFEST.md` | 14 | S |

Phases 3–7 (ContextVM/Signet track) can run in parallel with phase 2 (libmls track); they join at phase 8.

---

## 10. Verification

Focused gates for changed paths — not a repo-wide qualification programme.

| Gate | Required evidence |
|---|---|
| **MLS-EXTRACT** | Existing RFC, ratchet and Marmot tests pass against one libmls implementation; standalone installed static/shared consumers link |
| **MLSG-COMPAT** | Real v3/v4 fixtures load; unchanged states reserialize byte-identically; legacy-format migration preserved; current Groundhog encrypted-store fixtures reopen |
| **MARMOT-REGRESSION** | Marmot adopted/MDK package, Welcome, Commit, media and Groundhog MLS service entrypoints work (`tests/mls/test_mls_service.c`, `test_mdk_interop.c`, `tests/store/test_store_marmot.c`, `tests/ui/test_mls_ui.c`, gnostr mls-groups tests) |
| **CTX-WIRE** | Signed-25910-in-1059 and negotiated-21059 fixtures; no kind-13 seal; typed ids; recipient/peer/`e` checks; malformed/tampered envelopes rejected |
| **CTX-LIFECYCLE** | Legacy/stateless negotiation; no downgrade; cancellation (incl. mid-decrypt via fake signer); transient vs permanent decrypt errors; reconnect; `OK false`; `CLOSED`; AUTH via host policy; duplicate requests; response replay (all on `fake_host`) |
| **CTX-CEP22/41** | Threshold boundaries; chunk duplication/gaps/conflicts; digest/cap limits; stream accept/close/final-response separation; resource cleanup |
| **CORDN-COORDINATORS** | Real calls against pinned TS and Rust coordinators: encrypted `kp_publish/take`, signed publication evidence, Welcome receipts, join requests, multi-group fetch/subscribe |
| **CORDN-MLS-INTEROP** | C ↔ pinned TS client: create/join/chat/reply/reaction/add/remove/metadata; parent-epoch Commit sealing; last-resort packages |
| **CORDN-ORDER** | On `fake_coordinator`: fetch-first and subscribe-first orders; gaps; duplicate cursors/bytes; equivocation halts group; delayed `{subscribed:true}`; own-Commit ordering; lost `msg_post` response; stream abort; restart replay; ratchet-rewind guard forces self-update; both orders confirmed against the pinned real coordinator |
| **CORDN-OPTIONALS** | Bidirectional ref/metadata/media fixtures; edit/delete/pin/poll/vote through service and UI |
| **CORDN-SYNC** | Two libmls devices: upload/index adoption; unsupported ts-mls state; tombstones; concurrent Commit winner/loser; unsent-Welcome suppression; write-ownership safety |
| **GH-STORE** | v10→v11→v12→v13 with populated FK graph/read markers/outbox/reactions; `table_info`/`sqlite_master` diff shows only the CHECK changed; rollback injection; FK enforcement restored |
| **GH-ENTRYPOINTS** | Real `gh_app_outbox_create` binding; account switch/disposal; backend dispatch; composer enqueue/retry/report; attachment and action wiring; gift-wrap router (one decrypt per wrap; 13 → NIP-17, 25910 → ContextVM; NIP-17 receive unchanged); signer call count/latency budget with mock signer; leave/forget/remove-coordinator/account-delete teardown |
| **SIGNET-TRANSITION** | new/new, old/new and explicit new/old compatibility; no policy bypass; duplicate mutation runs once; Go call waits for application result |
| **BUILD-CONTRACT** | CMake/Meson; installed/pkg-config static/shared linkage; optional-feature-off builds; GIR registration/link closure |
| **PROTOCOL-REVIEW** | `nostr-protocol-smells` skill or `nostr-protocol-review` workflow before merging relay/protocol changes |

Test discipline: local relay/coordinator processes, explicit barriers, controllable clocks; **no sleeps** to establish correctness. Unit tests use no external network; interop tests are labeled and capability-gated, and missing required capabilities fail visibly (skips reported as skips). Crash/failure injection at transaction boundaries: private package saved before publication; candidate saved before post; post accepted before response; incoming state saved before model notification; Welcome accepted before receipt; documents uploaded before index publish. Groundhog leaks are fixed, not suppressed.

Commands:

```bash
scripts/install-hooks.sh                                          # once, before work
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --explain  # confirm selection includes new libs + downstream consumers
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA"            # execute affected checks per phase
ctest --test-dir <build> -R 'mgmt_replay|relay_pool_protocol|signetctl_intent|contextvm_transition'  # Signet phase
python3 scripts/test-selection-operational.py                     # only if selector/inventory wiring changes
```

Manual checks (phases 10–13): create a group and DM against a locally run pinned TS coordinator and a cordn-web client; import a `cordn1` ref; send/receive media; edit/pin/poll from both sides; link a second Groundhog device; confirm the privacy text and that no request goes out before the user confirms an unfamiliar coordinator.

---

## 11. Open questions (material, external)

1. **Upstream clarification on epoch-key retention** (spec 03 vs multi-device §14). The plan adopts the conservative profile in §6.5; ask Cordn maintainers which reading is normative before publicizing multi-device interop.
2. **TS coordinator license.** Needed before vendoring or shipping any coordinator artifact for tests/CI; until resolved, interop runs use an externally obtained pinned install.
3. **PublicMessage Commit support** in TS/Rust coordinators and ts-mls peers. If absent, multi-device **writes** stay disabled (read-only sync/linking only) per §6.6/phase 13.
4. **Coordinator behavior at the pinned revision:** legacy `initialize` vs stateless; `support_encryption_ephemeral` (which matters only for CEP-41 stream frames, since Groundhog RPC is always 1059); whether the stored `kp_publish` evidence is the decrypted inner signed event under CEP-4; and whether `msg_sub_many` registers before replay and replays from `after`, which fetch-first catch-up relies on (§6.4). AUTO negotiation and the documented fallback to subscribe-first handle either answer; Phase 1/8 interop results confirm them.
5. **Poll-closing time authority** for cordn-web parity: the plan uses the coordinator `at` because sender `created_at` can be backdated (§6.4). Confirm cordn-web's behavior so votes near `endsAt` are accepted consistently.
6. **Remote-signer decrypt UX/throughput:** is one NIP-44 decrypt per inbound wrap acceptable for NIP-46/NIP-55L signers under the privacy charter? Identity is the account key (KP evidence is the signed `kp_publish`), so a session key can't substitute. If it isn't acceptable, Cordn availability may be gated by signer type. Measure in Phase 9 before enabling.

## 12. Resolved during planning (for traceability)

- Library placement: top-level `libcontextvm/` and `libcordn/` (ContextVM is not a NIP; Cordn is a Marmot peer).
- MCP flow: `AUTO` negotiation, encrypted from the first message, no plaintext fallback.
- libmls/Marmot split: `MlsPolicy` hook + MLSG codec retained in libmarmot.
- Signet: two-minor-release dual-accept window (0.10.0–0.11.0), removal in 0.12.0 under stated criteria.
- Coordinator UX: no bundled defaults; per-account coordinator list; per-group coordinator from refs with confirmation.
- Design critique (`docs/reviews/groundhog-cordn-plan-critique-2026-11-11.md`) applied:
  - libmls keeps the in-place API, with transitions layered on snapshots;
  - relay transport is a host interface (Groundhog relay scope, Signet `relay_pool`);
  - one kind-parameterized NIP-59 family;
  - transient vs permanent decrypt errors;
  - Groundhog never uses 21059 for RPC;
  - `contextvm-glib` lives in libcontextvm and Signet takes no nostr-gobject dependency;
  - fetch-first catch-up;
  - gift-wrap router in `GhDmInbox`;
  - `fake_host` and `fake_coordinator` harnesses;
  - a backend-wide ratchet-rewind guard;
  - a teardown family;
  - neutral effect records;
  - explicit libsodium/UUID dependencies;
  - cumulative-shape v11 rebuild;
  - coordinator-`at` poll closing;
  - `secure_delete`-backed wipes.

Corrections to the planning-pass draft, verified against code on 2026-11-11: translation list is `po/POTFILES` (not `POTFILES.in`); resource manifest is `data/groundhog.gresource.xml`; nip59 has no Meson build (version source is `nips/nip59/CMakeLists.txt` only); `GhStoreMigration` has no flags field and each migration already runs in its own `BEGIN IMMEDIATE`, so the v11 FK-off rebuild needs an explicit runner flag (§6.7); Groundhog bump is concretely `0.12.1 → 0.13.0` because 0.12.1 already has a published prerelease; ContextVM unwrap must go through the host signer's NIP-44 (remote signers expose no secret key), so the NIP-59 extension adds decrypted-payload validation. Also corrected: the draft's "back up through the existing encrypted-store mechanism" — no such mechanism exists (§8.2).

## 13. References

- Cordn: https://github.com/Cordn-msg/cordn (spec/00–03, spec/applications/*), https://github.com/Cordn-msg/cordn-web, https://github.com/Cordn-msg/cordn-rs
- ContextVM: https://contextvm.github.io/contextvm-docs/ , https://github.com/ContextVM/sdk ; CEP-4/6/17/19/22/35/41 linked in §4.2
- NIPs 01, 17, 19, 22, 25, 44, 59, 65; MLS RFC 9420
- In-repo: `AGENTS.md`, `ARCHITECTURE.md`, `CODING_STANDARDS.md`, `VERSION_MANIFEST.md`, `docs/MLS_INTEGRATION.md`, `docs/MARMOT_API.md`, `docs/plans/groundhog-gnome-messaging-2026-09-25.md`, `docs/plans/groundhog-nip46-remote-signer-2026-10-07.md`, `docs/plans/nip-29-gnostr-plugin-2026-05-18.md`, `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`, `docs/investigations/signet-mcp-server-2026-08-19.md`, `signet/INTEGRATION.md`
