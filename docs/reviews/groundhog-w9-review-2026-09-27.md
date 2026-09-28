# Groundhog W9 integrated peer review — 2026-09-27

## Context / scope

Independent review of exactly `ea905fa4b220ee1e866e4cadb82b703e8489da16..a6082b73a381d1fbccb3a7af3d453491cdd7e77a` (`f728c7ae`, `968aabe2`, `a6082b73`). This is approval of those code and test changes, not Groundhog release readiness or live relay acceptance. No code or beads were changed.

## Findings

- **NIP-17 outbound codec: no blocking finding.** `gnome/groundhog/src/app/gh-nip17-envelope.c:216-268` constructs one unsigned kind-14 rumor and rejects malformed/self recipients. The recipient and self-copy paths use the selected signer for distinct NIP-44 encryptions and signed kind-13 seals, then create separately randomized, signed kind-1059 wraps (`:86-207`). `gh-signer.c:157-181` also checks the signed seal's tags and other signed fields against the request, preventing a valid-but-modified signer response from adding public metadata. This API does not publish (`gh-nip17-envelope.h:14-22`).
- **Cancellation and account binding: no blocking finding.** Per-call cancellation is combined with generation cancellation in `gh-account-controller.c:366-507`; the codec checks generation/cancellation before each signer step and again in `build_finish` (`gh-nip17-envelope.c:58-81,263-279`). Focused tests cover signer denial at all four calls, malformed/wrong-key signer results, cancellation and account switching at every stage, and switching before finish (`test_account_controller.c:1075-1217`). The round-trip test decrypts both wraps and compares their plaintext to the canonical rumor (`:1000-1073`).
- **Marmot proof: no blocking finding.** `libmarmot/src/kp_profile.c:204-220,285-295,369-377` rejects unsupported suite/scheme/leaf-key length and out-of-range proof timestamps before signing or verifying, without introducing receiver-clock expiry. `libmarmot/tests/test_kp_profile.c:56-81,210-223` covers those limits and adopted-KeyPackage rejection by the legacy group engine.
- **Jansson fuzz/tag fix: no blocking finding.** `libnostr/src/tag.c:49-68` rejects an empty prefix before indexing `prefix_len - 1`; `tests/test_json_filter_tags.c:21-47` exercises lookup and unique append. The filter fuzz harness installs Jansson (`tests/fuzz_filter_parse.c:6-17`), and `tests/test_fuzz_filter_parse.c:1-72` asserts compact and fallback seeds actually take their intended backend paths.
- **Versioning: consistent.** The changes are patch/security/internal additions; `VERSION_MANIFEST.md:14-22` matches `libnostr/CMakeLists.txt:8`, `libmarmot/CMakeLists.txt:18-21`, `libmarmot/meson.build:1-4`, and `gnome/groundhog/CMakeLists.txt:1-2` at libnostr `1.0.6`, libmarmot `0.3.3`, and Groundhog `0.5.1`. No other tracked component needs a bump for this diff.

## Verification / recommendations

`git diff --check ea905fa4..a6082b73` passed. With `BUILD_GROUNDHOG=ON`, `WITH_NOSTRDB=OFF`, `LIBNOSTR_WITH_NOSTRDB=OFF`, `BUILD_NOSTR_GOBJECT=OFF`, `BUILD_NOSTR_GTK=OFF`, and `BUILD_APPS=OFF`, the `groundhog`, `test-groundhog-account`, `test_kp_profile`, `test_fuzz_filter_parse`, and `test_json_filter_tags` targets built; the four corresponding CTest cases passed (4/4). A default configure/build was not verified because this worktree lacks `third_party/nostrdb`; its configure stops requesting `git submodule update --init third_party/nostrdb`. Run the full configured build/test suite with that submodule present before release. Relay discovery, publication, delivery, and live signer acceptance remain outside this bounded codec review.

**APPROVED** — scoped to integrated commit `a6082b73a381d1fbccb3a7af3d453491cdd7e77a` versus `ea905fa4b220ee1e866e4cadb82b703e8489da16`; no blocking code/test finding in the requested scope.
