# W25 review: slice L, group images and encrypted attachments in Groundhog (nostrc-m6tp, nostrc-q3a6)

- **Reviewer:** independent peer reviewer (AGENTS.md "Peer Review")
- **Branch reviewed:** `groundhog/w25-images-attachments` at `26db5684`, five commits on `08d35b4e` (master)
- **Review branch:** `review/w25-images-attachments` (this document only)
- **Date:** 2026-10-01 (file name per the W24/W25 review convention)
- **Verdict:** **CHANGES-REQUIRED** (small). One fix before merge:
  - **M1:** the admin's group-picture upload goes to the group's 0x800b endpoints without the public-address rule that every other group- or sender-named URL gets. It also doesn't say which server it contacts.
  - Everything else is Low or Nit.
  - The crypto core is correct: source epoch, in-transaction epoch check, reseal, wiping, and the cache binding and purge. The libmarmot producers are correct. The MDK 0.11 media case is a real live exchange, and I re-ran it.

## Commits

| Commit | Bead | Change |
|---|---|---|
| `d59b1541` | nostrc-m6tp | libmarmot: `marmot_update_group_blossom_image()` / `marmot_update_group_avatar_url()`, admin-only full-replacement AppDataUpdates of 0x8002/0x8007, one transaction each; legacy `MARMOT_ERR_UNSUPPORTED` |
| `67f098a1` | nostrc-q3a6, nostrc-m6tp | Groundhog: `GhMlsAttachments` (send/reseal/receive/cache, group picture), store schema v5 (`messages.mls_epoch`, `message_media`, `group_images`), `gh_mls_service_send_with_imeta()`, keyed Blossom upload, attachment UI group delegate, cards per file, Group Info picture section (Blueprint) |
| `8726b633` | nostrc-q3a6 | Tests: MDK 0.11 `white-noise-media` (driver `send_media`/`open_media` and parity test), window group delegate, GVfs refusal |
| `6752f5ae` | — | test: the RTL-override Save As case written as bytes (GCC `-Wbidi-chars`) |
| `26db5684` | — | CI: `test-groundhog-mls-files` in the Groundhog jobs, sanitizer job included |

No code or beads were changed.

- **Mutations.** Each was applied in the review worktree one at a time, then reverted. The tree was clean before this document was committed.
- **Docker.** The sanitizer gate reused its existing shared volumes (`nostrc-linux-gate-asan-arm64`, `nostrc-linux-gate-tests`). `docker volume ls` was the same before and after the run.

## Verification performed

| What | Result |
|---|---|
| `cmake -G Ninja -DBUILD_GROUNDHOG=ON` + `ninja` (macOS 27 env) | builds |
| `ctest -R 'marmot\|groundhog'` | **102/102 pass**. 4 skipped by environment: launch, keyring, background-gui, notifier-gui |
| `python3 scripts/check-unsequenced-args.py` | clean |
| `scripts/linux-gate.sh --sanitizers` (ASAN+UBSAN+LSAN, Ubuntu 24.04 GCC) | **53/53 pass**. Includes the new `groundhog-mls-files`, `-mls-media`, `-media-purge` and `-attachment-ui` |
| libmarmot `test_commits` and `test_adopted_commits` under macOS ASAN+UBSAN (`/tmp/rv-w25-asan`) | pass, including `test_group_image_component_updates`. The sanitizer job doesn't run libmarmot's own tests, so I ran these separately |
| `driver-0.11` built natively from the reviewed tree (`cargo build --locked`, Rust 1.97.1); `cargo test` | **5/5 parity tests pass**, including `media_v2_is_marmot_apps` |
| MDK 0.11 harness, native driver (`-DMDK011_INTEROP_DRIVER=…`): `control`, `white-noise-welcome`, **`white-noise-media`**, `adopted-commits` | **pass**. The verbose log shows Groundhog's imeta as MDK received it (`… "m image/png","filename photo.png","dim 1x1"`) and `ok 1 /groundhog/mdk011-interop/white-noise-media` |

**Mutation spot-checks** (each reverted afterwards):

| Reverted | Expected to catch | Result |
|---|---|---|
| `marmot_media_check_epoch()` block in `send_inner()` | `mls-files/epoch-change-reseals` | **fails** (`PUT == 2`: 1 == 2) |
| `message_media` arm of `MEDIA_FORGET` | `media-purge/mls-forget` | **fails** (plaintext still cached) |
| U+202A..202E in `unsafe_char()` | `mls-files/names` | **fails** (`report‮gpj.exe` kept) |
| `gh_attachment_path_on_remote_mount()` in `gh_attachment_ui_offer_file()` | `attachment-ui/group-delegate` | **fails** (the GVfs file was read) |
| one byte in the driver's copy of `media_aad` | `parity::media_v2_is_marmot_apps` | **fails** (body differs from marmot-app's) |
| the producer's `mls_adopted_component_state_valid()` call | — | **still passes** (N3) |

## Summary by focus area

### 1. Privacy

- **No fetch without a user action: holds.**
  - `gh_mls_attachments_lookup()` and card binding create an IDLE transfer. Only `gh_mls_attachments_download()` (the card's Download) contacts anything.
  - Groundhog has no auto-download preference.
  - The tests count Blossom GETs and SOCKS connects before and after: `send-receive-on-request`, `tor`, `test_gui_group_files` and `white-noise-media`.
  - **Group picture.** `gh_mls_attachments_get_picture()` reads only the store. `action_show_picture()` is the only call to `gh_mls_attachments_fetch_picture_async()`.
  - **URL avatars.** `gh_mls_picture_may_load(WEB, remote_images=FALSE)` is false, `fetch_picture_async` refuses any source but `MARMOT_GROUP_AVATAR_BLOSSOM`, and `WEB_UNVERIFIED` is never loadable.
- **Tor and the network mode: hold for every request.** Every request goes through `GhBlossomClient`, and so through `GhNetHttp`.
  - `test_tor` sees both the upload and the download through the SOCKS5 fixture, with the `.onion` passed by name (ATYP 3).
  - Downloads, including the picture and the 0x800b fallbacks, use `download_url_allowed()`/`host_public()` and `gh_net_http_get_public_async()`. That means public addresses only, checked again at connect time.
  - **Exception:** the keyed picture upload (M1).
- **Servers.**
  - The picture's fetch and upload, and attachment download fallbacks, use only the group's **verified** 0x800b `blossom-v1` endpoints. `gh_mls_media_fallback_urls()` and `gh_mls_media_dup_servers()` skip `base_url_unverified`.
  - Attachment uploads go to the user's own Blossom servers through the existing send sheet (D6: a server is never chosen for the user). They don't go to 0x800b. That is allowed: marmot-app prefers an explicit `blossom_server` over the policy's default endpoints (`media/mod.rs` `upload_encrypted_media` at 946e0547).
  - Downloads try the sender's `blossom-v1` locators, then the 0x800b fallbacks, exactly as marmot-app's `encrypted_media_fetch_candidates()` does.
  - Before sending, the code checks that the policy allows `blossom-v1` (`gh_mls_media_policy_allows_blossom`).
- **Names and metadata.**
  - The name sent is neutral: `photo.jpg`, `photo.png`, or `file.<1-8 alnum ext>` (`test_names`, MDK harness `filename photo.png`).
  - `gh_mls_media_seal()` strips metadata with `gh_attachment_prepare()`. The sheet keeps its "may still carry hidden details" notice for formats it can't strip.
  - A group picture must be a stripped JPEG or PNG.
  - Save As sanitizes the sender's name: C0/C1 controls, `/ \ :`, zero-width marks, LRE…RLO, the isolates and the BOM are replaced, leading dots are removed, and long names are cut on a character boundary. The RTL-override case is tested as bytes. See L4 and N2.

### 2. Crypto correctness

- **Source epoch.**
  - It comes from `MarmotMessageResult.app_msg.epoch` in `process_event()` (`gh-mls-service.c:1973-1976`), never from a tag or the current epoch.
  - `delegate_admit()` stores it in `messages.mls_epoch` for every MLS row, `restore_room()` restores it, and `gh_mls_media_open()` opens with it.
  - A pre-v5 row is NULL. Its files say they "can't be opened here", which is honest.
  - Own sends store the seal epoch, which the in-transaction check proves equal to the send epoch.
- **Reseal never sends a stale epoch.**
  - `send_inner()` does `gh_store_begin` → enqueue → `marmot_media_check_epoch(source_epoch)` → `marmot_create_message` → seal → `gh_store_commit`. That is one synchronous main-loop turn and one store transaction, rolled back on `EPOCH_CHANGED`, so no Commit can slip in between.
  - `on_send_uploaded()` reseals (fresh epoch) and re-uploads, up to `GH_MLS_ATTACHMENTS_SEND_TRIES` = 3 times.
  - Tested by `epoch-change-reseals` (the upload is held while a rename moves the epoch) and `stale-epoch-refused` (nothing listed, no 445 published). The mutation shows the test bites.
- **Plaintext wiping.**
  - `gh_mls_media_open()` and `gh_mls_media_open_picture()` return GBytes whose free function calls `OPENSSL_cleanse`.
  - Cache reads (`get_id`, `gh_store_group_image_get`) return `sodium_malloc` guarded copies.
  - Held plaintext is bounded (`GH_ATTACHMENTS_HELD_MAX`).
  - The decoded GdkTexture isn't wiped (GTK-owned). That is acceptable, as for NIP-17.
- **Cache binding and purge.**
  - The key is `gh_store_mls_media_file_id()`: a domain-separated, length-prefixed SHA-256 over group, epoch, ciphertext/plaintext hash, nonce, `m` and filename.
  - `put_id` is refused unless a stored message lists the id (`message_media`, FK cascade).
  - `MEDIA_FORGET` now unions `message_media` on every deletion path: outbox delete, expiry, retention and forget conversation. The `nip29` delete never carries MLS media.
  - `group_images` is deleted on forget, on Clear (prune 0), and whenever the group's 0x8002 state no longer matches.
  - Everything lives inside the SQLCipher store, with `secure_delete` (existing control test).

### 3. Is `white-noise-media` a genuine live MDK v2 exchange? Yes, with one test-fidelity gap (L6)

- **MDK side.** The MDK side is the real `cgka-session`/`cgka-engine` at `946e0547`. The media exporter secret comes from `exporter_secret_with_epoch(…, GROUP_ENCRYPTED_MEDIA_EXPORTER_CACHE_KEY, 32)`, the send is `SendIntent::AppMessage{expected_epoch}`, and receipt is the engine's own `sync`.
- **Groundhog side.** The production `GhMlsAttachments`, against the same local Blossom fixture.
- **The copied derivation code.** `media_key_info`, `media_aad` and `imeta_tag` are verbatim copies. The parity test compares their whitespace- and comment-stripped bodies with `crates/marmot-app/src/media/{crypto,mod}.rs` in Cargo's git checkout at `MDK_REV`, so it really is pinned to the MDK source; the `media_aad` mutation fails it.
- **What isn't pinned.**
  - `derive_media_file_key` is pinned by substring only.
  - The driver's own inline HKDF/ChaCha20 calls aren't compared. A divergence there would break the live exchange against libmarmot, whose v2 crypto and imeta are pinned byte for byte to MDK's vectors (`libmarmot/tests/test_media.c:172-232`, `:317`).
- **The gap.** MDK's *parser* (`parse_media_attachment` + `validate`) never judges Groundhog's imeta (L6).

### 4. The libmarmot producers

- **Admin authorization.** Through `load_group_for_commit()` → `require_admin=TRUE`, the same gate as `marmot_update_group_metadata()`. The test refuses Bob with `MARMOT_ERR_ADMIN_ONLY` and no Commit.
- **Transactions.** `update_image_component_txn()` wraps each call in `marmot_txn_begin`/`marmot_txn_end` (rollback on error; on a commit failure the returned JSON is freed). The legacy shim test sees one rolled-back transaction.
- **Validation.** `mls_adopted_component_state_valid()` runs first, then `finish_adopted_commit()` → `local_commit_precheck()` checks the Commit as receivers do (N3: the first is unreachable with invalid input).
  - Clearing writes the canonical empty state, never a Remove.
  - A no-op is `MARMOT_ERR_INVALID_ARG` with no Commit.
  - A cleared pending Commit leaves the MLS state byte-identical (tested).
- **Key handling.** The 0x8002 image key and upload key don't leak:
  - the encoded state is `sodium_memzero`ed;
  - `mls_group_free()` wipes `extensions_data`;
  - every Groundhog copy (`Op.image`, `SetPictureOp.image`, `PictureOp.components`, `get_components` results) is cleared with `marmot_group_blossom_image_clear()`, which uses `sodium_memzero`;
  - the keyed upload's copy is wiped, and so is its hex form in `auth_sign_ephemeral()`;
  - no log line or UI string prints component bytes;
  - the picture cache id is a one-way, domain-separated hash that includes the key;
  - the driver's `group_context` redacts key-bearing components.

  The keys live in MLS state in the encrypted store, which is inherent to the protocol.
- **Legacy groups.** `MARMOT_ERR_UNSUPPORTED` inside a rolled-back transaction. That is honest at the API level, but see L2 for the UI copy.

### 5. Schema v4 → v5

- **The migration is safe.**
  - It is `ALTER TABLE messages ADD COLUMN mls_epoch INTEGER`: nullable with no default, so no table rewrite.
  - It also creates two tables with FK cascades: `message_media` (WITHOUT ROWID, indexed by `file_id`) and `group_images`.
  - It runs in the migration transaction with `user_version`.
  - `make_v1_store()` now undoes v5, so the v1→v5 path is tested end to end.
  - An older Groundhog refuses a v5 store (`NEWER_SCHEMA`).
- **The one-way upgrade is documented only in `VERSION_MANIFEST.md`** ("forward-only like v4") and in `gh-store.h`'s comment. The charter's schema section wasn't updated (N5).

### 6. Blueprint UI and accessibility

- **Blueprint.** Both UI changes are Blueprint, with the generated `.ui` committed (`groundhog-blueprint` passes).
- **What's good.**
  - The picture row has `use-markup: false`, so the host from group state can't inject markup.
  - Remove uses `destructive-action`.
  - Spinners follow the codebase's existing `Spinner` use.
  - Card summaries say "N files attached".
  - Rejected references get a visible note, not a silent drop.
- **Issues:** L5 and N4.

## Findings

### M1 (Medium): the group-picture upload goes to group-named servers without the public-address rule, and without saying where

**Where:**
- `gnome/groundhog/src/mls/gh-mls-attachments.c:940-957`: `gh_mls_attachments_set_picture_async()` passes `gh_mls_media_dup_servers(&c)` to `gh_blossom_client_upload_keyed_async()`.
- `gnome/groundhog/src/media/gh-blossom-client.c:567` takes the given list as is, and `:425-441` `upload_put()` calls `gh_net_http_send_async()`, which is `request_start(…, public_only=FALSE)`.
- `libmarmot/src/group_image.c:522`: the endpoint profile accepts `http:`, and `url_classify()` has no private-address class. `libmarmot/tests/test_media.c:1285-1289` (an `http://` endpoint) and `:1305` (`https://127.0.0.1/…` decodes *verified*) confirm it.

**Failure scenario.**
1. A co-admin on White Noise sets the group's 0x800b `default_blob_endpoints` to `http://127.0.0.1:8384` or `https://192.168.1.1`. MDK 0.11 has the producer; Groundhog has none.
2. libmarmot admits both as *verified* endpoints.
3. A Groundhog admin in network mode "system" or "none" uses **Choose…** in Group Info.
4. `normalize_server()` accepts the URL, and Groundhog PUTs the encrypted picture with a Nostr `Authorization` header to `http://127.0.0.1:8384/upload` (GhNetHttp allows http to loopback literals and https anywhere), or to the LAN host.

The result is a request to a local service chosen by another group member. The same endpoints are **refused** for Show Picture and for attachment fallbacks (`download_url_allowed()`/`host_public()`, then the connect-time check of nostrc-qi5e), so the policy is asymmetric.

The admin is also never told the host:
- Show Picture's row says "downloads it from X, which can see your IP address";
- Choose… says nothing;
- the NIP-17 sheet names the server and whether Tor is used.

`gh-mls-attachments.h:15` describes the module as "GhNetHttp: the network mode, Tor, public hosts only".

**Fix:**
- For keyed uploads, i.e. any server list not taken from the user's own `blossom-servers`, drop servers whose host fails `host_public()` (unless `allow_private_hosts`, tests only).
- Connect with the public-only path: a `public_only` flag on `GhNetHttpRequest`, or a `gh_net_http_send_public_async()` doing the connect-time address check.
- Show the destination host and the Tor/IP note before uploading. A subtitle on the Choose row or a confirmation would do.
- Add a test: a group endpoint `http://127.0.0.1:<fixture>` with `allow_private_hosts` FALSE, and no PUT reaches the fixture.

### L1 (Low): no Groundhog test exercises an adopted group's picture

**Where:**
- `gh-mls-attachments.c:701-985`: `get_picture` READY/AVAILABLE/NO_SERVER/WEB, `fetch_picture_async`, `set_picture_async` → `commit_picture` → `on_picture_committed` caching.
- `gh-mls-group-info-dialog.c:398-841`.

**Tests today:** `test_mls_files.c:580` and `test_gui_group_files` cover only the legacy refusal. `test_picture_policy` is pure, and `test_mls_media/picture-roundtrip` is library-level.

**Failure scenario.** A regression passes every test, for example:
- caching under the wrong picture id;
- `get_picture()` no longer forgetting a replaced picture's plaintext;
- Show Picture fetching from an unverified endpoint;
- `action_remove_picture()` clearing 0x8002 when the URL avatar is what shows.

**Fix.** nostrc-46k7 explains why an in-process adopted group has no 0x800b. A test hook that gives the test-created adopted group a 0x800b on the loopback fixture would allow an end-to-end test (libmarmot's encoder accepts `http://` endpoints):
1. Alice sets a picture.
2. Bob's Group Info shows AVAILABLE with zero GETs.
3. Bob uses Show Picture: one GET, then READY from the cache.
4. Alice removes it, and Bob's `group_images` row is gone.

Or fold this into nostrc-8ave.

### L2 (Low): the legacy-group copy isn't honest

**Where:** `gh-mls-group-info-dialog.c:368-369` and `gh-mls-service.c:3617`: "This group was made with an older kind of encrypted group, which can't have a picture."

MIP-01 `NostrGroupData` carries `image_hash`, `image_key`, `image_nonce` and `image_upload_key` (`libmarmot/src/extension.c:15-18`), and libmarmot parses them (nostrc-g5zw says so).

**Failure scenario.** A legacy group made by MDK 0.8 or White Noise with a picture: the other members see it, and Groundhog tells the user the group can't have one.

**Fix.** "Groundhog can't show or change the picture of this older kind of group."

### L3 (Low): the Download and Show Picture notes name only the first host

**Where:**
- `gh-mls-attachments.c:451-466`: `download_note` uses `own[0]`.
- `:736`: `get_picture`'s `out_host` is `urls[0]`.
- Meanwhile `gh_mls_media_fetch_with_fallbacks_async()` (`urls_with_fallbacks`) tries every `blossom-v1` locator, then every verified 0x800b endpoint, and the picture fetch tries every endpoint.

**Failure scenario.** An MDK message carries two locators, or the first one is dead. Download says "from a.example, which can see your IP address", then also connects to b.example and to the group's servers.

**Fix.** Name the first host and say that others named by the sender or group may be tried, e.g. "…or, if it doesn't have it, 2 other servers".

### L4 (Low): Save As keeps the sender's extension, whatever the type

**Where:** `gh-attachment-ui.c:833-836` (sender name, sanitized only) and `gh-attachment-card.c:381`.

**Failure scenario.**
1. A member sends a `.desktop` file or a script declared `m image/png` with `filename holiday.desktop` (or `photo.png.sh`).
2. The card says "Photo".
3. After Download the decode guard fails, and the card says "This photo can't be shown here. Save it to open it."
4. Save As proposes `holiday.desktop`.

The NIP-17 path derives the name from the bytes and type (`gh_attachment_card_suggest_name()`), which avoids this.

**Fix.** Keep the sender's stem, but take the extension from the sniffed type, or the declared one when it can't be sniffed, whenever they disagree. Also, don't show the "Save it to open it" nudge when the bytes don't sniff as the declared image type.

### L5 (Low): Remove Picture isn't confirmed

**Where:** `gh-mls-group-info-dialog.c:823-841`. The tooltip says "Remove the Group's Picture for Everyone".

**Failure scenario.** One click commits removal for every member. The admin can't restore it without the original file. Leave and Remove from Group both go through an `AdwAlertDialog`.

**Fix.** Add a confirmation like `remove_dialog`, or an undo toast before the Commit is staged.

### L6 (Low): the Groundhog→MDK half isn't judged by MDK's parser

**Where:** `tests/interop/mdk/driver-0.11/src/main.rs:1576-1640`. `open_media`'s `field()` takes the first match, with no duplicate, canonical-`m` or locator checks. The README row says it opens "as marmot-app does".

White Noise runs `parse_media_attachment()` and `validate()` (`crates/marmot-app/src/media/mod.rs:1478` and `:518` at 946e0547) on a received imeta.

**Failure scenario.** libmarmot emits an imeta that marmot-app rejects: a duplicate field, a non-canonical `m`, or a locator `validate_locator` refuses. The case still passes.

**Mitigation.** libmarmot's builder is byte-exact to MDK's fixtures.

**Fix.** Copy `parse_media_attachment` + `validate` verbatim under the parity test and run the received tag through them. Then adjust the README wording.

### Nits

- **N1.** `gh_attachment_ui_groups_changed()` (`gh-attachment-ui.c:969`) has no caller, although `gh-mls-attachment-ui.h:14` asks for it. The attach button therefore follows only conversation changes. After Leave or removal while the group is shown, the button stays until the view changes. It is refused at use (`group_of()` re-checks, `send_inner()` refuses).
  - `can_send` (`gh-mls-attachment-ui.c:12`) also ignores `GH_MLS_READ_IDLE`, for which text sending is disabled ("Encrypted groups send only while you're online").
- **N2.** `unsafe_char()` (`gh-attachment-transfer.c:129-134`) misses U+061C ARABIC LETTER MARK. It is a `Bidi_Control` character like the covered U+200E/U+200F. The function also misses U+2028/U+2029.
- **N3.** The producer's `mls_adopted_component_state_valid()` call (`libmarmot/src/groups.c:1799`) can't be reached with an invalid state through the public API:
  - the encoders refuse everything it would refuse;
  - `finish_adopted_commit()` re-validates as a receiver;
  - removing it leaves the tests green.

  Fine as defence in depth, but say so, or test it through an internal entry point.
- **N4.** `set_picture_button` (`gh-mls-group-info-dialog.blp:87-99`) reads "Choose…" to a screen reader. Its sibling `add_member_button` has `accessibility { label: _("Add Members"); }` and the `flat` style. Add `accessibility { label: _("Choose Group Picture"); }`. A finished Show Picture is silent: only the avatar and subtitle change, with no toast or announcement.
- **N5.** The charter's schema section (`docs/designs/groundhog-privacy-ux-charter-2026-09-28.md:299`) stops at v4. Add a v5 line (`messages.mls_epoch`, `message_media`, `group_images`) and state the one-way upgrade (an older Groundhog refuses the store).
- **N6.** `gh-blossom-client.c:323` says the upload key is in "secure memory", but it is `g_malloc`. It is wiped on free, but not guarded or locked. Use `sodium_malloc` or fix the comment.
- **N7.** `on_filesystem_info()` (`gh-attachment-ui.c:548-563`) fails open when the filesystem query errors. The path check before it is the real guard. Worth a comment.
- **N8.** Some blobs are orphaned:
  - a resealed send leaves the first upload on the server;
  - a failed picture Commit leaves an uploaded picture whose upload key is then wiped, so it can't be deleted;
  - Remove doesn't DELETE the old blob.

  Consider a BUD-02 DELETE where the key is still held.
- **N9.** `gh_mls_service_set_image_async()` copies `media_type` with `g_strdup()`, which `marmot_group_blossom_image_clear()` later `free()`s. That is fine since GLib 2.46, but `strdup()` would match libmarmot's allocator.

## Before merge

- **M1:** keyed uploads to group-named endpoints must be public-only, and the destination must be named to the admin, with a test.
- **Recommended in the same pass (cheap):** L2 copy, L3 note, N1 wiring, N4 label.
- **Follow-ups (file or fold into existing beads):** L1 (nostrc-46k7/nostrc-8ave), L4, L5, L6, N5.

---

## Addendum: re-review of the fixes (2026-10-01)

- **Branch re-reviewed:** `groundhog/w25-images-attachments` at `0f1d2e87`, rebased onto `543ca0b6`. The five original commits are now `0a221b89`…`cd949f63`.
  - `git range-diff` and a diff of the two ranges excluding `VERSION_MANIFEST.md` match line for line: the only differences are hunk offsets.
  - The new commits are `7911db69` (libmarmot `marmot_update_group_media_policy()`), `5ad455e2` (Groundhog: M1, L1-L5, N1-N9) and `0f1d2e87` (driver, L6).
- **This review branch** was rebased onto `0f1d2e87`.
- **Final verdict: APPROVE-WITH-NITS.**
  - **M1 is fixed, and I verified it at both layers**, including a DNS-rebinding attempt.
  - Two of the Low fixes are incomplete:
    - **R1:** the L4 Save As allowlist can be bypassed with a double extension.
    - **R2:** the N1 wiring emits a GLib CRITICAL and never connects.
  - Both are a few lines, behind `GH_FEATURE_ENCRYPTED_GROUPS=0`, and are not merge-blocking at Low. I recommend fixing them, each with a test, before the flag flips, preferably in the same series.

### Verification at `0f1d2e87`

| What | Result |
|---|---|
| `ninja` (BUILD_GROUNDHOG, BUILD_MDK011_INTEROP with the native driver) | builds |
| `ctest -R 'marmot\|groundhog' -E mdk011` | **102/102 pass** (the same 4 environment skips). A first run failed `groundhog-blossom` only because its binary was still my scratch build. Rebuilt from the clean tree, it passes |
| `python3 scripts/check-unsequenced-args.py` | clean |
| `driver-0.11` rebuilt `--locked`; `cargo test` | 5/5 parity tests pass |
| MDK 0.11 `control`, `white-noise-welcome`, `white-noise-media`, `adopted-commits` | pass |
| libmarmot `test_adopted_commits` and `test_commits` under macOS ASAN+UBSAN (`/tmp/rv-w25b-asan`), with my scratch probes of the new producer | pass. Probes reverted |
| `scripts/linux-gate.sh --sanitizers` (ASAN+UBSAN+LSAN, Ubuntu 24.04 GCC; the existing shared volumes, `docker volume ls` unchanged) | **53/53 pass** |

### M1: fixed and verified

- **Preflight.** `gh_blossom_client_dup_public_servers()` runs the download rule (`normalize_server()` + `host_public()`: loopback, RFC 1918, link-local, CGNAT, `.local`/`.lan`/`.internal`/single-label names, numeric oddities) in two places:
  - in `gh_mls_attachments_set_picture_async()`, before sealing;
  - again in `upload_start()` for every keyed upload.

  With no server left, the result is `GH_BLOSSOM_ERROR_NO_SERVER` before any request. `test_mls_media/picture-roundtrip` now offers a loopback fixture, `192.168.1.1`, `[fe80::1]` and `printer.local`, and asserts `NO_SERVER` with zero fixture requests. `mls-files/adopted-picture` checks the same end to end.
- **Connect time.** The new `GhNetHttpRequest.public_only` is set for keyed uploads (unless `allow_private_hosts`, tests only). `gh_net_http_send_async()` passes it to `request_start()`, so outside Tor the PUT uses `public_session_new()`. That session is nostrc-qi5e's `GhPublicEnumerator`, which drops every resolved address `gh_net_address_is_public()` refuses. It is the same path the download rebinding test covers.
- **DNS rebinding, tried.** I added a scratch `/groundhog/blossom/keyed-upload-rebinding` test (patch kept at `/tmp/rv-w25b-rebind-test.patch`), reusing `test_blossom.c`'s `RebindResolver`:
  1. `https://rebind.groundhog.test:<port>` passes the preflight (`dup_public_servers` keeps it) and resolves to 127.0.0.1, where a listener counts connections.
  2. With the fix, the keyed upload fails with "This address leads to your own computer or local network, so it isn't used", the resolver was consulted, and **0 connections** were made.
  3. With `upload_put()`'s `.public_only` forced to FALSE (mutation), the same upload **connects** (1 connection; the TLS handshake fails).

  So the connect-time layer is what stops rebinding, and it works.
- **Confirmation.** "Choose…" no longer uploads. `gh_mls_group_info_dialog_set_picture()` presents `set_picture_dialog` ("Set This Picture for Everyone?"), whose body is `gh_mls_picture_upload_note()`:
  - It names every host the upload may go to, in order and only public ones (`gh_mls_attachments_dup_picture_upload_hosts()`): "a.example, b.example or c.example".
  - It says either that each server can see the IP address, or that the upload goes through Tor.
  - It says that everyone in the group will see the picture.

  Only "Upload and Set" starts the seal and upload. With no public host, the dialog doesn't open and a toast says why. `upload_key` is now `secure_alloc` (locked), so N6 is fixed too.
- **Residuals (Nit):**
  - **R3.** No committed test covers the connect-time layer for uploads. The new tests use literal private hosts, which the preflight already drops. Adding the scratch rebinding test above would pin it.
  - **R4.**
    - The confirmation lists hosts read when the file is chosen, but the upload re-reads the 0x800b state when confirmed. A Commit that changes the group's servers while the dialog is open sends the picture to servers the admin wasn't shown (still public-only). Pass the confirmed list through, or re-check it before uploading.
    - No GUI test presents `set_picture_dialog` or `remove_picture_dialog`, for example Cancel ⇒ no PUT and no Commit. Only the note helpers are tested.

### L4: content sniffing and allowlist mostly right, but bypassable (R1, Low)

**What the fix does.** `gh_attachment_card_safe_save_name()` (`gh-attachment-card.c`) picks the extension in this order:
1. the bytes (`gh_attachment_card_sniff_extension()`: JPEG, PNG, GIF, WebP, PDF, ZIP magic);
2. else the declared type (a 12-entry table);
3. else the sender's own extension, only if it is in `safe_extensions` (documents, media, archives, `.ics`/`.vcf`).

The "Save it to open it" nudge now appears only when the bytes sniff as an image. Otherwise the card says "This file isn't the photo it says it is, so it isn't shown". The committed cases (`test_attachments/save-names`) are right.

**The bypass.** The allowlist judges only the **last** extension, and the stem keeps everything before it. When neither the bytes nor the declared type yield an extension (e.g. `application/octet-stream`, `application/x-desktop`, or no `m`) and the last extension isn't allowlisted, the function returns the stem, whose own trailing extension then becomes the effective one. Probed through the real function (scratch, reverted):

| Sender name | Declared type | Save As suggests |
|---|---|---|
| `holiday.desktop.bin` | `application/octet-stream` | **`holiday.desktop`** |
| `run.sh.x` | `application/octet-stream` | **`run.sh`** |
| `setup.exe.dat` | none | **`setup.exe`** |
| `app.desktop.zzz` | `application/x-desktop` | **`app.desktop`** |

**Fix.** When the chosen extension isn't the sender's, or there is none, neutralize the dots left in the stem: `holiday_desktop`, or `holiday_desktop.bin` → `holiday_desktop`. Alternatively, judge every dotted segment against the allowlist. Add these four cases to `save-names`.

The risk stays Low: the user still confirms the name in the Save dialog, and GNOME doesn't launch an untrusted `.desktop` file or a non-executable script. But the fix's own claim, "never .desktop, a script or an executable", doesn't hold.

### N1: the fix never connects (R2, Low)

- **What's right.** `can_send` now uses the composer's own `gh_mls_send_reason()`, so attaching is refused at use when the group has ended, is leaving, or is offline.
- **The broken part.** `gh_mls_attachment_ui_attach()` connects with `g_signal_connect_object(view, "notify::conversation", G_CALLBACK(on_conversation_shown), follow, 0)` (`gh-mls-attachment-ui.c`). `follow` is a plain `g_new0` struct, not a GObject.
  - GLib asserts `G_IS_OBJECT (gobject)` and returns handler id 0. I reproduced it with the same struct layout: "g_signal_connect_object: assertion 'G_IS_OBJECT (gobject)' failed", handler id 0. It aborts under `G_DEBUG=fatal-criticals`.
  - With encrypted groups enabled, every window `gh_app_services_attach_window()` sets up would log a CRITICAL. The attach button still wouldn't follow conversation changes or the shown group's state; only the one `on_conversation_shown()` call at attach time runs.
  - No test reaches this function: `test_attachment_ui` installs the delegate with `gh_attachment_ui_set_groups()` directly. That is why the suite stays green.
- **Fix.** Connect with `g_signal_connect(view, …, follow)` and disconnect in `follow_free()` (keep a weak pointer to the view). Or connect with the window as the GObject and look `Follow` up through `FOLLOW_DATA`. Add a GUI test that calls `gh_mls_attachment_ui_attach()` under `fatal-criticals`, leaves the shown group, and asserts the attach button hides.

### `marmot_update_group_media_policy()`: admin-only and validated

- **Path.** It encodes with `marmot_group_media_policy_encode()`, then goes through the image producers' path: `update_image_component_txn()` → `update_adopted_image_component()`. That path gives:
  - one libmarmot transaction;
  - `mls_adopted_component_state_valid()`, now documented as defence in depth (N3);
  - `load_group_for_commit(require_admin=TRUE)`;
  - a no-op refused with no Commit;
  - `finish_adopted_commit()`, which checks the Commit as receivers check it;
  - the old state compared against `gc.media_policy` for 0x800b.
- **Admin-only.** The committed test refuses Bob with `MARMOT_ERR_ADMIN_ONLY` and no Commit, refuses NULL, checks that receivers follow (the endpoint is stored normalized and verified), and checks the no-op.
- **Validation, probed** (scratch, ASAN+UBSAN, reverted):
  - no locator kind, an unverifiable IDNA endpoint (`https://xn--…`), an endpoint whose kind isn't allowed, and an endpoint flagged `base_url_unverified` are each `MARMOT_ERR_INVALID_INPUT`, with no Commit staged;
  - on a legacy group it is `MARMOT_ERR_UNSUPPORTED` inside one rolled-back transaction (`expect_txns(1, 0, 1)`).
- **Nit.** Those negative cases aren't in the committed tests. Add them.
- **Note.** Like the codec, the producer accepts `http://` and private or loopback endpoints: libmarmot verifies the WHATWG form, not reachability, as MDK does. With M1 fixed, Groundhog applies the public-address rule on every consumer: Show Picture, download fallbacks and keyed upload.
- **Production use.** Groundhog calls the producer only from the `GH_MLS_TEST_HOOKS` hook `gh_mls_service_test_set_media_policy_async()`. A user-facing producer is still nostrc-46k7.

### Status of the other findings

| Finding | Status |
|---|---|
| L1 | **Fixed.** `mls-files/adopted-picture` covers it end to end: an admin's 0x800b Commit; the private host refused; Alice's keyed upload and her READY from the store; Bob AVAILABLE with 0 GETs; Show Picture fetches once, then READY from the store; a replaced copy and the removed copy go. Mutation: dropping `get_picture()`'s `gh_store_group_image_forget()` fails it (`'old' should be NULL`) |
| L2 | **Fixed.** "Groundhog can't show or change the picture of this older kind of group." The service errors and `test_mls_ui` were updated |
| L3 | **Fixed.** `gh_mls_download_note()` and the picture row name every public host in order: the sender's locators, then the group's servers ("the first of a, b or c that has it; each server asked can see your IP address"). Tested in `mls-files/names` |
| L4 | **Partly fixed:** R1 |
| L5 | **Fixed.** `remove_picture_dialog` (destructive, Cancel by default) runs before the Commit. Not GUI-tested (R4) |
| L6 | **Addressed.** `open_media` now refuses a duplicated `v`, hash, `nonce`, `m` or `filename`. The README says plainly that marmot-app's parser isn't used. Tracked as nostrc-qeyg (open, P3) |
| N1 | **Broken:** R2 |
| N2 | **Fixed:** U+061C, U+2028 and U+2029 are added to `unsafe_char()` and tested |
| N3 | **Documented** (comment in `update_adopted_image_component()`) |
| N4 | **Fixed:** `accessibility { label: _("Choose Group Picture"); }`, and a shown picture is announced (`gtk_accessible_announce`) |
| N5 | **Fixed:** the charter's schema section describes v5 and says every migration is one-way (`NEWER_SCHEMA`) |
| N6 | **Fixed:** `secure_alloc` |
| N7 | **Documented** |
| N8 | **Tracked:** nostrc-yrdm (open, P4) |
| N9 | **Fixed:** `strdup` |

### Recommended before `GH_FEATURE_ENCRYPTED_GROUPS` flips

1. **R1:** neutralize the stem's inner extensions, and add the four cases.
2. **R2:** connect the follower correctly, and add a GUI test under `fatal-criticals`.
3. **R3/R4:** commit the rebinding test, carry the confirmed host list into the upload, and add GUI tests for both picture dialogs.
4. **Producer tests:** add the negative policy cases to `test_adopted_commits`.

No code or beads were changed by this re-review. Every scratch probe and mutation was reverted, and the tree was clean before the addendum was committed.
