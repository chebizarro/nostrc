# Source-claim contract seam review (nostrc-0cfsg.36.27)

**Verdict: no runtime blocker.** Two low findings, both fail-closed or pre-existing, and four informational notes.

## Scope

- **Reviewed:** only the 3-file diff `aebb6eff6dd879033fb38bb016ea3127232aa37e..301e3979ac0b275558959c617714e3f994a23f92`, read-only, plus its load-bearing callers.
  - `scripts/test_selection_services.py`: one hunk, `source_claim_contract`, `SourceClaimError`, `_source_claims`.
  - `scripts/test_selection_execution.py`: one hunk, `verify_source_closure`, `_derived_claims`, `_claim_name`, `_claim_difference`.
  - `tests/selection/test_native_services.py`: new classes `SourceClaimContract` and `GenuineDescriptorAliasSourceClaims`.
- **Callers checked:**
  - `seal_native_lane` (services ~1215–1250): retains `source-inputs/<sha256(name)>.input`.
  - `attest_native_lane` (services ~1290–1315).
  - The execution native public-receipt verifier (execution ~119–150), which re-reads `source-inputs` by claim key.
  - The custody work-root claim loop (custody ~113–117).
  - The realization `input_hashes` producers (realization ~2700–2757).
- **Not done:** no Oracle, no test runs, and no source, `.beads`, docs or agent changes other than this file.
  - The one exception is a single in-memory spot-check of `source_claim_contract` against lexical edge spellings. It touched no filesystem state and sealed nothing.
- **Evidence:** `w11-next-batch-20261024/nostrc-0cfsg-36-27-source-claims/SHA256SUMS`: 25 entries, 0 non-OK under `shasum -a 256 -c`.
- **Not reviewed:** existing shared witness-pin failures are pending parent composition and are not in scope.

## Causal check

- **Before the change, the two sides diverged:**
  - The sealer keyed claims by the **raw** spelling, because `Path.is_relative_to` is lexical over parts and accepts `/src/apps/grotto/tests/../data/x`.
  - The qualifier only derived keys where `os.path.normpath(path) == path`, so it **skipped** that spelling.
  - Result: `sealed source claims differ from the sealed raw input audits` on a correct run, which matches job 120807749830.
- **After the change:** both sides call one lexical `source_claim_contract`, keyed by `normpath`. Spot-check results:
  - `/w/nostrc/./a//x.c` → `/w/nostrc/a/x.c`
  - `///w/nostrc/x.c` → `/w/nostrc/x.c`
  - relative keys and `/w/nostrc2/...` → not claimed
- **Root-mismatch hypothesis:** correctly refuted. Both sides already bound the same root; the spelling was the only difference.

## Findings (severity-ranked)

### L1: low, pre-existing, not introduced. `//root/...` source inputs escape the source closure

- **Mechanism:** POSIX `os.path.normpath` keeps exactly two leading slashes. A key `//w/nostrc/x.c` therefore canonicalizes to `//w/nostrc/x.c`, which is not under `prefix`, and is skipped as "outside the source root" on both sides. The spot-check confirms: `({}, {})`.
- **Why it matters:** the kernel opens it as the same file as `/w/nostrc/x.c`.
- **Not a regression:** the old sealer behaved the same way (pathlib keeps a `//` root, so `is_relative_to('/w/nostrc')` is False), and so did the old qualifier (`startswith`).
- **Why it isn't a blocker:**
  - The raw receipt, including that hash, is still sealed and byte-pinned, so there is no tamper path. The only gap is that the file isn't checked against the checkout.
  - These inputs are authored in CMake descriptors and runtime manifests of the tested commit.
  - No current producer is known to emit `//`. Not proven exhaustively.
- **Follow-up suggestion, not done here:** inside the shared contract, treat a key starting with exactly `//` whose single-slash form lies under the root as either an alias (canonicalize it, and require the sealer's physical alias proof) or a malformed path (refuse). Doing it there keeps the sealer and qualifier identical.

### L2: low, behaviour change, fail-closed. Keys that leave and re-enter the root are now claimed

- **What changed:** `/w/other/../nostrc/x.c` is not lexically under the root but normalizes into it, so it is now a source claim. The spot-check confirms it. Previously both sides ignored it.
- **Effect:** better coverage, plus a possible new refusal. The sealer's physical proof calls `Path(raw).resolve(strict=True)`, which raises an untyped `FileNotFoundError` if `/w/other` doesn't exist. A symlinked `/w/other` that resolves elsewhere refuses with "alias resolves differently".
- **Still fails closed:** `finalize_native_lane` catches `BaseException`, the anchor stays empty, and `attest_native_lane` refuses with `trusted native finalizer seal missing`.
- **Test note:** no test names this shape. If the parent wants it pinned, a single contract-level subTest would cover it.

### I1: info. Missing canonical/alias raises `OSError`, not `SourceClaimError`

- Missing canonical files and aliases surface as `FileNotFoundError`. Tests deliberately accept `(ServiceError, OSError)`.
- The base had the same behaviour (`resolve(strict=True)`). It affects the diagnostic type only; the finalizer refusal path is identical.

### I2: info. The qualifier doesn't re-prove alias resolution in its own checkout

- **What the qualifier checks:** offline/portable qualification is lexical. The sealed claim set must equal the shared contract over the sealed raw receipt. Each canonical name must also be:
  - normalized
  - under `repo_root`
  - tracked under `ls-files --recurse-submodules`
  - resolved within the checkout
  - hash-equal
- **Hash coverage still holds** through two links:
  - The trusted sealer proves that every raw spelling resolves physically to the same file as the canonical name, and that the canonical bytes hash to the audited value.
  - The qualifier proves that the checkout's canonical bytes hash to the same value.
- **Possible follow-up:** re-resolving each raw spelling inside the aggregate checkout would be defence in depth only. It isn't needed for soundness while the sealer is the trusted root.

### I3: info, outside this diff. The custody work-root loop still claims raw spellings

- `test_selection_custody.py` ~115 (`Path(path).is_relative_to(work)`) claims evidence-root keys by their raw spelling.
- A `..` key under the evidence root finds no sealed entry, so it **refuses**; it never fails open.
- This is .25-owned code, recorded here only so the same spelling class isn't rediscovered as a source-claim bug.

### I4: info, unchanged. Trusted root binding

- **The two roots:**
  - The sealer derives against `Path(source).resolve(strict=True)`. `test_symlinked_gate_source_spelling_seals_the_same_canonical_claims` covers a symlinked gate spelling.
  - The qualifier derives against `locator.real['repo_root']`.
- **If they diverge,** the claim sets differ or are empty, so the closure refuses (`unsupported/empty` or inequality). `test_post_seal_drift_and_wrong_qualifier_root_refuse` covers this.
- **Gap, unchanged by .27:** no explicit equality check binds the two roots; refusal relies on the claim-set comparison.

## Verified unchanged or unweakened

- **Original bytes, FD and cancellation:**
  - The services diff is a single hunk confined to the claim functions. `_trusted_output_bytes`, `_original_output`, the seal-time output re-pin, `finalize_native_lane` (signal-mask caller, `os._exit` contract, 130 code, empty anchor on refusal), the seal FD write/close and `attest_native_lane` are byte-unchanged.
  - The new refusal types are `ServiceError` subclasses or `OSError`, and both go through the existing `except BaseException` refusal.
  - `test_finalizer_seals_canonical_claims_on_success_failure_and_cancellation` asserts verified seals at codes 0, 1 and 130. It also asserts `(130, '')`, no seal, and an attest refusal for a divergent alias during cancellation.
- **Canonical keys are used consistently.** The sealer retains source inputs under `sha256(canonical)`, and both `attest_native_lane` and the execution public verifier look them up by the same canonical key. The sealer reads `Path(canonical)` bytes and checks them against the audited hash before sealing.
- **Symlink confusion is refused** in each of these cases (sealer, physical):
  - a symlinked component before `..` that leaves the checkout
  - an in-tree symlink pointing at a different file with identical bytes
  - a canonical file that is itself a symlink out of the checkout ("escaped checkout")
- **Malformed and conflicting input refuses on both sides,** and the qualifier wraps it as `violate the source claim contract`:
  - malformed mappings: `None`, `[]` or `str`. The old qualifier accepted `None` through `or {}`; the old sealer crashed untyped.
  - non-string paths
  - non-64-lowercase-hex hashes for source claims. The old sealer already refused these at its hash compare.
  - control characters
  - the root itself
  - lexical escapes (`/root/../x`, `/root/../root2/x`). These match the old sealer's "escaped checkout".
  - contradictory aliases or duplicates
- **Coverage didn't shrink.** Every `input_hashes` mapping at any depth still contributes, and nothing is intersected. Relative and outside-root keys are excluded exactly as before, apart from the L1 and L2 shapes. The realization producers (runtime, signer, docker and policy-controller fingerprints, compatibility hashes, default build execution) emit `hexdigest()`/`sha256` values that pass the new format check.
- **Diagnostics don't leak.** `_claim_difference` shows root-relative names only, never hashes or bytes. `_REDACT` masks token- or hex-shaped runs (32+ hex characters). Control characters become `?`, and non-ASCII is backslash-escaped. Names outside the root print as `<outside root>`.
- **Offline/portable mode works.** `_derived_claims` lazily imports services, which imports only the standard library at module level; the execution verifier already imported it. The genuine CMake fixture aggregates after the worker tree is moved away (`test_shared_contract_seals_qualifies_and_aggregates_offline`).
- **Genuine discrimination is shown.** `test_genuine_receipt_carries_both_spellings_and_old_predicates_disagree` reproduces the exact old qualifier error on a real CMake `INPUT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/../vector.txt` audit, and the new contract passes it.

## Not established by this review

- real signer, pidns or hosted proof
- final combined-source behaviour after the parent composes witness pins
- an exhaustive search for `//`-prefixed producers (L1)

No product version bump: this is CI and test tooling only.

## Parent resolution after the bounded review

The review above remains source-bound to `301e3979a`; no second review or Oracle call was made. Parent applied the two concrete recommendations through the source owner in `14af5ede5`: double-leading-slash spellings that could name source inputs now refuse as typed `SourceClaimError`, and missing intermediate/outside-prefix paths or symlink loops refuse with typed, redacted root-relative diagnostics. The physical raw-to-normalized same-file check, full claim/hash equality, original receipt bytes, trusted FD and cancellation functions are unchanged.

Parent independently verified all 25 original plus 13 hardening checksum entries. The owner reports 13 focused tests passing and the 130-case seam cohort with 57 explicit Linux-only skips on macOS; this is not a pidns, AppArmor or real signer proof. The genuine configure/build/audit/seal/qualification fixture still succeeds, and old predicates/individually weakened checks fail the discriminating controls. Shared historical source records require a newer exact composition layer; old reviewed data must not be rewritten.

Actual hosted evidence at `aebb6eff6` remains failed: manual run40072096156, job120807749830 executed successfully, but final transported-source closure comparison refused. Withholding metadata, API originals, console and verifier refusal are retained (17 checksum entries independently checked); raw originals were not uploaded or durably retained. The source-level cause is confirmed by the Grotto CMake INPUTS producer and the genuine reproducer. The exact hosted mismatched paths remain inferred because the raw receipt is unavailable. Next source must obtain its own actual qualification proof; no receipt is reconstructed or promoted.

The PR's affected request already emits an effective broad plan when these CI controls change. One new PR shadow run is sufficient for fresh full normal qualification; an additional manual broad would duplicate the 13 profiles and 47-job broad family. PR proofs bind the synthetic merge SHA, not the pushed head. Whole-profile, complete-gate, W11/W12, performance and release acceptance remain separate.
