# Review: `marmot/w23-mdk-interop` (W23, nostrc-7gx7 / nostrc-77pa)

- **Reviewer:** independent peer review (AGENTS.md §3), read-only worktree at `254bf044`
- **Commits:** `728400b5` libmarmot 0.11.0 wire conformance; `254bf044` Groundhog <-> MDK 0.8 harness and refusing unproven invitees before create
- **Base:** master `43f39adf`
- **Intent doc:** `docs/analysis/marmot-mdk-interop-2026-09-30.md`

## Verdict: **APPROVE-WITH-NITS**

I found no correctness or security defect that blocks the merge. The new cryptography matches the stated scheme. The Commit-as-PrivateMessage path authenticates its sender and cannot get around the account-proof policy. The codec rejects malformed input safely. The Groundhog change creates nothing and publishes nothing when it refuses. One Medium finding (M1) should be filed as a follow-up bead: the new PrivateMessage-Commit path, which is security-sensitive, has no in-tree regression or negative test. Right now the only thing exercising it is the opt-in Docker/MDK harness.

## Gates run (macOS 27, `source /tmp/nostrc-macos27-env.sh`)

| Gate | Result |
|---|---|
| `python3 scripts/check-unsequenced-args.py` | PASS ("No call modifies and uses a variable in different arguments") |
| `cmake -S . -B /tmp/rv-mdk -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/rv-mdk` | PASS (2462/2462; first needed `git submodule update --init --recursive` in the fresh worktree; only linker "duplicate libraries" warnings) |
| `ctest --test-dir /tmp/rv-mdk -j6 -R 'marmot\|groundhog'` | **95/95 passed**. Skipped: groundhog-launch, -store-key-keyring, -background-gui, -notifier-gui (environmental, same as master) |
| `groundhog-mdk-interop` (Docker) | Not run: `BUILD_MDK_INTEROP` is opt-in and off here. I relied on the report's six-of-six runs and the nightly workflow |

## Focus-area assessment

### 1. MIP-03 ChaCha20-Poly1305 kind:445 (`libmarmot/src/messages.c:119-207`)
- **Key derivation:** `group_event_key()` = `mls_exporter(exporter_secret, "marmot", "group-event", 32)`. `mls_exporter` (`mls_key_schedule.c:773`) is RFC 9420 §8.5: DeriveSecret(label) → ExpandWithLabel("exported", Hash(context), L). The key is derived per epoch from that epoch's stored exporter_secret, and the lookback loop tries the current epoch and up to 5 previous ones. ✔
- **Nonce:** 12 fresh bytes from `randombytes_buf` per event, sent as a prefix. All senders in an epoch share the key, but a random 96-bit nonce stays well inside the birthday bound for any realistic per-epoch volume. ✔
- **AAD:** empty, as MIP-03 specifies. This means the h-tag, epoch and ephemeral pubkey are not bound to the ciphertext. That is by spec, and the inner MLS framing binds group_id and epoch anyway. ✔
- **Tag check:** libsodium `crypto_aead_chacha20poly1305_ietf_decrypt` verifies the tag in constant time (`crypto_verify_16`) before it releases any plaintext. On failure `pt` is freed and `*out_plaintext` stays NULL. Short input (< 12+16 bytes) and invalid base64 are refused before decryption. ✔
- **Zeroization:** the derived `key` is `sodium_memzero`'d in both encrypt and decrypt, and the loop's `exporter_secret` buffer is zeroed after the loop (`messages.c:922`). The plaintext here is an MLS PrivateMessage, so it is still ciphertext. ✔
- The `SIZE_MAX` overflow guard on encrypt is present. ✔

### 2. Commit as PrivateMessage (RFC 9420 §6.3) (`mls_group.c:2934-3080`, `commits.c:1801-1869, 2067-2180`)
- **Sender authentication:** `private_message_sender()` checks group_id, epoch and content_type against the judging state. It decrypts SenderData with that epoch's `sender_data_secret` and requires the leaf to be occupied. `private_commit_open()` then decrypts with the sender's handshake ratchet. `mls_handshake_content_decode()` sets `sender = member(leaf from SenderData)`, so the sender comes from authenticated data and never from the clear header. `commit_authenticate()` requires `from == sender_leaf` and verifies the signature with **`wire_format = mls_private_message`**, which is bound into FramedContentTBS, so a PublicMessage signature cannot be replayed as a PrivateMessage. It skips the membership tag only for PrivateMessage, which is correct. The confirmed-transcript input uses the arrival wire format (§8.2). ✔
- **Ratchet state:** the handshake ratchet is saved and restored around the trial decrypt. `MlsSenderRatchet` is all fixed arrays, so the `memcpy` snapshot is safe, and `sender_discard` zeroizes the snapshot. ✔
- **Policy:** PrivateMessage Commits take the same `stage_inbound()` → `mls_group_process_commit()` → `marmot_commit_authorize(parent, post, sender, allow_unproven_members, …)` path as PublicMessage Commits (`commits.c:1469-1486`). The 0x8009 binding (`leaf_binding_check`, `marmot_tree_members_bound`) and the we6g sender/leaf binding therefore apply unchanged. Default mode still refuses unproven leaves on Welcome (`welcome_tree_bound`, `welcome.c:585`), on our own Add (`groups.c:1020`) and on inbound Commits (`commits.c:1483`). ✔
- **Routing:** `is_handshake()` (`messages.c:88`) parses only the clear header, with correct bounds (`remaining < gid_len` checked before advancing). A lie in the header cannot help an attacker: content_type sits in the AEAD-authenticated `PrivateContentAAD`, so a mislabeled message fails to decrypt later. Standalone PrivateMessage Proposals return `UNSUPPORTED`, as PublicMessage ones already did. ✔

### 3. MIP-01 QUIC-varint group-data codec (`extension.c`)
- Every length goes through `mls_tls_read_vli` (shortest-encoding check, remaining-bytes check before `malloc`) and a per-field cap: names ≤ 65535, admins % 32 and ≤ 1000, relays ≤ 100 × 4096 B, total ≤ 1 MiB checked before decoding. Optional fixed fields must be exactly 0 or N bytes. Image fields are all-or-nothing on both read and write. Trailing bytes are accepted only when `version > 2` and are kept in `extra`. Every failure path frees partial state (`take_*` free their input, `relays[]` is calloc'd so a partial fill is safe to free). ✔
- **Legacy 0.10.0 fallback** (`decode_libmarmot_0_10`) runs only when the MIP-01 parse fails. It is strict: `version ≤ 2`, `admins_len` checked against the remaining bytes before `malloc`, `has_image`/`has_upload` ∈ {0,1}, and it must consume the whole input. Some inputs parse under both layouts (e.g. 0 admins, 0 relays, no image), but both decoders produce the same values for them, and the data comes from an authenticated GroupContext anyway. ✔ (see L3)

### 4. required_capabilities and relays
- `write_required_capabilities()` (`groups.c:149`) encodes `RequiredCapabilities{[0xF2EE],[],[]}` as `02 F2 EE 00 00`, which is correct. New groups carry it, and a metadata Commit adds it to old groups exactly once (`has_required`). `test_group_context_required_capabilities` covers both cases. ✔
- The Add path now fills the Welcome rumor's `relays` tag from the post-Commit `marmot_group_data`, and only when exactly one extension is present. The tests assert this for both creation and Add. ✔
- A Welcome is refused unless the group carries exactly one decodable 0xF2EE (`welcome.c:611-627`). ✔

### 5. Groundhog: refused New Group (`gh-mls-service.c:2024-2043`)
- The pre-check runs in `invitees_ready()` before `create_group_now()`. It uses `marmot_key_package_event_has_account_proof()`, which returns `MARMOT_LEAF_PROOF_VALID` only for a cryptographically valid proof, so a forged proof cannot slip past it and leave a group of one behind. The error path matches the existing pattern (return the task error, unref the task), so nothing leaks. The updated test asserts zero listed groups and zero group-relay events. ✔
- The test hook is compiled only under `GH_MLS_TEST_HOOKS`, which is defined only on `test-groundhog-mdk-interop` (`CMakeLists.txt:1660`). Production code cannot reach `allow_unproven` (it is always FALSE). ✔

### 6. Versions (`VERSION_MANIFEST.md`)
- libmarmot 0.10.0 → **0.11.0** in `CMakeLists.txt`, `meson.build` and the manifest, with a breaking-wire note and README compatibility notes. This is correct under AGENTS.md's 0.x rule (a breaking change is a MINOR bump, called out). ✔
- marmot-gobject and gnostr: no source change. Their "no bump" is justified, with a note to carry the wire warning. ✔
- groundhog 0.11.1: no bump, because the feature is still behind `GH_FEATURE_ENCRYPTED_GROUPS=0` and the hook and option are test-only. ✔
- `MarmotGroupDataExtension` gains `extra`/`extra_len` at its end. The manifest says so. The struct comes from `marmot_group_data_extension_new()` (calloc), so this is acceptable for 0.x.

### 7. Relays and CI
- The diff adds no `relay.damus.io`. `wss://nos.lol` and `wss://relay.example[.com]` appear only as string data in codec/rumor unit tests, with no network I/O. The MDK driver dials only the URLs the test hands it (`dial_url` rewrites only 127.0.0.1/localhost). ✔
- The workflow uses no secrets, triggers only on `workflow_dispatch` and a nightly `schedule`, and pins MDK to rev `575ae29d` (Cargo.toml and Cargo.lock, `cargo build --locked`) and OpenMLS to `04c50d7f`. ✔ (see L4, N5)

## Findings

| # | Sev | File:line | Finding | Recommendation |
|---|---|---|---|---|
| M1 | Medium | `libmarmot/src/mls/mls_group.c:2966` (also `commits.c:1816`, `messages.c:88`, `mls_framing.c:1431`) | The whole Commit-as-PrivateMessage path (`is_handshake`, `commit_route`, `mls_group_handshake_sender`, `private_commit_open`, `mls_handshake_content_decode`, PRIVATE wire format in the signature and transcript) has **no in-tree unit test**. Only the opt-in Docker/MDK harness runs it, nightly and never in `ctest` or pre-push. That leaves no negative tests either: tampered ciphertext, SenderData naming another or empty leaf, a PublicMessage signature re-wrapped as a PrivateMessage, non-zero padding, and an Add of an unproven KeyPackage in a PrivateMessage Commit being refused in default mode. A regression here would get past every gate that runs by default. | File a bead to add a fixed MDK-generated PrivateMessage Commit vector (group state + commit bytes) to `test_interop.c`/`test_commits.c`, with positive, tamper and unproven-Add cases. This does not block the merge because the code checks out on review and the harness passes. |
| L1 | Low | `libmarmot/src/welcome.c:475` | `refuse_welcome()` now also permanently delists a Welcome when `mls_load("welcome_data")` fails, and that error is returned as `MARMOT_ERR_STORAGE`, which can be transient (locked or busy DB). Before, a failure here was recorded but the Welcome stayed pending. Now an invitation can be lost for good on an I/O hiccup. | Delist only when storage reports not-found, or keep the pending state on `MARMOT_ERR_STORAGE`. |
| L2 | Low | `libmarmot/src/welcome.c:139` | `refuse_welcome()` ignores the result of `save_welcome()`, while `record_welcome_failure(…, true)` still records the failure as final. If the save fails, the two records disagree: the Welcome stays listed as pending but is marked failed. | Check the result, or at least log it. |
| L3 | Low | `libmarmot/src/extension.c:384` | The 0.10.0 fallback applies to **all** input, including a Welcome's or a Commit's GroupContext from the wire, not only to groups already stored locally. It is strict and memory-safe, but it widens the parser surface, and peers keep accepting a layout MIP-01 does not define. Also, a MIP-01 **v1** extension without `image_upload_key` fails both decoders (`extension.c:206` accepts `version ≥ 1` but the v2 field set is mandatory). | Consider restricting the fallback to state loaded from storage, or document it as intentional. State in the README that v1 layouts are not read. |
| L4 | Low | `.github/workflows/marmot-mdk-interop.yml:14` | No `permissions:` block, so the job gets the repo's default `GITHUB_TOKEN` scope (possibly write) while building and running third-party code (MDK, crates.io deps). | Add `permissions: { contents: read }`. |
| N1 | Nit | `libmarmot/src/mls/mls_framing.h:418-432` | The new `mls_handshake_content_decode` doc comment and prototype were inserted between `mls_application_content_decode`'s doc comment and its prototype, which leaves that doc comment attached to the wrong function. | Move the new block above the orphaned comment. |
| N2 | Nit | `libmarmot/src/messages.c:932`, `:643` | Decrypt and encrypt failures still return `MARMOT_ERR_NIP44`, although NIP-44 is gone from this path. The API is stable, so keeping the code is fine, but the name now misleads. | Add a doc note, or an alias (`MARMOT_ERR_GROUP_EVENT_DECRYPT`) in a later MINOR. |
| N3 | Nit | `libmarmot/src/messages.c:912` | Each lookback epoch base64-decodes the same payload again (up to 6 times). This is harmless. | Decode once and loop `_with_key` over the derived keys. |
| N4 | Nit | `libmarmot/src/mls/mls_framing.c:1369` | In `padded_content_len()`, `next_power <<= 1` would wrap to 0 and loop forever if `len > SIZE_MAX/2 + 1`. The MLS content bounds make this unreachable in practice. | Add an early `len > SIZE_MAX/2` guard. |
| N5 | Nit | `.github/workflows/marmot-mdk-interop.yml:21,60`; `tests/interop/mdk/driver/Dockerfile:12,24` | Actions (`@v4`) and base images (`rust:1.93-slim-bookworm`, `debian:bookworm-slim`) are pinned by tag, not by SHA or digest. | Pin by commit SHA or image digest, to fit the "pinned MDK rev" posture. |
| N6 | Nit | `libmarmot/src/mls/mls_group.h:444` | The reflowed doc line is about 140 columns. | Rewrap. |
| N7 | Nit | `libmarmot/src/mls/mls_group.c:2974` | `private_commit_open()` casts away `const` on `group->secret_tree` and restores it afterwards. This is correct single-threaded, but a caller holding a `const MlsGroup *` may assume it is safe to read concurrently. | Document it in `mls_group_handshake_sender`/`commit_authenticate` ("mutates and restores; not concurrent-safe"), or pass a scratch copy. |

## Suggested follow-ups (not filed by this read-only review)
1. M1: an MDK PrivateMessage-Commit vector and negative tests in the default `ctest`.
2. L1/L2: make Welcome refusal robust to transient storage errors.
3. L4/N5: harden the workflow (least-privilege token, SHA/digest pins).
