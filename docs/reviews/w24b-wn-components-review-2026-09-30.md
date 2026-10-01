# W24b review: slice I, White Noise groups admitted, group components read (nostrc-qp24.5.2, nostrc-m6tp)

- **Reviewer:** independent peer reviewer (AGENTS.md "Peer Review")
- **Branch reviewed:** `marmot/w24b-wn-components` at `169c64b7`, one commit on `7f73738f` (master)
- **Review branch:** `review/w24b-wn-components` (this document only)
- **Date:** 2026-10-01 (file named for the W24 review series)
- **Verdict:** **APPROVE-WITH-NITS.**
  - The slice is correct on its own. Both new codecs agree with MDK v0.11.0's own decoders on 360,000 differential inputs. No sanitizer finding came up on macOS or under Linux LSan. All eight revert mutations are caught, and the live MDK 0.11 run is real.
  - **M1 is a merge condition, not a defect of this commit.** Slice H's WIP (`31dab75a`) refuses every Commit that changes `0x8006` or `0x800b`, and it carries a second copy of the component validators. Whichever slice lands second must switch H to this slice's validators. Two H tests flip on a trial merge (details below).
  - Everything else is Low or Nit.

| Commit | Beads | Change |
|---|---|---|
| `169c64b7` | nostrc-qp24.5.2, nostrc-m6tp | libmarmot 0.12.0 (unreleased) adopted admission now accepts SelfRemove, `0x8006` (receive role `0xF2D1`) and `0x800b`, and validates `0x8002`/`0x8005`/`0x8007` wherever present. Adopted leaves and KeyPackages advertise the White Noise set. New `marmot-group-components.h`: 0x8006/0x800b codecs and `marmot_get_group_components()`. MDK verdict corpus. driver-0.11 gains `white_noise` and marmot-app's invite precheck. New harness case `white-noise-welcome` |

No code or beads were changed. Scratch work lived in:
- `/tmp/rv-w24b-wn-fuzz`: the C harness, the Python drivers and the corpus;
- `/tmp/rv-w24b-wn-mdk`: a private MDK v0.11.0 clone holding the committed `policy-verdicts` crate, rebuilt from source; its `Cargo.lock` diff adds only that crate, so `url` stays at 2.5.8;
- `build-asan` in the review worktree.

Mutations were applied one at a time and reverted. The live run used a privately tagged driver image (`nostrc-mdk-interop:rv-w24b-wn`, removed afterwards), so the shared `:0.11.0` tag was never touched. The trial merge was a dangling commit in a scratch worktree, now removed. The sanitizer gate reused its shared volumes, and the volume list before and after is identical.

## Summary by focus area

### 1. Codec conformance: holds, byte for byte

**`0x8006`** (`group_image.c` `marmot_agent_text_stream_policy_decode`) matches MDK `AgentTextStreamQuicPolicyV1::decode_component_state` + `validate()` (`crates/traits/src/agent_text_stream.rs:152-226`):
- exactly 12 bytes: u8 required, u8 allowed, then BE u32 frame, BE u32 ttl and BE u16 padding;
- a non-empty required mask, and no unknown bits in either mask;
- required ⊆ allowed;
- `1 ≤ frame ≤ 65519`, `ttl ≤ 300`, `padding ≤ 4096`.

The spec (`agent-text-stream-quic-v1.md`) gives the same rules. `user_to_agent_default()` encodes to `010300001000000000000000`, MDK's own test vector.

**`0x800b`** (`marmot_group_media_policy_decode`) matches `decode_encrypted_media_policy_v2` (`app_components/encrypted_media_v2.rs:121-207`):
- **Outer vectors.** Three QUIC-varint vectors with identical bounds: 64, then 16×66 = 1056, then 16×2118 = 33888 (`encrypted_media.rs:15-20`). Shortest prefixes only: MDK's `decode_quic_varint` and libmarmot's `mls_tls_read_vli` both refuse non-minimal ones, and the fuzzer emits them.
- **Format and locator kinds.** The format is exactly `encrypted-media-v2`. There are 1..16 unique kinds of 1..64 bytes of `[a-z0-9-]`.
- **Endpoints.** 1..16 endpoints, unique as (kind, url) pairs, and each kind must be allowed. The kind is checked before the URL, as MDK does.
- **Endpoint URLs.** A URL is judged three ways: valid, invalid, or unverified (kept, never contacted), as for `0x8007`.

**Differential run.** I compared libmarmot (ASAN+UBSAN) against MDK's own decoders, rebuilt by me from the committed generator. The inputs were 6 seeds × 30,000 `0x8006` + 30,000 `0x800b` states: structural generators biased to the boundaries, non-minimal varints, a 100-URL pool of WHATWG edge cases plus random URL mutations, and byte-level corruption.
- **`0x8006`:** exact agreement on all 180,000 inputs. Both accept the same 10,820; every accepted state re-encodes byte-identically.
- **`0x800b`, the hard properties:**
  - never refuses an MDK-valid state (0);
  - never calls a state *verified* that MDK refuses (0);
  - every verified state (4,304) re-encodes byte-identically.
- **`0x800b` residual:** 946 states accepted *unverified* that MDK refuses. 942 of them are invalid IPv4 or IPv6 literals: see **L2**.

**Whole GroupContext.** I ran 90,000 structurally mutated White Noise GroupContexts through `mls_adopted_group_context_parse()` and `marmot_adopted_components_from_extensions()`. The mutations swapped in fuzzed `0x8006`/`0x800b` states, added random `0x8002`/`0x8005`/`0x8007`/`0x8008`/`0x800a`/`0x9000` entries, perturbed the required list, reordered entries and corrupted bytes.
- The parse and the read side agree on every input.
- No path reports send or fanout as accepted.
- There was no sanitizer report.

The same 90,000-input corpus replays clean under **ASAN+UBSAN+LSan** in `nostrc-linux-ci:arm64`, with no leak on any error path.

### 2. The receive role (`0xF2D1`): obligations met for Groundhog, latent for gnostr

The spec (`agent-text-stream-quic-v1.md`; `features/agent-text-streams-quic.md:58-60,341-343`) defines `receive` as:
- "understands the component, accepts kind 1200 start payloads and final kind 9 stream anchors, and can safely ignore the live QUIC route";
- clients that rendered no preview "MUST treat the final kind 9 payload as normal chat text and MAY ignore the stream tags".

Groundhog meets that:
- `gh_message_new_from_mls()` admits only kind 9 (`gh-message.c:614`). Kind 1200 starts, and kinds 1201/1202, are processed (ratchet advanced) and dropped as "read but not shown" (`gh-mls-service.c:1952`).
- The final kind 9 is shown as chat, the spec's minimum.
- No QUIC, broker or endpoint candidate is ever contacted.
- The stream exporter is never derived.

Privacy and security:
- Advertising `0xF2D1` makes Groundhog look like every marmot-app client: every MDK front-end (`cli`, `marmot-c`, `marmot-uniffi`, `agent-connector`) uses marmot-app's registry, which advertises all three roles. It does not single Groundhog out.
- No authentication or confidentiality assumption depends on the role.

The gap is gnostr. Its mls-groups plugin shows **every** inner kind as a chat message (**L3**). That is harmless today, because gnostr cannot be invited into an adopted group while the KeyPackage producer is compiled off.

### 3. Fail-closed: holds; the every-member rule is spec-correct and stricter than MDK

What is refused:
- A group requiring `send` or `fanout` (`mls_app_components.c:617-630`) → `MARMOT_ERR_UNSUPPORTED`. The check runs whether or not `0x8006` is required, matching MDK's `required_role_capabilities_from_group`, which reads the component whenever it is present.
- `0xF2D2`/`0xF2D4` in `required_capabilities` → `UNSUPPORTED`.
- A required `0x8005` → `UNSUPPORTED`. A disappearing-messages White Noise group therefore fails closed on both sides: MDK makes every `CreateGroupRequest.app_components` entry required (`cgka-engine/src/key_package.rs:284-286`), and Groundhog's KeyPackage does not advertise `0x8005`.

The every-member `0xF2D1` check (`mls_app_components.c:829-835`) is exactly the spec's resulting-epoch invariant. MDK v0.11.0 enforces less:
- **Where MDK checks roles:** only on invitees (`key_package.rs:233,298`, `send.rs:210`) and on the joiner's *own* capabilities (`group_lifecycle.rs:1289`).
- **Where it doesn't:** its resulting-epoch check `validate_resulting_leaf_capabilities` (`app_components.rs:1252-1288`) checks `required_capabilities` and required components on every leaf, but **not** roles.

**Could it refuse a legitimate White Noise group?** I found no path:
- every marmot-app build since 0.9.0 advertises all three roles (`/tmp/mdk-v0.9.0` `marmot-app/src/lib.rs:2911-2926`);
- earlier MDK groups are legacy-profile and carry no `0x8006`;
- marmot-app's invite precheck refuses a role-less invitee.

What remains is an MDK/libmarmot asymmetry on crafted or third-party Commits: **L1**.

### 4. Read-side API: correct ownership; only the GroupContext's own secrets

- `marmot_get_group_components()` (`adopted.c:407`) loads, and so re-validates, the stored MLS state, and zeroizes the stored blob.
- It returns owned copies. `marmot_group_components_clear()` frees everything, and `marmot_group_blossom_image_clear()` `sodium_memzero`s the image struct.
- Every early return leaves `*out` zeroed and safe to clear.
- The parse-time `0x8002` decode in `validate_component_state()` clears its stack copy.
- No new table copies the image key (`MarmotGroup.image_*` stays unset for adopted groups, which the commit documents).

Exposed secrets:
- **`image_key`/`image_nonce`:** needed to render the image.
- **`image_upload_key`:** MDK exposes it to apps too (`marmot-app/src/conversions.rs:68`, `projection.rs:69`). See **N4**.

One hygiene gap: the loaded `MlsGroup`'s GroupContext bytes, which hold the same keys, are freed unzeroized (**L4**).

### 5. Harness honesty: a real live MDK run, a precise XFAIL

I rebuilt the driver-0.11 image from this commit, including its parity `cargo test`, and ran the whole `^groundhog-mdk011-interop` matrix live:
- **control:** passed;
- **four older cases:** XFAIL as before;
- **`white-noise-welcome`:** a real XFAIL.

Inside `white-noise-welcome`:
1. MDK 0.11's parser reports the libmarmot KeyPackage as `"profile":"Current"`, `mls_extensions ["0x0006","0xf2d1"]`, `mls_proposals ["0x0008","0x000a"]`, components `0x8001 0x8003 0x8004 0x8006 0x8009 0x800b 0x800c`.
2. marmot-app's invite precheck passes, and `create_group white_noise` yields a GroupContext with `0x8006` and `0x800b`.
3. Groundhog joins and reads both policies through `marmot_get_group_components()`.
4. Kind 9 flows both ways.
5. After Carol's rename, the test asserts the exact current state: same epoch, old name, the next message decrypt-pending and not change-refused, group active.
6. Any epoch advance is a hard `XPASS` error.

The XFAIL reason names the cause (adopted Commits, slice H). The driver's new precheck on every `create_group` is stricter than before, and the control and the older cases still pass with it.

### 6. Merge with slice H: textually clean, two test flips, one functional gap (M1)

`git merge-tree` of `marmot/w24b-adopted-commits` (`31dab75a`) and `169c64b7` gives a clean tree (`3da3b35e`); the two branches share six files. Building that tree, 22 of the 24 libmarmot tests pass. The two failures are semantic, and both are anticipated in H's own comments:
- **`test_adopted_departures`** (H `test_adopted.c:1930`) expects `marmot_can_self_remove()` to fail because "a leaf lacks" SelfRemove. With this slice every adopted leaf advertises it, so the call now succeeds.
- **`test_adopted_commits` "unsupported_component"** (H `test_adopted_commits.c:689`) expects `UNSUPPORTED` for a Commit setting `0x800b` to `00`. This slice's entered-epoch parse now refuses that malformed state first, as `MARMOT_ERR_MLS_PROCESS_MESSAGE`.

The untested, functional half is M1.

## Findings

### M1 (Medium; merge condition for whichever of H and I lands second): H refuses every `0x8006`/`0x800b` change and carries a second, divergent copy of the component validators

- **Where:**
  - slice H `libmarmot/src/commits.c:356-420` (`adopted_component_valid()`, `case ADOPTED_COMPONENT_AGENT_TEXT_STREAM_V1` / `MARMOT_COMPONENT_GROUP_ENCRYPTED_MEDIA_V2` → `MARMOT_ERR_UNSUPPORTED` at `:411-415`);
  - this slice's `libmarmot/src/mls/mls_app_components.c:538` (`validate_component_state()`, `static`).
- **Failure scenario:** after both land as they are:
  1. A White Noise admin commits a valid AppDataUpdate replacing the `0x800b` endpoints, or a valid `0x8006` policy (MDK allows both for admins).
  2. The MLS-layer entered-epoch check passes, because this slice validates both states.
  3. H's Marmot-layer dictionary check then returns `UNSUPPORTED`.
  4. Groundhog cannot follow that epoch, and every later message stays unreadable.

  The same happens to a libmarmot-created group in which a White Noise admin adds `0x8006` or `0x800b`; libmarmot's leaves now advertise support for both.
- **Second problem:** `0x8001`/`0x8002`/`0x8005`/`0x8007` are validated twice, once in each file, and the copies can drift. For example, H's `0x8002`/`0x8007` cases do not distinguish `MARMOT_ERR_MEMORY`.
- **Fix:**
  1. Export one validator from `mls_app_components.c`, e.g. `int mls_adopted_component_state_valid(uint16_t id, const uint8_t *d, size_t n)` = `validate_component_state()` without the `gc` side effects.
  2. Have H's AppDataUpdate check call it, keeping H's removal rules and lifecycle fail-closed. Delete H's copy.
  3. Update the two H tests above.
  4. Add a positive test: an MDK Commit that validly changes `0x800b` (and one that changes `0x8006` to another receive-only policy) is followed.

  Doing step 1 in this slice now would make H's rebase trivial.

### L1 (Low): the every-member `0xF2D1` rule is spec-correct but stricter than MDK, so crafted Commits split verdicts

- **Where:** `libmarmot/src/mls/mls_app_components.c:829-835`; it applies in Welcome trees, on load, and (with H) on every Commit's resulting epoch. MDK: `cgka-engine/src/app_components.rs:1252-1288` checks no roles.
- **Failure scenario:** a Commit that leaves a role-less leaf in a group requiring `receive`. Two ways to get one:
  - a non-marmot-app client, or a modified one, Adds a KeyPackage without `0xF2D1` to a White Noise group;
  - an admin adds `0x8006` to a group in which some member lacks `0xF2D1`. MDK's outbound `UpdateAppComponents` path also skips roles (`app_components.rs:555-580`).

  The result:
  - MDK members accept the epoch;
  - a libmarmot joiner refuses the Welcome (`MARMOT_ERR_VALIDATION`);
  - after H, a libmarmot member refuses the Commit and falls behind.

  No *legitimate* White Noise group triggers this (§3).
- **Fix:**
  - Keep the spec behaviour, and say in the README's Adopted-profile section that this is a deliberate divergence from MDK.
  - Raise it upstream: MDK should merge `required_role_capabilities_from_group()` into `validate_resulting_leaf_capabilities()`.
  - When H lands, make sure such a refusal surfaces as "fell behind" (change-refused), not as a silent stall.

### L2 (Low): endpoint (and avatar) URLs with provably invalid IP literals are accepted "unverified"

- **Where:** `libmarmot/src/group_image.c:762` (`url_classify()`: any normalizer failure becomes `AVATAR_UNVERIFIED`), with `host_ipv4()` at `:351-370`, which already knows when a host is WHATWG-invalid.
- **Failure scenario:** a crafted Welcome or Commit whose `0x800b` endpoint is `https://256.1.1.1/`, `https://1.2.3.4.5/`, `https://x.123/` or `https://[1::2::3]/`.
  - MDK refuses it ("invalid IPv4 address" / "invalid IPv6 address").
  - libmarmot admits it, with the endpoint flagged unverified and never contacted.
  - The spec says "Component-state validity is the same for every member".

  This was 942 of the 946 residual cases in the 360k-input differential. Only the accept direction is affected, and nothing is contacted. The behaviour already existed for `0x8007` (slice G); this slice extends it to `0x800b`.
- **Fix:** let `url_normalize()` report "WHATWG-invalid" separately from "outside the verifiable subset", and classify the first as `AVATAR_INVALID`. Cases:
  - `host_ipv4()` returns -1;
  - a numeric last label that is not a canonical dotted quad;
  - a bracketed host that fails IPv6 parsing, or carries a zone id.

  Add the four URLs above to the corpus's `never` list.

### L3 (Low, latent): gnostr would show agent-stream start payloads as chat; the `receive` claim is libmarmot-wide

- **Where:** `apps/gnostr/plugins/mls-groups/model/gn-group-message-model.c:69-120` (appends every decrypted inner event, whatever its kind); `libmarmot/include/marmot/marmot-group-components.h:17-18` (says only that *Groundhog* renders only the final message).
- **Failure scenario:**
  1. Once adopted KeyPackages are published (the producer gate), gnostr can join White Noise groups.
  2. An agent's hidden kind-1200 start arrives, with canonical-JSON metadata as its content.
  3. gnostr appends it to the timeline as a chat bubble, and also shows reactions and deletions as bubbles.

  That contradicts the `receive` semantics the shared libmarmot leaf now advertises for every consumer. Today gnostr publishes only legacy KeyPackages and cannot be invited into adopted groups.
- **Fix:** before enabling `MARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER` for any consumer:
  - make gnostr show only kind 9 as chat (and its kind 7/5 handling);
  - state in `marmot-group-components.h` that every consumer must keep kinds 1200–1202 out of chat history.

  Track this in nostrc-ji2j or a gnostr bead.

### L4 (Low): the read path leaves unzeroized heap copies of the group image secrets

- **Where:** `libmarmot/src/adopted.c:407-424` → `load_stored_group()` → `mls_group_deserialize()`; then `mls_group_free()` (`libmarmot/src/mls/mls_group.c:1148`) `free()`s `extensions_data` without `sodium_memzero`.
- **Failure scenario:** every `marmot_get_group_components()` call, and every load, leaves a freed heap copy of the GroupContext. When the group has a `0x8002` image, that copy contains `image_key` and `image_upload_key`, which can surface in a core dump or later reuse.

  The stored blob itself is zeroized. The pattern already existed, but GroupContexts now routinely carry group secrets, and this slice adds a public, repeatable reader.
- **Fix:** `sodium_memzero(g->extensions_data, g->extensions_len)` before the `free` in `mls_group_free()`, and on the deserialize failure paths.

### N1 (Nit): garbled file header in `mls_app_components.h`

`libmarmot/src/mls/mls_app_components.h:4-10`. The intro sentence ("The structural half of admitting an adopted-profile group (marmot-protocol/…") was overwritten by a fragment meant for the `mls_adopted_tree_check()` doc: "the founding Commit, as in MDK).  When the GroupContext carries 0x8006 …". The header now reads as nonsense. Restore the intro and move the 0x8006 paragraph to the `mls_adopted_tree_check()` doc (`:167-181`).

### N2 (Nit): stale creation comment

`libmarmot/src/adopted.h:35-38` still says a new adopted group's `app_components` requires `MLS_ADOPTED_SUPPORTED_COMPONENTS`. Since this commit it is `MLS_ADOPTED_CREATE_COMPONENTS` (5 ids, `adopted.c:164`).

### N3 (Nit): out-of-memory is reported as malformed component state

`libmarmot/src/group_image.c:48-49`: `read_vec()` returns -1 when `malloc` fails. The `0x8002`, `0x8007` and `0x800b` decoders turn that into `MARMOT_ERR_MEDIA_INVALID_REFERENCE`, and admission turns that into `MARMOT_ERR_EXTENSION_FORMAT`. The `err == MARMOT_ERR_MEMORY` checks at `mls_app_components.c:595,607,635` therefore never see a vector-allocation failure.

Under transient memory pressure, a valid White Noise Welcome could be recorded as malformed rather than retried. Return a distinct code from `read_vec()` for allocation failure.

### N4 (Nit): flag `image_upload_key` as a write credential

`marmot-group-components.h:169` calls `image_key` and `image_upload_key` "group secrets". The upload key is in fact the Blossom *write* key, which can replace or delete the group's image blob.

Exposing it matches MDK, but a render-only consumer needs neither to keep nor to copy it. Say so in the header, or keep the upload key out of the read struct until the write path (H) needs it.

### N5 (Nit): bead notes cite the pre-amend hash

The NOTES of nostrc-qp24.5.2 and nostrc-m6tp cite `a4999e2f`. The commit is `169c64b7`; the qp24.5.2 close reason has it right.

## Verification

macOS 27, with `/tmp/nostrc-macos27-env.sh`, `cmake -G Ninja -DBUILD_GROUNDHOG=ON`:

| Check | Result |
|---|---|
| `ninja` (full tree) | OK |
| `ctest -R marmot` | 25/25 passed |
| Full `ctest` (minus the MDK matrix) | 446/451. The 5 "Not Run" are the xvfb-gated `test_nostr_gtk_*` widget tests: their executables are not built on macOS, and nostr-gtk is untouched |
| `python3 scripts/check-unsequenced-args.py` | clean |
| ASAN+UBSAN (macOS): `test_media`, `test_adopted`, `test_kp_profile`, `test_commits` | pass |
| ASAN+UBSAN+**LSan** (`nostrc-linux-ci:arm64`): all 23 `marmot_test_*` | pass |
| Same image: replay of the 90,000-input fuzz corpus (0x8006, 0x800b, GroupContext) | rc 0, no report |
| `scripts/linux-gate.sh --sanitizers` (clean checkout of `169c64b7`) | 50 tests passed |
| Live MDK 0.11 matrix, driver image rebuilt from this commit (`-DBUILD_MDK011_INTEROP=ON`, private tag) | control passed; 5 XFAIL-skipped as documented; `white-noise-welcome` ran end to end |
| Differential vs MDK v0.11.0 decoders | 360,000 component states + 90,000 GroupContexts; no hard disagreement (§1) |
| Trial merge with slice H `31dab75a` | textually clean; 22/24 libmarmot tests; two expected flips (§6) |

Revert spot-checks: each mutation was applied alone, rebuilt under ASAN, and reverted. All 8 were caught.

| # | Mutation | Caught by |
|---|---|---|
| 1 | SelfRemove not an admissible requirement | `test_adopted:553` |
| 2 | every-member role check removed | `test_adopted:1544` (`test_white_noise_member_roles`) |
| 3 | send/fanout requirement accepted | `test_adopted:641` |
| 4 | `replay_ttl_secs <= 300` → `< 300` | `test_media` MDK-verdict corpus and `test_adopted:636` |
| 5 | KeyPackage/leaf drops `0xF2D1` | `test_adopted:1761` and `test_kp_profile:200` |
| 6 | duplicate `0x800b` endpoint accepted | `test_media` (verified, MDK refuses) |
| 7 | optional `0x8002` left unvalidated | `test_adopted:690` |
| 8 | endpoint query allowed | `test_media:1319` |

## Follow-ups to file (author)

- **M1:** a shared component validator, adopted by slice H, plus positive `0x800b`/`0x8006` update tests. This is a merge condition.
- **L1:** raise the role-invariant gap with MDK upstream, and note the divergence in the libmarmot README.
- **L2:** tighten `url_classify()` for WHATWG-invalid IP literals. This could go to nostrc-lyup or a new bead.
- **L3:** gnostr inner-kind filtering, required before the adopted KeyPackage producer is enabled for any consumer. This could go to nostrc-ji2j or a gnostr bead.
- **L4:** zeroize `MlsGroup.extensions_data` on free.

---

## Addendum (2026-10-01): re-review of the fixes, final verdict

- **Reviewed:** `marmot/w24b-wn-components` at `b991e6c3`, four commits on `169c64b7`:

| Commit | Addresses |
|---|---|
| `a562b94b` | M1 prep, N1 |
| `6fc97ccd` | L2 (with the macOS `inet_pton` fix) |
| `c3416e42` | L4, N3 |
| `b991e6c3` | L1, L3, N2, N4 (docs) |

  The review branch is rebased onto that tip; the original document above is unchanged.
- **Final verdict:** **APPROVE.** Every finding is resolved, or is now correctly owned by slice H or the merge. One new Nit (A1) is informational.

### Per finding

- **M1 (prep): resolved on this slice's side.**
  - **The validator.** `mls_adopted_component_state_valid()` (`mls_app_components.c:649-660`) is `validate_component_state()` itself, run on a zeroed scratch context. It cannot drift from admission, by construction.
  - **Fuzzed:**
    - 80,000 single states: 30,000 `0x8006`, 30,000 `0x800b`, and 20,000 random states for 13 other ids. The validator's code equals the decode verdict on every one: 0 when it decodes (`0x8006`: and requires only `receive`), `UNSUPPORTED` for send/fanout, `EXTENSION_FORMAT` otherwise.
    - 29,826 intact mutated White Noise GroupContexts. A parse of 0 implies every entry validates, and any malformed entry implies the parse is `EXTENSION_FORMAT`; zero violations. The other 28 cases had a corrupted outer extension type, which the parse rightly refuses first as `UNSUPPORTED`.
  - **Test:** `test_component_state_validator` covers valid replacement `0x8006`/`0x800b` states and every documented refusal. The header (`mls_app_components.h:161-190`) tells slice H to call it.
  - **Still open, on slice H:** H's current tip (`54971451`) still has `adopted_component_valid()` refusing `0x8006`/`0x800b` (`commits.c:411-415`). Switching it to the new validator, plus the positive update tests, stays the merge condition for whichever slice lands second. No bead records the hand-off yet; adding a note to nostrc-qp24.5.1 would help.
  - **Textual conflicts:** a trial merge of the two tips now has conflicts, all outside libmarmot sources: `VERSION_MANIFEST.md`, `libmarmot/README.md`, `tests/interop/mdk/README.md`, the `gnome/groundhog/CMakeLists.txt` case list and `test_mdk011_interop.c`. The source files merge cleanly.
- **L1: resolved.** The README "Divergence from MDK v0.11.0" section and the `mls_adopted_tree_check()` doc state the stricter rule and its consequences accurately. Upstream bead nostrc-0b99 is filed.
- **L2: resolved, and verified differentially.**
  - **The change.** `url_normalize_ex()` now reports a host that no WHATWG parser accepts, and `url_classify()` makes that `AVATAR_INVALID`. Such a host is either an IPv4 parse failure (`host_ipv4()` = -1) or a bracketed host that is no IPv6 address or carries a zone id. IDNA and `_` hosts stay unverified.
  - **The original corpus.** Rerun unchanged (6 seeds × 60,000): no hard disagreement, and the accepted-verified counts are unchanged. Accepted-unverified states MDK refuses fell from **946 to 20**.
  - **A new IP-literal corpus** (4 seeds, 112,477 stored URLs, raw and MDK-normalized): random IPv4 hosts (octal, hex, overflow, 1-6 parts, numeric last label after a domain) and IPv6 hosts (compression, upper case, embedded IPv4 tails with leading zeros, 256 or 3/5 parts, zone ids, unclosed brackets).
    - 0 disagreements with MDK, and **0** unverified-but-MDK-refuses.
    - The `0x8007` avatar class equals the `0x800b` endpoint class on every https URL.
    - All 1,944 bracketed hosts with a leading-zero IPv4 tail are refused, as MDK refuses them.
  - **The macOS `inet_pton` fix is needed.** macOS `inet_pton` accepts `::1.2.3.04` and `::01.2.3.4`; glibc refuses both. With the embedded-IPv4 check removed, macOS normalizes `[::1.2.3.04]`, which MDK refuses, and `test_media` catches it (R2 below).
  - **Platform parity.** The whole 723,120-line corpus gives byte-identical verdicts on macOS and on Linux (glibc, `nostrc-linux-ci:arm64`): the original, GroupContext, IP-literal and validator inputs.
- **L3: resolved as tracked.** `marmot-group-components.h:22-26` makes "show only kind 9 as chat" every consumer's duty, and names gnostr's gap. nostrc-ruwy (P1 bug) blocks gnostr from publishing adopted KeyPackages until it is fixed.
- **L4: resolved.** GroupContext bytes are now `sodium_memzero`ed in three places:
  - `mls_group_free()`, which also covers every `mls_group_deserialize()` failure path, since they `goto fail` → `mls_group_free`;
  - `mls_group_info_clear()`;
  - `apply_group_context_extensions()` when the context is replaced.

  The remaining `free(...extensions_data)` sites (`mls_group.c:1537,1898`) hold KeyPackage and leaf extensions, not GroupContexts.
- **N1: resolved.** The file header is restored, and the 0x8006 paragraph moved to the `mls_adopted_tree_check()` doc.
- **N2: resolved** (`adopted.h:38-39`).
- **N3: resolved.** `read_vec()` returns `VEC_NOMEM`, which every caller maps to `MARMOT_ERR_MEMORY` in the `0x8002`, `0x8007` and `0x800b` decoders. This is by inspection; allocation failure is not injected by any test.
- **N4: resolved.** The header now distinguishes the render secret from the Blossom write credential, and tells render-only consumers not to keep the upload key.
- **N5: resolved.** The nostrc-qp24.5.2 and nostrc-m6tp notes cite `169c64b7` and the fix commits; `a4999e2f` is gone.

### A1 (Nit, new, informational): 20 corner-case URLs remain unverified rather than invalid

These are the 20 residual cases, across 180,000 states:
- **IP literals the host charset check pre-empts.** Hosts with `_` or `~` inside a numeric-ending IPv4 (`https://1.2_3.04/`, `https://256.1~1.1/`) fail libmarmot's host charset before the IPv4 parser runs. Hosts with an unclosed `[` (`https://[\::ffff:1.2.3.4]/`: the backslash ends the authority) never reach the IPv6 parser. WHATWG refuses all of these.
- **IDNA and path forms**, the documented slice G residual.

Only the accept direction is affected, and the endpoints are never contacted. If wanted: run the numeric-last-label test before the charset check, and treat an unclosed `[` in the authority as WHATWG-invalid.

### Re-verification

| Check | Result |
|---|---|
| `ninja` (full tree) | OK |
| `ctest -R marmot` | 25/25 |
| `check-unsequenced-args.py` | clean |
| ASAN+UBSAN (macOS): `test_media`, `test_adopted`, `test_kp_profile`, `test_commits` | pass |
| ASAN+UBSAN+LSan (Linux): 23/23 `marmot_test_*`, plus the 723,120-line corpus | rc 0, no report |
| `scripts/linux-gate.sh --sanitizers` (clean checkout of `b991e6c3`) | 50 passed |
| MDK oracle | the committed `policy-verdicts` crate, rebuilt in a private MDK v0.11.0 clone; its main.rs is unchanged by the fixes |
| Docker volumes | unchanged |

Revert spot-checks of the fixes, each applied alone and reverted:

| # | Mutation | Caught by |
|---|---|---|
| R1 | `url_classify()` ignores `whatwg_invalid` | `test_media:1182` |
| R2 | embedded-IPv4 check in `host_ipv6()` removed | `test_media:1206` (macOS) |
| R3 | IPv6 bracket failure not flagged | `test_media:1182` |
| R4 | validator turns `UNSUPPORTED` into 0 | `test_adopted:1551` |
