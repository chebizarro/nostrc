# Review: About dialog and the app icon (7dfd8d11)

**Branch**: `groundhog/about-dialog`  
**Base**: a874c884 (master)  
**Reviewer**: Independent peer review (AGENTS.md)  
**Date**: 2026-10-03

## Verdict: CHANGES REQUIRED

One blocker — the CI workflow must be updated to build and require the new
tests — and one nit.

---

## Findings

### B1 — Blocker: CI workflow does not build or require the new tests

**File**: `.github/workflows/groundhog-ci.yml`  
**Severity**: Blocker

The commit adds `groundhog-about` and `groundhog-about-gui` tests via
`add_test()` in CMakeLists.txt, but does not update the CI workflow:

1. The explicit build target list (`cmake --build build --parallel 2
   --target …`) does not include `test-groundhog-about`.
2. The Python `required` set in "Require complete Groundhog test
   registration" does not include `groundhog-about` or
   `groundhog-about-gui`.

**Failure scenario**: CI runs `ctest -R '^(groundhog-|…)'` which matches
the newly registered `groundhog-about` and `groundhog-about-gui` tests,
but the executable was never built. ctest reports the tests as "Not Run",
the CI catches this (`grep -Eq '\*\*\*Skipped|Not Run'`), and exits 1.
**Every CI run on this branch will fail.**

**Fix**: Add `test-groundhog-about` to the build target list, and add
`'groundhog-about', 'groundhog-about-gui'` to the `required` set. The
`groundhog-about` test (headless mode, no display) may also be a
candidate for `GROUNDHOG_SANITIZER_TESTS` in the sanitizer job, though
its coverage (no heap allocations, only build-time constants and GTK API
calls) makes this low-priority.

---

### N1 — Nit: Mnemonic overlap between \_About Groundhog and \_Add Contact…

**File**: `gnome/groundhog/data/ui/gh-sidebar-page.blp:302`  
**Severity**: Nit

`_About Groundhog` (mnemonic `A`, section 2) overlaps with `_Add
Contact…` (mnemonic `A`, section 1). In GTK popover menus the mnemonic
scope spans all visible sections, so pressing `A` activates the first
match (`_Add Contact…`); the user must press `A` again to cycle to
`_About`. Mitigated: `_Add Contact…` has `hidden-when:
"action-missing"` and may not be visible in all builds. Still, if both
are present, the mnemonic is ambiguous. A possible fix is to use
`A_bout Groundhog` (mnemonic `b`) or leave as-is given that `_About` is
the standard GNOME HIG convention.

---

## Areas reviewed — no issues found

### Privacy charter compliance ✅

- The website and issue URLs are build-time string constants in
  `gh-about-dialog.c`, set as properties on `AdwAboutDialog`.
  `AdwAboutDialog` opens them via `gtk_show_uri` only when the user
  clicks. Groundhog never fetches them itself.
- The two `url-literal` exceptions in `check_privacy.py` are justified:
  they name the exact URLs, the exact source file, and explain the
  user-action trigger. The privacy-static test passes.
- `translator-credits`: uses the standard GNOME `_("translator-credits")`
  pattern. When untranslated, `AdwAboutDialog` receives the literal
  string `"translator-credits"` and hides the translator section. This
  is correct and intentional (adw_about_dialog.c checks for this
  sentinel).

### Icon correctness ✅

- **Resource layout**: `/org/nostr/Groundhog/icons/{512x512,256x256}/apps/org.nostr.Groundhog.png`
  follows the hicolor icon theme directory structure without the
  `hicolor/` prefix, exactly as `gtk_icon_theme_add_resource_path`
  expects (it implicitly treats the resource path as a theme root).
- **Install destinations**: `$prefix/share/icons/hicolor/{512x512,256x256}/apps/`
  are the correct freedesktop icon theme locations. The standard hicolor
  index.theme includes both 512×512 and 256×256 directory entries with
  `Context=Applications`.
- **Old SVG removal**: `icons/org.nostr.Groundhog.svg` is removed from
  the resource bundle, the CMake build, and the install rule. Grep of
  the entire repo shows the only remaining reference is the test
  assertion that confirms it is gone (`test_about.c:33`). The desktop
  file uses `Icon=org.nostr.Groundhog` (an icon name, not a path),
  which resolves via the icon theme to the installed PNGs.
- **Metainfo**: has no explicit icon element; appstream resolves it from
  the desktop file's `Icon=` key. No change needed.

### Startup order and idempotency ✅

- `gh_about_dialog_register_icons()` runs in `activate()` before
  `create_window()`, so the icon theme resolves the app icon before any
  widget needs it.
- `gtk_window_set_default_icon_name(GROUNDHOG_APP_ID)` runs immediately
  after, setting the window icon for all windows.
- `register_icons()` is idempotent: it checks `g_strv_contains` before
  calling `gtk_icon_theme_add_resource_path`. The redundant call in
  `gh_about_dialog_present()` is harmless defensive coding.
- The composer's own `add_icon_path()` (gh-composer.c:559) uses the
  same path (`/org/nostr/Groundhog/icons`) with the same idempotency
  check. No duplicate resource paths result.

### HIG ✅ (minus the mnemonic nit above)

- `_About Groundhog` is placed after `_Keyboard Shortcuts` and before
  `_Quit`, in the standard GNOME primary menu position.
- The action is `app.about`, the standard scope for About dialogs.
- `AdwAboutDialog` provides all standard accessible names.

### Build system ✅ (minus the CI blocker above)

- The `groundhog-about` static library and `test-groundhog-about`
  executable are correctly defined in CMakeLists.txt.
- The test registers two ctest entries: `groundhog-about` (headless)
  and `groundhog-about-gui` (display, skip-return-code 77).
- Both set `TIMEOUT 60`, `G_DEBUG=fatal-criticals,fatal-warnings`,
  `SKIP_RETURN_CODE 77`.
- `check_data.py` updated to assert PNGs exist and old SVG is absent.

### Test correctness ✅

- `test_icon_resource`: decodes both PNGs from the resource bundle,
  asserts width/height match the directory name, asserts alpha channel,
  asserts the old SVG is absent.
- `test_gui_dialog`: registers icons, creates the dialog, checks all
  properties, presents it in a window, spins the main loop, asserts
  mapped, force-closes. Clean lifecycle via GObject floating reference.

## Build and test results

| Test | Result |
|------|--------|
| `groundhog-about` | ✅ Passed |
| `groundhog-about-gui` | ✅ Passed |
| `groundhog-data` | ✅ Passed |
| `groundhog-privacy-static` | ✅ Passed |
| `groundhog-resource` | ✅ Passed |
| `groundhog-launch` | Skipped (no display — expected on macOS) |
| `check-unsequenced-args.py` | ✅ Passed |
| `linux-gate.sh` | ✅ 453 tests passed |
