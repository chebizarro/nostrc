# Apple test incompatibilities

This documents the Apple-specific build and test incompatibilities encountered while trying to complete the full AGENTS.md landing workflow on macOS.

## Status

As of 2026-03-27, the main-window refactor work itself compiles, but the full clean build/test workflow is blocked by unrelated Apple-only test/build issues.

## Tests disabled on Apple

These are now disabled in `apps/gnostr-signer/tests/CMakeLists.txt` for `APPLE` builds:

### `signer/dbus`
- Prior symptom: the test completed its assertions but did not terminate under CTest/GTestDBus.
- Status as of 2026-03-31: fixed and re-enabled on Apple.
- Current handling: runs normally in CTest.

### `signer/ui`
- Symptom: the test hangs immediately in the first window test on macOS when no usable window-server session is available.
- Evidence: direct invocation stalls after:
  - `# Start of ui tests`
  - `# Start of window tests`
- Status as of 2026-03-31: no longer CTest-disabled on Apple.
- Current handling: enabled in CTest, but forced through the test binary's self-skip path in headless Apple environments.

## `apps/gnostr` integration targets incompatible on Apple

These targets are not currently viable in this macOS environment because they are underlinked when built as standalone test executables against `nostr_gtk`:

### `gnostr-test-delete-authorization`
- Prior symptom: linker failure on arm64.
- Root cause: the standalone test target depended on app/UI symbols that were only satisfied in the full app link.
- Status as of 2026-03-31: fixed and re-enabled on Apple.
- Current handling: builds and passes in CTest on this macOS setup.

### `gnostr-test-ndb-main-thread-violations`
- Prior symptom: same class of linker failure on arm64.
- Root cause: same underlinked standalone-test composition problem.
- Status as of 2026-03-31: fixed and re-enabled on Apple.
- Current handling: builds and passes in CTest on this macOS setup.

### `gnostr-test-real-bind-latency`
- Prior symptom: same Apple integration-test composition risk as the previous target.
- Status as of 2026-03-31: fixed and re-enabled on Apple.
- Current handling: builds and passes in CTest on this macOS setup.

## Build fixes made while investigating

These were genuine build fixes, not Apple skips:

### `apps/blossom-cache/tests/test_blob_store.c`
- Added `#include <glib/gstdio.h>`
- Reason: `g_unlink` and `g_rmdir` were undeclared during clean build.

### `libnostr/src/connection.c`
- Removed a stale assignment to `last_pong_ns`.
- Reason: the private struct no longer defines that field.

### `apps/gnostr/src/ui/gnostr-dm-service.c`
- Added missing `gnostr_dm_send_result_free(...)` implementation.
- Reason: link failure from declared/used but undefined symbol.

## Follow-up work

1. Replace the temporary Apple self-skip for `signer/ui` with a true headless-safe GTK/libadwaita strategy, or split out the non-windowed assertions.
2. Replace the current Apple-specific standalone-test link workarounds with a more explicit/shared test-link strategy if we want less platform-specific linker behavior.
3. Keep the re-enabled `apps/gnostr` integration targets covered in future Apple clean-build/CTest passes.

## Important constraint

These issues blocked completion of the AGENTS.md landing workflow in this session:
- no final green clean build
- no final green ctest pass
- therefore no commit/review/push was performed
