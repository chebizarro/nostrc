# Identity pickers and sidebar: UX, behavior and boundary review

Date: 2026-11-07. Review task: `nostrc-w20e6`; implementation: `nostrc-u6v55.72` and `nostrc-u6v55.73`.

## Boundary and verdict

Reviewed exactly `git diff de2a40876...8f3851a43` on `grotto/identity-picker-sidebar`:

- Reviewed HEAD: `8f3851a4389412cfd93d3bb7fe7eef5df56a4aae`; base: `de2a40876`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-60ebfe6b-grotto-identity-picker-sidebar-d04545bd`.
- Scope: paste/scan pairing preview, Connection Link, permission-editor identity choice, shared picker/cache, both hosts' sidebar ordering and stable navigation, catalogs, Blueprint drift and unchanged versions. No full suite, push, merge, deployment or product fixes.
- This review changes only this report. Diagnostic source files and receipts are outside the repository, under `/tmp/grotto-review-20261107/`.

**Verdict:** all eight requested checks pass (9/9 with the automatic test-home fixture). Cache-only rendering, public-key selection and the reordered navigation are sound in the reviewed paths. **Request changes for the accessible-name export gap, F1.** F2 is a smaller discrepancy between the explicitly stated fallback hierarchy and the implemented extra profile-name tier. Neither finding demonstrates a wrong-key dispatch, secret exposure or new network fetch.

Evidence labels: **confirmed/executed** means run during this review; **confirmed/static** means established from source and its production caller. S2 denotes an accessibility/conformance defect and S3 a low-priority presentation discrepancy, not exploit severity. References are repository-relative at the reviewed HEAD. Container/Xvfb/AT-SPI evidence is not native desktop, hosted CI, Orca speech, owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed and static — The explicit label-based accessible name is attached to a nameless generic container, not exported on popup options

**Where:** `gnome/grotto-panels/src/grotto-identity-picker.c:48,63,89-94`; test assertion at `gnome/grotto-panels/tests/test-management-panels.c:386-395`.

`picker_render()` sets `GTK_ACCESSIBLE_PROPERTY_LABEL` on `p->box`, an ordinary `GtkBox`. This retains the generic accessible role; GTK describes that role as a nameless container ([GTK role reference](https://docs.gtk.org/gtk4/enum.AccessibleRole.html)). The factory does not name the selectable popup item. The test checks the stored property on the avatar's parent under `GTK_A11Y=test`, rather than the exported option name. This falls short of the requested label-based accessible names and the [HIG accessible-name guidance](https://developer.gnome.org/hig/guidelines/accessibility.html#accessible-names).

**Failure scenario:** open any of the three shared identity pickers and use the keyboard to focus an option. An AT-SPI client requesting that focused option's accessible name receives an empty string, not the device label. A diagnostic linked to the unchanged production panels library, with two identities both labelled `Same device label`, exported:

```text
combo box name='Identity to Connect'
  ... list item name='' states=focused,selected,showing
        ... label name='Same device label'
        ... label name='npub1qqqqqqq…zqujme'
      list item name='' states=showing
        ... label name='Same device label'
        ... label name='npub1lllllll…q7lrjw'
```

The intervening generic panels also exported empty names. The child text **is** accessible, including the distinct short keys; this is not a claim that all identity text disappears or that Orca necessarily speaks silence. No Orca speech session was run. It is a confirmed failure of the explicit accessible-name export, masked by the test-backend property assertion. Expose the label on an appropriately named accessible option/container and preserve the key discriminator; verify the real backend rather than only the stored generic-box property. Keyboard selection itself passed and retained the correct second key.

### F2 — S3, confirmed/static — The fallback hierarchy inserts `profile.name` before the requested short npub

**Where:** `gnome/grotto-panels/src/grotto-identity-picker.c:39-43`; intentional test expectation at `gnome/grotto-panels/tests/test-management-panels.c:432-440`.

The current review brief specifies device label → `display_name` → short npub. The implementation instead uses device label → `display_name` → profile `name` → short npub. Its test deliberately covers this extra tier, and the implementation bead also records it; it is not accidental test drift.

**Failure scenario:** an identity has no device label, cached `display_name` is empty, and cached `name` is nonempty. All three pickers show that profile `name` as the primary text/initials seed, rather than falling back to the short npub as stated in the current brief. The short npub remains secondary and the underlying selected key is unchanged. This is a small scope/conformance discrepancy, not an authorization flaw: either follow the stated hierarchy or obtain explicit owner acceptance of the extra tier.

## Owner-policy and regression checks

- **Real picker entrypoints:** paste and scan converge on `grotto_pairing_present()` (`apps/grotto/src/ui/pairing-flow.c:97-150`), which obtains the real window service and constructs the shared picker. Connection Link uses it at `apps/grotto/src/ui/sheets/sheet-qr-display.c:408-415`, through the window action and page fallback (`signer-window.c:325-329`, `page-connect.c:45-51`). The inactive wildcard permission editor uses it at `gnome/grotto-panels/src/grotto-app-detail.c:298-311`. No remaining raw-key-only signing-identity combo/drop-down was found by searches of both source trees and UI resources for `GtkDropDown`, `AdwComboRow`, string-list construction, selection and chooser wording. The other combos are permission policy/duration/kind, lock timeout and activity filters, not identity selection.
- **Remaining key displays, not silently classified as fixed:** the unchanged Other Identities navigation list uses a device-label/`Identity` title, cached avatar and full-key secondary text for signing identities (`gnome/grotto-panels/src/grotto-identity-panel.c:212-234`). It is not one of the three changed signing-choice controls and does not gain the new profile-name fallback. Fixed-identity permission details still show the key at `grotto-app-detail.c:311`; that is a scope display, not a chooser. Public-key/detail/backup displays are likewise intentionally not relabelled by this patch.
- **Cache-only, not an application-wide offline claim:** rendering calls only `grotto_panels_service_dup_profile()` and reads the identities model (`grotto-identity-picker.c:21-48`). The accessor searches/copies the in-memory display cache and does not invoke a service fetch (`gnome/grotto-panels/src/grotto-panels-service.c:107-125`); the production model accessor just returns its existing model (`apps/grotto/src/grotto-panels-control1-service.c:292-294`). Avatar rendering uses an already-cached texture or initials. Profile/model signals rerender without adding I/O. The executed management-picker test ends with zero fake-service command invocations. This is source-grounded proof about picker rendering, not packet-capture proof that opening the whole app/Connection Link sheet never contacts anything: existing inventory/status/link D-Bus work and independent remote-signing/profile services remain outside that claim.
- **Duplicate labels and exact key mapping:** the isolated production-factory probe used two model keys with the same device label. Both label/short-key pairs appeared in the popup's exported child text, and Return → Down → Return selected index 1 with the exact second model key. Production pairing submits the selected `GtkStringObject`, not its label (`pairing-flow.c:66-76`); Connection Link does the same (`sheet-qr-display.c:338-342`), with executed two-identity selection/reopen coverage (`test-ui-layout.c:1725-1740`). Permission save uses the parallel key array at the same zero-based index and rejects an invalid selection (`grotto-app-detail.c:210-216,299-310`); the executed real-host fixture retains its explicit-selection/fail-closed permission assertions. The duplicate-label probe is not an end-to-end daemon authorization test of all three flows, and the paste fixture checks selection/presentation rather than dispatching Review. F1 remains the accessible-name exception.
- **Avatar/fallback behavior:** the management-picker test exercises selected and popup factories, device-label priority, cached display/profile names, cached image, initials fallback, and profile/model refresh. It passes. Text is assigned with `gtk_label_set_text`, and cosmetic labels never replace the model's public keys. F2 records the exact hierarchy discrepancy rather than claiming the requested three-tier chain is implemented literally.
- **Keyboard and sizing:** the shared picker opens and changes selection with real X11 Return/Down/Return input. Production preview min-width and 360-pixel sizing assertions pass in `signer/ui-layout`; the separate `signer/dialog-sizing` check passes. Spanish/RTL and accessibility fixture execution passes; that does not override F1's real-backend observation.
- **Sidebar order, both hosts:** descriptors and actual widgets are Identity, Connected Apps, Remote Signing, Activity (`gnome/grotto-panels/src/grotto-apps-panel.c:24-30`, `gnome/nostr-settings/src/ui/nss-window.c:11-16`). Grotto's name/index maps, remote configuration/link binding and Activity filtering consistently use the reordered positions (`signer-window.c:170,345,739,903-910,944,985`). An isolated copy of the existing real-window fixture, linked to unchanged production objects, passed Down/Return from Apps to Remote Signing (index 2), then Activity (index 3), and Up/Return back. In the actual Nostr Settings executable, Shift+Tab reached each selected sidebar row and Down/Return and Up/Return selected the expected neighboring page. No source was changed for these diagnostics.
- **Stable page/deep links:** Nostr Settings' actual executable was launched separately with `--page remote-signing` and `--page activity`; AT-SPI selected-row assertions passed for both. Its CLI forwarding remains `src/ui/main.c:11-18,30-34,47-50`; panel-mount also asserts correct page types and row-to-page names. Grotto has no `--page` option in this HEAD: its existing management action forwards section/target to `signer_window_open_management()` (`apps/grotto/src/main_app.c:736-748`), and its `identity`, `apps`, `remote-signing`, `activity` identifiers and Activity target routing remain unchanged. Grotto's real-window navigation assertions passed; no Grotto CLI `--page` pass is claimed.
- **Boundary, catalogs and generated UI:** `grotto-panels/no-secrets` passes. The picker adds presentation/cache reads, not secret material, signing, host-policy dispatch or network APIs. All five Grotto `.po` catalogs and panels Spanish pass `msgfmt --check --check-format`; none has fuzzy entries, and neither POT has fuzzy markers. Blueprint 0.22.2 drift passes against the committed UI/resources, including panels. No `.blp` or `.ui` is changed in the reviewed diff.

## Verification actually run

Image: `nostrc-linux-ci:arm64`, ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; executed architecture `aarch64`, GTK **4.14.5**, libadwaita **1.5.0**, Blueprint **0.22.2**. A new disposable container mounted this worktree read-only at `/src`. A private copy of the existing read-only `nostrc-hig-build` seed was reconfigured/rebuilt at `/build`; the seed and other agents' containers/volumes were not modified. `xdotool`, AT-SPI and its Python bindings were installed only in this container. Test homes/buses were isolated; no developer keychain command was run.

| Command/check | Actual result |
|---|---|
| `cmake -S /src -B /build -DGROTTO_TEST_XDOTOOL=/usr/bin/xdotool -DENABLE_NOSTR_SETTINGS=ON` | PASS after setup corrections below; inherited cache uses `NIP55L_SECRET_BACKEND=libsecret`, `NIP55L_TEST_TRUST_ENV=ON` |
| `cmake --build /build --parallel 1 --target test-ui-layout test-qr-scanner test-grotto-panels-smoke test-grotto-panels-admission test-nostr-settings-panel-mount nostr-settings` | PASS; real host/daemon and requested test prerequisites rebuilt |
| `xvfb-run -a ctest --test-dir /build --parallel 1 -R "^(signer/(ui-layout\|ui-accessibility\|dialog-sizing\|qr-scanner\|blueprint-drift)\|grotto-panels/(management-panels\|no-secrets)\|nostr-settings/panel-mount)$" --output-on-failure` | PASS, **9/9**, including automatic test-home fixture; **198.51 s**, no skips |
| `msgfmt --check --check-format -o /dev/null <catalog>` for all six shipped catalogs; fuzzy-marker searches of those catalogs and both POTs | PASS |
| Production shared-factory duplicate-label keyboard/AT-SPI diagnostic | Correct distinct short keys and exact index-1 key selection; empty option/generic-container names exported as F1, not an accessibility-name pass |
| Isolated real Grotto-window fixture with only sidebar keyboard assertions added, selecting `/signer/layout/real-templates-navigation-focus-screenshots` | PASS, 1/1; bounded diagnostic returns after sidebar assertions, not a replacement full layout-suite run |
| Actual Nostr Settings `--page remote-signing` / `--page activity`, plus sidebar keyboard and AT-SPI selection assertions | PASS, including Shift+Tab keyboard reachability and Remote Signing ↔ Activity order |
| `git diff --check de2a40876...8f3851a43`; version-source diff inspection | PASS; source version declarations unchanged |

Local receipts: `/tmp/grotto-review-20261107/{configure.log,build.log,tests.log,LastTest.log,catalogs.log,environment.log,picker-selection.log,picker-atspi-*.log,sidebar-keyboard.log,nss-deeplink.log,nss-*-atspi.log}`; diagnostic sources/scripts are alongside them. The container is removed after receipt collection; the existing seed volume and worktree are preserved.

Setup/diagnostic failures are not concealed or counted as product-test passes: the image already had `/build`; this worktree initially lacked the pinned nostrdb checkout; the seed had Nostr Settings disabled. The configured local submodule clone initially refused file transport, then succeeded with a command-local allowance at the recorded revision `2d459e8fa9ba0af5af091c37920e5d2078d881ca`. Missing generated-header includes initially stopped the standalone diagnostic compile. The first duplicate-label diagnostic accidentally replaced the fake model twice, leaving one metadata row; only the corrected two-row fixture is duplicate-label evidence. Reusing a populated fixture home initially aborted the sidebar diagnostic; the successful run used a fresh home. Nostr Settings' AT-SPI `grabFocus` was unsupported, and pointer-seeded focus moved into content; the successful check used real Shift+Tab reachability instead. Earlier failure/setup logs remain alongside the final receipts. No product test was retried to manufacture green results: the requested equipped/configured CTest run passed on its first execution.

The full repository suite, unrestricted affected gate, sanitizers, native desktop/Orca speech, live relays/keyring, deployment and owner acceptance were not run. No accepted unrelated baseline was repaired, rerun as a new prerequisite or counted as a regression. Dolt synchronization was attempted once and reported no configured remote; no synchronization route or code push was invented.

## Version and delivery decision

Source and manifest versions are unchanged across the reviewed range: Grotto **0.1.1**, grotto-panels **0.1.0**, Nostr Settings **0.1.0** (`VERSION_MANIFEST.md:31,34-35` and their authoritative CMake/Meson declarations). The panels build-file changes only add the new source to the two inventories; they do not change version declarations. Grotto/Settings UI corrections are PATCH-class; the exported `grotto_identity_picker_new()` is additive public functionality, normally MINOR-class for panels. Per the current explicit unchanged-version direction, these remain in their existing unreleased waves. No release columns or tags were changed or published. This review is documentation-only: **no component bump**.

Delivery is this report, committed **locally only** on `grotto/identity-picker-sidebar`; implementation HEAD above remains the reviewed source boundary. The report commit is not an implementation fix. No push or merge is authorized or performed.

## Merge recommendation

**Request changes for F1 before merging unchanged.** Make the intended label-based name reach the real accessible option/container, retain the distinct key text and key-based selection, and verify that finite correction on the actual backend. Resolve F2 by following the stated fallback hierarchy or explicitly accepting the extra profile-name tier. Keep the cache-only factory, existing selection/permission behavior, compact layout, new sidebar order and stable page names; there is no evidence here for changing authorization, adding confirmation steps, making rendering fetch profiles, or starting a broader qualification programme. All requested focused checks are green; the exported-name defect is a targeted diagnostic finding, not a claim that those checks failed.

## Re-check (5d83516f2)

Date: 2026-11-07. Re-check task: `nostrc-xarpy`. Reviewed fix: `5d83516f26e16b84ae1987b427173aac7bc2acdb`, on the same local branch/worktree. Scope was only F1/F2 and repetition of the original serial checks; `git diff 5d83516f2^..5d83516f2` changes the picker, its public comment, focused management tests and the two GTK accessibility imports in the boundary allowlist. No implementation, catalog, template, version or test file was modified by this re-check. The original findings above remain historical evidence.

### Findings resolved

- **F1 resolved — confirmed/executed and static.** `gnome/grotto-panels/src/grotto-identity-picker.c:47-50` now sets the actual `GtkListItem` accessible label to the chosen identity name and its description to the short npub, rather than labelling the generic box. A weak reference avoids an item/child ownership cycle (`:9,18,66`); unbind clears both accessible properties (`:100-105`). The updated management test checks the option's list-item role, label and description, including duplicate labels, cached images and model/profile updates. It passed against GTK 4.14.5/libadwaita 1.5.0.
- **Real AT export, not just test-backend properties:** a separately compiled diagnostic used the unchanged production panels library and two distinct public-key model items both labelled `Same device label`. Under `GTK_A11Y=atspi`, the selected presentation and both popup options exported that name, with distinct descriptions `npub1qqqqqqq…zqujme` and `npub1lllllll…q7lrjw`. The focused popup option was named, not empty. Return → Down → Return selected index 1 with the exact second full key; the closed selected presentation then exported the second key's description. Thus the same-label options remain distinguishable to AT without substituting their cosmetic label for their key. This verifies exported AT-SPI metadata and keyboard behavior, not Orca spoken output.
- **F2 resolved — confirmed/executed and static.** The renderer now implements exactly device label → cached `display_name` → short npub (`grotto-identity-picker.c:31-42`), with no `profile.name` tier. The independent AT diagnostic ran three separate fixtures: both label and display name populated; empty label with display name populated; then empty label/display name with a deliberately nonempty `profile.name`. Selected, popup and keyboard-chosen names passed at each tier (nine phase assertions). The last fixture exported the short npubs and never rendered `Unexpected profile name`. Each fixture also retained the exact second key after keyboard selection. The production management test independently covers removal of each fallback tier and retains the nonempty profile-name exclusion assertion.
- **No authority/I/O expansion:** models still contain the original keys; the cached profile accessor and avatar path are unchanged. The only newly allowed library imports are the two `gtk_list_item_set_accessible_*` setters. `grotto-panels/no-secrets` passed; no fetch/network/signing/policy path was added.

Representative new backend output (the tuple fields are name, description, focused):

```text
PASS label popup [('Same device label', 'npub1qqqqqqq…zqujme', False), ('Same device label', 'npub1qqqqqqq…zqujme', True), ('Same device label', 'npub1lllllll…q7lrjw', False)]
PASS label chosen [('Same device label', 'npub1lllllll…q7lrjw', False)]
PASS display chosen [('Cached display name', 'npub1lllllll…q7lrjw', False)]
PASS short chosen [('npub1lllllll…q7lrjw', 'npub1lllllll…q7lrjw', False)]
```

### Re-check verification actually run

A new disposable `nostrc-linux-ci:arm64` container used the same image ID recorded above, confirmed `aarch64`, GTK **4.14.5**, libadwaita **1.5.0**, Blueprint **0.22.2**. This worktree was mounted read-only; `/build` was a private copy of the read-only seed, reconfigured with `-DGROTTO_TEST_XDOTOOL=/usr/bin/xdotool -DENABLE_NOSTR_SETTINGS=ON`. `xdotool` and the AT-SPI runtime/Python bindings were installed only in the container. The same six build targets listed above were rebuilt with `--parallel 1`, including the production Settings executable and real-host/daemon prerequisites. Checks and diagnostics ran serially; no full suite or concurrent test run was started.

| Repeated command/check | Actual result at `5d83516f2` |
|---|---|
| Same `xvfb-run -a ctest --test-dir /build --parallel 1 -R` expression recorded above: five signer checks, panels management/no-secrets, Settings panel-mount | **PASS, 9/9**, including automatic test-home fixture; **199.83 s**, no skips; first execution passed |
| Production-factory AT-SPI duplicate-label/fallback diagnostic, three fixtures × selected/popup/chosen phases | **PASS**, nine phase assertions, distinct key descriptions, named focused options and exact second-key selection in all three fixtures |
| Isolated copy of the existing real Grotto-window fixture, linked to current production objects, with sidebar keyboard assertions | **PASS, 1/1**: Apps → Remote Signing (index 2) → Activity (index 3), then back; fresh isolated home; bounded diagnostic, not another full layout-suite run |
| Actual Nostr Settings executable: separate `--page remote-signing` and `--page activity` launches, Shift+Tab reachability, Down/Return and Up/Return | **PASS**: correct selected page, unchanged sidebar order and keyboard navigation |
| `msgfmt --check --check-format -o /dev/null` for all six catalogs, fuzzy-marker checks for catalogs and both POTs | **PASS**, no fuzzy markers |
| Blueprint drift, via the selected CTest check | **PASS**, pinned 0.22.2 |
| `git diff --check 5d83516f2^..5d83516f2`; authoritative version/build/template diff inspection | **PASS**; no version or `.blp`/`.ui` changes in the fix |

Receipts and diagnostic sources: `/tmp/grotto-recheck-5d83516f2/`, notably `tests.log`, `LastTest.log`, `picker-keyboard.log`, `picker-{label,display,short}-{closed,popup,chosen}.log`, the per-tier selection logs, `sidebar-keyboard.log`, `nss-deeplink.log`, `catalogs.log`, `environment.log`, `configure.log` and `build.log`. All new checks/diagnostics passed; earlier failures remain in the original review's separate receipt directory. The task-owned container was removed after collection; the borrowed seed and other resources were preserved. Dolt sync was attempted once and still reported no configured remote. No code or tracker push was performed.

Versions remain Grotto **0.1.1**, grotto-panels **0.1.0**, Nostr Settings **0.1.0**, as directed; no release/tag change. This report-only append warrants **no component bump**. No native desktop/Orca speech, hosted CI, live-relay/keyring, sanitizer, deployment or full-suite acceptance is claimed.

### Final verdict

**Approve the reviewed identity-picker/sidebar changes with `5d83516f2`; F1 and F2 are resolved.** This verdict supersedes the original request-changes recommendation above. No outstanding finding remains within this re-check's scope. Delivery is only this appended report, committed locally on `grotto/identity-picker-sidebar`; no product fix, merge or push was performed by the reviewer.
