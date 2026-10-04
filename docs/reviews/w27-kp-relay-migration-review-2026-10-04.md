# W27 KeyPackage write-relay migration — independent review (2026-10-04)

**Verdict: CHANGES-REQUIRED.** Reviewed `groundhog/w27-kp-relay-migration` at `a0fb5f2a` for `nostrc-qaqh` against `origin/master` `827c2b47`. The ordinary A→B migration works, but rapid successive signed 10002 changes leave a current KeyPackage on an intermediate relay that is no longer in the write set. The filed `nostrc-3e6g` deferral is **not acceptable for this migration fix**: the result violates the W24 placement/privacy rule and survives a service restart. I did not modify the author worktree or main checkout and did not push.

## Findings

### F1 — High: intermediate relay copies are not reconciled after A→B→C

**Location:** `gnome/groundhog/src/mls/gh-mls-service.c:7903-7909`, `:7989-8006`, `:8334-8345`.

The only persisted baseline is the last *fully reconciled* set (A); the in-flight target (B) is discarded by `key_package_migration_reset()`. If B accepts a 30443 and the signed 10002 changes to C before reconciliation completes, the C round compares C only with A. Its NIP-09 cleanup names A, not B. After restart, there is still no record that B was ever a candidate. A reviewer-only wire-relay test reproduced this: A was W+H, B was X with its OKs held, then a newer 10002 selected C=R. After releasing B's OKs, both formats reached C and A was deleted, but X still held **one** current KeyPackage (`on_intermediate->len == 1`, expected 0). The probe was removed after the run. This is the concrete `nostrc-3e6g` case, not a hypothetical timing concern.

It also means rapid changes are not coalesced: the service signs/publishes to a superseded B before proceeding to C. Persist the union of candidate targets before publishing; reconcile every candidate no longer in the latest 10002 only after both formats are ACKed on every latest target. Recheck the signed list before sending a stale job, and add a regression for partial B ACK followed by C and restart. Keep deletion ACK failures retryable.

### F2 — Medium: removing the last write relay leaves old 30443 copies indefinitely

**Location:** `gnome/groundhog/src/mls/gh-mls-service.c:8297-8300`.

If another device publishes a valid 10002 with no write-capable `r` entries, `key_package_maybe_publish()` sets `NO_RELAYS` and returns before comparing against the saved baseline or sending NIP-09. The former write relays continue serving both 30443 slots even though no current 10002 authorizes them. Unlike a refused new relay, there is no pending target that could later ACK and release this state. Distinguish a completed empty write set from incomplete discovery and define a withdrawal path: request deletion on the former relays, retain private init keys until those deletions are ACKed, and test the remote-device empty-set transition. This reconciles the W24 write-relays-only rule without deleting keys while a relay still serves the package.

### F3 — Low: the committed revert check fails at a setup precondition

**Location:** `gnome/groundhog/tests/mls/test_mls_kp_lifecycle.c:1664-1665`.

With the service implementation replaced by `origin/master` and the new test unchanged, the test aborts at `mls_delete("gh/key-package-write-relays") == MARMOT_OK`: the baseline key does not exist in base code (`-101`, NOT_FOUND). It never reaches the assertion that the new relay lacks both unchanged slots. I independently relaxed that *test-only precondition* to allow NOT_FOUND, rebuilt against the reverted service, and the test then failed at the intended observation: “both unchanged slots on the new relay did not happen within 90 s.” Restoring the branch service and original test passed. Make the committed test tolerate the absent pre-fix baseline (or seed it explicitly), so the claimed counterfactual pins the behavior rather than an implementation artifact.

## Verified behavior and scope

- The new path calls `marmot_republish_key_package_unsigned()` for held adopted and legacy profiles, preserving each MLS KeyPackageRef and stable `d` slot. Its signed envelopes pass the profile validator. Migration OKs do not call lifecycle confirmation or delete init keys. The lifecycle test keeps the keys even after old-relay cleanup.
- An externally injected signed kind-10002 event on the discovery relay, not a local UI change, triggers `GhAccountRelays::changed` and migration. `GhRelayListSetup` was not changed; its never-clobber behavior is outside this diff.
- A reviewer-only two-new-relay probe held one relay's OKs: one relay's ACK left both old relays undeleted and init keys held; releasing the second relay's OKs allowed cleanup. A separate reviewer-only migration signer-denial probe left the old copies and keys intact. Both probes passed and were removed.
- Old-relay NIP-09 is launched after both produced formats report all target ACKs in the ordinary A→B case. Neither the added code nor the new test publishes kind 10051; adopted 30443 remains strictly tagged and legacy 30443 carries its new write-relay `relays` tag.
- The rapid-change probe above means the no-publish-storm/no-stale-target requirement is **not verified**. The current state machine serializes rounds, but does not coalesce superseded targets or remember their accepted copies.

## Verification

- `source /tmp/nostrc-macos27-env.sh`; clean Ninja configure with `BUILD_GROUNDHOG=ON` and `BUILD_MDK011_INTEROP=ON`; full build: **pass**.
- `groundhog-mls-kp-lifecycle`, `groundhog-mls-kp-lifecycle-adopted`, `groundhog-mls-service`: **3 passed**. MDK 0.11 matrix: **18 passed, one expected `mdk09-probe` skip** (private image `nostrc-mdk-interop:0.11.0`).
- `python3 scripts/check-unsequenced-args.py`: **pass**. `scripts/linux-gate.sh --sanitizers /tmp/rv-w27-kp-relay-migration`: **60 passed**, no sanitizer reports. Existing Docker volumes were reused; no review-created volume to remove.
- Actual `git cherry-pick --no-commit a0fb5f2a` in a disposable worktree at `origin/master` `827c2b47`: **clean**, no conflicts; disposable worktree removed.
- Counterfactual: original test fails prematurely on missing baseline; after only the test precondition was relaxed, the reverted service fails at missing migration; restored focused test passes. All temporary source probes and reverts were restored before this document was committed.

## Backlog

`nostrc-3e6g` tracks F1 but is insufficiently deferred for this change. F2 and F3 are not covered by the reviewed bead or its discovered follow-up; the author should track them when addressing this review. No issue state was mutated during this independent review.

## Version assessment

The added `libmarmot` public function is MINOR-class, and the Groundhog behavior is PATCH-class. Both are correctly folded into the still-unreleased `0.12.0`: `VERSION_MANIFEST.md` has decision rows for both, and the listed libmarmot CMake/Meson and Groundhog CMake sources agree at `0.12.0`. No additional bump is required. The review document itself is documentation-only and needs no bump. Mention these decisions in the peer-review handoff.
