# Review: Marmot Welcome plaintext leak fix (6fc53d15)

## Context / Scope

Independent review of commit `6fc53d15` (`nostrc-j5vw`) against `026a2da0`, limited to this upstream security fix—not Groundhog release readiness. No code was changed.

## Findings

- **No blocking finding.** The commit removes the predictable `/tmp/gi.bin` write after GroupInfo decryption and replaces the parse-failure byte dump with numeric status, lengths, and position (`libmarmot/src/mls/mls_welcome.c:559-605`). The other diagnostics in this Welcome processor are constant strings (`:674-726`). The higher-level accept path calls this same parsed processor (`libmarmot/src/welcome.c:439-447`) and records a constant failure reason, not decrypted contents (`:107-118`). No remaining plaintext file/log sink was found in the inspected Welcome path.
- The regression exercises a valid Welcome and an authenticated malformed GroupInfo that reaches the parser; it checks the former dump path and rejects the old hex-prefix diagnostic (`libmarmot/tests/test_mls_welcome.c:214-346`). Its path check is specific to `/tmp/gi.bin` and log inspection reads 511 bytes, so it is a regression for the reported leak rather than a general proof against every future sink. This is not a push blocker for this bounded fix.
- Version impact is **PATCH** for libmarmot. The already-declared unreleased `0.3.1` agrees across `VERSION_MANIFEST.md:18`, `libmarmot/CMakeLists.txt:16-18`, and `libmarmot/meson.build:2`; no second bump is needed for this commit.

## Verification

- `git diff --check 6fc53d15^ 6fc53d15`: passed.
- CMake configure and `test_mls_welcome` build: passed after initializing this worktree's `third_party/nostrdb` submodule.
- `ctest --test-dir _build -R '^marmot_test_mls_welcome$' --output-on-failure`: **1/1 passed**.
- Standalone Meson configure failed on the pre-existing `libmarmot/meson.build:71` `endif()` syntax; this commit does not change that file. The full libmarmot suite was not rerun in this review.

## Recommendation

**APPROVED** for pushing commit `6fc53d15` as a partial upstream security fix. This approval does not represent Groundhog release acceptance.
