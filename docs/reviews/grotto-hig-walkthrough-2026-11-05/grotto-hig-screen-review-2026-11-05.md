# Grotto 0.1.1 (branch `grotto/hig-pass`) — screen-by-screen HIG/UX review

Date: 2026-11-05. Scope: every surface of the installed Grotto desktop app as
deployed on the owner's UTM VM (Ubuntu 24.04.5 arm64, GNOME Shell 46.0, GTK
4.14.5, libadwaita 1.5.0), package `grotto 0.1.1-1+local20261105.<sha>` built
from this branch. Screenshots in this directory were taken in the real GNOME
session through the Screenshot portal and cropped to the GTK frame; the app ran
on XWayland (`GDK_BACKEND=x11`) so it could be driven with `xdotool`. Bead:
`nostrc-u6v55.34`. HIG text is quoted from the `gnome-hig` skill references
(upstream 97028eac3792, 2026-06-20).

**Verdict.** After the conformance pass on this branch the app is a clean pass
on the mechanical HIG rules (menu structure, dialog button order and defaults,
capitalization, ellipses, access keys, boxed lists, symbolic icons, adaptive
layout to 360 px, light and dark). It is **not** a clean pass overall: two
dialogs rendered as unusable ~90 px strips (fixed on this branch after the
walkthrough, F1), the Identity panel can sit on "Loading profile…" indefinitely
with editing blocked (F2), and app principals are shown as raw `exe:` paths
instead of app names (F3). F2 and F3 are the only S2 items left open (F13, also S2, was found by the
Spanish accessibility test and is fixed); the rest are polish.

Evidence labels: `confirmed/visual` = seen in a screenshot here;
`confirmed/static` = seen in source at the reviewed commit; `plausible` = not
reproduced. Severity: S1 blocks comprehension, consent or accessibility; S2 is a
clear HIG deviation a user will notice; S3 is polish.

## Findings, ranked

### F1 — S2, confirmed/visual, FIXED on branch (1caf44739): two dialogs opened as ~90 px strips
Where: `apps/grotto/data/ui/sheets/sheet-qr-display.blp` (Share Public Key / QR
sheet) and `apps/grotto/src/ui/page-settings.c:77` (Ask-by-Default Event Types).
Screenshots: `share-public-key-qr-sheet-before-fix.png`,
`ask-by-default-event-types-before-fix.png`.
What the HIG says: dialogs are "a message or task that the user must respond to
before continuing"; a dialog whose content is clipped to one row cannot be
responded to — https://developer.gnome.org/hig/patterns/feedback/dialogs.html
Why it matters: the QR code and the kind checklist were cut off; the user has
to discover a 60 px scroll region to use them.
Fix: both scrolled windows now propagate natural height
(`propagate-natural-height: true` /
`gtk_scrolled_window_set_propagate_natural_height`), so the floating dialog
sizes to its content. Re-captures after the fix: `share-public-key-qr-sheet.png`,
`ask-by-default-event-types.png`.

### F2 — S2, confirmed/visual, OPEN: "Loading profile…" never resolves for identities without a published profile
Where: Identity panel for the `UTM`, `Gus` and `UdoU` identities
(`identity-panel.png`, `other-identity-detail-gus.png`,
`other-identity-detail-udou.png`): after 4–9 s the panel still shows
"Loading profile…" with **Edit Profile…** insensitive; the `Biz` identity
(which has a published kind 0 and relays) loads in ~3 s
(`identity-panel-biz-profile.png`). Source: `gnome/grotto-panels/src/grotto-profile-view.c`
(`fetched`/`busy` gating of `edit`), `apps/grotto/src/grotto-profile-service.c`
(`discover()` bootstrap REQ).
What the HIG says: "Progress needs to be indicated whenever an operation takes
more than around three seconds"; spinners suit operations under ~30 s and
"should be used sparingly" —
https://developer.gnome.org/hig/patterns/feedback/spinners.html
Why it matters: a user with a fresh identity cannot create a profile at all
while the fetch hangs, and gets no indication whether it will ever finish.
Fix: bound the discovery fetch (EOSE or ~10 s), then render an explicit empty
state ("No profile published yet") with **Edit Profile…** sensitive; show a
`GtkSpinner` next to the text while fetching. Not reproduced to root cause
(plausible: bootstrap REQ waiting for relays that never send EOSE).

### F3 — S2, confirmed/visual, OPEN: apps are named by raw `exe:` principal
Where: `connected-apps-panel.png` ("exe:/usr/bin/groundhog"), `app-detail.png`
header, `activity-panel.png` ("exe:/usr/libexec/nostr-session-relayd"),
`shell-notification-sign-request.png` ("gdbus"). Source:
`gnome/grotto-panels/src/grotto-apps-panel.c`, `grotto-activity-panel.c`,
`apps/grotto/src/approval-controller.c` (notification title).
What the HIG says: list rows should carry a recognisable title and, where
useful, an icon; "Use icons that users will recognise" —
https://developer.gnome.org/hig/patterns/containers/boxed-lists.html,
https://developer.gnome.org/hig/guidelines/ui-icons.html
Why it matters: "exe:/usr/libexec/nostr-session-relayd" tells a normal user
nothing about *which app* used their key — the one fact a signer exists to
convey.
Fix: resolve the principal to a `GAppInfo`/desktop entry (name + icon) when a
`.desktop` file matches the executable or the D-Bus app id; keep the principal
in the row subtitle / Details. The approval dialog already does this partly
("gdbus wants to sign a note", avatar initial).

### F4 — S3, confirmed/visual, OPEN: Permission drop-down option embeds a bare npub
Where: `approval-permission-options.png` — "Always allow this app to sign notes
(kind 1) as npub17qs9l9d…7lqls2p5a92" wraps to four lines in the popover.
Source: `apps/grotto/src/ui/approval_dialog.c` (permission model labels).
What the HIG says: "Try to find the fewest possible words to satisfactorily
convey the necessary meaning" — https://developer.gnome.org/hig/guidelines/writing-style.html
Fix: use the identity's local name ("as UTM") and keep the npub in the
"Signing As" row, which already shows it.

### F5 — S3, confirmed/visual, OPEN: Preferences duplicates main navigation
Where: `preferences-dialog-scrolled-before-fix.png` — rows **Remote Signing**
(navigates to the sidebar panel) and **About Grotto** (same item as the primary
menu). Source: `apps/grotto/data/ui/page-settings.blp`.
What the HIG says: "About App … Every primary menu should include this item";
secondary surfaces "generally shouldn't reproduce menu items that are included
in primary menus, like Preferences and About" —
https://developer.gnome.org/hig/patterns/controls/menus.html
Fix: the About row is removed on this branch (1caf44739); the Remote Signing
row should go too — it is one click away in the sidebar.

### F6 — S3, confirmed/visual, OPEN: stray caption "4 identities on this device"
Where: `identity-panel.png`, under the **Edit Profile…** button row. Source:
`gnome/grotto-panels/src/grotto-identity-panel.c`.
What the HIG says: "Do not build sentences across multiple widgets"; boxed-list
groups carry their own title/description —
https://developer.gnome.org/hig/guidelines/writing-style.html,
https://developer.gnome.org/hig/patterns/containers/boxed-lists.html
Fix: move the count into the **Other Identities** group description, or drop
it (the list already shows them).

### F7 — S3, confirmed/visual, OPEN: group title "Privacy / Network"
Where: `preferences-dialog.png`. Source: `apps/grotto/src/ui/page-settings.c`.
What the HIG says: header capitalization for headings; prose, not symbols —
https://developer.gnome.org/hig/guidelines/writing-style.html#capitalization
Fix: "Privacy and Network" (or split into two groups: "Privacy", "Network").

### F8 — S3, confirmed/visual, OPEN: Lock Management does nothing without a management password
Where: Ctrl+L and the **Lock Management** menu item had no visible effect on
this VM (no password set; `lock-screen.c:136 gn_session_manager_has_password`).
What the HIG says: "Make invalid buttons insensitive, rather than showing an
error message" (and silently doing nothing is worse) —
https://developer.gnome.org/hig/patterns/controls/buttons.html
Fix: make `win.lock` insensitive until a password exists, or route it to the
Preferences row that sets one.

### F9 — S3, confirmed/visual, OPEN: "Add an Identity" window keeps the setup footer
Where: `add-identity-window.png` — launched from **Add Identity…** the window
still shows **Back** / **Skip Setup**, although there is no setup to skip and
nowhere to go back to.
Fix: hide **Back** on the first page and label the secondary action **Cancel**
when the assistant is opened from a running session.

### F10 — S3, confirmed/visual, OPEN: review dialog clips its Details row in a 640 px window
Where: `approval-dialog.png`, `approval-dialog-dark.png` — the **Details**
expander is half hidden behind the footer until scrolled; at 360×518
(`approval-dialog-360.png`) the bottom sheet shows it fully.
What the HIG says: smallest supported desktop size is 1024×600 —
https://developer.gnome.org/hig/guidelines/adaptive.html
Fix: let the footer sit inside the scrolled area or reduce the header block
(avatar + title + caption) so the four content rows and the expander fit at
560 px content height.

### F12 — S3, confirmed/visual, OPEN: indistinguishable rows in Ask-by-Default Event Types
Where: `ask-by-default-event-types.png` — "Encrypted message" appears three
times and "Private message" twice; nothing tells them apart. Source:
`apps/grotto/src/ui/page-settings.c` (kind catalogue labels).
What the HIG says: rows need a title that identifies the item; use a subtitle
for secondary information — https://developer.gnome.org/hig/patterns/containers/boxed-lists.html
Fix: add the kind number (and the NIP where it helps) as the row subtitle, or
merge rows that are the same family.

### F13 — S2, confirmed/executed, FIXED on branch: Spanish lock-screen title overflowed the 350 px floor
Where: `apps/grotto/data/ui/lock-screen.blp` `lbl_title` (`title-1`, no
wrap) — `signer/ui-accessibility` (Spanish, `GTK_A11Y=test`) failed with the
main window requesting 428 px at 350 px after the catalog merge carried
"La gestión está bloqueada" onto the previously untranslated heading.
What the HIG says: design from the narrowest supported size up —
https://developer.gnome.org/hig/guidelines/adaptive.html
Fix: the heading now wraps and is centred; the Spanish string is "Gestión
bloqueada". Both `signer/ui-layout` and `signer/ui-accessibility` pass in the
Linux container.

### F11 — S3, confirmed/static, FIXED on branch: mechanical HIG conformance pass (9ca74364d, f58df4b04, 3f276ec27)
- Primary menu: standard last group **Preferences / Keyboard Shortcuts / About
  Grotto**, no **Quit**, Pause/Resume Signing grouped with Lock Management
  (`primary-menu.png`) — https://developer.gnome.org/hig/patterns/controls/menus.html
- "About Grotto…" → "About Grotto"; Connect rows that open sheets gained
  ellipses ("Scan QR Code…", "Create Pairing Link…") —
  https://developer.gnome.org/hig/guidelines/writing-style.html#ellipses
- Onboarding's final affirmative "Done" → "Open Grotto" ("Label the affirmative
  button with a specific imperative verb … clearer than a generic label like OK
  or Done") — https://developer.gnome.org/hig/patterns/feedback/dialogs.html
- Lock screen heading "Management locked" → "Management Locked"; unlock
  button's accessible description no longer says "Click".
- Access keys on onboarding/backup/sheet buttons, carried through dynamic
  labels and into translations (Spanish collisions resolved; ja/fa use the
  "label(_K)" convention) — https://developer.gnome.org/hig/guidelines/keyboard.html
- Create Pairing Link sheet rebuilt as a boxed list (`AdwComboRow` identity,
  `AdwEntryRow` app name) with a toolbar footer (`create-pairing-link-sheet.png`)
  instead of a bare label + `GtkDropDown` + placeholder-only `GtkEntry` —
  https://developer.gnome.org/hig/patterns/controls/text-fields.html
- "Clear" → "Clear Permissions" on the permissions-conflict alert;
  "authorised" → "authorized".

## Screen-by-screen notes

| Screen | File(s) | Notes |
|---|---|---|
| Identity panel (default identity) | `identity-panel.png`, `identity-panel-scrolled.png`, `identity-panel-bottom.png`, `identity-panel-dark.png`, `identity-panel-360.png` | Banner "This identity isn't backed up" + **Back Up…** is the right vehicle (ongoing state). F2, F6. Sidebar collapses correctly at 360 px (`sidebar-360.png`). |
| Identity panel with a published profile | `identity-panel-biz-profile.png`, `-scrolled.png` | Banner and avatar load (after the redirect fix from nostrc-u6v55.33); NIP-05 status line reads well. |
| Other identity detail | `other-identity-detail-gus*.png`, `other-identity-detail-udou*.png` | Same layout with a back button; **Remove Identity…** is the only destructive button and sits last — good. F2 applies. |
| Share Public Key (QR) | `share-public-key-qr-sheet-before-fix.png` → `share-public-key-qr-sheet.png` | F1. After the fix: full QR, **Copy to Clipboard**, Details expander. |
| Profile editor | `profile-editor.png`, `profile-editor-scrolled.png`, `profile-editor-bottom.png` | `AdwDialog` with Cancel/Save header, preview expander, `AdwEntryRow`s, avatar/banner rows with crop limits, Links/Publication expanders; **Republish** / **Discard Local Changes** as a linked pair at the end. Only reachable once F2 resolves for a fresh identity. |
| Remove Identity alert | `remove-identity-alert.png` | Cancel first, Remove destructive and insensitive until the npub is typed, Esc cancels — matches the dialogs page. |
| Add Identity / onboarding | `add-identity-window.png`, `onboarding-welcome.png`, `-choice.png`, `-create.png`, `-import.png`, `-connect.png` | Status page + per-page navigation headers; F9. Check/Backup/Ready pages need a real identity and were not captured here (see `apps/grotto/data/screenshots/onboarding-*`). |
| Backup page | `backup-page.png`, `backup-page-scrolled.png`, `backup-advanced-expanded.png`, `backup-encrypted-passphrase-sheet.png` | Advanced actions behind an expander; passphrase sheet uses `AdwPasswordEntryRow`. Recovery-phrase and secret-key reveals were deliberately not captured. |
| Connected Apps | `connected-apps-panel.png`, `-dark.png`, `-360.png`, `connected-apps-pending-request.png`, `connected-apps-pending-360.png` | Pending Requests group appears above the switch when a request is waiting; sidebar badge "1". F3. |
| App detail | `app-detail.png`, `app-detail-scrolled.png`, `app-detail-details-expanded.png`, `app-permissions-page*.png`, `app-recent-activity.png` | Radio list for trust level is the HIG choice for 3 options; **Remove App…** destructive pill last. The Local Name entry takes initial focus with a visible ring (minor). F3. |
| Ask-by-Default Event Types | `ask-by-default-event-types-before-fix.png` → `ask-by-default-event-types.png` | F1, F12. |
| Add App dialog | `add-app-dialog.png`, `create-pairing-link-sheet.png`, `qr-scanner-sheet.png`, `open-link-file-chooser.png` | Rows end with arrows and carry ellipses; QR scanner shows "Ready to scan" with **Start Camera** suggested. |
| Review Request (approval) | `approval-dialog.png`, `-dark.png`, `-360.png`, `approval-details-expanded.png`, `approval-details-scrolled.png`, `approval-more-options-menu.png`, `approval-permission-options.png` | Deny first / Allow suggested, Return does not activate Allow (security), one Details expander, Block actions behind ⋮. F4, F10. |
| Notification | `shell-notification-sign-request.png` | Title "Grotto (Preview)", body "gdbus / Sign Text note" — mirrors in-app state (badge + Pending Requests). F3. |
| Activity | `activity-panel.png` | Search in the header bar, filters as a boxed list; rows are honest about failures. F3. |
| Remote Signing | `remote-signing-panel.png`, `remote-signing-details-expanded.png` | Switch + status row; methods and reset behind **Details**; **Reset Remote Connections…** destructive alert confirmed in source. |
| Preferences | `preferences-dialog.png`, `-dark.png`, `preferences-dialog-scrolled-before-fix.png` → `preferences-dialog-scrolled.png` | `AdwSwitchRow`/`AdwComboRow`/`AdwSpinRow` throughout; F5, F7. |
| Primary menu, shortcuts, about | `primary-menu.png`, `keyboard-shortcuts-window.png`, `about-dialog.png` | Menu now matches the HIG standard; `GtkShortcutsWindow` lists the app shortcuts; `AdwAboutDialog`. |
| Lock screen | — | Not capturable on this VM (F8). |

## Verification actually run

- `python3 apps/grotto/tests/test-blueprint-drift.py apps/grotto` — pass (pinned Blueprint 0.22.2).
- `msgfmt --check --check-format` on all Grotto and panels catalogs — pass; no fuzzy or obsolete entries.
- Native macOS build of `grotto`, `test-onboarding`, `test-ui-layout` — pass.
- Linux CI container (`nostrc-linux-ci:arm64`, Ubuntu 24.04, GTK 4.14.5): `signer/onboarding`, `identity-management`, `qr-scanner`, `ui-layout`, `blueprint-drift`, `grotto-panels/{no-secrets,control1-service,management-panels}` — pass serially. `signer/ui-accessibility` (Spanish + `GTK_A11Y=test`) — failed once (F13), passes after the lock-screen fix.
- `scripts/test-gate.sh --base master --head <sha> --explain` / run — Linux lanes were projected only (local profile), not executed; the container runs above cover the affected UI tests directly.
- Independent code review of the pass (`/code-review medium`) — 9 findings, all addressed in f58df4b04 / 3f276ec27.

Not covered: Nostr Settings hosting of the shared panels, the Shell extension,
onboarding Check/Backup/Ready pages, Paste App Link (clipboard-dependent), Save
to File, high-contrast style, screen reader.
