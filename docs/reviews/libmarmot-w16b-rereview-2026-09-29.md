# libmarmot W16b re-review: va60, 5q55, 8u1k and the W16 remediation

- **Reviewer:** independent peer reviewer (AGENTS.md "Pre-Push Requirements → Peer Review")
- **Branch:** `marmot/w16b-commit-fixes` at `00c701c8`, stacked on `4f2306b5`
- **Commits reviewed:**
  - `5b184066` (nostrc-va60)
  - `e938bb31` (nostrc-5q55 + nostrc-v79g)
  - `3e113bbf` (nostrc-8u1k)
  - `00c701c8` (W16 F1–F4, O2)
- **Previous review:** `docs/reviews/libmarmot-w16-review-2026-09-29.md` (`1ab7d115`)
- **Date:** 2026-09-29
- **Verdict:** **REQUEST CHANGES**. One blocking finding: **N1**, the Gnostr `group-error` toast.

No code or beads were changed. Builds and experiments ran in throwaway worktrees (`/tmp/rr16b`, `/tmp/rr16b-cf`), which were removed afterwards.

## Summary

- **Every W16 blocking finding is resolved**, and each fix is pinned by a test that fails without it:
  - F1: libmarmot 0.4.0 with README migration notes.
  - F2: the committer blanks its full direct path.
- **The non-blocking findings are addressed:** F3 (mutation-pinning tests), F4 (version-decision table) and O1 (committer `unmerged_leaves`, nostrc-v79g).
- **The three new libmarmot fixes are correct against RFC 9420 and fail closed:**
  - va60: the committer keeps its path keys, and a staged copy is installed only on success.
  - 5q55: resolution minus added leaves on both sides, with ciphertext counts checked on every node.
  - 8u1k: a real GroupContextExtensions proposal.
- **Private keys in the new cache** are zeroized on prune, on overwrite and on free.
- **Build and tests:** full build ok; `ctest -R 'marmot|mls'` 22/22; ASAN+UBSAN 20/20.

The remaining blocker is the new Gnostr "out of sync" toast (**N1**). It fires on *any* per-event processing error, with no filtering or coalescing, so:
- relay replays of pre-join history can flood the open chat view with toasts;
- anyone who knows a group's public `h` tag can trigger a toast per event.

I raised O2 as an optional suggestion; this implementation of it regresses UX. Either narrow and coalesce the toast, or drop the Gnostr hunk from this branch.

## Previous findings: status

| W16 finding | Status | Evidence |
|---|---|---|
| **F1** (High, blocking): breaking wire change versioned as PATCH | **Resolved** | 0.4.0 in `libmarmot/CMakeLists.txt:16-18`, `libmarmot/meson.build:2` and `VERSION_MANIFEST.md:18`. `libmarmot/README.md` "0.4.0 (unreleased)" covers the mixed-version split, persisted ≤0.3.x leaves plus the self-update advice, the interop gain, the UpdatePath shape changes and the known gaps (nostrc-9ata, nostrc-il4i). |
| **F2** (Medium, blocking): committer does not blank its unfiltered direct path | **Resolved** | `generate_update_path` blanks the whole direct path before installing the filtered nodes (`mls_group.c:289-302`, RFC 9420 §7.4/§7.5). `test_committer_blanks_unfiltered_direct_path` (`test_mls_group.c:3503`) seeds the ≤0.3.6 shape and passes. With the blanking removed, it aborts at `f.alice.tree.nodes[3].type == MLS_NODE_BLANK`. |
| **F3** (Low): 11 untested validation branches | **Resolved for the requested set** | M1, M2, M3 (both sub-cases), M4 and M7 now each fail `test_mls_group` with exactly that case ACCEPTED. M5, M6 and M8–M11 still survive; I did not ask for them. |
| **F4** (Low): dependent-component version decisions not recorded | **Resolved** | `VERSION_MANIFEST.md` "Recorded version decisions" covers libmarmot MINOR, marmot-gobject no bump, gnostr no bump (0.1.0 unreleased) and groundhog deferred to the coordinator. It includes the release-note obligations. |
| **O1**: committer Add omits `unmerged_leaves` | **Resolved** (nostrc-v79g) | A shared `tree_add_unmerged_leaf()` (`mls_group.c:110`) serves both producer and processor. With the producer call disabled, `test_add_unmerged_leaf_off_committer_path` fails (rc ≠ 0). |
| **O2**: silent forks in Gnostr | **Implemented, but see N1** | |

## Findings

### N1 (Medium, blocking): the Gnostr `group-error` toast fires on any per-event error, uncoalesced

**Where**
- `apps/gnostr/plugins/mls-groups/gn-mls-event-router.c:167-178`: emits `group-error` for every GError from `marmot_gobject_client_process_message_finish`.
- `apps/gnostr/plugins/mls-groups/ui/gn-group-chat-view.c:72-93`: adds a new `AdwToast` for each emission, with no dedup or rate limit.
- The error sources:
  - `marmot-gobject/src/marmot-gobject-client.c:994-999` maps every non-OK `MarmotError` to a GError.
  - `libmarmot/src/messages.c:654-657`: a kind:445 event that no stored exporter secret can decrypt returns `MARMOT_ERR_NIP44`.
- The event supply: `apps/gnostr/plugins/mls-groups/mls-groups-plugin.c:251-262` subscribes to `{"kinds":[445],"limit":500}`, with no `#h` or `since`. There is no per-event dedup on intake, and `parse_group_event` does not verify signatures (kind:445 uses ephemeral keys anyway).

**Failure scenarios**
1. **Replay flood.** A member who joined at epoch N has the group chat open when a relay (re)connects. The relay replays up to 500 stored kind:445 events. Every event from before the member joined fails the NIP-44 layer. It is never recorded as processed, so it fails again on every replay. Each failure is one "This group may be out of sync" toast, and `AdwToastOverlay` queues them, so the view shows toasts for minutes. The member is *not* out of sync.
2. **Outsider spam.** The `h` tag (nostr_group_id) is public on relays. Anyone can publish arbitrary kind:445 events carrying it. Each one yields `MARMOT_ERR_NIP44`, and therefore a toast in every member's open chat view. Nothing bounds this.

**Why this is fixable cheaply.** The NIP-44 layer is keyed by the epoch exporter secret, which only members hold. An event that passes it is a member's.
- **A real divergence** looks like this: a peer's Commit decrypts at the NIP-44 layer (same epoch) and then fails in MLS with `MARMOT_ERR_MLS`. With nostrc-9ata open, that happens for every peer Commit.
- **Noise** fails before that point: `MARMOT_ERR_NIP44`, `GROUP_NOT_FOUND`, deserialization errors.

**Fix.** Either of these is fine:
- Emit `group-error` only for errors raised after NIP-44 authentication. Check `error->code` against `MARMOT_ERR_MLS` and related codes. Then coalesce: at most one toast per group until `group-updated`, or a single persistent banner instead of toasts.
- Drop the Gnostr hunk from this branch and file it separately. O2 was optional.

### N2 (Low): the receiver-side key prune (forward secrecy) is not pinned by a test

**Where.** `prune_own_path_keys(group)` in `process_commit_impl` (`mls_group.c:3533`).

**What.** With this call removed, all tests pass, both unit and interop. The code is correct. The gap: the va60 test (`test_mls_group.c:2501-2509`) never checks the receiver's cache right after Bob's Remove. Bob's filtered path is `[1]`, so the Remove leaves root node 3 blank. Alice would keep her old node-3 private key until the following re-Add re-keys node 3 and overwrites it, and that is the only point the test observes. That is exactly the window in which a stale private key would outlive its epoch.

**Fix.** Call `assert_path_keys_current_for_test(&f.alice)` right after delivering the Remove.

### N3 (Low): the Add producer leaks `pre_gc` on its error paths (pre-existing; now exercised)

**Where.** `add_member_staged` returns without `free(pre_gc)` on these paths:

| Line | Failure |
|---|---|
| 1263-1264 | `mls_tree_add_leaf` |
| 1294-1302 | KeyPackage copy |
| 1313-1316 | `generate_update_path` |
| 1322-1342 | Commit build / serialize |
| 1350-1355 | `begin_commit_public_message` |

`remove_member_staged` and `path_commit_staged` free it.

**Evidence.** `leaks --atExit -- test_mls_group` reports **1 leak, 272 bytes**, allocated in `mls_group_context_build` ← `add_member_staged` ← `mls_group_add_member`, and triggered by `test_commit_producers_fail_closed`. ASAN on macOS has no LeakSanitizer, and CI runs with `detect_leaks=0` (nostrc-ugrz). That is why the commit's "ASAN+UBSAN … no reports" did not catch it.

**Impact.** 272 bytes per failed Add. No correctness impact.

**Fix.** Use a single cleanup label. While there, zeroize `root_path_secret` on the failure after `generate_update_path`.

### N4 (Low, hardening, pre-existing): secrets can survive in freed serializer buffers

**Where.** `mls_group_serialize` starts from a 4 KiB `MlsTlsBuf`. `buf_ensure` grows it with plain `realloc` (`mls_tls.c:52`). The signature private key, leaf private key and epoch secrets are written before the tree. Once the tree pushes the blob past 4 KiB (roughly 15 or more members), the freed block still holds them.

**Context.** `group_stage_clone` (`mls_group.c:1014`) now zeroizes the final blob, which is an improvement. But staging now serializes on every produced Commit as well as every processed one.

**Fix.** Pre-size the buffer, or use a zeroizing grow for secret-bearing buffers.

### Informational

- **Tracked follow-ups:**
  - **nostrc-il4i (P1):** the Welcome has no `path_secret`, so a joiner cannot follow a Commit that encrypts to its lowest common ancestor with its adder.
  - **nostrc-9ata (P1):** the metadata Commit is discarded, and `marmot_process_message` never ingests Commits.

  Both predate this branch, are documented in the README, and are correctly out of scope. No consumer calls `marmot_update_group_metadata` (grep of marmot-gobject, Gnostr and Groundhog), so the discarded-Commit fork cannot be reached from any UI today.
- **MIP-01 admin checks on receive:** a receiver accepts a GCE from any member, and `nostr_group_id` immutability is not checked. 9ata tracks this. It is not reachable until Commits are ingested.
- **GCE producer in-path failure:** there is no test for a failure inside `path_commit_staged` for a GCE Commit. It uses the same staging wrapper as the tested producers.

## New code against RFC 9420

### va60: path-key cache and staged producers: correct

**Caching**
- `generate_update_path` caches `(node, sk, pk)` for each installed filtered node on the staged group (`:322`, RFC 9420 §7.4).
- Receivers cache the keys they derive from the lowest common ancestor upward.
- `remember_own_path_key` (`:2483`) reuses the same-node slot, overwriting the full secret key, and fails closed when the cache is full.

**Pruning** (`prune_own_path_keys`, `:2521`)
- It keeps only entries on the member's own direct path whose node is still a parent with the same public key.
- It runs after the committer installs its path (`:386`) and on the receiver's stage before install (`:3533`).
- Pruned entries are `sodium_memzero`ed.
- `lookup_own_path_key` requires a public-key match, so a stale entry can never decrypt.

**Bound.** After a prune the cache holds at most the tree depth. Before a prune it holds at most twice that, 62 < 128. The `_Static_assert(... >= 32)` pins this.

**Key lifetime**
- `group_stage_clone` zeroizes the serialized blob.
- `group_install_staged` frees the old state through `mls_group_free`, which zeroizes the signature key, leaf key, path keys and epoch secrets, and zeroes the moved-from stage.
- Stack copies of `MlsGroup` are released the same way.

**No mutation on failure**
- `mls_group_add_member`, `_remove_member`, `_self_update` and `_commit_extensions` all build on a stage and install it only when rc == 0.
- `result` is written only after the last failure point (e.g. Add at `:1613-1616`).
- `test_commit_producers_fail_closed` shows byte-identical state after an in-path HPKE failure for Add, Remove and self-update. With staging removed from Add, it fails.

**Callers.** No caller holds pointers into the group across these calls: `groups.c` loads, commits and serializes. So replacing the heap fields on install is safe.

### 5q55: resolution minus added leaves: correct

- One helper, `resolution_excluding()` (`:138`), serves the committer's encryption, the receiver's decryption index and a new receiver check. That check runs on every UpdatePathNode before decryption (`:3255-3272`): `secret_count` must equal |copath resolution \ added leaves| (§12.4.1/§12.4.2), and `node_count` must equal the filtered-path length (§7.6).
- `secret_count` is counted from the ciphertext bytes at deserialize time, not read from a claimed field, so the check cannot be spoofed.
- Checking every node, not just the receiver's own, makes all members accept or reject a Commit together.
- The filtered path is still computed over the post-proposal tree including added leaves. A node whose copath holds only new members keeps a zero-ciphertext entry, which matches OpenMLS; the passive-client vectors still pass.

### 8u1k: GroupContextExtensions (§12.1.7, §12.2): correct

- **Application:** `apply_group_context_extensions` replaces the list wholesale (§12.1.7).
- **Path required:** GCE is path-required through `commit_path_required`'s default branch (RFC 9420 §17.4).
- **At most one per Commit:** enforced in `validate_proposal_ordering` (`:2088`, §12.2).
- **Validation:** `group_context_extensions_validate()` (`:2702`) is shared by sender and receiver. It requires:
  - a well-formed list without repeated types;
  - no type libmarmot refuses (`app_data_dictionary`);
  - every member leaf supports each non-default extension and any `required_capabilities`.
- **Receiver scope:** the receiver runs this on the *post-proposal* tree (`:3193`), so added leaves are included and removed ones excluded (§12.1.7). The committer's leaf is exempt because its UpdatePath leaf is re-validated against the new extensions by `leaf_node_validate`.
- **Context and epoch:** `path_commit_staged` (`:1821`) captures `pre_gc` (the old context, for framing) *before* applying the GCE. It applies the GCE before `generate_update_path`, so the provisional GroupContext and the new epoch use the new extensions on both sides.
- **Marmot layer:**
  - The sender-side admin check is kept.
  - GroupData is merged field by field, and only the 0xF2EE entry is replaced. `nostr_group_id` comes from the current GroupData, not the config.
  - It fails closed without MLS state or a Commit.
  - The record changes only after the Commit succeeds.

### Gnostr UI

- `g_signal_connect_object(..., self, 0)` ties the handler's lifetime to the view.
- The group match accepts either the Nostr or the MLS id.
- The plugin does not translate strings anywhere, so the untranslated toast text is consistent with the rest of it.
- The one defect is the toast trigger policy: see N1.

## Counterfactuals (each change alone, at `00c701c8`)

| Change | Result |
|---|---|
| F2 blanking removed | `test_mls_group` aborts: `f.alice.tree.nodes[3].type == MLS_NODE_BLANK` |
| va60: `remember_own_path_key` in `generate_update_path` disabled | aborts: committer lacks its path key (`held`) |
| va60: committer-side prune removed | aborts: stale-key assertion |
| va60: Add not staged | aborts: fail-closed tree-hash mismatch |
| va60: receiver-side prune removed | **all tests pass (N2)** |
| 5q55: committer encrypts to added leaves | aborts: Bob cannot process `add_charlie` |
| 5q55: receiver ciphertext-count check removed | "UpdatePath encrypting to the added leaf ACCEPTED" |
| v79g: producer `tree_add_unmerged_leaf` removed | aborts (rc ≠ 0) |
| 8u1k: receiver GCE validation removed | "GroupContextExtensions a member cannot support ACCEPTED" |
| 8u1k: at-most-one-GCE removed | "two GroupContextExtensions proposals ACCEPTED" |
| 8u1k: sender GCE validation removed | aborts (`rc != 0`) |
| M1 / M2 / M3 / M4 / M7 | each fails with exactly that case ACCEPTED |
| M5, M6, M8, M9, M10, M11 | pass (not requested) |

The interop and protocol tests stayed green under every mutation above.

## Verification log

- Build: `cmake -S . -B /tmp/rr16b-build -G Ninja -DBUILD_GROUNDHOG=ON && ninja` on `00c701c8` ok. The new code adds no libmarmot warnings.
- `ctest --test-dir /tmp/rr16b-build -R 'marmot|mls' -j6`: **22/22 passed**.
  - MDK tree-validation 14; treekem 62 UpdatePaths; passive-client 22 clients, 226 commits, 0 XFAIL; 10373 asserted checks.
- ASAN+UBSAN (`-DSANITIZE=address,undefined`, Debug; ASAN runtime confirmed): `ctest -R '^marmot_test'` **20/20 passed**, no reports.
- `leaks --atExit`:
  - `test_marmot_interop`: 0 leaks.
  - `test_protocol`: 0 leaks.
  - `test_mls_group`: **1 leak, 272 bytes** (N3).

## Recommendation

**REQUEST CHANGES**, for N1 only.

- **N1:** narrow `group-error` to errors raised after NIP-44 authentication and coalesce it per group, or drop the Gnostr hunk from this branch.
- **N2 and N3:** small test/cleanup fixes worth making in the same pass.
- **N4:** hardening that can be filed separately.

With N1 addressed, the libmarmot work is ready to approve. F1 and F2 are resolved, and va60, 5q55 and 8u1k are RFC-faithful, fail closed and well pinned.

---

## Addendum: confirmation pass on `a152b005` (2026-09-29)

This is a focused re-check of the fix commit `a152b005` on `marmot/w16b-commit-fixes`, which stacks on `00c701c8`. It was built in a throwaway worktree, since removed. No code or beads were changed.

| Finding | Status | Evidence |
|---|---|---|
| **N1** (blocking): toast on any error, uncoalesced | **Resolved** | See below. |
| **N2**: receiver-side prune untested | **Resolved** | See below. |
| **N3**: `pre_gc` leak in `add_member_staged` | **Resolved** | See below. |
| **N4**: secrets left in freed serializer buffers | **Resolved** | See below. |

### N1: resolved

- **Filter.** `gn_mls_group_error_is_divergence()` admits only MLS-level codes in the `"marmot-gobject-client-error"` domain. That domain string matches `marmot_gobject_client_error_quark()` (`marmot-gobject-client.c:31-34`), so the filter does not silently drop everything. Inside `marmot_process_message`, `MARMOT_ERR_MLS` is returned only after the member-keyed NIP-44 layer has decrypted. Pre-join backfill and junk published under the public `h` tag fail with `MARMOT_ERR_NIP44`, `GROUP_NOT_FOUND` or parse errors, and are now debug-level only.
- **Gate.** A per-router gate reports at most once per group and is re-armed on a Commit result.
- **Test.** `gnostr-test-mls-group-error` passes. It covers the noise codes vs MLS codes, a foreign error domain, 500 failures producing one report per group, and a reset that re-arms only that group.
- **Informational residue:**
  - A member's message from a previous epoch still inside the lookback window also yields `MARMOT_ERR_MLS`, so one toast per group is still possible without real divergence.
  - The gate is only re-armed on `MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT`, which is never produced until nostrc-9ata lands. In practice that means at most one toast per group per session.

  Both are bounded and acceptable.

### N2: resolved

`test_committer_keeps_own_path_keys` now checks both receivers' caches right after Bob's Remove. With the receiver-side `prune_own_path_keys()` removed from `process_commit_impl`, `test_mls_group` aborts (rc 134) in `assert_path_keys_current_for_test`, on the stale node-3 key.

### N3: resolved

- Every failure in `add_member_staged` now leaves through `fail_wire_msg` / `fail_pre_gc`. That path frees `pre_gc` and wipes `root_path_secret` and `commit_secret`.
- The success path frees `pre_gc` only after the last `goto` (`:1623-1625`), so there is no double free.
- **`leaks --atExit -- test_mls_group`: 0 leaks for 0 total leaked bytes**, with all 58 cases passing. It was 1 leak of 272 bytes on `00c701c8`.

### N4: resolved

- `buf_ensure` now allocates, copies, `sodium_memzero`s and frees the old block instead of calling `realloc`.
- `mls_tls_buf_free` wipes `cap` bytes before freeing.
- No `MlsTlsBuf` is built by hand anywhere in `libmarmot/src`. The ownership transfers (`extension.c:203`, `credentials.c:339`, `groups.c:211`, `mls_group.c:4416`) null out `data` and leave `cap` alone, so `cap` is always accurate.

### Runs on `a152b005`

- Full build with `BUILD_GROUNDHOG=ON`: ok.
- `ctest -R 'marmot|mls' -j6`: **23/23 passed**, including `gnostr-test-mls-group-error`.
- ASAN+UBSAN (`^marmot_test`): **20/20 passed**, no reports.

### Final verdict

**APPROVED.** Every finding from both reviews is resolved and pinned by a test that fails without its fix. The remaining gaps are tracked beads (nostrc-il4i, nostrc-9ata, nostrc-3hzu, nostrc-11r1, nostrc-nh0r) and are documented in the libmarmot 0.4.0 README entry.
