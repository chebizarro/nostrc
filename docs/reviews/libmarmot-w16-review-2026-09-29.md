# libmarmot W16 review: filtered-path parent_hash (nostrc-lz4f) and LeafNode validation (nostrc-2io4)

- **Reviewer:** independent peer reviewer (AGENTS.md "Pre-Push Requirements → Peer Review")
- **Branch:** `marmot/w16-review` at `4f2306b5`, based on `b7789088`
- **Commits:** `4785fde3` (nostrc-lz4f), `4f2306b5` (nostrc-2io4)
- **Date:** 2026-09-29
- **Verdict:** **REQUEST CHANGES**. Blocking findings: **F1** and **F2**.

No code or beads were changed. All experiments (counterfactuals, mutations, cross-version runs) ran in a throwaway worktree (`/tmp/w16cf`), which was removed afterwards.

## Summary

The protocol work is careful and mostly correct.

- **nostrc-lz4f:**
  - The committer now links `parent_hash` along the *filtered* direct path, as RFC 9420 §7.9 requires.
  - It encrypts path secrets under the same canonical provisional GroupContext the receiver rebuilds.
  - The receiver now checks the committer leaf's `parent_hash` explicitly.
- **nostrc-2io4:**
  - One shared LeafNodeTBS routine now carries the §7.2 `group_id`/`leaf_index` suffix.
  - Update and UpdatePath LeafNodes now get §7.3/§12.4.2 validation, all on the staged clone.
- **Tests:** the new tests genuinely fail without each fix. The OpenMLS-signed MDK passive-client vectors pin the TBS encoding.
- **Memory safety:** ASAN+UBSAN and `leaks(1)` are clean.

Two issues block the push:

1. **F1: versioning.** 2io4 makes 0.3.7 reject every path-bearing Commit from ≤0.3.6 senders. That is a breaking wire change on a 0.x component, so AGENTS.md and repo precedent require a **MINOR** bump (libmarmot 0.4.0) plus README migration notes. The branch declares PATCH 0.3.7.
2. **F2: committer-side direct-path blanking.** `generate_update_path` never blanks the unfiltered part of the committer's direct path, although receivers do (RFC 9420 §7.4/§7.5). lz4f removed the empty-filtered-path shortcut, which used to mask this. As a result, a group persisted by ≤0.3.6 can now commit an Add whose Welcome can never be joined. Under 0.3.6 the same operation failed closed. I verified this.

## Findings

### F1 (High, blocking): 2io4 is a breaking wire change but is versioned as PATCH

**Where**
- Declared version: `libmarmot/CMakeLists.txt:18`, `libmarmot/meson.build:2` and `VERSION_MANIFEST.md:18`, all 0.3.7.
- Behaviour change:
  - The TBS suffix in `libmarmot/src/mls/mls_tree.c:527-531`.
  - UpdatePath leaf validation in `libmarmot/src/mls/mls_group.c:2929-2940`.

**Why it breaks policy.** AGENTS.md "Component Versioning Policy" says:
- Public surfaces include "documented configuration, storage, or wire formats".
- PATCH is for a fix "that adds no incompatible behavior".
- For a 0.x component, "use a MINOR bump for a breaking public-surface change and call out the incompatibility in the release notes".
- "A breaking change must not land without the corresponding version-source update."

Repo precedent agrees. `38c3de4f` took libmarmot from 0.1.0 to 0.2.0 because it was "a breaking wire change on a 0.x component, so it takes a MINOR bump plus migration notes in the README". That README changelog entry is at `libmarmot/README.md:232`. The 2io4 commit message itself says mixed groups "need every member on >= 0.3.7", which is an incompatibility by definition.

**Failure scenario (verified).** I built the same harness against `b7789088` (0.3.6) and `4f2306b5` (0.3.7). Alice and Bob share a group at epoch 1.
- **0.3.6 Alice → 0.3.7 Bob:** Alice (0.3.6) commits a self-update. Bob (0.3.7) gets `mls_group_process_commit` rc **−116** (`MARMOT_ERR_MLS_PROCESS_MESSAGE`) and stays at epoch 1, while Alice is at epoch 2.
- **0.3.7 Alice → 0.3.6 Bob:** the Commit is accepted (rc 0), so the break is one-directional.

The group forks at the first path-bearing Commit (Add, Remove, self-update) from any member who hasn't upgraded. There is no in-band recovery: Bob has to be removed and re-added.

In Gnostr the fork is silent. `apps/gnostr/plugins/mls-groups/gn-mls-event-router.c:167-172` only calls `g_warning` and drops the message, and every later kind-445 message stays undecryptable for Bob. Old peers really exist: `gnostr-v0.1.0-preview` is tagged and bundles the mls-groups plugin with libmarmot 0.1.0, which signs commit leaves without the suffix.

**Required**
- Bump libmarmot to **0.4.0** in CMake, Meson and `VERSION_MANIFEST.md`, with PATCH reset.
- Add a README changelog entry "0.4.0 (unreleased)" covering:
  - **Breaking wire change:** LeafNodeTBS for update/commit sources now binds `group_id<V>` and `uint32 leaf_index` (§7.2). ≤0.3.x path Commits are rejected, and every member must be on ≥0.4.0 before anyone sends a Commit with a path.
  - **Persisted groups:** the state format is unchanged and loads fine. But every leaf signed by ≤0.3.x stays in the tree until that member commits again: the creator leaf, and each member's last commit leaf. RFC-conformant joiners (MDK/OpenMLS) validate every LeafNode of a Welcome ratchet tree (§12.4.3.1) and will reject such trees. libmarmot joiners will too once nostrc-3hzu lands. Recommend a self-update after upgrading.
  - **Interop gain:** MDK/OpenMLS now accept libmarmot path Commits. Before this fix they could not, because the suffix was missing.
  - **lz4f wire behaviour:** a lone committer now sends a zero-node UpdatePath (§7.6).

### F2 (Medium, blocking): the committer does not blank its unfiltered direct path; the removed shortcut was masking it

**Where.** `libmarmot/src/mls/mls_group.c:225-246`: `generate_update_path` overwrites only the `fdp[i]` nodes. The receiver, by contrast, blanks the whole direct path first (`mls_group.c:603-609`, `mls_treekem_apply_update_path`).

**RFC.** §7.4, step 1: the committer blanks every node on its direct path, then installs keys on the filtered direct path. §7.5: a receiver merging the UpdatePath does the same.

**Why it matters now.** Before `4785fde3`, an empty filtered path made the committer regenerate its *full* direct path (the removed shortcut). That incidentally overwrote every direct-path node. Without the shortcut, a direct-path node that is non-blank but filtered out (its copath is empty) stays non-blank at the committer and is blanked at every receiver.

A fresh 0.3.7 tree cannot contain such a node, because each Remove blanks the removed leaf's direct path. Trees built by ≤0.3.6 through the shortcut do contain them.

**Failure scenario (verified).**
1. With 0.3.6, Alice creates a group, adds Bob and Charlie (4-leaf tree), removes Bob, then removes Charlie. The shortcut installs parents at nodes 1 and 3, both over an empty copath. I persisted Alice's state:
   `0:L 1:P(ph=32) 2:_ 3:P(ph=0) 4:_ 5:_ 6:_`
2. With 0.3.7, I load that state. `mls_group_add_member(Dave)` returns **0**, and Alice advances to epoch 5. Her filtered path is `[1]`, so node 3 stays non-blank.
3. Dave's `mls_welcome_process` returns **−71**: "welcome invalid: tree hash/parent hash" (`mls_welcome.c:698-699`). Node 3 has no parent-hash-valid descendant.

With 0.3.6 the same Add failed closed with −4 (the lz4f bug), so no Commit was emitted. The branch therefore turns a fail-closed error into a committed epoch that includes a member who can never join. A fresh 0.3.7 run of the same sequence is fine: all parents are blank after the last Remove, and Dave joins.

**Fix**
- In `generate_update_path`, blank the committer's full direct path before installing the filtered nodes, mirroring lines 603-609.
- Add a regression that seeds a non-blank, filtered-out direct-path parent (for example, a parent key at node 3 of the lone-member tree). Check that the next Add's Welcome joins and that the committer's tree hash equals a receiver's.
- The existing lz4f test cannot catch this: in its scenario the Remove has already blanked node 3 (`test_mls_group.c:2285`).

### F3 (Low): 11 of the new validation branches are untested (mutation survivors)

I disabled each branch below on its own at `4f2306b5` and rebuilt. In every case, `test_mls_group` and `test_marmot_interop` still passed.

| # | Check disabled | Location (`mls_group.c`) | RFC 9420 |
|---|---|---|---|
| M1 | Update must change `encryption_key` | 2535-2538 | §7.3 step 7 |
| M2 | Installed leaf `signature_key` unique | 2558-2559 | §7.3 step 8 |
| M3 | UpdatePath *node* keys fresh / not repeated (only the leaf key kept) | 2580 | §12.4.2 |
| M4 | UpdatePath leaf `signature_key` not another member's | 2594-2599 | §7.3 step 8 |
| M5 | Credential type mutually supported with every member | 2489-2498 | §7.3 step 4 |
| M6 | Leaf extensions listed in its own capabilities | 2500-2503 | §7.3 step 6 |
| M7 | `required_capabilities` contents | 2429-2430 | §7.3 step 3 |
| M8 | mls10 version / group ciphersuite advertised | 2476-2482 | §7.3 step 3 / §7.2 |
| M9 | Commit leaf `parent_hash` empty or hash-sized | 2457-2459 | §7.2 |
| M10 | Update leaf carries no `parent_hash` | 2454-2456 | §7.2 |
| M11 | Update target is a current member | 2530 | §12.1.2 (defence in depth) |

The most valuable to cover are:
- **M1 and M3:** nothing else catches them. For M3 this holds while nostrc-11r1 is open.
- **M2/M4 and M7.**

`build_update_path_for_test()` and `make_update_leaf_for_test()` make each of these a few lines.

### F4 (Low): version decisions for dependent components are not recorded

AGENTS.md "Updating versions", step 5, says: "Mention all version decisions (including 'no bump') in the bead and peer review request."

libmarmot builds as a static library (`libmarmot.a`). It is linked into marmot-gobject (`marmot-gobject/CMakeLists.txt:102-103`), the Gnostr mls-groups plugin and Groundhog, so their shipped behaviour changes with F1. Neither the commits nor the beads state a decision for marmot-gobject 1.1.0, gnostr 0.1.0 or groundhog 0.8.0. Precedent `0af22a5b` recorded "marmot-gobject: no change, no bump". Record the same kind of line when fixing F1.

### Informational (no action required for this branch)

- **I1: duplicate Updates are accepted.** Two by-reference Updates for the same leaf are both validated against the pre-Commit leaf, and the second silently wins. §12.2 makes that Commit invalid. This is tracked in nostrc-nh0r.
- **I2: node keys vs decrypted path secrets.** A mismatch between UpdatePath node keys and the decrypted path secrets is still accepted. This is tracked in nostrc-11r1.
- **I3: capability enforcement vs y3iy.** libmarmot receivers now enforce §7.3 step 3, while libmarmot's own commit leaves are reset to Marmot capabilities with no non-default proposals (nostrc-y3iy). libmarmot-created groups carry only 0xF2EE, so this is fine today. But a group whose `required_capabilities` lists a proposal type such as SelfRemove 0x000a would reject libmarmot Commits. MDK KeyPackages advertise that type in `protocol-vectors.json`. I could not confirm MDK's group configuration offline. Consider raising y3iy's priority.
- **I4: test count.** The commit evidence says "ctest 21/21". The same `-R 'marmot|mls'` filter now selects 22 tests, including `marmot_gobject_test` and `groundhog-store-marmot`. All pass.

## RFC 9420 conformance

### §7.2 LeafNodeTBS: conforms

`mls_leaf_node_tbs_serialize()` (`mls_tree.c:484-534`) follows the struct in order:
- `encryption_key`, `signature_key`, then the Credential (type + identity).
- Capabilities: versions, ciphersuites, extensions, proposals and credentials.
- `leaf_node_source`, then the source-dependent field: Lifetime for key_package, `parent_hash<V>` for commit, nothing for update.
- `extensions<V>`.
- For update/commit sources only, `group_id<V>` and `uint32 leaf_index`.

All `opaque*` writers use the QUIC varint length prefix (`mls_tls.c:156-182`), so `<V>` is encoded correctly.

A NULL `group_id` fails for update/commit, and an unknown source has no TBS. The two private copies are gone, and KeyPackage signing uses the same routine with no suffix.

The encoding is pinned by a real peer, not just by self-consistency. The unit tests sign and verify with the same function, so they could not detect an encoding error. The OpenMLS-signed passive-client Commits could: with the suffix removed, `passive-client-handling-commit.json` vector 0 fails with rc −116.

### §7.3 LeafNode validation: conforms (coverage gaps in F3)

`leaf_node_validate()` (`mls_group.c:2442-2512`):

| Step | Check |
|---|---|
| 1 | Basic credential, identical identity to the replaced leaf (successor policy, §5.3.1). |
| 2 | Signature over the suffixed TBS, bound to the target leaf. |
| 3 | mls10, the group ciphersuite and the leaf's own credential type are advertised. Every non-default GroupContext extension is supported, and `required_capabilities` is parsed with the default exemptions from §7.2. |
| 4 | Credential type is mutually supported with every other member. |
| 5 | Correctly skipped: only key_package leaves carry a lifetime. |
| 6 | Leaf extensions appear in the leaf's own capabilities. Malformed or duplicate-type lists fail. |
| 7 | Source matches, and an Update changes `encryption_key` (`:2535`). |
| 8 | `leaf_keys_unique()` after the proposals; `update_path_keys_fresh()` for the path. |

Update leaves are checked against the GroupContext extensions of the epoch being entered (the last GCE). The UpdatePath leaf is checked against `group->extensions_data` after any GCE has been applied. Both are correct.

### §7.9 parent hash over the filtered direct path: conforms

- **Sender** (`mls_group.c:248-279`): walks top-down over `fdp`. The topmost filtered node, root or not, keeps an empty `parent_hash`. Each lower node gets `ParentHash(fdp[pos])`, taken over the child subtree that does not contain the committer; skipped levels are not links. The leaf gets `ParentHash(fdp[0])`, or empty when `fdp` is empty. This matches OpenMLS's top-down loop over the filtered path.
- **Receiver** (`:625-657`): mirrors the sender. It now requires the committer leaf to be commit-source with `parent_hash == ParentHash(fdp[0])` (empty for an empty path), as §7.5/§7.9.2 require, then runs the full-tree `mls_tree_verify_parent_hashes`.
- **Vector coverage:** instrumented, the interop run hit 66 of 302 `mls_treekem_apply_update_path` calls whose topmost filtered node is not the root. So the MDK treekem and passive-client vectors exercise the lz4f shape on the receive side. No vector has an empty filtered path, but that case has no receivers in practice.
- **Compatibility of lz4f alone:** 0.3.6 receivers already skipped the top node's link, so lz4f by itself is wire-compatible. Only 2io4 is the break (F1).

### Encryption-context GroupContext for path secrets (§7.6, §12.4.1/§12.4.2): conforms

The sender now uses `mls_group_tree_hash()` (`mls_group.c:302-312`): the tree hash of the tree after the path, over the canonical (truncated) representation, together with `epoch + 1`, the *old* confirmed transcript hash and the current extensions. The receiver builds the same values from a serialize/deserialize round trip (`:3034-3045`). It then cross-checks that tree hash against the final tree with `sodium_memcmp` (`:3134`).

Before the fix, a Remove that truncated the right edge made the two hashes differ, per §12.1.3. Counterfactual 2 below reproduces exactly that: with the fix reverted, Bob rejects Alice's removal Commit.

### §12.2 Commit validity: conforms for this scope

`update_proposal_validate()` (`:2521-2540`) rejects:
- inline Updates, because a by-value proposal counts as sent by the committer;
- by-reference Updates that the committer itself proposed.

The target comes from the authenticated framing of the proposal in the proposal store (`:2008-2019`, `:2041-2043`). An Update whose leaf a Remove in the same Commit blanks again fails key uniqueness. The remaining §12.2 rules are tracked in nostrc-nh0r.

### §12.4.2 UpdatePath processing and key uniqueness: conforms on receive; committer gap (F2)

Before any part of the path is merged, and against the tree after proposals:
- The leaf is validated with source commit, bound to the committer's leaf (`:2929-2940`).
- `update_path_keys_fresh()` rejects any path public key already present in the tree, which covers "different from the committer's current leaf", and any key repeated within the path.
- The leaf signature key must not belong to another member.

The committer side does not blank its direct path (F2).

## Cross-cutting checks

- **No mutation on failure:** holds on the receive side. Every new check runs on the staged clone before `*live_group = staged`. The tests compare the full serialized state after each rejection (`assert_group_matches_snapshot_for_test`). The committer side still mutates the live group before `generate_update_path` can fail (nostrc-hexf, open). lz4f removes the known trigger but not the pattern.
- **Constant-time comparisons:** the diff compares only public values with `memcmp`: public keys, credential identities, parent hashes. The one new secret-adjacent comparison, the leaf `parent_hash`, uses `sodium_memcmp`. The confirmation tag and tree hashes remain `sodium_memcmp` (`:3134`, `:3200`). No secret is compared with `memcmp`.
- **Tests fail without the fix:** confirmed. Each change below was made alone:

  | Counterfactual | Result |
  |---|---|
  | lz4f: `mls_group.{c,h}` reverted to `b7789088`, new test kept | `test_remove_filters_root_from_committer_path` aborts (rc 134) |
  | lz4f: only the HPKE context reverted to `mls_tree_root_hash` | Same test aborts at Bob's `process_commit` of the removal |
  | 2io4: the four validation call sites disabled | All 7 Update and all 4 UpdatePath defect cases are **ACCEPTED** and reach the derived epoch; `assert(accepted == 0)` fires |
  | 2io4: TBS suffix removed | MDK passive-client rc −116; `test_group_creator_leaf_signature_bound_to_group` aborts |

- **Interop vectors:** real coverage.
  - The passive-client Commits go through `mls_group_process_commit_ex_with_psks`, so they pass the full new validation: 120 commit-source and 2 update-source LeafNode validations.
  - The treekem vectors now also check the new leaf `parent_hash`.
  - The lz4f non-root-top shape is covered (see §7.9 above).
- **Compatibility**
  - **Wire:** see F1.
  - **Persisted state:** the format is unchanged. Existing leaves are not re-validated on load or when processing a Commit. The caveats about ≤0.3.x-signed leaves are in F1; the shortcut-built trees are F2.
  - **Groundhog:** `GhStoreMarmot` stores opaque `mls_group` blobs (`gnome/groundhog/src/store/gh-store-marmot.c:16`). The Marmot app integration (nostrc-qp24.13) is not live, and `groundhog-store-marmot` passes. Only a rebuild is needed.
  - **Gnostr mls-groups:** gains no new API but inherits the fork behaviour and silent drop described in F1.
  - **marmot-gobject:** API unchanged; the version decision is F4.
- **Memory safety:** ASAN+UBSAN (`-DSANITIZE=address,undefined`, Debug; ASAN runtime confirmed with `otool -L`) passed 20/20 `^marmot_test` targets. `leaks --atExit` reports 0 leaks for both `test_mls_group` and `test_marmot_interop`.

## Out-of-scope observations (pre-existing; please file beads)

- **O1 (suggest P1): the committer-side Add never records `unmerged_leaves`.**
  - **Where:** `mls_group_add_member` (`mls_group.c:1154-1165`) places the new leaf but never appends it to the non-blank parents on its direct path. `process_commit_impl` does append it (`:2808-2821`, §12.1.1). Whenever the new leaf lands under a non-blank parent that is not on the committer's filtered path, the committer's tree diverges from everyone else's.
  - **Repro (verified identically at `b7789088` and `4f2306b5`):**
    1. Members at leaves 0–6 of an 8-leaf tree.
    2. Leaf 0 removes leaf 5.
    3. Leaf 4 self-updates, which sets node 11.
    4. Leaf 1 adds a member, who lands at leaf 5.
    5. Every existing receiver rejects the Commit (rc −4, −116 for leaf 4 per nostrc-va60), and the joiner's Welcome fails with −71.
  - No existing bead covers this.
- **O2: silent forks in Gnostr.** The mls-groups router drops rejected Commits with a `g_warning` only. A member that falls behind an epoch gets no signal. Consider surfacing "group out of sync" state.

## Verification log

- `git submodule update --init third_party/nostrdb third_party/nsync`: ok.
- `cmake -S . -B /tmp/w16mr -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w16mr`: ok (2233 steps). New code in `libmarmot/src/mls` adds no warnings.
- `ctest --test-dir /tmp/w16mr -R 'marmot|mls' -j6`: **22/22 passed**.
  - Interop: MDK tree-validation 14 trees; treekem 62 UpdatePaths across 11 cases; passive-client 22 clients, 226 commits, 0 XFAIL; 10373 asserted checks.
- ASAN+UBSAN build (`/tmp/w16asan`): `ctest -R '^marmot_test'` **20/20 passed**.
- `leaks --atExit`: `test_mls_group` 0 leaks; `test_marmot_interop` 0 leaks.
- Counterfactuals, mutations, cross-version runs and the F2/O1 repros used throwaway harnesses in `/tmp/w16cf`, built against `b7789088` and `4f2306b5`. They are not committed.

## Recommendation

**REQUEST CHANGES.**

**Must fix before push**
- **F1:** bump libmarmot to 0.4.0 with README migration notes.
- **F2:** blank the committer's full direct path in `generate_update_path`, and add the seeded regression.

**Should fix in the same pass**
- **F3:** add tests for at least M1, M2/M4, M3 and M7.
- **F4:** record version decisions for marmot-gobject, gnostr and groundhog.

**Please file beads for** O1 (P1) and O2.

The rest of the lz4f and 2io4 work is RFC-faithful and well tested. I'd expect to approve quickly once F1 and F2 are addressed.
