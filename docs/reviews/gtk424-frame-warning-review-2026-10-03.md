# Review: gtk424-frame-warning (nostrc-ykxf)

**Commit**: 2382d06e `groundhog/tests: tolerate GTK 4.24 macOS frame-clock warning`
**Branch**: `groundhog/gtk424-frame-warning` (one commit on master a874c884)
**Reviewer**: Independent peer review (AGENTS.md)
**Date**: 2026-10-03

## Verdict: APPROVE-WITH-NITS

The core mechanism is sound. The writer function correctly:
- Re-enforces fatal-warnings for every non-forgiven warning (abort after print).
- Preserves fatal behaviour for CRITICAL (via `g_log_set_always_fatal` mask, ORed before the writer sees it) and ERROR (`G_LOG_FATAL_MASK`, immutable).
- Does **not** swallow the forgiven message — it prints via `g_log_writer_default()` with `G_LOG_FLAG_FATAL` stripped.
- Has no behaviour change on Linux (GTK 4.14 in CI never emits the Gdk frame warning; the writer is a no-op for non-matching messages).
- Properly handles `G_DEBUG=fatal-warnings` (removes WARNING from always-fatal mask, compensates in the writer).
- The tolerance test is well-designed: positive case (forgiven message exits 0) and negative case (other Gdk warning aborts) via `g_test_trap_subprocess`.

Tests using `g_test_expect_message()` (`test_expiry.c`, `test_store_conversations.c`) are unaffected — they don't install the tolerance and aren't GUI tests.

**Build**: cmake + ninja clean. All 456 ctest tests pass (0 failures, 5 skipped). `check-unsequenced-args.py` clean. No library code changed, so `linux-gate.sh --sanitizers` not required.

---

## Findings

### F1 — Missed GUI tests that present windows (Medium)

**Files**: `gnome/groundhog/tests/ui/test_mls_ui.c:741`, `nostr-gtk/tests/test_listview_recycle_stress.c:290`, `nostr-gtk/tests/test_bind_latency_budget.c:167`

Three GUI tests call `gtk_window_present()` but do **not** install `gh_test_tolerate_gdk_frame_warning()`:

| Test | Window presentation | Currently passes? |
|------|-------------------|-------------------|
| `test_mls_ui.c` (groundhog-mls-ui-gui) | `gtk_window_present(window)` L741 | Yes (intermittent) |
| `test_listview_recycle_stress.c` | `gtk_window_present(h->window)` L290 | Not registered in this build* |
| `test_bind_latency_budget.c` | `gtk_window_present(win)` L167/L242 | Passed (intermittent) |

\*Gated on `BUILD_APPS`, which wasn't enabled.

**Failure scenario**: On macOS with GTK 4.24.1, Quartz's frame clock fires `gdk_frame_timings_presented() called on skipped frame` during `gtk_window_present()`. Without the tolerance, `g_test_init()`'s fatal-warnings policy turns it into SIGTRAP → test crash. This is exactly the bug the commit fixes for the other 14+3 tests, but these three are left exposed.

**Suggestion**: Add `gh_test_tolerate_gdk_frame_warning()` to all three. For `test_mls_ui.c`, it's a one-line addition after `g_test_init()` at L2263. For the nostr-gtk tests, they already have the include directory wired up by this commit's CMake change.

### F2 — Layering violation: nostr-gtk tests include Groundhog test header (Medium)

**File**: `nostr-gtk/CMakeLists.txt:435`

```cmake
target_include_directories(${_gtk_widget_test} PRIVATE
  ${CMAKE_SOURCE_DIR}/gnome/groundhog/tests)
```

nostr-gtk is a lower-level library that Groundhog depends on. Having nostr-gtk's tests include a header from `gnome/groundhog/tests/` creates an upward dependency from a lower layer to a higher layer. This makes it possible for nostr-gtk test builds to accidentally depend on Groundhog internals, and creates a coupling that prevents building nostr-gtk tests in isolation.

**Suggestion**: Move `gh-test-gdk-frame.h` to a shared test-utilities location that both layers can reference, e.g.:
- `tests/common/gh-test-gdk-frame.h` (repo-root shared test support), or
- `cmake/` as a cmake module that generates/installs the header, or
- Duplicate the header into `nostr-gtk/tests/` (small, header-only, no maintenance burden for a 87-line file).

### F3 — Substring match, not exact match (Low)

**File**: `gnome/groundhog/tests/gh-test-gdk-frame.h:42`

```c
message != NULL && strstr(message, GH_TEST_GDK_FRAME_MESSAGE) != NULL;
```

The matching uses `strstr` (substring) rather than `strcmp` (exact). The header doc and commit message say "forgives exactly" and VERSION_MANIFEST says "exactly … the warning", but the implementation forgives any message **containing** the substring.

This is intentional — the tolerance test itself emits the message with a trailing period (`"...skipped frame."`) while the constant omits the period, relying on `strstr` to match. GTK's actual message formatting may vary across point releases.

**Failure scenario**: A hypothetical future Gdk WARNING whose text contains `"gdk_frame_timings_presented() called on skipped frame"` as a substring would be silently forgiven. Risk is extremely low given domain+level+substring specificity.

**Suggestion**: Document the substring-match choice in the header comment (replace "exactly" with "containing"), or tighten to `g_str_has_prefix` if the only variation is trailing punctuation.

### F4 — Tolerance test environment doesn't match real GUI tests (Nit)

**File**: `gnome/groundhog/CMakeLists.txt:142`

The tolerance test runs under `G_DEBUG=fatal-criticals`, but the real GUI tests (via `gn_add_gtest_xvfb` in `cmake/GnTest.cmake:35`) run under `G_DEBUG=fatal-warnings,gc-friendly`. The tolerance correctly handles both environments (it removes WARNING from `g_log_set_always_fatal` regardless of how it got there), but the test doesn't exercise the more demanding `fatal-warnings` configuration.

**Failure scenario**: None currently — the logic is environment-agnostic. A future GLib change to `G_DEBUG=fatal-warnings` handling could break the real tests while the tolerance test still passes.

**Suggestion**: Add a second tolerance test variant with `ENVIRONMENT "G_DEBUG=fatal-warnings,gc-friendly"`, or change the existing test to use that environment.

---

## Checklist

| Check | Result |
|-------|--------|
| Writer preserves fatal for non-forgiven WARNING | ✅ `abort()` after `g_log_writer_default(level \| G_LOG_FLAG_FATAL, ...)` |
| Writer preserves fatal for CRITICAL | ✅ `g_log_set_always_fatal` mask ORs FATAL before writer sees it |
| Writer does not swallow G_LOG_LEVEL_ERROR | ✅ Falls through unchanged; ERROR is in immutable `G_LOG_FATAL_MASK` |
| Forgiven message still printed | ✅ `g_log_writer_default(level & ~G_LOG_FLAG_FATAL, ...)` prints without aborting |
| G_DEBUG=fatal-warnings respected | ✅ Removed from always-fatal, re-enforced in writer |
| `g_test_expect_message()` compatibility | ✅ No test using expect_message also installs tolerance |
| Exact domain+level+text match | ⚠️ Domain exact, level exact, text is substring (`strstr`) — see F3 |
| No behaviour change on Linux | ✅ Warning never fires on Linux; writer is a pass-through |
| All macOS GUI tests updated | ⚠️ 3 missed — see F1 |
| nostr-gtk layering | ⚠️ Upward include dependency — see F2 |
| Build clean | ✅ ninja: 0 errors, 0 warnings (in new code) |
| ctest | ✅ 456 passed, 0 failed, 5 skipped |
| Unsequenced args | ✅ Clean |
| linux-gate.sh --sanitizers | N/A (no library code changed) |
