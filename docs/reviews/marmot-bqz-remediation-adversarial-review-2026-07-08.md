# Adversarial Verification Review — libmarmot/marmot-gobject Production Remediation (epic nostrc-bqz)

**Date:** 2026-07-08
**Reviewer:** independent adversarial pass (skeptical verification of other agents' closed issues)
**Branch:** master (post-merge 9f22a3f3)
**Build/test:** `ctest --test-dir build -R marmot` → **19/19 PASS** (with nostrdb ON)
**Method:** read production code + tests directly; spot-checked every load-bearing probe claim against source; ran the full suite.

---

## Bottom line

The reviewed surface is **substantially genuine — not green theater.** The crypto/auth core (HPKE KATs, commit auth, welcome auth, KeyPackage/LeafNode validation, parent-hash, secret-tree OOO/FS) is backed by **real RFC-conformant code and real *asserting* negative tests** that reject tampered signatures, wrong confirmation tags, and corrupted parent hashes. I could not find a faked or test-weakened crypto fix.

Two real gaps remain, neither an auth bypass:

1. **Wire format (yfk/6s3) — WEAK / the one thing that regressed the "leave it red" instruction.** `mls_message_deserialize()` has an **opaque-preservation fallback** (`mls_framing.c:1105-1128`) that returns success even when `PublicMessage`/`PrivateMessage` body parsing fails. The MDK `message-protection`/`messages` interop assertions are byte-preserving roundtrips, so they now pass **via the fallback, not via a proven semantic parser.** The interop test the epic memory said to keep red is green for this reason. This is an interop-conformance/test-integrity concern, **not** a commit-auth weakness (see 6z0 — the commit path re-parses and rejects a fallback-shaped message).
2. **Storage snapshot rollback (xmc) — WEAK.** Delete hooks are real and transactional, but `create_snapshot`/`rollback_snapshot` in the **sqlite and nostrdb** backends are **false-success stubs** (store `X'00'`/timestamp, restore nothing, return `MARMOT_OK`). The memory backend honestly returns `UNSUPPORTED`. No core flow calls them (create/commit/welcome use manual compensating `mls_delete`), so this is a **latent booby-trap**, not an active bug.

Everything else in scope verified PASS (several with minor notes).

---

## Per-issue verdict table

| ID | Area | Verdict | One-line evidence |
|----|------|---------|-------------------|
| **3z3** | HPKE/DHKEM RFC 9180 | **PASS** | Real A.1.1 KAT asserted end-to-end (`test_mls_crypto.c:196-313`: ikm `7268600d…`→ss `fe0e18c9…`→ct `f938558b…`); LabeledExtract/Expand/DeriveKeyPair real (`mls_crypto.c:376-535`) |
| **6z0** | Commit auth | **PASS** | FramedContent sig (`mls_group.c:1657`) + membership tag (`:1658`) verified pre-mutation on staged clone; confirmation tag constant-time `sodium_memcmp` (`:1922`); negative tests `test_bad_committer_signature_rejected`, `test_wrong_confirmation_tag_rejected` (`test_mls_group.c:946,977`) |
| **az0** | Welcome auth | **PASS** | GroupInfo sig verified w/ `"GroupInfoTBS"` (`mls_welcome.c:562-586`); old "skip" block + 2-leaf fallback removed (commit a40d6d12); real HPKE Open welcome decrypt; negative test `test_tampered_group_info_signature_rejected` re-encrypts+re-seals then asserts reject (`test_mls_group.c:1048-1130`) |
| **wud** | Commit staging | **PASS** | Serialize→deserialize staged clone (`mls_group.c:1678`); `validate_proposal_ordering`; Add calls `mls_key_package_validate`; unknown proposal → reject (`test_unknown_proposal_type_rejected`); Remove blanks path to root |
| **kdh** | KeyPackage/LeafNode | **PASS** | LeafNode sig verified over `LeafNodeTBS` (`mls_key_package.c:429`); `sig_len==MLS_SIG_LEN` enforced (`:396`); deserialize rejects wrong len (`mls_tree.c:566-571`); negative test `test_validate_bad_leaf_signature` re-signs outer KP to isolate leaf sig |
| **2y7** | Parent-hash | **PASS** (note) | `mls_tree_verify_parent_hashes` recomputes+compares (`mls_tree.c:770-811`), enforced on UpdatePath apply (`mls_group.c:1883`); negative test `test_mls_tree.c:895-903`. *Note:* subtree containment is structural recursion (`mls_tree.c:684-691`), not resolution/unmerged-leaf-aware |
| **yfk / 6s3** | Wire format | **WEAK** | VLI/MLSMessage envelope + byte-length vectors implemented and many MDK vectors byte-assert; **but** opaque fallback masks PublicMessage parse failure (`mls_framing.c:1105-1128`) → interop passes via byte roundtrip, not proven parse |
| **av8** | Secret-tree OOO+FS | **PASS** | Bounded skipped cache (cap 32, `mls_key_schedule.h:77`; double bound check `:426-435`); replay rejected + wiped on take (`:375-383,421-424`); `sodium_memzero` throughout |
| **ev8** | Sender data | **PASS** (note) | Parsed once, leaf-index range-checked before ratchet consume (`mls_framing.c:328-343,402-410`). *Note:* helper `..._with_sender_data()` accepts pre-parsed data (caller-misuse surface, internally re-validated) |
| **fpf** | Secret-tree KAT (prior) | **PASS** | Real expected values still asserted (`test_mls_key_schedule_vectors.c`); suite green |
| **dej** | Test integrity (MDK) | **PASS** (note) | Real JSON loaders + `assert_bytes_eq` real byte compares; XFAIL classes counted separately in `g_mdk_deferred`, printed "not green coverage", tracked by 3lh. *Note:* MLSMessage path can satisfy assertion via opaque fallback (see yfk/6s3) |
| **yvh** | kind:443 event auth | **PASS** | id recompute + Schnorr `nostr_event_check_signature` + pubkey↔credential bind + `i`-tag↔KeyPackageRef (`credentials.c:82-98,555,595-617`); signed API signs w/ `nostr_sk` (`:351-360`) |
| **bzd** | Raw-JSON fallback gate | **PASS** | `allow_legacy_raw_messages` default **false** (`marmot_types.c:70-81`); send/receive reject non-MLS unless opt-in (`messages.c:384-389,699-705`) |
| **gaz** | nostrdb kp vtable | **PASS** | 4 kp slots → real LMDB fns (`storage_nostrdb.c:1699-1702`, impls `:1128-1279`); compiles + tests green with nostrdb ON (19/19) |
| **1ht** | SQLite fail-closed (prior) | **PASS** | Encryption requested w/o SQLCipher → close + return NULL (`storage_sqlite.c:1400-1407`) |
| **bk4** | Storage write propagation (prior) | **PASS** (note) | Mandatory hooks checked; mls_store/exporter writes propagate + rollback (`groups.c:458-635`). *Note:* `record_welcome_failure` ignores persist failure (`welcome.c:107-117`) |
| **xmc** | Delete + rollback hooks | **WEAK** | Delete_group/delete_exporter_secret real + transactional (sqlite/nostrdb). **Rollback is false-success stub**: sqlite/nostrdb store no state and return OK (`storage_sqlite.c:1204-1242`, `storage_nostrdb.c:1442-1488`); memory returns UNSUPPORTED — inconsistent |
| **acn** | Outer event persistence | **PASS** (note) | Real outer kind:445 id decoded, `save_processed_message` called + propagated (`messages.c:780-829`); distinct welcome outcomes (`welcome.c:121-132`). *Note:* welcome stores inner rumor JSON, not outer gift-wrap JSON (API only receives rumor) |
| **tg1** | Extension bounds | **PASS** | Count caps + `%32` checks (`extension.c:264-295`); trailing-byte reject `if(!mls_tls_reader_done)` (`:357`) |
| **7fo/f6g/k5o** | gobject (prior) | **PASS** (not re-audited deeply) | `marmot_gobject_test` PASS; relied on green + prior audit, not independently re-traced this pass |

---

## WEAK/FAIL items — specifics

### 1. yfk/6s3 — opaque fallback masks PublicMessage parse (WEAK, most important)
`libmarmot/src/mls/mls_framing.c:1105-1128`:
```c
if (rc != 0) {
    uint16_t wire_format = msg->wire_format;
    mls_message_clear(msg);
    msg->wire_format = wire_format;
    reader->pos = body_start;
    msg->opaque_content_len = mls_tls_reader_remaining(reader);
    ... /* copy raw bytes */
    return 0;   /* success even though PublicMessage/PrivateMessage parse FAILED */
}
```
The MDK interop assertions (`test_interop.c:1186-1202`) require `deserialize==0`, full consume, and byte-for-byte reserialize — **all satisfiable by opaque preservation.** So:
- The interop test that epic memory said to "leave red at :1192" is green **because of this fallback**, not because a conformant `PublicMessage` parser now exists.
- Risk: genuine incompatibility with an MDK `message-protection` PublicMessage layout would be silently masked as "roundtrips fine."

**Not an auth bypass:** `mls_group_process_commit` (`mls_group.c:1633-1650`) requires `wire_format==PUBLIC_MESSAGE` **and** reads structured `pm->content` fields (sender type/index, content type, group_id, epoch, confirmation tag). A fallback-shaped message has zeroed `content`, so sender/epoch checks fail → `MARMOT_ERR_MLS_PROCESS_MESSAGE`. Commit/welcome auth do not trust the fallback.

**Recommend:** for `PUBLIC_MESSAGE`/`PRIVATE_MESSAGE`, do **not** fall back to opaque — propagate the parse error (keep opaque only for the deliberately-deferred WELCOME/GROUP_INFO/KEY_PACKAGE `rc=-2` cases). Then keep the interop MLSMessage assertion and let it stay red until the parser is genuinely conformant (honors the original nostrc-6s3 intent). Track under 6s3/3lh.

### 2. xmc — snapshot rollback false success (WEAK)
`storage_sqlite.c:1204-1242` / `storage_nostrdb.c:1442-1488`: `create_snapshot` persists no group state (`X'00'`/timestamp); `rollback_snapshot` deletes the marker and returns `MARMOT_OK` — it **claims to have rolled back while restoring nothing.** The memory backend returns `MARMOT_ERR_UNSUPPORTED` (honest). No caller in the core create/commit/welcome flows invokes these (they perform manual compensating `mls_delete`), so it is latent. **Recommend:** make sqlite/nostrdb return `MARMOT_ERR_UNSUPPORTED` too (fail-closed, consistent with memory) until real state serialization exists, so a future caller cannot silently rely on a no-op rollback. Remove the "For now" comment or file a tracking issue.

### Minor notes (not blocking)
- **2y7:** `subtree_contains_node` is structural (left/right recursion), not resolution/unmerged-leaf aware. Adequate for current tree shapes; revisit if unmerged-leaf parent-hash cases arise.
- **ev8:** exported `mls_private_message_decrypt_with_sender_data()` trusts caller-parsed `MlsSenderData`; internally re-validates range before ratchet consume, so safe, but the API shape invites misuse.
- **bk4:** `record_welcome_failure` (`welcome.c:107-117`) ignores the return of `save_processed_welcome` — a failure marker can silently fail to persist.
- **acn:** stored welcome `event_json` is the inner rumor, not the outer gift-wrap JSON (an API-surface limitation, wrapper_event_id is real).

---

## Newly-introduced stubs/TODOs found
Full sweep of `libmarmot/src/**.{c,h}` for `TODO|FIXME|for now|not implemented|placeholder|stub|hardcoded|HACK` → **4 hits, all benign or already noted:**
- `mls_key_package.c:214` "extensions … empty for now" — RFC-permissible empty leaf extensions.
- `marmot_error.c:18` — error-string table entry.
- `storage_nostrdb.c:37` — correct fail-closed stub (returns NULL when nostrdb not compiled).
- `storage_sqlite.c:1210` "For now, just mark the snapshot with timestamp" — the xmc rollback stub above.

No hardcoded-to-vector cheats, no `assert(1)`, no disabled assertions found in the crypto paths.

---

## Test-integrity assessment (meta)
- MDK vector loaders are **real parsers**; comparisons use `assert_bytes_eq` (`test_interop.c:1166-1179`) which aborts on mismatch and increments `g_mdk_asserted`.
- The 5 deferred classes (psk_secret, tree-operations, tree-validation, treekem, passive-client) are **honestly XFAIL/DEFERRED**, counted separately (`g_mdk_deferred`), printed as "not green coverage" (`:1760-1763`), and tracked by **nostrc-3lh**. Each requires a feature genuinely unimplemented (PSK combiner, stateful tree replay API, TreeKEM UpdatePath replay, passive-client Welcome/commit). I found **no XFAIL masking a passing/regressed assertion.**
- The one integrity soft spot is the MLSMessage/PublicMessage assertion being satisfiable via the opaque fallback (yfk/6s3 above) — flagged, not fatal.

---

## Is the reviewed surface production-ready?
**For the security-critical MLS crypto/auth core: yes.** Signature verification, confirmation/membership tags, HPKE, parent-hash, and secret-tree forward secrecy/replay are real, correctly ordered (verify-before-mutate on staged clones), and covered by genuine adversarial negative tests. The prior "green theater" has been replaced with real assertions.

**Remaining before calling the whole surface production-complete:**
1. Fix the `PUBLIC_MESSAGE`/`PRIVATE_MESSAGE` opaque fallback so wire-format conformance is genuinely proven (and restore the interop MLSMessage assertion as a real gate) — nostrc-6s3/yfk/3lh.
2. Make sqlite/nostrdb snapshot rollback fail-closed (`UNSUPPORTED`) instead of faking success — nostrc-xmc follow-up.
3. Minor hardening: propagate `record_welcome_failure` persist errors; document/guard the `_with_sender_data` helper.

None of these is an authentication bypass; items 1–2 are correctness/latent-safety and should be tracked as follow-ups rather than reopening the crypto-auth issues.
