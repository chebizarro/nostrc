# W28 encrypted-groups release review — 2026-10-04

**Branch:** `review/w28-encrypted-groups-release`

**Candidate:** `groundhog/w28-encrypted-groups-release` at `4a4545e6` (`c00d3d7d`, `4a4545e6`)

**Base:** `origin/master` at `cf616c3d`
**Verdict: CHANGES-REQUIRED.** The release metadata still makes an interoperability claim broader than the tested and supported formats. No implementation or build failure was found.

## Findings

1. **Medium — metainfo overstates Marmot interoperability.** `gnome/groundhog/data/org.nostr.Groundhog.metainfo.xml:18` says encrypted groups “work with people using apps that support Marmot” before narrowing to White Noise/MDK 0.11 and MDK 0.8-era apps. That first clause is universal, but this branch's MDK 0.9 probe explicitly reports `XFAIL [unsupported]`: its KeyPackage is rejected by both MDK 0.11 and Groundhog, and the New Group row says the person has not set up encrypted groups. A person using an MDK 0.9-based Marmot app can therefore reasonably expect the advertised interop and be unable to join. Replace the general claim with the verified version/formats and say other Marmot implementations or versions are not guaranteed. `gnome/groundhog/src/ui/gh-about-dialog.c:44` similarly says groups “work with other Nostr apps” without a compatibility qualifier; consider “compatible” there. The `<release>` entry at metainfo:34 already uses the safer word “compatible.”

2. **Low — the About test does not verify the Nostr link destination.** `gnome/groundhog/tests/ui/test_about.c:40-50,81` finds only a label containing the npub. Mutating `gh-about-dialog.c:36` from `nostr:` to an HTTPS URL left `groundhog-about-gui` green. The actual code currently constructs the requested `nostr:npub…` link and contains no email, but a future regression could open a web page instead of the user's Nostr handler without a test failure. Inspect the link widget's URI (and copyable text) in the GUI test.

3. **Low — the charter's before-Create Blossom disclosure claim is conditional in the UI.** `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md:1052` now says the creation page discloses Blossom-server exposure before Create. `gnome/groundhog/src/ui/gh-mls-new-group-page.c:155-170` hides the media row for legacy groups and, with no configured server, shows only “You can add servers later,” not the IP/blob-size warning. A user creating either group sees no such warning on that page, though Preferences and the attachment sheet do disclose it before an upload. Either make the charter conditional (configured adopted policy and upload-time disclosure) or show a concise conditional warning on the creation page.

## Release-gate assessment

- `bd show nostrc-7gx7` including all comments supports the 2026-10-04 owner decision: MDK 0.11 adopted-profile and MDK 0.8 older-format evidence, W27 two-device and supported-server attachment evidence, then a default-on flag. The UI and metainfo cover the two linkable KeyPackage events by default, the newer/older split, visible members, relay ciphertext/timing/group relays, no group-history export, local leave until admin removal, and Blossom IP/blob-size exposure. Apart from finding 1, the copy does not claim encryption with everyone.
- The obsolete preview option is accepted and erased from the CMake cache; the test-control override is gone. A normal build and an explicit `-DGROUNDHOG_ENCRYPTED_GROUPS_PREVIEW=OFF` build both compile, and the default-on assertion passes. CI's deleted preview-guard test entry matches the removed test. Active source/scripts have no preview-gate use beyond the CMake compatibility shim; old review/run reports and `/tmp/ghsmoke` retain historical preview-era descriptions, while the W28 plan states the current behavior.
- Picker hardening is in scope: the async check holds its row until callback completion; pasted 64-character hex is normalized; pasted and programmatic selections respect the 32-person cap. The release GUI test exercises all three. Targeted mutants of the hex path, programmatic cap, and row reference fail at their respective assertions; the restored test passes.
- Groundhog's authoritative CMake version and `VERSION_MANIFEST.md` agree on **0.12.0**, the unreleased MINOR encompassing this feature. The metainfo release is `0.12.0`, dated `2026-10-04`. No second 0.13.0 bump is required; this review document alone does not change a shipped component.
- About displays Biz and the requested npub via `adw_about_dialog_add_link(..., "nostr:npub…")`; no email address is configured. The full About-credit revert makes its GUI test fail, but finding 2 identifies its link-specific blind spot.

## Verification

| Check | Result |
| --- | --- |
| Clean macOS Ninja build after `source /tmp/nostrc-macos27-env.sh`, `-DBUILD_GROUNDHOG=ON` | PASS |
| Full macOS CTest | PASS: 465/465 registered, 460 passed, 5 expected skips; About and MLS GUI suites passed |
| `check-unsequenced-args.py`, `check_data.py`, `check_privacy.py --self-test` | PASS (privacy: 19 rules and 79 mutation fixtures) |
| Explicit retired-option OFF configure, Groundhog build, default-on test | PASS; CMake reports option retired and removes its cache value |
| MDK 0.11 matrix, private image tag `nostrc-mdk-interop:0.11.0-w28-release-review` | PASS: 19/19 registered, 18 passed and MDK 0.9 probe expected-XFAIL/skipped |
| Targeted reverts: flag default, About credit, creation-page disclosure, picker hex/cap/row lifetime | Each failed at the expected assertion; restored GUI tests passed |
| Two candidate commits cherry-picked with `--no-commit` onto `origin/master` in a disposable review worktree | PASS, no conflicts; `git diff --cached --check` passed |
| `scripts/linux-gate.sh --sanitizers` with a private Docker image and volume | PASS: 60/60 tests, no sanitizer reports (`SANITIZER_RC=0`) |

No implementation files were intentionally changed by this review; the temporary mutation work was restored before commit. Per the independent-review instruction, the review branch is not pushed.
