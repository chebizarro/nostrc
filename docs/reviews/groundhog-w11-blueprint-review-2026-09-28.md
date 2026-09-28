# Groundhog W11 Blueprint migration peer review — 2026-09-28

## Context / scope

This is an independent review of exactly `9efd0ad96341fc0c9c8a700d58ab5adc872c5404..a0d6d4a27710a3ea43e2cf7b5a916274ab7ad59a`. The range contains:

- `a0d6d4a2`: Groundhog UI moved from procedural C to Blueprint templates, and Groundhog bumped to 0.5.3 PATCH (nostrc-qp24.8.3).
- `0544bd99`: a beads-only commit, which this review ignores.

I compared the new Blueprint and C against the pre-change C UI at `9efd0ad9`: `src/main.c` `create_window`, `src/app/gh-shell.c` and `src/app/gh-account-ui.c`. I also reviewed:

- the build integration against the repository convention (`apps/gnostr/CMakeLists.txt:41-117`, `nostr-gtk/CMakeLists.txt:58-84,245-247`, `cmake/Blueprint.cmake`);
- the drift check, the CI change, the tests and the version bump.

This review covers the code and tests only. It does not cover Groundhog release readiness or a manual GNOME screen-reader check. I changed no code or beads.

## Findings

### Blocking

None.

### Reviewed areas — no blocking finding

- **The widget tree is preserved.** Each piece of the old procedural tree maps to Blueprint with the same values:

  | Old procedural tree | New Blueprint |
  | --- | --- |
  | Window title `Groundhog`, icon `org.nostr.Groundhog`, 900×600 default size | Same (`gh-window.blp`) |
  | `AdwToastOverlay` → `AdwNavigationSplitView` with `show-content: false` | Same |
  | Sidebar and content `AdwNavigationPage`s titled `Conversations` and `Messages` | Same |
  | `AdwToolbarView` with a top `AdwHeaderBar` on each page | Same |
  | Sidebar `AdwWindowTitle` titled `Groundhog`, subtitle left empty | Same; the test asserts the subtitle is `""`, not unset |
  | Stack page order `conversations`, `empty`, `error` | Same (`gh-sidebar-page.blp`) |
  | Content page: vertical `Box` holding a revealed `AdwBanner` (same title) and the `mail-read-symbolic` status page | Same (`gh-content-page.blp`) |

  Status pages keep their old properties:
  - The `hexpand`/`vexpand` split is kept. The shell's `empty`/`error`/onboarding pages and the content page expand. The five account-state pages still do not, just as the old `state_page()` did not.
  - Every icon, title and description string is byte-for-byte the old string.
  - Each has the `groundhog-shell-status` class.
  - Action buttons keep mnemonic `use-underline`, `halign: center` (`3` in the compiled `.ui`), the `pill` class and `account.refresh`.

  The menu keeps the same structure: an identities section, then "No Account (Read-Only)" → `account.select` with an empty string target, and "_Refresh Accounts" → `account.refresh`. GtkBuilder parses an untyped `<attribute name="target">` as type `s`, so the target type is unchanged; `test_account_ui.c:152-162` asserts it.
- **Accessibility is preserved.**
  - **Labels.** The accessible labels `Conversations` (list) and `Conversation list status` (stack) come from Blueprint `accessibility {}` blocks, and so do the account `MenuButton`'s label and tooltip `Account`. Tests assert them through `gtk_test_accessible_assert_property`, along with the list's `GTK_ACCESSIBLE_ROLE_LIST` (`test_shell_layout.c:78-82`, `test_account_ui.c:148-150`).
  - **Announce logic.** It is unchanged (`gh-account-ui.c:96-117`): the announcement fires only on a real page transition.
  - **Focus target.** It moved from `"gh-state-action"` object data to `adw_status_page_get_child()` of the visible page. This is equivalent today: exactly the pages that used to set the tag have a child, the others have none, and `empty` has no child. The test asserts focus lands on the button, and that a network-monitor notify does not steal focus back. See non-blocking item 3 for the new implicit invariant.
- **The 600sp breakpoint is preserved.**
  - **Declaration.** `Adw.Breakpoint { condition ("max-width: 600sp") setters { split.collapsed: true; } }` compiles to the same condition and setter that the C code added with `adw_application_window_add_breakpoint`.
  - **New mapped test.** It is not mock theater. It shows no breakpoint and a non-collapsed split at 900px, then a collapsed split at 400px with the current breakpoint's condition string equal to `max-width: 600sp` (`test_shell_layout.c:185-206`). It passed under Xvfb, and also on macOS.
- **Build flavours.**
  - **Standalone.** With `GROUNDHOG_HAVE_ACCOUNTS=0`, `gh_sidebar_page_show_onboarding()` adds the builder-file onboarding page as `onboarding` and makes it visible. This matches the old `#if !GROUNDHOG_HAVE_ACCOUNTS` block. The smoke check now also asserts this in standalone builds (`main.c`), and `test_shell_layout.c:46-55` checks the 4-page versus 3-page shape.
  - **Accounts.** Accounts builds still have no onboarding page (`test_account_ui.c:141-142`). The account pages are added after `conversations`/`empty`/`error`, in the old order and under the old stack names.
- **Template lifecycle.**
  - **Dispose.** All three templates call `gtk_widget_dispose_template()` in `dispose`. That function (GTK ≥ 4.8; Groundhog targets 4.14) drops the template-child references and NULLs the bound instance fields, so no separate `g_clear_pointer` is needed. No type owns non-template state, so no `finalize` is needed.
  - **Builder files.** The `GtkBuilder` in `gh_account_ui_attach` and in `show_onboarding` is `g_autoptr`. Each built widget is reparented (stack, header bar) before the builder is dropped. The `account_menu` is kept alive by the `MenuButton`'s `menu-model`. The `identities` section gets an explicit ref that `account_ui_free` releases.
  - **Signals.** `changed` and `notify::network-available` are still connected with `g_signal_connect_object(..., window, G_CONNECT_SWAPPED)`. GObject invalidates these closures in the window's base `dispose`, and nothing re-enters `update()` synchronously during the template dispose that runs first. An app-lifetime `GhAccountController` therefore cannot call into a destroyed window. This mechanism is unchanged from `9efd0ad9`.
  - **Finalization.** `test_account_ui.c:204-211` destroys the window and then waits on a weak pointer for the controller to finalize. That wait succeeds only if the window finalizes and `account_ui_free` drops its controller ref. It would therefore catch a reference cycle introduced by the template or builder.
- **Convention fit and the deliberate difference from gnostr.**
  - **Same pattern.** Groundhog follows the repository pattern: the `.blp` is authoritative, the compiled `.ui` is committed beside it, and the build works with or without `blueprint-compiler`.
  - **The difference.** gnostr and nostr-gtk compile *into the source tree* whenever a compiler exists. Groundhog compiles into `build/blueprint/data/ui/`: `Blueprint.cmake` resolves `_rel_dir` against the caller's `CMAKE_CURRENT_SOURCE_DIR`. It then passes that directory as the first `--sourcedir`, and `glib-compile-resources` searches `--sourcedir`s in order, so the fresh output wins. I confirmed the generated `build.ninja` orders them that way.
  - **Why the difference is sound.** Blueprint 0.12 and 0.20 emit different bytes, so the gnostr approach would dirty tracked files on every developer host. The explicit `groundhog-update-ui` target is never part of `ALL`, and the drift test plus CI turn stale `.ui` files into a failure rather than silent churn.
  - **Trade-off.** Stale committed `.ui` files only affect builds without a compiler, and the gate is CI, not the local build. That is the right place for it.
  - **Build dependencies.** The build-dir outputs are `DEPENDS` of the resource command, so editing a `.blp` recompiles and re-bundles. Without a compiler, the committed `.ui` files are the `DEPENDS`.
  - **Version gate.** A compiler older than 0.12 falls back to the committed `.ui` with a warning. Because `groundhog-blueprint` is then not registered, CI's required-test list fails loudly if the runner's compiler regresses.
- **The drift check is real.** I mutated scratch copies of `data/ui` and ran `check_blueprint.py` against each:

  | Mutation | Result |
  | --- | --- |
  | Clean tree (0.20.4) | Pass |
  | Breakpoint condition in `.blp` | **Fail** |
  | Accessible label in `.blp` | **Fail** |
  | Translatable marker removed in `.blp` | **Fail** |
  | Property order swapped in `.ui` | **Fail** |
  | New `.blp` with no `.ui` | **Fail** |
  | `translatable="yes"` → `"true"` in `.ui` | Pass, by design |
  | Whitespace padding inside a string in `.ui` | Pass; see non-blocking item 1 |

  - **`check_data.py`.** It adds a bidirectional `.blp`↔`.ui` pairing and requires the gresource `ui/` entries to equal exactly the compiled set.
  - **Normalization.** Only the header comment and whitespace-only text are dropped, and the `translatable` spelling is unified. Element order, every attribute and every value still count. Apart from item 1, this is not too lax.
- **CI.**
  - **Compiler and registration.** CI installs `blueprint-compiler`, which is 0.12.0 on Ubuntu 24.04, prints its version, and adds `groundhog-blueprint` to the required-registration set.
  - **Clean-tree gate.** A final `git diff --exit-code` catches any rewrite of a tracked file. CI configures `BUILD_NOSTR_GTK=OFF` and `BUILD_APPS=OFF`, so the in-source gnostr and nostr-gtk rewrites cannot trip that gate.
  - **Skip-grep.** It still fails the job if any Groundhog test is skipped under Xvfb.
- **The tests are real.**
  - **Real resource and templates.** The GUI tests link `groundhog-ui`, the same static library and compiled resource the app ships. They instantiate the real `GhWindow`, `GhSidebarPage` and `GhContentPage`.
  - **Assertions.** They check structure, types, CSS classes, actions, menu attributes and accessible properties. They also map a window to test the breakpoint.
  - **Skipping.** They still self-skip with 77 only when `gtk_init_check()` fails. `groundhog-launch` keeps its macOS opt-in skip.
  - **Resource test.** `test_resource.c` now asserts that all five `ui/*.ui` resources exist and are non-empty.
  - **Remaining fake.** The only fake is the in-process identity store, which was already present and is documented.
- **Version and plan-doc rule.** The version is 0.5.3 in both `gnome/groundhog/CMakeLists.txt:2` and `VERSION_MANIFEST.md:21`, changed in the implementation commit. The plan-doc version sentence is updated as well (`docs/plans/groundhog-gnome-messaging-2026-09-25.md:196`). PATCH is correct: Groundhog is an application with no installed API, the user-visible tree and strings are unchanged, and the new resource paths are internal. The plan now states that all Groundhog UI must be Blueprint (`:126`), as the bead's acceptance criteria require.

### Non-blocking (follow-up suggested)

1. **`check_blueprint.py` masks whitespace changes inside strings (`tests/check_blueprint.py:30`).**
   - **Cause.** `(element.text or "").strip()` also strips meaningful edge whitespace.
   - **Effect.** A committed `.ui` whose label differs from the `.blp` only by leading or trailing spaces (reproduced with `" Messages "`) passes, and a no-compiler build would ship that string.
   - **Suggested fix.** Drop the text only when it is whitespace-only, and otherwise compare it verbatim: `text = element.text if (element.text or "").strip() else ""`.
2. **A pre-existing `groundhog-account` failure is misdiagnosed in nostrc-qp24.8.5.** This is outside the range: the test binary contains none of the reviewed code.
   - **What I saw.** In my serial `ctest` run, `groundhog-account` reported **Timeout 30 s**. The log shows `/groundhog/nip17/envelope-cancel-switch` aborting with `GLib-FATAL-WARNING: poll(2) failed due to: Bad file descriptor`. It then hangs because the aborted test orphans its `GTestDBus` `dbus-daemon`, which keeps ctest's output pipe open.
   - **Reproduction.** Running that one subtest in isolation, serially and without load, it failed **2/30** times with rc 133 and left 5 orphaned `dbus-daemon`s, which I killed.
   - **Why the bead is wrong.** This is a closed or reused file descriptor, most likely a socket or pipe fd closed while a `GMainContext` still polls it during cancel or account switch. It is not CPU starvation under `-j8` as the bead states.
   - **Suggested next step.** Update nostrc-qp24.8.5 with this signature. The fix belongs in the NIP-17 envelope cancel path or its test's D-Bus teardown, not in a longer deadline.
3. **The focus target is now an implicit invariant (`gh-account-ui.c:108-116`).** Any `AdwStatusPage` child now receives `grab_focus` when its page becomes visible. If a future state page puts a non-actionable child there, such as a spinner or a box of several buttons, focus will go somewhere odd. The `.blp` comment documents the rule and `assert_account_widgets` pins today's five pages. Keep that assertion in step when pages are added, or reintroduce an explicit marker such as a CSS class or a builder id.
4. **The app ID is now duplicated in Blueprint (`data/ui/gh-window.blp:9`).** `icon-name: "org.nostr.Groundhog"` replaces the `GROUNDHOG_APP_ID` macro. `check_data.py` cross-checks the app ID across the desktop, service, metainfo, schema and icon files, but not across `.blp`. Add `gh-window.blp` or `.ui` to that check.
5. **The CI clean-tree gate only sees tracked files (`.github/workflows/groundhog-ci.yml:90-94`).** `git diff --exit-code` would not notice a build that *creates* files in the source tree. `test -z "$(git status --porcelain)"` covers both cases. Today the build writes nothing to the source: my Docker run mounted the source read-only and passed.
6. **Minor observations, no action needed now.**
   - **Translatable strings.** The strings are now marked `_()`, but Groundhog sets no gettext translation domain yet, so GtkBuilder falls back to the process default and runs unchanged until i18n lands. When it does, set the builder `translation-domain` or `textdomain`.
   - **Bundling.** Both flavours bundle both builder files: `gh-account-ui.ui` in standalone builds and `gh-onboarding-page.ui` in accounts builds. Each file is only a few KiB and only its own flavour loads it.
   - **Unusable compiler.** A host whose `blueprint-compiler` passes `--version` but lacks the Gtk 4 or Adw 1 typelibs fails the build instead of falling back. gnostr and nostr-gtk behave the same way.

## Verification

- **Main build.** I ran `git submodule update --init third_party/nsync third_party/nostrdb`, then `cmake -S . -B /tmp/gh-w11-review -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF && cmake --build /tmp/gh-w11-review -j8` on macOS with blueprint-compiler 0.20.4. It built 1107/1107 targets and configured with "compiling Blueprint with blueprint-compiler 0.20.4".
- **Main tests.** `ctest --test-dir /tmp/gh-w11-review -R groundhog- --output-on-failure` gave 16/17 passed, including `groundhog-shell`, `groundhog-account-ui`, `groundhog-resource`, `groundhog-data` and `groundhog-blueprint`.
  - `groundhog-launch` was the expected macOS skip.
  - `groundhog-account` timed out because of the pre-existing NIP-17 EBADF abort in non-blocking item 2. Re-run alone it passed 3/3.
- **Linux, CI-equivalent.** I used Docker image `local/groundhog-ci:24.04`, installed blueprint-compiler **0.12.0** with apt, and mounted the **source read-only**. I configured with CI's exact flags, built CI's target list, and ran `dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' ctest -R '^groundhog-'`. The result was **18/18 passed with no skips**, including `groundhog-launch` (accounts flavour), `groundhog-shell` with the mapped breakpoint test, `groundhog-account-ui` and `groundhog-blueprint`. So the committed `.ui` files match 0.12 output, and the build writes nothing to the source tree.
- **Standalone `gnome/groundhog` (`GROUNDHOG_HAVE_ACCOUNTS=0`).** I built it three ways:

  | Compiler | Configure message | Tests |
  | --- | --- | --- |
  | None (`BLUEPRINT_COMPILER-NOTFOUND`) | "not found; using the committed data/ui/*.ui" | 7/7 |
  | Fake compiler reporting 0.11.0 | Warning "older than 0.12.0; using the committed" | 7/7 |
  | Real 0.20.4 | Build-dir `--sourcedir` ordered first | 8/8, incl. `groundhog-blueprint` |

  `groundhog-launch` is the macOS skip in each case. The worktree stayed clean after every build.
- **macOS GUI smoke.** The opt-in `GROUNDHOG_RUN_GUI_SMOKE=1 groundhog --smoke` timed out (20 s) in both flavours. The standalone `9efd0ad9` base build times out the same way, so this is the pre-existing nostrc-qp24.8.2 and not a regression.
- **Drift-check mutations.** See the table above.
- **Whitespace.** `git diff --check 9efd0ad9..a0d6d4a2` passed.

**APPROVED**: scoped to `a0d6d4a27710a3ea43e2cf7b5a916274ab7ad59a` versus `9efd0ad96341fc0c9c8a700d58ab5adc872c5404` (the beads-only `0544bd99` excluded). The Blueprint migration preserves the widget tree, accessible properties, focus and announcement behaviour, stack page names, banner and toast wiring, the 600sp breakpoint and the standalone flavour, with sound template lifecycle and real tests. The six non-blocking items above are recommended follow-ups; item 2 concerns a pre-existing, out-of-range flaky test whose bead diagnosis should be corrected.
