---
name: gnome-hig
description: >
  GNOME Human Interface Guidelines (HIG) for GTK 4 + libadwaita apps, with the
  full upstream text converted into references/ so it can be read and cited
  offline. Use this whenever designing, implementing, reviewing or critiquing
  GNOME-style UI: windows, header bars, dialogs, toasts vs banners vs
  notifications, boxed lists and preferences pages, sidebars, view switchers,
  tabs, buttons and menus, keyboard shortcuts and access keys, label wording
  and capitalization, typography, icons, accessibility, light/dark/high-contrast
  and adaptive (phone-width) layouts. Reach for it on any nostrc GTK work
  (gnostr, Grotto, Groundhog, gnome/*, nostr-gtk) that touches .ui/.blp files,
  Adw*/Gtk* widget code, CSS, or user-facing strings — even when the request is
  only "does this look native", "UX review", "which Adw widget should I use",
  "fix the wording", or "is this dialog right" and never says "HIG".
version: "1.0.0"
---

# GNOME HIG

The GNOME Human Interface Guidelines are the design contract for GTK 4 +
libadwaita apps. This skill carries the whole HIG as Markdown under
`references/` (one file per upstream page, same path layout as
https://developer.gnome.org/hig/) so you can read the exact guidance for a
pattern instead of guessing, and cite it by URL in reviews.

Start with `references/INDEX.md` for the page list. Each page begins with its
upstream URL; cite that URL (plus `#anchor`) when you report a finding, the
way existing nostrc reviews in `docs/reviews/` do.

## How to use it

**Designing or implementing UI**

1. Name the pattern you are about to build (a dialog, a preferences page, a
   sidebar...). Use the decision tables below if you are unsure which pattern
   fits.
2. Read that pattern's page in `references/patterns/...` — they are short
   (20–120 lines) and contain the specific rules (button order, label style,
   shortcut keys, size limits).
3. Use the widget map below to pick the libadwaita widget the HIG expects. The
   pages link to the libadwaita/GTK API docs for each widget.
4. Check the cross-cutting guidelines that apply to every UI change:
   `writing-style.md` (capitalization, ellipses), `keyboard.md` (shortcuts,
   access keys, focus order), `accessibility.md`, `adaptive.md`.

**Reviewing or auditing UI**

Work through the checklist in "Review checklist" against the `.ui`/`.blp`
files and widget code, then read the HIG page for anything that looks off
before calling it a violation — several rules have explicit exceptions (for
example, label-only buttons are discouraged in header bars but fine in action
dialogs). Report using the format at the end of this file.

**Writing or fixing copy**

Go straight to the "Capitalization and wording" section; it covers the rules
that are most often wrong in practice. `references/guidelines/writing-style.md`
and `typography.md` have the full text.

## Decision tables

### Telling the user something

| Situation | Pattern | Widget | Page |
|---|---|---|---|
| Transient event tied to what the user just did (saved, deleted, copied); may offer Undo | Toast | `AdwToast` in `AdwToastOverlay` | patterns/feedback/toasts.md |
| Ongoing state of the view (offline, read-only, unlock to edit) | Banner | `AdwBanner` | patterns/feedback/banners.md |
| Event the user should see even while using another app | Notification | `GNotification` | patterns/feedback/notifications.md |
| Message the user must respond to before continuing; destructive confirm with no undo possible | Alert dialog | `AdwAlertDialog` | patterns/feedback/dialogs.md |
| Options to set before an action runs (Save, Print, Export) | Action dialog | `AdwDialog` with header bar | patterns/feedback/dialogs.md |
| Empty view (first run, empty folder, no results) | Placeholder page | `AdwStatusPage` | patterns/feedback/placeholders.md |
| Operation of ~3–30 s, proportion unknown | Spinner | `GtkSpinner` (`AdwSpinner` from libadwaita 1.6) | patterns/feedback/spinners.md |
| Operation over ~30 s, proportion or time remaining known | Progress bar | `GtkProgressBar` | patterns/feedback/progress-bars.md |
| Explain an icon-only control | Tooltip | `tooltip-text` | patterns/feedback/tooltips.md |

Prefer the least interruptive option that still works: undo-toast over
confirmation dialog, toast over error dialog, in-window state over
notification. The HIG treats dialogs as a cost to the user, not a default.

### Choosing a control

| Need | Use | Not | Page |
|---|---|---|---|
| Binary on/off setting | Switch (`AdwSwitchRow` in lists) | Checkbox, unless a switch reads oddly | patterns/controls/switches.md |
| Binary option whose both states need naming | Two radio buttons | Negative-labelled checkbox | patterns/controls/checkboxes.md, radio-buttons.md |
| Pick one of 2 options | Radio or linked toggle buttons | Drop-down | patterns/controls/drop-downs.md |
| Pick one of 3+ options, little space or long list | Drop-down (`AdwComboRow` in lists) | Many radios | patterns/controls/drop-downs.md |
| Value in a fixed ordered range, relative adjustment matters | Slider | Spin button | patterns/controls/sliders.md |
| Exact numeric value matters | Spin button (`AdwSpinRow` in lists) | Slider | patterns/controls/spin-buttons.md |
| Single-line text in a form/preferences | `AdwEntryRow` / `AdwPasswordEntryRow` | Bare `GtkEntry` with a side label | patterns/controls/text-fields.md |
| Mode or setting toggle in a cramped header bar / row | Toggle button | Switch | patterns/controls/buttons.md |
| Controls that should fade when idle (media, image viewer) | Overlaid controls (`.osd`) | Permanent toolbar | patterns/controls/overlaid.md |

### Structuring the app

| Need | Pattern | Widget | Page |
|---|---|---|---|
| 3–5 equal top-level views | View switcher | `AdwViewSwitcher` + `AdwViewSwitcherBar` (moves to bottom when narrow) | patterns/nav/view-switchers.md |
| More views than fit a switcher, or dynamic locations (chats, accounts) | Sidebar | `AdwNavigationSplitView` | patterns/nav/sidebars.md |
| Parent view → detail view, with Back | Browsing | `AdwNavigationView` | patterns/nav/browsing.md |
| Mutable set of documents/pages | Tabs | `AdwTabView` + `AdwTabBar` | patterns/nav/tabs.md |
| Extra tools/info beside the main view, toggled with F9 | Utility pane | `AdwOverlaySplitView` | patterns/containers/utility-panes.md |
| Small set of controls behind a button | Popover | `GtkPopover` | patterns/containers/popovers.md |
| Small static list of settings/info rows | Boxed list | `AdwPreferencesGroup` + `Adw*Row` | patterns/containers/boxed-lists.md |
| Large or dynamic list | List view | `GtkListView` / `GtkColumnView` | patterns/containers/list-column-views.md |
| Collection of images/thumbnails | Grid view | `GtkGridView` | patterns/containers/grid-views.md |
| Act on many items at once (3+ actions) | Selection mode | `GtkActionBar` + `.selection-mode` | patterns/containers/selection-mode.md |
| App-wide settings / about | Secondary window or dialog | `AdwPreferencesDialog`, `AdwAboutDialog` | patterns/containers/windows.md |

Keep hierarchies one level deep where possible and don't invent hybrid
navigation; `guidelines/navigation.md` explains the flat / hierarchical /
combined structures the HIG expects.

## Widget map (HIG pattern → libadwaita)

The HIG pages were written against libadwaita 1.x and still name
`AdwPreferencesWindow` / `AdwAboutWindow`. nostrc targets libadwaita ≥ 1.5,
where the in-window dialog variants are the right choice; the same HIG rules
apply to both.

| HIG pattern | Preferred widget | Notes |
|---|---|---|
| Window | `AdwApplicationWindow` + `AdwToolbarView` | Header bar in the toolbar view's top slot; content below |
| Header bar | `AdwHeaderBar` | Icon-only buttons; label-only buttons via `AdwButtonContent` (icon + label) |
| Primary menu | `GtkMenuButton` with `open-menu-symbolic`, tooltip "Main Menu" | Last group: Preferences, Keyboard Shortcuts, Help, About *App* — never Quit/Close |
| Alert dialog | `AdwAlertDialog` | Cancel first, affirmative last; one `suggested`/`destructive` response per dialog; Esc = cancel |
| Action dialog / sheet | `AdwDialog` | Affirmative button labelled with the verb (Save, Export), insensitive until inputs are valid |
| Preferences | `AdwPreferencesDialog` → `AdwPreferencesPage` → `AdwPreferencesGroup` → rows | Rows should hold at most two controls |
| About | `AdwAboutDialog` | Menu item is "About <App Name>" |
| Boxed list rows | `AdwActionRow`, `AdwSwitchRow`, `AdwComboRow`, `AdwEntryRow`, `AdwSpinRow`, `AdwExpanderRow` (`AdwButtonRow` from 1.6) | Rows that navigate end with `go-next-symbolic` |
| Toasts | `AdwToast` + `AdwToastOverlay` | Short title, optional single button (commonly Undo) |
| Banner | `AdwBanner` | Informal-heading style title |
| Placeholder | `AdwStatusPage` | Heading names the empty state; description tells how to fill it |
| Max content width | `AdwClamp` / `AdwClampScrollable` | Lists and text need min and max widths for adaptive layouts |
| Adaptive layout | `AdwBreakpoint` / `AdwBreakpointBin` | Design from the narrowest size up |
| Avatar | `AdwAvatar` | |
| Light/dark/system style | `AdwStyleManager` | Offer three options if the app has its own preference |

## Capitalization and wording

Two capitalization styles, and which one applies is decided by the element,
not by taste:

| Header capitalization ("Create a Document") | Sentence capitalization ("The document cannot be found") |
|---|---|
| Headings, window/view/tab titles | Checkbox and radio labels |
| Button labels, menu items, tooltips | Slider, entry, field and combo labels |
| Switch labels (nouns: "Automatic Location") | Body/explanatory text in dialogs, notifications |
| View-switcher labels (nouns: "Albums") | Banner and toast titles (informal heading style) |
| Notification titles | Notification body |

Header capitalization capitalizes every word of four or more letters, all
verbs and nouns, the first and last word, and both halves of hyphenated words
("Self-Test"); short articles/prepositions stay lower-case ("Find and Replace").

Other rules that are easy to get wrong:

- Buttons and menu commands use imperative verbs: "Save", "Delete", "Connect".
  Avoid "OK" / "Done" on affirmative dialog buttons — name the action.
- Add "…" (U+2026, not three periods) when the command needs further input
  before acting: "Save As…", "Delete…". Do **not** add it to "Preferences" or
  "Properties".
- Headings drop articles and auxiliary verbs: "Three Documents Updated", not
  "Three documents have been updated". Informal headings (banners, toasts) are
  written as sentences in sentence case instead.
- No trailing periods on labels, headings or single-sentence descriptions.
- Neutral voice; avoid "you"/"my". If ownership must be stated, "Your Records".
- No Latin abbreviations ("e.g.", "i.e.") — they translate badly and screen
  readers mangle them. Do not build sentences across multiple widgets.
- Use proper Unicode: “ ” ’ … × – and a narrow no-break space in "32 GB".
- No all-caps, no italics, no hard-coded font sizes; use the `title-1`…`title-4`,
  `heading`, `body`, `caption` style classes.
- App names: one or two nouns, < 15 characters, header-capitalized, no "G"
  prefix, no trademarks (`guidelines/app-naming.md`).

## Keyboard and input

- Every action must be reachable by keyboard. Test by unplugging the pointer
  mentally: Tab/Shift+Tab order logical, Return activates, Space toggles, Esc
  closes transient containers (menus, popovers, dialogs), F10 opens the menu.
- Use the standard shortcuts in `references/reference/keyboard.md` (Ctrl+W
  close, Ctrl+Q quit, Ctrl+F search, Ctrl+, preferences, Ctrl+? shortcuts,
  F9 side pane, Ctrl+N/O/S, Ctrl+Z / Shift+Ctrl+Z, Alt+Left/Right back/forward,
  Ctrl+T / Ctrl+Page Up/Down for tabs). App shortcuts are Ctrl+letter,
  Shift+Ctrl+letter for the reverse/extended variant. Never Super (system) and
  never Alt (reserved for access keys).
- Give every labelled control an access key (mnemonic, `_Label` in GTK);
  prefer the first letter, avoid thin letters (i, l) and descenders (g, y).
- Primary action = activate/open/select; secondary (right-click / long press)
  only opens a context menu, never a destructive action. No middle-click, no
  double-click, no chording, nothing that only works on hover.
- Three/four-finger gestures and top/bottom edge drags belong to the system.

## Sizes, styling, accessibility

- Smallest desktop size every app must support: 1024×600. Phone-capable apps:
  360×294. Design from that size up; use breakpoints for layout changes, not
  for removing functionality.
- Constrain width with clamps so lines of text and lists never grow
  uncomfortably wide.
- Default to the light style and follow the system setting; a dark default is
  for media-heavy apps. Test light, dark **and** high-contrast (GTK Inspector
  can toggle both — see `skills/gtk-inspector`), large text, screen reader,
  and on-screen keyboard.
- Use libadwaita style classes and CSS variables rather than custom colours so
  high-contrast and dark adapt automatically. Never convey information by
  colour alone; no flashing elements.
- Symbolic icons are 16×16 SVGs, monochrome, 2 px strokes; use them at 16/32/64
  only. Controls show an icon **or** a label, not both (sidebars and view
  switchers are the exception). Only use icons users will recognise.
- Every interactive element needs a short, descriptive accessible name —
  especially custom widgets, `GtkCheckButton`s with custom children, and
  icon-only buttons.

## Review checklist

Use this to audit `.ui`/`.blp` files and widget code. Each line names the
HIG page that backs it so you can quote the rule.

Windows and header bars (`patterns/containers/windows.md`, `header-bars.md`)
- Primary windows resizable, sensible default size, Ctrl+W closes, Esc closes modals.
- Secondary windows/dialogs have a parent, are modal to it, are not stacked.
- Header bar has few controls, blank draggable space, context-dependent contents.
- Header bar buttons are icon-only or `AdwButtonContent`; no `suggested`/`destructive`/linked styles in primary header bars.
- All primary header bar controls have tooltips.

Dialogs (`patterns/feedback/dialogs.md`)
- Shown only in response to a deliberate user action.
- Cancel precedes the affirmative; affirmative is a specific verb.
- Return activates the affirmative unless destructive; Esc cancels.
- Destructive actions prefer Undo toasts; confirmation only if undo is impossible.
- Initial focus set to the control the user will use first.

Buttons, menus, controls (`patterns/controls/*.md`)
- At most one `suggested-action` **or** `destructive-action` button per view.
- Buttons beside each other share a width; invalid buttons are insensitive, not error-producing.
- Menus: 3–12 items, groups separated, no nested submenus, standard last group, no Quit.
- Checkbox/radio labels in sentence case and positively phrased; switch labels are header-case nouns.
- Drop-downs have 3+ items; two options → radios or toggle buttons.

Lists (`patterns/containers/boxed-lists.md`, `list-column-views.md`)
- Boxed lists for small static content, list views for large/dynamic content.
- Rows hold ≤ 2 controls; clicking the row triggers the control; rows that navigate end with an arrow.
- Lists have min and max widths; icons in rows are symbolic.

Feedback (`patterns/feedback/*.md`)
- Right vehicle per the table above (toast / banner / notification / dialog / status page).
- Progress indication appears after ~3 s; spinner for < 30 s, progress bar with text beyond.
- Notifications are rate-limited, removable, and mirrored by in-app state.

Text (`guidelines/writing-style.md`, `typography.md`)
- Capitalization matches the element type; ellipsis/period rules followed.
- No hard-coded fonts; standard style classes; proper Unicode characters.

Keyboard and accessibility (`guidelines/keyboard.md`, `accessibility.md`)
- Standard shortcuts, no Super/Alt accelerators, access keys on labelled controls.
- Full keyboard reachability; accessible names on custom/iconic controls.
- Works in high contrast, dark, large text; colour never the only cue.

Adaptive (`guidelines/adaptive.md`)
- Usable at 1024×600 (and 360×294 if phone-capable); breakpoints keep all functionality.

## nostrc specifics

- GTK apps: `apps/gnostr` (client), `apps/grotto` (signer),
  `gnome/groundhog` (messaging), `gnome/nostr-settings`,
  `gnome/nostr-share`, and the shared `nostr-gtk` widget library. UI lives in
  `data/ui/*.ui` and Blueprint `*.blp` files alongside C sources; app CSS in
  `apps/gnostr/data/ui/styles/gnostr.css` and `gnome/groundhog/data/style.css`.
- These apps already lean on `AdwPreferencesGroup`/`AdwActionRow`,
  `AdwToolbarView`, `AdwAlertDialog`, `AdwToast`, `AdwNavigationView`,
  `AdwStatusPage`, `AdwBanner` and `AdwBreakpoint`; new UI should reuse those
  rather than hand-built `GtkBox` layouts.
- Grotto is a consent/signing surface: the dialog rules (cancel first, no
  default activation of destructive or irreversible responses, honest
  summaries, accessible names on every checkbox) carry security weight there,
  not just polish. Treat a dialog that auto-activates "Allow" on Return as a
  defect.
- To see the UI as the user will, launch under Broadway and screenshot with
  Playwright (`skills/broadway-debug`); toggle high-contrast and dark styles
  from GTK Inspector (`skills/gtk-inspector`) during accessibility checks.
- Previous HIG reviews live in `docs/reviews/*-review-*.md` and show the
  expected evidence discipline. Track work in beads (`bd`), not ad-hoc files.

## Review report format

Keep findings verifiable and attributable, matching existing nostrc reviews:

```
# <Scope> — HIG/UX review

Verdict: one paragraph. Say whether it is a clean pass and name the blockers.

## Findings, ranked

### F1 — S1, confirmed/static: <one-line defect>
Where: <file:line> (repo-relative, at the reviewed commit)
What the HIG says: <quoted rule> — https://developer.gnome.org/hig/<page>.html#<anchor>
Why it matters here: <user/consent/accessibility consequence>
Fix: <concrete change, naming the Adw widget/property or wording>
```

Severity: S1 blocks comprehension, consent or accessibility; S2 is a clear
HIG deviation a user would notice; S3 is polish. Evidence labels:
`confirmed/static` (seen in source), `confirmed/visual` (seen in a screenshot),
`plausible` (not reproduced). Don't call something a violation without
re-reading the page — quote the sentence you are relying on.

## Keeping the references current

`references/` is generated; do not hand-edit it. With a checkout of
https://gitlab.gnome.org/Teams/Websites/developer.gnome.org-hig and pandoc
installed, run:

```bash
skills/gnome-hig/scripts/sync-hig.py --hig-dir ~/Documents/Dev/developer.gnome.org-hig
```

It rewrites every page, resolves cross-references to local links, substitutes
the palette colours, and records the upstream commit in
`references/UPSTREAM.json` and in each file's header. Commit the regenerated
output together with any SKILL.md updates the new upstream text requires.

## Related

- `skills/gtk-inspector/SKILL.md` — inspect widget trees, toggle high contrast / dark, live CSS
- `skills/broadway-debug/SKILL.md` — run the apps headless and screenshot them
- libadwaita docs: https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/
- GTK 4 docs: https://docs.gtk.org/gtk4/
