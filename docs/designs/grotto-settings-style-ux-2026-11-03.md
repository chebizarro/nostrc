# Grotto Settings-style UX: design (G17)

Status: **first draft, awaiting owner sign-off.** Bead: `nostrc-u6v55.23`. Date of owner direction: 2026-11-03.
Design only; no source changes. Scope: the signer's management UI.

Revision note (2026-11-04): continued the existing complete a–g draft in place within the first working minutes, then refined it against checkout `71c0e3fcd`. This document is a proposal, not implementation or acceptance evidence. The smoke-host tests below are specified, **not executed**. The bead stays open for owner sign-off. No builds or Docker captures are part of this task. Existing unrelated working-tree changes are outside this design.

**Phase boundary:** W1–W5 rebuild Grotto's management experience; W6 mounts the same panels in Nostr Settings next release. No new signer policy, key custody, relay transport, consumer profile editor, wallet, or GNOME Control Center plugin is included. Nostr Settings is the in-tree application, not permission to embed a third-party panel into upstream Control Center.

## 0. Decisions at a glance

| Question | Decision |
|---|---|
| Shape of the product | Four panels in a shared library, plus a small **trusted Grotto UI** (review dialog, onboarding, backup) that never moves into another process |
| Replaces Home | **Identity panel**, modelled on GNOME Settings' Users page. Stat tiles, avatar strip and "management selection" are deleted |
| Library | New `gnome/grotto-panels` shared library (MIT, GTK + libadwaita only), **not** `nostr-gtk` (§c.1) |
| Data boundary | Panels receive an injected `GrottoPanelsService`; they carry no signer policy and never see secret material (§c.2) |
| Hosting blocker found | **`Control1` rejects Nostr Settings today** (`grotto_control.c:666-669`). Hosting needs a new narrow *manager* role in the daemon; approval and secret export stay approver-only (§c.4). **Owner decision Q1** |
| Review dialog | Same nine daemon decision modes and corrected vocabulary; plain summary, one permission selector with an always-visible scope explanation, and two commit buttons (§e) |
| Pending requests | Notification (unchanged) plus an inbox group on Connected Apps and a count on its sidebar row. No dashboard |

Reference-model details and corrections to the inherited draft, checked against source:

- **`panels/user-accounts` is not the GNOME 46 path.** On the `gnome-46` branch the Users panel is `panels/system/users` ([cc-users-page.ui], [cc-user-page.ui]).
- GNOME 46's actual **Add User…** is a centred suggested pill ([cc-users-page.ui]); the owner specifically requested a flat action, which this design follows as **Add Identity…**. Use `GtkButton` with `.flat`, not the above-floor `AdwButtonRow`. Copy the Users panel's hierarchy, not every visual detail.
- **Nostr Settings already is Settings-style.** Its six `AdwPreferencesPage` pages sit in an `AdwPreferencesWindow` (`gnome/nostr-settings/src/ui/nss-window.c:11-18,138-151`). It has no sidebar yet. Hosting the proposed navigation-page panels needs a shell adapter, not a rewrite of its existing Relays, Notifications, Wallet, Media or Files behavior (§c.5).
- **`signer/home-model` runs `test-home-dashboard`**, not `test-home-model` (`tests/CMakeLists.txt:590`).

## a. HIG audit of current surfaces

Severity: **S1** major task/consent-comprehension problem; **S2** material usability, consistency or accessibility problem; **S3** polish. These are design severities, not demonstrated security exploits. Priority 1–3 follows the owner's ordering independently of severity. Paths are under `apps/grotto/`; `.blp` lines refer to `data/ui/`, `.c` lines to `src/ui/` unless fully qualified.

The audit distinguishes directly observable HIG conflicts (terminology, action wording, navigation, dialog structure) from design judgments (density, unnecessary controls). A custom widget or a mobile pattern is not automatically a HIG violation. Normative reference points are [boxed lists][boxed-lists], [dialogs][dialogs], [preferences/secondary windows][windows] and [writing style][writing-style].

### a.1 `approval-dialog.blp` (owner priority 1)

| Sev | Where | Violation | Fix (§e) |
|---|---|---|---|
| S1 | `:15-92`, `approval_dialog.c:470-479,540-555` | Loose stacked text mixes caller, operation and scope with similar visual weight. Density is a design judgment; the data itself is present | Grouped information rows with clear titles |
| S2 | `:26`, `approval_dialog.c:324-330` | **Already has “Signing as:”** at runtime; not a missing-label bug. A full npub in one monospace label is hard to scan alongside the caller | “Signing As” row: local label plus abbreviated key, full key selectable in Details |
| S1 | `:58` | Scope, the fact that matters most, is `dim-label` text | Always-visible full scope sentence |
| S1 | `:67-68`, `approval_dialog.c:172-179` | Checkbox plus conditional checkbox hides the relationship between once, temporary and persistent consent; the commit label only says “Allow this app” | One permission selector and explicit scope text, each option 1:1 with a daemon mode |
| S1 | `:78-90` | "Trust this app for routine operations" and both Block actions sit inside a Details expander, below the commit buttons | Trust moves to the permission selector; Block moves to a header menu |
| S1 | `approval_dialog.c:306` | **`Ctrl+A` approves.** With initial focus on Deny, the standard Select All shortcut signs the request | Remove; use `Alt` mnemonics |
| S2 | `:41-47` | Stepper is a homogeneous box of text buttons in the body | Arrow buttons in a compact row |
| S2 | `:48-57` | Bare `GtkExpander` holding a commit button, buried mid-body | `AdwExpanderRow` |
| S2 | `:59-66` | `NIP-44` jargon; long technical checkbox labels | Plain copy; separate ephemeral-flow variant |
| S2 | `:71-77` | Three homogeneous buttons in body flow scroll out of view; cancel-type action placed last. HIG: cancel before affirmative | Pinned bottom bar; Not Now in the header |
| S2 | `approval_dialog.c:200-203` | "Block app" alert has no destructive appearance | `ADW_RESPONSE_DESTRUCTIVE` |
| S2 | `approval_dialog.c:176,178,200-201` | Untranslated strings | `_()` |
| S3 | `:11,22` | Hides the title, then redraws it as a body label | Real heading |
| S3 | `:29` | `GtkFrame` around preview | `.card` |

### a.2 `page-home.blp` / `page-home.c` (owner priority 2)

| Sev | Where | Violation | Fix |
|---|---|---|---|
| S1 | `:39-53` | Statistics displace identity and app-management tasks, especially at 360 px; design mismatch with the requested Settings model, not a ban on statistics in the HIG | Delete |
| S1 | `:22`, `page-home.c:26-31` | One click both changes a "management selection" and opens a dialog; a subtitle then has to explain the invented concept | Delete the concept; identity rows navigate |
| S2 | `:13-17` | Horizontal avatar strip conceals identities and uses a second selection model alongside page navigation | “Other Identities” boxed list |
| S2 | `page-home.c:64` | `suggested-action` used to mean "selected" | Removed with the strip |
| S2 | `page-home.c:71` | Literal `"+ Add"` label | "Add Identity…" |
| S2 | `page-home.c:131-142` | Two stacked custom `.grotto-badge` pills plus arrow plus three-line subtitle in one row. HIG: one control, clean layout | One-line subtitle and arrow |
| S2 | `page-home.c:120` | Row title falls back to the raw principal | Daemon-provided display name |
| S2 | `:54` | Errors as an unstyled label | `AdwBanner` |
| S2 | `:61` | "Tap Connect": touch wording on a desktop | New empty state |
| S2 | `:59` | `AdwStatusPage` nested in a preferences group | Page-level empty state |
| S3 | `page-home.c:123-125` | Presentation classifies browser origins by string prefix. Not a HIG violation or authorization decision; move display classification behind the adapter | Service display field (§c.3) |
| S3 | `:30` | `view-grid-symbolic` used as a QR icon | Dedicated QR symbolic icon |
| S3 | `page-home.c:126` | "· 0 pending" shown on every row | Show only when non-zero |

### a.3 `page-identity-detail.blp` / `.c` (owner priority 3)

| Sev | Where | Violation | Fix |
|---|---|---|---|
| S1 | `:14-51` | About fifteen loose widgets, eleven of them stacked buttons, mixing renaming, secret export, rotation and deletion on one scroll | Identity page plus separate trusted Backup page |
| S1 | `:27` | "Show" seed words is the page's only `suggested-action`; revealing secrets is promoted as the primary action | Deliberate flow, no suggested style |
| S1 | `:26` | `activatable-widget: btn_seed`: a click anywhere on the row reveals the seed | Row navigates; reveal happens in a dialog |
| S1 | `:30-32` | Seed words render inline among unrelated settings | Modal dialog, cleared on close |
| S2 | `page-identity-detail.c:259` | Every confirmation's button says "Continue". HIG: a specific imperative verb | "Show Secret Key", "Remove", "Create Identity" |
| S2 | `page-identity-detail.c:347-353` | Hierarchical page presented as a 560×640 dialog with its own navigation stack | Pushed navigation page |
| S2 | `:15` | Dropdown switches identity inside a page about one identity | Remove |
| S2 | `:18-19` | Bare `Entry` plus separate Save button | `AdwEntryRow` with apply button |
| S2 | `:22` | Five-sentence group description | One line on Backup page |
| S2 | `:33-37` | Passphrase field is shared by two distant buttons, with only a bare label | Dialog per action |
| S2 | `:37,50` | Destructive buttons interleaved with neutral ones | Single pill at page end; secret copy behind a confirmation |
| S2 | `:47-49` | Developer language: "Retry Identity Self-Test", "Export Saved Migration Announcement" | Plain copy under Advanced |
| S2 | `page-identity-detail.c:236-281,334` | Status and alert strings are not translatable | `_()` |
| S2 | `page-identity-detail.c:264` | Must retype a 63-character npub with no copy affordance | Keep the daemon's typed confirmation; add Copy |
| S3 | `:16,51` | Status conveyed by loose labels | Banner and toast |

### a.4 `page-client-detail.blp` / `.c`

| Sev | Where | Violation | Fix |
|---|---|---|---|
| S1 | `:19` | "Explicit denials win. Saved permissions override presets; ask-by-default kinds need a scoped first grant." Policy-lawyer copy | Plain description |
| S2 | `:5-9` | `NavigationView` inside a navigation page, with title buttons removed under the shared window header | Single panel navigation |
| S2 | `:13` | Slash title, "Verified identity / fingerprint" | "Verified As", "Fingerprint" rows |
| S2 | `:22` | Accessible name says "Full" while the visible title says "Routine access" | Same words |
| S2 | `:28` | Title and subtitle are identical | Distinct subtitle |
| S2 | `:35,42` | "Client" and "Remembered" are pre-G15 vocabulary; buttons loose in groups | "App"; actions at page end |
| S3 | `:15` | Bare transient status label in a group | Toast for success, banner for recoverable error |
| S2 | `page-client-detail.c:310-369` | C-built permission editor mixes scope text, bare dropdowns, spin control, catalogue and expiry without row labels/group hierarchy | Preferences rows: Identity, Operation, Event Type, Decision, Duration; fixed Save/Cancel actions |
| S2 | `page-client-detail.c:369-371` | Save and Remove are adjacent neutral buttons with different consequences | Save in action bar; Remove Permission… separate, warning that removal of a deny can permit access |

### a.5 Remaining surfaces

| Sev | Surface and line | Violation |
|---|---|---|
| S2 | `signer-window.blp:15` | One header bar spans sidebar and content; each split-view pane should own its header |
| S2 | `signer-window.blp:7-8,76` | Two navigation systems: sidebar, plus a phone-style `ViewSwitcherBar` under 500 px |
| S2 | `signer-window.blp:16-19` | Sidebar toggle belongs to overlay split views; `AdwNavigationSplitView` collapses into a navigation stack |
| S3 | `signer-window.blp:34-54` | Hand-set row margins on a `navigation-sidebar`; `:21` menu button has no tooltip |
| S2 | `page-settings.blp:7-9` | Three overlapping appearance and high-contrast controls duplicate system settings |
| S2 | `page-settings.blp:10` | In-app language picker with "restart required" |
| S2 | `page-settings.blp:21` | Pause state is a one-shot button, not a state-reflecting switch |
| S2 | `page-settings.blp:25-29` | Remote signing appears here, on `page-connect.blp:30` and in `sheet-create-bunker.blp` |
| S2 | `page-settings.blp:37,40` | Navigation and About as loose buttons in preferences |
| S2 | `page-connect.blp:31-34` | Long jargon paragraph as a bare label |
| S3 | `page-connect.blp:9-26` | Rows that open dialogs use arrows. Otherwise the best-conforming page |
| S2 | `page-history.blp:9` | Filter by raw "Event type number" |
| S2 | `page-history.blp:10` | Untranslated raw enum values such as `expired-caller` |
| S2 | `page-history.blp:13-16` | Growing history uses a boxed `ListBox` with Load Older; **not unbounded by proof here**, but a dynamic log should use a paged list view |
| S2 | `onboarding-assistant.blp:3,109-113` | Generic Next/Back assistant adds navigation chrome and vague action wording; `Adw.Window` itself is valid, not a violation |
| S2 | `onboarding-assistant.blp:27,45,54,75` | Jargon: BIP-39, NIP-49, "unpublished event", "ephemeral peer" |
| S2 | `onboarding-assistant.blp:34-37` | Radio buttons plus Next instead of a one-click choice list |
| S2 | `onboarding-assistant.blp:56` | Makes the user classify their own input when it can be detected |
| S2 | `onboarding-assistant.blp:64-69` | The Connect step is a dead end that says to go Back |
| S2 | `onboarding-assistant.blp:87` | Warning in parentheses inside a button label |
| S3 | `onboarding-assistant.blp:4,11` | Two different titles for one window |
| S2 | `sheets/sheet-create-bunker.blp:5-25` | Titled as an identity question, but holds relay config, an enable toggle, an apply button, pairing and reset: no header bar, loose controls, no primary action |
| S2 | `sheets/sheet-qr-scanner.blp:10-12,103-105,211-219`, `src/ui/sheets/sheet-qr-scanner.c:319-327` | Hint/accessibility remain import-specific although C relabels the pairing result button to “Choose Identity…”. Update the whole task context, not just that button |
| S3 | `sheets/sheet-qr-display.blp:21-31,47` | "Done" as `suggested-action`; title drawn twice; `:10-13` untranslated accessibility strings |
| S3 | `lock-screen.blp:28-115` | Hand-built status page; decorative icon has accessible text (`:43-46`) |
| S3 | `shortcuts-window.blp:9-16` | No structural HIG conflict found; keep dialog, update navigation labels/accelerators, preserve explanation that Quit does not stop authorized signing |
| S2 | `data/css/app.css:5-11,203-216,244-251` | Hard-coded colours; the owner's reference model uses none |
| S2 | `data/css/app.css:220,233,239` | Redefines libadwaita's own `.card`, `.heading`, `.caption` |
| S2 | `data/css/high-contrast.css:1-40`, `page-settings.blp:7-9` | Parallel high-contrast variants increase maintenance and can fight system styling; remove overrides with their switches, not accessibility support |
| S3 | `widgets/gn-secure-entry.blp:18-41` (support widget, beyond the 14 top-level surfaces) | No required redesign. Keep reveal/caps-lock feedback and secure-entry behavior; label per import/export task rather than generic “Password” |

### a.6 Committed visual evidence (no new captures)

`apps/grotto/data/screenshots/README.md:3-12,29-31` dates these fixtures to 2026-10-14: Ubuntu 24.04 arm64 **container/Xvfb**, GTK 4.14.5, libadwaita 1.5.0. They are not the owner's later GNOME 46 live session. All 12 PNGs are **stale for current wording**, predating G15; layout observations below are also checked against current Blueprints. No committed approval or identity/backup image exists, so those audits are source-only, not visual acceptance.

| Files (under `apps/grotto/data/screenshots/`) | Observation / use |
|---|---|
| `home-light.png`, `home-dark.png` | Wide dashboard: avatar strip, large statistics and badges compete with identity/app tasks. Light inspected directly; dark is its companion, not separately inspected |
| `home-360-light.png`, `home-360-dark.png` | Narrow layout: bottom navigation competes with detail navigation; badges crowd the app row. Light inspected directly |
| `client-light.png`, `client-dark.png` | Light inspected: old Full/Medium/Low and required-confirmation language is obsolete, **not target copy** |
| `permissions-light.png`, `permissions-dark.png` | Light inspected: permission semantics become a long list of explanatory rows; replace repeated policy prose with daemon-reported status |
| `connect-light.png`, `connect-dark.png` | Light inspected: keep the strongest existing pattern, the short action list; shorten its footer |
| `settings-light.png`, `settings-dark.png` | Dark inspected: appearance overrides dominate before management controls; defer to the desktop. Light companion not separately inspected |

Regenerating captures belongs to W4 implementation only. Fixture values such as “117” are not real signing history.

## b. Target panel architecture

### b.1 Panels

| Panel | Contents | Replaces |
|---|---|---|
| **Identity** | Primary identity page, Other Identities, Add Identity… | Home, identity detail (everyday part) |
| **Connected Apps** | Pending Requests (only when non-empty), Pause Signing, app list, Add App… | Home client list, client detail, Connect |
| **Activity** | Search, outcome filter, event log, Clear… | Activity, retention setting |
| **Remote Signing** | Enable, status, explicit relays, method allowlist, Reset… | Preferences group, `sheet-create-bunker` config |

Remote Signing is the **signer endpoint**, not a replacement for Nostr Settings' social/public Relays page. Existing transport consent, explicit relay configuration and before-network pairing review remain unchanged; this design adds no relay defaults or transport behavior.

**Preferences is not a panel.** After removing what duplicates system settings, what remains is Grotto-host configuration: locking, pause on screen lock, background operation and launch at login. These go in a standard `AdwPreferencesDialog` from the primary menu (`Ctrl+,`). HIG treats preferences as a secondary window. About moves to the primary menu only. Move **Ask-by-Default Event Types** to Connected Apps → Advanced, where its explanation is next to permissions: “Ask before a first grant; saved permissions still apply”. It remains daemon-owned configuration, not a second evaluator in the library. Activity retention goes to Activity. This leaves Preferences genuinely host-specific; pause/resume stays in Connected Apps and the existing desktop indicator.

Every panel is an `AdwNavigationPage` subclass, as in `CcUsersPage` ([cc-users-page.ui]), holding `Adw.ToolbarView` + `Adw.PreferencesPage`. Sub-pages push onto the panel's own `AdwNavigationView`.

**Visual contract:** one avatar header, a few semantically grouped boxed lists, standard spacing/type roles, and theme-provided colors. No custom trust badges, green/red approval surfaces, decorative statistic cards or profile downloads. Use only GTK 4.14 / libadwaita 1.5 APIs: `AdwNavigationSplitView`, `AdwNavigationView`, `AdwPreferencesPage/Group`, `AdwActionRow`, `AdwSwitchRow`, `AdwComboRow`, `AdwEntryRow`, `AdwDialog`, `AdwAlertDialog`, `AdwPreferencesDialog`, `GtkSpinner`. **Do not use `AdwBottomSheet`, `AdwSpinner`, `AdwToggleGroup`, `AdwButtonRow` or newer property-row APIs.** Informational property rows below mean ordinary `AdwActionRow` widgets.

**Lock/unlock:** borrow the Users panel's honest separation of viewability and editability, not its system-administrator authorization. Grotto's current management lock remains a host-local privacy barrier; its exact authentication implementation is not replaced here. A lock never implies that signing is paused or the keyring is locked. Pending reviews remain subject to the existing locked-session rules. Nostr Settings must not present a fake “Unlock” button that grants daemon authority: role-denied actions say “Open in Grotto”. Pause/resume remain distinct daemon operations.

### b.2 Window (Grotto host)

```blueprint
template $SignerWindow: Adw.ApplicationWindow {
  default-width: 920; default-height: 640;
  width-request: 360; height-request: 480;
  Adw.Breakpoint {
    condition ("max-width: 600sp")
    setters { split_view.collapsed: true; }
  }
  Adw.ToastOverlay toast_overlay {
    Stack main_stack {
      StackPage { name: "locked"; child: $GnLockScreen lock_screen {}; }
      StackPage {
        name: "unlocked";
        child: Adw.NavigationSplitView split_view {
          min-sidebar-width: 200; max-sidebar-width: 280;
          sidebar: Adw.NavigationPage {
            title: _("Grotto");
            child: Adw.ToolbarView {
              [top] Adw.HeaderBar {
                [end] MenuButton menu_btn {
                  icon-name: "open-menu-symbolic";
                  primary: true;
                  tooltip-text: _("Main Menu");
                }
              }
              content: ScrolledWindow {
                hscrollbar-policy: never;
                ListBox panel_list {
                  selection-mode: browse;
                  styles ["navigation-sidebar"]
                }
              };
            };
          };
          content: $GrottoIdentityPanel current_panel {}; // replace with selected panel instance
        };
      }
    }
  }
}
```

The selected `GrottoPanel` is the split view's content page, not a page nested in another header-bearing wrapper. Each pane owns its header, removing `sidebar_toggle_btn` and `bottom_bar`. The sidebar collapses at one breakpoint with no second navigation system. The sidebar list is built from `grotto_panels_get_descriptors()`, with `avatar-default-symbolic`, a linked-apps icon, `document-open-recent-symbolic` and `network-server-symbolic`. Connected Apps gets a `.numeric` count suffix when requests are pending.

### b.3 Pending requests

1. **Notification** stays exactly as plan §5.5 defines it: Review and Deny on a single request, Review only on the summary, never Approve.
2. **Inbox group** at the top of Connected Apps, visible only when non-empty. Each row is `‹app› · ‹operation›`, with time remaining. Activating it opens the review dialog.
3. **Sidebar count** on the Connected Apps row.
4. **Launch rule:** if requests are pending when the window opens, the initial panel is Connected Apps; otherwise Identity.

There are no tiles and no “Signed today”. The Shell indicator (plan §5.12) is unchanged. A request arriving while the owner is managing an identity **does not change pages or pop a dialog**: notification plus inbox/count only. Notification absence is a visible banner; the inbox remains usable. “Not Now” leaves its row present without re-notifying. Completed, expired or old-daemon-owner IDs never open a replacement request; show the stale-action message instead. Count and rows derive from the same pending model, not separately polled statistics.

### b.4 What each host owns

| Surface | Grotto now | Nostr Settings next release |
|---|---|---|
| Identity panel | Hosted | Hosted; replaces signer-management part of `nss-page-identity.c`, retains its signer-relay summary/link |
| Connected Apps panel | Hosted | Hosted; security mutations hand off to Grotto (§c.4) |
| Activity panel | Hosted | Hosted |
| Remote Signing panel | Hosted | Same panel, status plus “Open in Grotto” for configuration |
| Review dialog | **Grotto only** | Activates Grotto |
| Onboarding, Add Identity, import | **Grotto only** | Activates Grotto |
| Backup, seed words, secret export | **Grotto only** | Activates Grotto |
| Preferences dialog | Grotto only | Grotto only |
| Daemon, notifications, Shell indicator | Unchanged | Unchanged |

Next-release end state: when the panel-capable version of Nostr Settings is installed, explicit “Manage Identities” opens it on Identity; an ordinary management launch may route there only after version/capability detection. Approval/onboarding/backup launches always remain Grotto-owned. Do not make the mere existence of the old desktop file sufficient or create a launch loop. The Grotto window remains the fallback for Flatpak and non-GNOME installs, so the panels never become dead code.

## c. Shared library and data boundary

### c.1 Placement: new library, not `nostr-gtk`

| Consideration | `nostr-gtk` | New `gnome/grotto-panels` |
|---|---|---|
| Licence/source boundary | GPL-3.0-or-later (`nostr-gtk/CMakeLists.txt:6`) | New MIT implementation, matching Nostr Settings's `nss-*.c` SPDX; legacy Grotto widgets keep their own licences |
| Current consumers | Neither Grotto nor Nostr Settings links it | Both, by design |
| Floor | `gtk4>=4.6`, `libadwaita-1>=1.2` (`nostr-gtk/CMakeLists.txt:43-44`) | GTK 4.14 / libadwaita 1.5, pinned as `gnome/nostr-settings/CMakeLists.txt:33-39` does |
| Dependencies | `nostr-gobject` and a larger GTK helper/build closure | GTK, libadwaita and their GLib/GIO base only; no signer core, relay, web, wallet or key-store dependency |
| Stability | Existing public 2.x API | New versioned 0.x panel API shared by the two in-tree hosts |

Adopts the previous attempt's separate-library conclusion: reusable management panels have a narrower responsibility than the general Nostr widget toolkit. `gnome/grotto-panels/` builds `libgrotto-panels-0.so.0` with a deliberately small exported/annotated API, `grotto-panels-0.pc`, a private GResource prefix `/org/nostr/GrottoPanels`, and a `VERSION_MANIFEST.md` row starting at 0.1.0. Both CMake and standalone Meson consumers must resolve the same library; add its build target before either host and its runtime package dependency to both hosts. No shared-library reference back into either executable is allowed.

New code follows the intended MIT component licence; **do not relicense copied GPL widgets or the bridge implementation**. The bridge is an architectural reference. `GnSecureEntry` stays in trusted Grotto, including its existing licence. Regenerate the display-only kind catalogue from its owning registry rather than duplicate policy constants. Templates use Grotto's committed `.blp` + `.ui` and drift-check convention (`apps/grotto/tests/test-blueprint-drift.py`).

### c.2 Service injection

`gnostr-signer-bridge.h` contributes three principles: the library never links the app, a missing operation reports "not initialized" rather than failing the build, and the host installs the implementation. It is a **process-global vtable with no change notification**, which suits a helper but not a live UI. So we keep the principles and change the mechanism: a `GInterface` passed per panel.

```c
G_DECLARE_INTERFACE(GrottoPanelsService, grotto_panels_service, GROTTO_PANELS, SERVICE, GObject)

struct _GrottoPanelsServiceInterface {
  GTypeInterface parent;
  /* Live models; a host updates them from daemon signals. */
  GListModel *(*get_identities)(GrottoPanelsService *self);
  GListModel *(*get_apps)(GrottoPanelsService *self);
  GListModel *(*get_requests)(GrottoPanelsService *self);
  GListModel *(*get_activity)(GrottoPanelsService *self);
  /* Daemon-reported ability for this host, never inferred by a panel. */
  gboolean    (*can)(GrottoPanelsService *self, const char *operation);
  /* Finite management commands only; not arbitrary D-Bus method dispatch. */
  void        (*invoke_async)(GrottoPanelsService *self, GrottoPanelCommand command, GVariant *args,
                              GCancellable *cancellable, GAsyncReadyCallback cb, gpointer user_data);
  GVariant   *(*invoke_finish)(GrottoPanelsService *self, GAsyncResult *res, GError **error);
};

/* What the host does for a panel. A NULL member hides the corresponding row. */
typedef struct {
  void (*show_toast)(gpointer host, const char *message);
  void (*review_request)(gpointer host, const char *request_id);
  void (*add_identity)(gpointer host);
  void (*add_app)(gpointer host);
  void (*open_backup)(gpointer host, const char *npub);
  void (*remove_identity)(gpointer host, const char *npub);
  void (*replace_identity)(gpointer host, const char *npub);
  void (*open_in_signer)(gpointer host, const char *target);
} GrottoPanelsHost;
```

Proposed implementations: `GrottoPanelsControl1Service` (common thin GIO adapter, used by both hosts) and `GrottoPanelsFakeService` (test-only). The widgets only see the injected interface. In Phase 1, a Grotto-owned composite adapter joins Control1 data with non-secret `AccountsStore` metadata; the library never imports that store. The interface is **per-instance**, not a new global singleton.

`GrottoPanelCommand` is a closed enum for panel management (label, default identity, trust, replace permissions, revoke, pause, activity and remote configuration), each mapped to a fixed typed tuple. It excludes signing, `DecideRequest`, import, secret export and arbitrary method names. Unknown commands fail `NotSupported`; missing service displays “Signer unavailable”, never permissive local fallback. Host-only callbacks cover review, onboarding, backup, removal and replacement-identity actions. In Grotto `open_backup` pushes its trusted page; in Nostr Settings it activates `org.nostr.Grotto` with a public target and canonical npub, never credentials.

The service emits **`capabilities-changed`** and **`availability-changed`** on the GTK main context, as well as list-model item/property notifications. Panels evaluate `can()` on attachment and those signals, including daemon loss/restart and pause changes. Missing authorization does not just hide a control after a failed click.

Models and callbacks have explicit lifetimes: panels retain the service, callback user data has a destroy notifier, detach disconnects signals/cancels view work, and asynchronous results carry owner generation plus snapshot revision. All widget updates occur on the GTK main context. `can()` reports supported-and-authorized commands; until per-role capabilities exist, the Settings adapter must default to read-only and hand-off. Capability flags are hints for UI, **not authorization**; every daemon call checks the caller again.

### c.3 Boundary rules (each is tested)

1. **No policy.** Panels never decide what a permission means. They render daemon fields: `state`, `reason`, `needs-review`, `access-policy`, `blocked` (plan §5.4 "GTK does not infer effectiveness"). `page-home.c:123-125`'s display-only classification moves behind the adapter, then to a daemon field in W6; it never grants authority.
2. **No secrets.** The library contains no call to `ExportIdentity`, `ExportRawIdentity`, `ImportIdentity`, `StoreKeyWithMnemonic` or `DecideRequest`, and does not link `gn-secure-entry`. A static test asserts this over library sources and exported symbols.
3. **No Grotto symbols.** The library has no dependency on `accounts_store`, `session-manager` or the approval controller. A link test proves this.
4. **Untrusted names are text.** Any app-supplied string renders with `use-markup: false`, as `page-home.c:119` already does.
5. **No polling.** Models update from `RequestsChanged`, `ClientsChanged`, `StatusChanged` and `ActivityChanged`, subscribing before listing (plan §5.5). Preserve owner-generation/tombstone handling from the approval controller and home model; discard late snapshots, retain request deadlines, never renew TTL on remount. Activity record sequence is not a snapshot revision. Lazy page mount/remount does not destroy the application-owned pending controller.

Data gaps the service exposes (W6 unless noted):

| Need | Today | Move to |
|---|---|---|
| Identity list/default | `org.nostr.Signer.ListIdentities` plus Grotto's existing identity/metadata adapter | Grotto adapter in W3; additive approver/manager-readable Control1 identity snapshot in W6. Nostr Settings must not enumerate private keyring items to populate this panel |
| Label, setup and backup acknowledgement | GUI-private `~/.config/grotto/accounts.ini` (`apps/grotto/src/accounts_store.c:73-80`) | Daemon-owned non-secret Control1 fields in W6; never the phrase/key. Sharing a file is not the cross-host boundary. Import metadata once with an explicit schema/provenance marker before switching writers; preserve labels and acknowledgement, do not claim that acknowledgement proves a recoverable backup |
| Transport display | Inferred in GTK | Daemon `ListClients` field |
| Kind names | `src/ui/kind-catalog.inc` | Library display catalogue generated from the same registry: names, not permission evaluation |

### c.4 Blocker: the approver gate, and the manager role

**W6 implementation status:** the former gate below is replaced by fresh,
PID/path/device/inode manager authentication for the installed Nostr Settings
host. Managers can read status, public signing identities, clients, permissions,
requests and activity, and change cosmetic client labels only. Both pause values,
tiers, permissions, signing, approval and custody remain refused. Caller-specific
role/capability reporting drives the host; the existing Grotto lock is unchanged.
Settings now mounts all four real shared panels in a sidebar shell, preserves
service pages, page selection/window state/screenshots and the relay-summary link,
and hands public targets to Grotto's navigation-only `open-management` action.
Backup navigation selects the identity; the user must choose Backup in Grotto.

The additive identity snapshot reports canonical stored signing npubs/default
and explicit unknown setup/backup metadata. It does not migrate GUI-private
labels, watch-only inventory or recovery acknowledgement to daemon ownership.
The current W6 direction scopes delivery to public metadata and manager hosting;
that larger metadata-writer migration remains separate, not fabricated by a
shared-file Settings reader. Unknown status is displayed honestly in the panel.

Historically, every `Control1` method except `GetStatus` and `SetPaused(true)` required `signer_caller_is_approver()` (`grotto_control.c:666-669`). This matches an exact installed path and inode against `NIP55L_APPROVAL_UI_PATHS`, which is `${BINDIR}/grotto` (`signer_caller.c:364-376`, `nips/nip55l/CMakeLists.txt:219`). A panel inside `nostr-settings` would therefore get `NotAuthorized` on every read. A fake-service smoke test alone would hide that, so W6 has a real-daemon test.

| Option | Verdict | Reason |
|---|---|---|
| A. Add `nostr-settings` to the approver list | Rejected | Grants `DecideRequest` and `ExportRawIdentity` to a process that parses relay and wallet responses from the network |
| B. Proxy through a Grotto D-Bus service | Rejected | Confused deputy: any same-UID process could use the proxy |
| C. Separate windows, no hosting | Rejected | Contradicts the owner's phasing; GTK 4 has no cross-process widget embedding |
| **D. New manager role plus hand-off** | **Recommended** | In-tree precedent: Nostr Settings already "administers grants, not money" with `nostr-wallet-agent`, by the same path-and-inode proof (`gnome/nostr-settings/README.md:45-57`) |

Implemented `signer_caller_is_manager()`: PID-attested Linux caller, exact installed executable path plus current device/inode of `${BINDIR}/nostr-settings`, same UID; deny an arbitrary same-UID executable, name owner, claimed app ID, replaced binary and unsupported sandbox shape. No new UID-only manager fallback. Grotto's existing approver compatibility exception is not made into a cross-platform security promise.

| Method | Approver (Grotto) | Manager (Nostr Settings) |
|---|---|---|
| `GetStatus` | Yes | Yes (coarse same-UID status remains) |
| `SetPaused(true)` | Yes | **No** (the manager does not inherit the ordinary session pause exception) |
| `ListClients`, `ListPermissions`, `ListActivity`, `ListRequests` | Yes | **Yes**, read-only |
| `SetClientLabel` | Yes | **Yes**, cosmetic only; preserve verified principal and policy |
| `SetClientTrust`, `SetPermissions`, `RevokeClient`, `ClearActivity`, `SetDefaultIdentityForLegacy`, `SetPaused(false)` | Yes | **No** in this phase: `open_in_signer` |
| `ListIdentities` public snapshot | Yes | **Yes**, signing npubs/default; unavailable local metadata stays explicitly unknown |
| `DecideRequest`, `Export*`, `ImportIdentity`, `StoreKeyWithMnemonic`, `RemoveIdentity`, `RotateIdentity`, `SelfTestIdentity`, bunker and pairing methods | Yes | **No** |

**Why not a “lowering only” generic permission editor?** Removing a deny can increase effective authority; lowering a tier retains explicit grants. Comparing arbitrary replacement sets safely would add policy complexity to a UI project. Keep the panel, but hand off all security mutations to Grotto at the selected app/identity. The user sees “Change in Grotto…” instead of a switch that fails after activation. The activated target only navigates: Grotto refetches daemon state and asks for the actual change. A URI/desktop activation is never approval. The copied callback must not replay a decision supplied by another process.

If the owner later requires every Settings mutation inline, authorize a separate daemon-mediated management-review design. That is not silently included in this draft.

Add caller-specific management capability reporting in W6 without turning same-UID `GetStatus` into a sensitive read: capability/role information is not itself authority. Preserve version-skew read-only behavior. Existing limitation: on UID-only buses `signer_caller_is_approver` returns TRUE for same-UID unattested callers (`nips/nip55l/src/glib/signer_caller.c:364-375`); the manager separation described here is Linux PID-attested behavior only, and Nostr Settings is Linux-only.

### c.5 Nostr Settings shell change

`nss-window.c:138-151` uses `AdwPreferencesWindow`, which is deprecated in 1.6 and accepts only preferences pages. Six pages plus three or four panels exceeds a view switcher; the HIG says to use a sidebar "when you need to expose more views than a standard view switcher can hold" ([sidebars]). W6 replaces it with the same split view as §b.2, wraps each existing `AdwPreferencesPage` in a navigation page, moves `nss_toast` (`nss-widgets.c:57`) to an `AdwToastOverlay`, and keeps `--page`, `last-page` and the screenshot mode. Retain page names `identity`, `relays`, `notifications`, `wallet`, `media`, `files`; add `apps`, `activity`, `remote-signing`. `--page identity` still works. Keep Nostr Settings' existing non-signer pages as wrappers; keep the Identity page's signer-relay summary and route its public-relay editing to Relays, not Remote Signing. Remove the duplicate direct-keyring inventory and old “Signer settings” launch row only after the shared Identity panel is populated. Shell-wide preferences search is not automatic in the split view; preserve search within app/activity/permission catalogues. Do not rebuild a universal settings search as a delivery prerequisite.

### c.6 Smoke-host test

Two levels. Only the second counts as proof of production hosting.

1. **`grotto-panels/smoke-host` (W2).** A test executable that links only GTK, libadwaita and `libgrotto-panels`, and builds a Settings-style split view. Using the fake service it mounts every descriptor; navigates to each; pushes and pops a sub-page; emits a model change and asserts the row updates; removes an operation from `can()` and asserts the row hides or offers hand-off; exercises each `GrottoPanelsHost` callback; rebuilds at 360 px; unmounts and asserts no leak under the existing sanitizer route. `nm -D` asserts no `accounts_store_*` symbol.
2. **`nostr-settings/panel-mount` (W6).** The real `nostr-settings` window with the real daemon on a private bus (`nostrc_test_bus_up`). It mounts **the real shared Identity and Connected Apps panels** through `nss_window_new`, checks public identity/app rows populate, changes a label through the manager API, receives a daemon signal, and verifies security edits/backup/review activate Grotto at the selected target. Grotto must only navigate, not approve or mutate on activation. Direct `SetPermissions`, `SetClientTrust`, `ExportRawIdentity` and `DecideRequest` calls from the Settings process return `NotAuthorized`; the fixture's approver can still perform its authorized actions. An arbitrary same-UID test client is denied the manager reads, and replacing the trusted test executable invalidates its role. Use disposable installed-path fixtures with the repository's supported test caller configuration, not a production allowlist broadened to the test runner.

**W6 test implementation:** `gnome/nostr-settings/tests/test-panel-mount.c` uses
real `nss_window_new`, shared panels and a real `grotto-daemon` on a private bus.
Disposable executable-role fixtures test all current forbidden Control1 methods,
actual identity/app rows, label-change signals, public activation targets, daemon
loss/restart, arbitrary same-UID/name-owner rejection and inode replacement. Only
the desktop activation receiver is captured. Missing bus/display/role support
fails visibly. Execution results belong in bead `nostrc-u6v55.23.6`; a saved plan
or test implementation alone is not a passing execution or VM acceptance.

## d. Blueprint sketches and dispositions

Sketches are design-level. Grotto's real templates also carry accessibility properties and mnemonics.

### d.1 Review dialog

See §e.

### d.2 Identity panel (replaces Home)

```blueprint
template $GrottoIdentityPanel: $GrottoPanel {
  title: _("Identity");
  tag: "identity";
  child: Adw.NavigationView navigation {
    Adw.NavigationPage identity_page {
      title: _("Identity");
      tag: "identity-page";
      child: Adw.ToolbarView {
        [top] Adw.HeaderBar {}
        [top] Adw.Banner setup_banner {
          title: _("This identity isn't backed up");
          button-label: _("Back Up…");
        }
        content: Adw.PreferencesPage {
          Adw.PreferencesGroup {
            Adw.Avatar avatar { size: 96; show-initials: true; halign: center; }
          }
          Adw.PreferencesGroup {
            Adw.EntryRow name_row {
              title: _("_Name");
              use-underline: true;
              show-apply-button: true;
            }
            Adw.ActionRow key_row {
              title: _("Public Key");
              subtitle-selectable: true;
              [suffix] Button copy_key {
                icon-name: "edit-copy-symbolic"; valign: center;
                tooltip-text: _("Copy Public Key");
                styles ["flat"]
              }
              [suffix] Button show_qr {
                icon-name: "qr-code-symbolic"; valign: center;
                tooltip-text: _("Show QR Code");
                styles ["flat"]
              }
            }
          }
          Adw.PreferencesGroup {
            Adw.ActionRow backup_row {
              title: _("_Backup");
              use-underline: true;
              subtitle: _("Not backed up");
              activatable: true;
              [suffix] Image { icon-name: "go-next-symbolic"; }
            }
            Adw.ActionRow apps_row {
              title: _("Connected Apps");
              subtitle: _("3 apps use this identity");
              activatable: true;
              [suffix] Image { icon-name: "go-next-symbolic"; }
            }
            Adw.ActionRow default_row {
              title: _("Default for Older Apps");
              subtitle: _("Used by apps that don't choose an identity");
              [suffix] Button set_default {
                label: _("Set as Default"); valign: center;
              }
            }
          }
          Adw.PreferencesGroup other_group {
            title: _("Other Identities");
            visible: false;
            ListBox other_list { selection-mode: none; styles ["boxed-list"] }
          }
          Adw.PreferencesGroup add_group { // root only, after Other Identities
            Button add_identity {
              label: _("_Add Identity…"); use-underline: true;
              halign: center; styles ["flat"]
            }
          }
          Adw.PreferencesGroup advanced_group { // pinned identity detail only
            title: _("Advanced");
            Button remove_identity {
              label: _("_Remove Identity…"); use-underline: true;
              halign: center; styles ["destructive-action", "pill"]
            }
          }
        };
      };
    }
  };
}
```

Behaviour:

- The first page shows the **daemon's default identity** (else the first listed). Choosing an Other Identity pushes the same template pinned to that npub, as `CcUsersPage` pushes `CcUserPage`. There is no selection state, so `accounts_store_set_selected` and the explanatory subtitle go away.
- **Add Identity…** is the flat action after Other Identities on the root, per owner direction. Root content describes the actual default identity, not a synthetic “active account”. Other identity rows push pinned pages; Back never changes the daemon default. **Remove Identity…** is available on the pinned identity detail under its separate Advanced group, never adjacent to Add. For the root/default identity, an “Identity Details” navigation row opens that same pinned view. Both call trusted host actions; Nostr Settings uses a clearly labelled Grotto hand-off.
- Name edits are explicitly **local labels**, not Nostr profile publishing. For an identity without a label, use “Identity” plus abbreviated npub; never invent a downloaded profile. All `ngettext` app counts and dynamic sentences are translated as full strings.
- No identities: `AdwStatusPage`, "No Identity Yet", "Create or import an identity to start signing", with pill "Add Identity…".
- `default_row` when this identity is the default: subtitle "Apps that don't choose an identity use this one", `object-select-symbolic` suffix, no button.
- Watch-only identities remain navigable metadata with “Watch only — cannot sign”; hide backup, default-for-signing and signing self-test actions. Imported keys with no stored phrase show “No recovery phrase is stored for this identity” on Backup and offer encrypted export. Unsupported capabilities hide only the unavailable action and explain alternatives, never fabricate seed words.
- Remove: `AdwAlertDialog`, heading "Remove Identity?", body "This identity is deleted from this device. Without a backup it can't be recovered.", extra child is typed npub entry with Copy, responses Cancel and destructive **Remove**, default Cancel.

### d.3 Backup page (trusted, in Grotto, pushed from `backup_row`)

```blueprint
template $GrottoBackupPage: Adw.NavigationPage {
  title: _("Back Up Identity");
  tag: "backup";
  child: Adw.ToolbarView {
    [top] Adw.HeaderBar {}
    content: Adw.PreferencesPage {
      description: _("Anyone with this backup controls your identity. Grotto developers will never ask for it");
      Adw.PreferencesGroup {
        Adw.ActionRow phrase_row {
          title: _("Show Recovery Phrase…");
          subtitle: _("Words to write down and keep offline");
          activatable: true;
        }
      }
      Adw.PreferencesGroup {
        title: _("Encrypted Backup");
        description: _("Protected by a passphrase you choose");
        Adw.ActionRow save_row {
          title: _("Save to File…");
          activatable: true;
        }
        Adw.ActionRow qr_row {
          title: _("Show QR Code…");
          activatable: true;
        }
      }
      Adw.PreferencesGroup {
        title: _("Advanced");
        Adw.ActionRow raw_row {
          title: _("Copy Secret Key…");
          subtitle: _("Clipboard managers may keep a copy");
          activatable: true;
        }
        Adw.ActionRow test_row {
          title: _("Test This Identity");
          subtitle: _("Signs and encrypts a test message on this device");
          activatable: true;
        }
        Adw.ActionRow replace_row {
          title: _("Create Replacement Identity…");
          subtitle: _("Apps aren't moved and permissions aren't copied");
          activatable: true;
        }
        Adw.ActionRow announcement_row {
          title: _("Save Migration Announcement…");
          activatable: true;
          visible: false;
        }
      }
    };
  };
}
```

These are design-level trees, not compiled Blueprint. Action rows that **navigate to pages** get arrows; dialog actions use ellipses, no misleading navigation arrow. Recovery phrase, passphrase and raw-key dialogs remain Grotto-owned and pin the displayed identity through async completion. `AdwPreferencesPage.description` is within the floor. Dialogs on this page:

| Action | Dialog | Copy | Commit | Notes |
|---|---|---|---|---|
| Show Recovery Phrase… | `AdwDialog`, words in a grid | “Write these words down in order. Keep them private and offline” | Check row “I wrote these down” enables **Record Backup**; Close otherwise | Fetch only after deliberate activation; clear on close/unmap, lock and cancellation. Acknowledgement is not proof of recovery. Keep secondary **Copy Recovery Phrase…** behind the existing clipboard warning; no silent capability removal |
| Save to File… / Show QR Code… | `AdwDialog` with `GnSecureEntry` in a row | "Choose a passphrase. It's needed to restore this backup" | **Save…** / **Show** | Each action has its own field; clear on close |
| Copy Secret Key… | `AdwAlertDialog` | Heading "Copy Secret Key?"; body "Anyone with this key controls your identity. Clipboard managers may keep a copy" | Cancel, destructive **Copy Key** | Default Cancel |
| Create Replacement Identity… | `AdwAlertDialog` | Heading “Create Replacement Identity?”; body “Both identities stay on this device. Apps keep using this one until changed” | Cancel, **Create Identity** | Retain existing signed migration-announcement save, not automatic publication |

On failure, show a local error with Retry, not a successful toast. Native save uses `GtkFileDialog`; cancellation or write failure must not set backup acknowledgement. No secret in `GListModel`, activity, notification, diagnostics, D-Bus activation target or shared-panel resource. Wipe transient buffers; clear a copied secret only while Grotto still owns that clipboard content, never erase another application's later copy. Keep current backend custody and developer-keychain safeguards unchanged.

### d.4 Connected Apps panel

```blueprint
template $GrottoAppsPanel: $GrottoPanel {
  title: _("Connected Apps");
  tag: "apps";
  child: Adw.NavigationView navigation {
    Adw.NavigationPage {
      title: _("Connected Apps");
      child: Adw.ToolbarView {
        [top] Adw.HeaderBar {}
        [top] Adw.Banner paused_banner {
          title: _("Signing is paused");
          button-label: _("Resume");
        }
        content: Adw.PreferencesPage {
          Adw.PreferencesGroup pending_group {
            title: _("Pending Requests");
            visible: false;
            ListBox pending_list { selection-mode: none; styles ["boxed-list"] }
          }
          Adw.PreferencesGroup {
            Adw.SwitchRow pause_row {
              title: _("_Pause Signing");
              use-underline: true;
              subtitle: _("Apps can't sign until signing resumes");
            }
          }
          Adw.PreferencesGroup apps_group {
            title: _("Apps");
            ListBox apps_list { selection-mode: none; styles ["boxed-list"] }
          }
          Adw.PreferencesGroup {
            Button add_app {
              label: _("_Add App…");
              use-underline: true;
              halign: center;
              styles ["pill"]
            }
          }
        };
      };
    }
  };
}
```

App row: title is label or verified name (never an unverified claim); subtitle one line, e.g. “Local app · Ask unless permitted · Used today”; arrow. Add App… calls **`host->add_app`**. In Grotto it opens the trusted `AdwDialog` listing Paste Link / Scan QR Code / Open Link File / Create Pairing Link (today's `page-connect.blp` rows), with “Apps on this computer ask the first time they need to sign”. Nostr Settings sends the fixed public navigation target `add-app` to Grotto. Pairing secrets, URI ingestion and import-capable scanning never travel through the panel command enum. Choose identity and review supplied relays **before any supplied endpoint is contacted**, preserving the existing Connect contract.

App page: avatar header; `AdwEntryRow` Local Name; Verified As, Fingerprint, Connected Since; group “Access” with three radio rows using G15 words (**Trust this app for routine operations** / **Ask for sensitive operations** / **Ask unless permitted**) and description “Blocked actions always stay blocked”. Preserve “Previous access policy” and its explicit upgrade; selecting the new routine preset must not silently upgrade old Full access. Lowering a preset does **not** clear explicit grants; show that fact beside the change. Permissions row (arrow) shows daemon-reported active/inactive/expired status and reason; keep duration editing there, including custom exact-kind inputs and conflict refresh. Recent Activity links to the filtered log. **Remove App…** names the consequences: pairing, permissions and pending work are revoked; it is not **Block App…**. In Nostr Settings security controls are replaced by **Change in Grotto…**, pinned to this app, not fake editable rows.

### d.5 Activity and Remote Signing

Activity: a `GtkListView` over the paged model replacing the boxed list; `GtkSearchEntry` in header bar; outcome `AdwComboRow` with translated labels; kind filter drops raw number entry for a catalogue-backed search that still accepts a number. Remote Signing: `AdwSwitchRow` Enable; status row; relay list editing per the boxed-list editable pattern; Allowed Methods row; pill-less "Reset Endpoint…" row with confirmation.

### d.6 Dispositions

| Surface | Disposition | Destination |
|---|---|---|
| `approval-dialog` | **Keep, rewrite** | Grotto trusted UI, §e |
| `page-home` | **Drop** | Identity and Connected Apps panels |
| `page-identity-detail` | **Split** | Identity panel (library) + Backup page (Grotto) |
| `page-client-detail` | **Move, rewrite** | App page in Connected Apps |
| `page-connect` | **Merge** | Add App… dialog |
| `page-history` | **Move, rewrite** | Activity panel |
| `page-settings` | **Split, shrink** | Preferences dialog + Remote Signing panel; appearance and language dropped |
| `signer-window` | **Rewrite** | §b.2 |
| `onboarding-assistant` | **Keep, simplify** | Grotto trusted UI; `AdwNavigationView` pages, each with its own header |
| `lock-screen` | **Keep, simplify** | Rebuild on `AdwStatusPage` (Q3) |
| `shortcuts-window` | **Keep** | Update entries |
| `sheet-create-bunker` | **Split** | Identity chooser in Add App…; config to Remote Signing |
| `sheet-qr-display` | **Keep, simplify** | Public-key-only QR view may live in library with an npub-only typed entrypoint; existing pairing-token and backup/secret QR presenter remains Grotto-only. Do not expose a generic arbitrary QR payload setter in the library |
| `sheet-qr-scanner` | **Keep, fix** | Grotto trusted UI, since it can read keys |
| `gn-secure-entry` | **Keep** | Grotto trusted UI only |
| `data/css/app.css`, `high-contrast.css` | **Drop** almost entirely | Keep only what a test proves necessary |

## e. Review dialog redesign

### e.1 Principles

Three questions in order: **who** is asking, **what** would happen, **how far** the permission extends. No daemon behavior changes: every control maps to one existing `DecideRequest` mode or flow call. **The normal review contains one permission selector, not four expanded radio rows plus saved-policy and flow groups.** Default visible content: app, action/impact, signing identity, short preview, Permission (Allow once), full scope explanation, expiry, Deny Once / Allow Once. Details is technical data only; it cannot hide the consent scope. Special contexts replace this normal form, rather than stacking more forms onto it.

### e.2 Sketch

Widget tree/copy below is an implementation guide, not a compiled template. Populate the ComboRow model with full translated strings and a wrapping factory; the closed row's companion Scope text must always expose the complete current choice. All non-normal groups start hidden.

```blueprint
template $GnostrApprovalDialog: Adw.Dialog {
  title: _("Review Request");
  content-width: 420;
  width-request: 300;
  follows-content-size: true;
  Adw.ToolbarView {
    [top] Adw.HeaderBar {
      show-end-title-buttons: false;
      [start] Button btn_not_now {
        label: _("_Not Now");
        use-underline: true;
        tooltip-text: _("Keep This Request Waiting");
      }
      [end] MenuButton more_menu {
        icon-name: "view-more-symbolic";
        tooltip-text: _("More Options");
        menu-model: block_menu;
      }
    }
    content: Adw.PreferencesPage {
      Adw.PreferencesGroup {
        Box {
          orientation: vertical; spacing: 6; halign: center;
          Adw.Avatar app_avatar { size: 64; show-initials: true; }
          Label header_title { wrap: true; justify: center; styles ["title-2"] }
          Label label_verified { wrap: true; justify: center; styles ["dim-label"] }
        }
      }
      Adw.PreferencesGroup {
        Adw.ActionRow row_action { title: _("Action"); subtitle-lines: 0; }
        Adw.ActionRow row_identity { title: _("Signing As"); subtitle-lines: 0; }
        Adw.ActionRow row_relay { title: _("Relay"); visible: false; subtitle-lines: 0; }
        Adw.ActionRow row_reason { title: _("Reason Given by App"); visible: false; subtitle-lines: 0; }
        Adw.ActionRow row_impact { title: _("Effect"); subtitle-lines: 0; }
        Adw.ActionRow row_expiry { title: _("Expires"); subtitle-lines: 0; }
      }
      Adw.PreferencesGroup group_preview {
        title: _("Content");
        Label content_preview {
          wrap: true; wrap-mode: word_char; xalign: 0; selectable: true;
          styles ["card"]
        }
      }
      Adw.PreferencesGroup group_scope {
        Adw.ComboRow permission_choice {
          title: _("Permission");
          // Model holds only daemon-supported choices for this context.
          // Ordinary: Allow once / Allow for 24 hours /
          // Always allow this app to … / Trust this app for routine operations.
        }
        Adw.ActionRow scope_summary {
          title: _("Scope");
          subtitle-lines: 0; // full sentence; never hidden in Details
        }
      }
      Adw.PreferencesGroup group_saved {
        visible: false;
        Adw.ActionRow saved_status {
          title: _("Saved Permission Is Inactive");
          subtitle-lines: 0; // daemon reason, exact identity, original expiry
        }
        // Policy-review replaces permission_choice's model with:
        // Allow once / Activate saved permission / Discard saved permission.
      }
      Adw.PreferencesGroup group_flow {
        title: _("This Message Only");
        visible: false;
        Adw.ActionRow row_flow_encrypt {
          title: _("Allow encryption for this message only");
          activatable-widget: chk_flow_encrypt;
          [prefix] CheckButton chk_flow_encrypt {}
        }
        Adw.ActionRow row_flow_seals {
          title: _("Allow signing its encrypted copies");
          activatable-widget: chk_flow_seals;
          [prefix] CheckButton chk_flow_seals {}
        }
      }
      Adw.PreferencesGroup group_more {
        Adw.ActionRow stepper {
          visible: false;
          title: _("1 of 3");
          [suffix] Box {
            valign: center; styles ["linked"]
            Button btn_previous { icon-name: "go-previous-symbolic"; tooltip-text: _("Previous Request"); }
            Button btn_next { icon-name: "go-next-symbolic"; tooltip-text: _("Next Request"); }
          }
        }
        Adw.ExpanderRow batch_expander {
          title: _("Requests in This Flow");
          visible: false;
        }
        Adw.ExpanderRow details_expander {
          title: _("Details");
        }
      }
    };
    [bottom] Box {
      homogeneous: true; spacing: 12;
      margin-start: 12; margin-end: 12; margin-top: 12; margin-bottom: 12;
      Button btn_deny { label: _("_Deny Once"); use-underline: true; }
      Button btn_approve {
        label: _("_Allow Once");
        use-underline: true;
        receives-default: false;
        styles ["suggested-action"]
      }
    }
  }
}

menu block_menu {
  section {
    item { label: _("Block This Operation for This App"); action: "review.block-operation"; }
    item { label: _("Block App…"); action: "review.block-app"; }
  }
}
```

### e.3 Copy by request type

| Case | `header_title` | Permission selector / visible fields |
|---|---|---|
| Note | “Jank wants to sign a note” | Four choices only when supported. Effect: “Jank can publish this signed note”; Grotto does not publish it |
| Contact list | "Jank wants to update your contact list" | All four |
| AUTH | “Jank wants to sign in to a relay” | `row_relay` shows full canonical URL. Persistent choice: “Always allow this app to sign in to wss://nos.lol as Alice”; no hostname-only scope |
| Encrypted message | "Jank wants to sign an encrypted message" | "Encrypted message": no ciphertext preview |
| Encrypt or decrypt | "Jank wants to decrypt a message" | Peer fingerprint in Details |
| Custom kind | "Jank wants to sign a custom event (kind 31234)" | Trust hidden as it doesn't apply; Always available |
| `connect` | "Jank wants to connect" | No ladder. Identity, relays, fingerprint rows. Commit **Pair Application** |
| Policy-review | Same as operation | Replace selector with Allow once / Activate saved permission / Discard saved permission; show original expiry and daemon reason |
| Identity self-test | “Test Alice's identity” | No reusable choices. Show signing, encryption and decryption together; nothing is published and no app permission is stored |
| Room-send flow | “Jank wants to prepare encrypted messages” | Offer “This message only…” when eligible; show unchecked encryption/seal choices only when selected, with count, exact identity and actual remaining window |

Identity row: "Alice" with subtitle `npub1abcd…wxyz`. If a saved grant exists for another identity: "Saved permission is for Bob". `label_verified`: "Verified local app", "Via browser bridge · example.org", or for an unverified claim, "Unverified: claims to be “Jank”".

`btn_approve` tracks the selected scope: **Allow Once**, **Allow for 24 Hours**, **Always Allow**, **Trust App**, **Activate Permission**, **Discard Permission**, **Pair Application**, or **Allow for This Message**. The full corrected vocabulary stays in the selector; its selected item and scope summary must wrap rather than truncate a key distinction. `Discard Permission` is a neutral cleanup action, never styled as an allow.

Scope summaries (full translated strings, label and fingerprint both visible):

- Once: “Only this request from Jank, signing a contact list as Alice (npub1…abcd). No permission is saved”.
- Temporary: “Jank may sign contact lists as Alice (npub1…abcd) for 24 hours”.
- Persistent: “Always allow this app to sign contact lists as Alice (npub1…abcd), until revoked”. AUTH adds the **exact canonical relay**, not just a hostname.
- Encrypt/decrypt: state “all peers” for a saved operation grant; the peer in this request is **not** a peer-scoped saved grant. Encryption and decryption are separate choices.
- Routine trust: “Trust this app for routine operations” plus “Changes this app's access preset, not only this request or identity. Includes publishing, messaging, relay sign-in, profile/contact changes, deletion and zap requests. Custom kinds and other ask-by-default kinds still need their own grant. Blocks remain in effect”. The current ask-by-default routine request gets its scoped grant atomically, as today.
- Policy review: “Activate the saved permission for Alice at [relay if AUTH]. Original expiry: [time / until revoked]”. Identity drift never aliases A's saved scope to B.
- Flow: “[N] encrypted copies, including your copy; at most [remaining] left from first arrival. Count is supplied by the app, not proof of recipients. No permission is saved to disk”. Do not imply one body/recipient set is authenticated by a grouping hint.

At 360 px the content scrolls and scope remains readable; actions stay in the bottom bar. No raw ciphertext, secret QR, private message body, remote image or automatic relay/profile fetch is displayed. If all information is not loaded, show `GtkSpinner` and keep allow disabled.

### e.4 Consent semantics: unchanged, one by one

| Control | Mode or call | Next identical request (honesty table, investigation §5 as amended by G15) |
|---|---|---|
| Allow Once | `allow-once` | Re-evaluates policy; ordinarily prompts |
| Allow for 24 Hours | `allow-24h` | Silent until the 24-hour expiry |
| Always Allow | `allow` | Silent until revoked |
| Trust App | `trust-routine`: atomic | Current request succeeds; preset is saved, plus this exact first grant when needed. Other ask-by-default kinds/custom kinds still ask without their own grant; explicit deny wins |
| Deny Once, `Esc`, ordinary close | `deny-once` | This request is denied without saving a block; later requests are re-evaluated |
| Not Now | No decision RPC | Stays pending until expiry; notification withdrawn |
| Block This Operation… | `block-operation` | Silently denied |
| Block App… (confirm) | `block-app` | All operations denied until removed |
| Activate saved permission | `activate-saved` | Allows the pinned request with saved scope and original expiry |
| Discard saved permission | `discard-saved` | Deletes inactive saved authority; **current request remains pending**. Refetch and reset review to Allow once; do not close as though signing succeeded |
| Flow check rows | `approve-flow` signal | Ephemeral consent; bounded window |
| Allow Selected Once | `approve-selected` signal | Individual once decisions for visible selected IDs only; no future authority |

For Discard, keep/refetch the still-pending request rather than using a generic “success closes dialog” callback (`nips/nip55l/src/glib/signer_approval_service.c:1250-1261`). For selected batches, list each operation, kind, peer and identity; selecting a presentation group never authorizes unseen future work. Saved policy and flow controls are alternative contexts, not additive consent checkboxes.

Safety invariants to keep and test:

- Initial focus on **Deny Once**; no default widget; Return is never consent. The HIG permits no default where the action is inconvenient or irreversible ([dialogs]).
- **Scope resets to Allow once on every step, new request and daemon-owner change.** A choice never carries over to another request. Persistent, flow and saved-policy actions are mutually exclusive; no invisible checkbox precedence. Changing away from flow clears its checkboxes.
- Choices follow daemon fields already present (`rememberable`, `routine_trust_available`, `policy_review`, pinned identity, operation, flow metadata); this is visibility, not a second policy evaluator. If an explicit allowed-mode list is later added, feature-detect it. Empty `claimed:` cannot receive reusable consent; named UID-only claims are labelled unverifiable. No special “always-confirm” operations are introduced.
- Sensitivity is rechecked in the decision handler, as it is today (`approval_dialog.c:89-91`).
- App-supplied text uses `use-markup: false`; preview is bounded plain text per plan §5.5.
- **`Ctrl+A` and `Ctrl+D` are removed**; `Alt+A`, `Alt+D` and `Alt+N` mnemonics replace them.

**Capitalisation note.** The vocabulary words are unchanged, with HIG capitalisation by control type ([writing-style]): radio rows use sentence case ("Allow once"), buttons and menu items use header case ("Allow Once", "Block App…"). The existing test asserting `"Allow once"` on the button changes accordingly.

## f. Migration from the G10 IA

### f.1 Mapping

| G10 | G17 |
|---|---|
| Home → identity strip and card | Identity panel root |
| Home → stat tiles | Dropped |
| Home → pending group | Connected Apps → Pending Requests |
| Home → clients | Connected Apps → Apps |
| Client detail | Connected Apps → app page |
| Identity detail dialog | Identity page + Backup page |
| Activity | Activity panel |
| Connect | Connected Apps → Add App… |
| Preferences | Preferences dialog + Remote Signing panel |

Compatibility to keep:

- Existing desktop `settings` action still opens Preferences; `gnome/nostr-settings/src/ui/nss-page-identity.c:153` still launches it until W6 replaces the redundant link. Add explicit public navigation targets for identity/apps/backup without changing existing action semantics. No secret-bearing command-line arguments.
- `win.connect`, `win.activity` and `signer_window_show_page()` keep working by mapping `home` → `identity`, `connect` → `apps`, `settings` → Preferences dialog. Stale notification actions now say "That request is no longer waiting" on Connected Apps rather than Home.
- GSettings: removed theme, contrast and language keys are simply ignored. No user data migration: labels stay in `accounts.ini` until W6.
- No stored permission semantics change. Identity metadata migration only belongs to W6; preserve single-writer ownership and back up metadata before schema adoption, not key material. Removing the Home view must not remove approval-controller/background holds, reconnect catch-up, notification actions or stale-request tombstones.
- Ship feature slices behind existing routes, then switch the default when the shared Identity and Apps routes work. Do not keep two independently writable permission/identity editors. Retire old templates, C-only fragments and unused CSS resources together with callers; keep API/action aliases only where compatibility requires them.

### f.2 Tests that change

| Test | Change |
|---|---|
| `signer/ui-layout` (`test-ui-layout.c`) | `:244-245`, avatar opens dialog → row pushes page. `:248-255`, stack of four named pages → descriptor list. `:264-299`, 360 px now asserts collapse with no bottom bar; screenshot names change (`home-*` → `identity-*`), and `metainfo.xml` and `test-desktop-contract.py` update together |
| `signer/home-model` (`test-home-dashboard.c`) | Extract dashboard-specific counters but keep/re-home daemon-signal, owner-loss, revision/conflict and version-skew read-only assertions against the adapter. Keep `signer/home-model` as a transitional alias or update selector registrations when renaming to `grotto-panels/control1-service`; never leave a stale selected test name |
| `test-home-model.c` (`/home/identities/*`) | Inventory and persistence cases stay against the Grotto adapter; `selection-not-default` is deleted with the concept |
| `signer/approval-dialog` (`apps/grotto/tests/test-approval-dialog.c`) | Replace `chk_remember`/`chk_24h` and Details-only trust/block assertions with `permission_choice`, `scope_summary`, mode-specific commit and menu assertions. Keep identity pinning, expiry, stepper, deny focus and handler sensitivity tests. **Add:** each selector choice emits exactly its daemon mode; once/24h/until-revoked copy names the identity/relay; trust says app-wide; all-peers crypto scope; reset per request; flow is mutually exclusive; Discard leaves request pending; `Ctrl+A` is Select All, not consent |
| `signer/onboarding` | Follows the rewritten steps |
| `signer/blueprint-drift` | Adds library templates; drops removed ones |
| `signer/desktop-contract` | Screenshot names plus preserved desktop action targets |
| `signer/ui-accessibility` | Real panel/approval entrypoints at 360 px; keyboard Back and deny focus; full scope readable; translated long strings; no color-only state or custom high-contrast dependency |
| New: `grotto-panels/smoke-host`, `grotto-panels/no-secrets`, `nostr-settings/panel-mount` | §c.3, §c.6 |

`signer/approval-integration` and the existing `test-permission-repeat` repeat-permission cases should pass **without weakened assertions**: they cover the daemon semantics that must survive. Complement them with real-window controller wiring: select a reusable mode in the redesigned dialog, submit, restart/reconcile, and send the next matching request through the actual gate; scoped allow suppresses only that scope, explicit deny wins, another identity/relay still asks. Self-test must still use one management review, not normal reusable app consent.

Implementation verification uses `scripts/test-gate.sh --explain` to inspect affected selection, then explicitly executes the relevant checks. Run `python3 scripts/test-selection-operational.py` **only if selector/workflow registration changes**. Preserve the existing private-bus/display isolation, fatal-warning and sanitizer safeguards. A missing display/backend or skip is not a pass. The proposed checks above are focused cases, not a new universal platform or benchmark programme.

Owner walkthroughs, when implementation exists: review one contact-list request with once and persistent choices; find a waiting request after Not Now without a notification; rename an identity and back it up without seeing recovery material on the everyday page. Repeat the three priority surfaces in the existing GNOME 46 acceptance environment. Design review alone is not that live acceptance.

### f.3 Versions (AGENTS.md)

**This task: no bump for any component**; only this design is edited, no shipped artifact changes. The following are decisions for implementation commits, not version edits in this task:

| Component | Decision at implementation |
|---|---|
| New `grotto-panels` | Start 0.1.0; add manifest row, authoritative CMake/Meson version, exported header/SONAME and pkg-config contract together |
| Grotto Preview (manifest 0.1.1) | MINOR-class user-visible functionality/IA → 0.2.0 if baseline unchanged; update `apps/grotto/CMakeLists.txt` `GROTTO_VERSION` and Meson `project` plus manifest in the owning commit |
| nip55l (manifest 0.8.0) / daemon boundary | No policy change in W1–W5. W6 additive management role/metadata API is MINOR-class → 0.9.0 if baseline unchanged; update `nips/nip55l/include/nostr/nip55l/signer_ops.h` version and manifest. Grotto daemon ships under Grotto's version, not a second invented component |
| Nostr Settings (`NOSTR_SETTINGS_VERSION` 0.1.0) | W6 MINOR → 0.2.0 if baseline unchanged; update `gnome/nostr-settings/CMakeLists.txt`, add/align its manifest row and any authoritative sources present then |
| `nostr-gtk`, `nostr-gobject`, native-messaging host, other Nostr Settings services | No bump unless implementation actually changes their shipped public surface; dependency rebuild alone is not functionality |

Recheck baseline versions before landing each work item; no repeated bump for slices already folded into the same unreleased version. Do not update Latest Release, release tags or publish a tag from this design.

## g. Sequenced plan

Sizes: **S** ≤ 2 days, **M** ≈ 1 week, **L** ≈ 2 weeks of focused work.

| # | Work item | Size | Depends on | Done when |
|---|---|---|---|---|
| **W1** | Review dialog redesign (§e) | M | — | `signer/approval-dialog` and `signer/approval-integration` pass; owner accepts on GNOME 46 |
| **W2** | `grotto-panels` scaffold: service interface, Control1 and fake services, `GrottoPanel` base, smoke host (level 1) | M | — | `grotto-panels/smoke-host` and `no-secrets` pass; library builds with API pins |
| **W3** | Identity panel + trusted Backup page + Grotto metadata adapter, behind existing routes | L | W2 | Identity/Backup real routes work and `signer/ui-layout` covers them; old Home remains default until W4 preserves app/pending access |
| **W4** | Connected Apps, Activity, Remote Signing; window shell; Preferences; CSS/resource cleanup; screenshots | L | W2, W3 | Switch default to Identity and retire Home only after Identity/Apps/pending routes and action aliases work; relevant layout/accessibility, model, permission-editor, drift and desktop-contract checks pass; representative fixture screenshots replaced and provenance updated |
| **W5** | Remaining in-scope dialogs/onboarding: sheet copy, purposeful action labels, i18n | M | W1, W4 | Audit dispositions implemented without changing custody/transport behavior; focused onboarding, identity/backup and QR tests pass; owner reruns the priority walkthroughs |
| **W6** | Nostr Settings hosting next release: narrow manager reads/labels, public identity metadata, shell adapter and real-host mount test | L | W2, W4; Q1 sign-off | Actual `nss_window_new` mounts shared panels with daemon data; security edits hand off; forbidden methods remain denied. `nostr-settings/panel-mount` passes, not just fake service smoke |

W1 and W2 are independent; W1 addresses the worst-felt surface without waiting for the library. These six rows are **proposed child-bead briefs**, not a parallel markdown task ledger; status/progress belongs in Beads. Do not create implementation tasks or treat this parent as signed off merely because the draft exists. The owner may authorize creation from these briefs after review.

Landing/ownership boundaries for those briefs: W1 owns approval Blueprint/C/controller wiring and its tests; W2 owns only the new module and dependency wiring; W3 owns Identity/Backup plus Grotto's metadata adapter; W4 owns management routing/remaining panels and obsolete resource removal; W5 owns remaining dialogs/onboarding/copy; W6 owns the additive daemon manager contract and Nostr Settings adapter, not consumer-service rewrites. Coordinate shared CMake/tests/resources through the landing owner; no child may loosen a gate to make a panel mount. Each item records actual selected checks and its component version decision in its bead.

## Design checks performed and remaining acceptance

Performed for this draft: read the amended plan/investigation, all 14 Blueprint surfaces (plus secure-entry support widget), relevant owning C and Nostr Settings files; inspect six existing PNGs; verify GNOME/HIG references online; run a bounded consistency review of the document. The review corrected action placement, trusted Add App routing, capability-change notification and W3/W4 sequencing. No build, product test, smoke host, Docker, new screenshot, live GNOME session, commit, push or merge was executed for this design. No source file was modified.

The exact implementation test names in §f and smoke-host commands in §c describe future checks. They are not passing receipts. Acceptance still belongs to the owner: review this first draft and its provisional manager boundary, approve the three priority surfaces, then authorize child work. The parent bead stays in progress, not closed.

## Owner decisions — resolved 2026-11-04
Q1 manager role (approval authority stays separate) — **accepted**; Q2 remove in-app appearance/language, follow the system — **accepted**; Q3 keep the existing management lock, no redesign; Settings-hosted panels show capability or hand off — **accepted**; Q4 header-case buttons around the unchanged vocabulary — **accepted**. Priority surfaces §d.1/§e, §d.2, §d.3 approved; W1 ∥ W2 started.

## Owner decisions needed (as asked)

| # | Question | Recommendation |
|---|---|---|
| Q1 | Accept manager-readable panels with security changes/backup/review handed to Grotto next release, rather than making Settings a full approver? | Yes; approval authority remains separate. W1–W5 do not depend on this decision |
| Q2 | Remove in-app appearance and language settings? | Yes, follow the system |
| Q3 | Accept retaining Grotto's existing management privacy lock without pretending it authorizes Settings-hosted operations? | Yes; no lock/authentication redesign. Settings shows daemon capability or Grotto hand-off, not fake unlock |
| Q4 | Accept header-case buttons around unchanged vocabulary? | Yes |

## References

Verified online 2026-11-04. GNOME GitLab raw endpoints were unavailable during this pass; the official GNOME GitHub mirror's `gnome-46` branch supplied the Users UI source. HIG is current guidance, **not** a claim every widget in its latest examples exists on libadwaita 1.5. No memory-only HIG rule is asserted as verified.

- [GNOME 46 Users page][cc-users-page.ui]: current/other-user navigation and the actual Add User styling.
- [GNOME 46 user detail][cc-user-page.ui]: avatar, editable Name row, permission infobar, header/page hierarchy. Its OS-account privilege boundary is **not** borrowed as signer authorization.
- [HIG boxed lists][boxed-lists]: semantic grouping, short settings lists and row/control layout.
- [HIG dialogs][dialogs]: modal deliberate actions, specific imperative buttons, safe default/close behavior.
- [HIG Windows — Secondary Windows][windows]: current HIG guidance covering **Preferences Windows**. The guessed old `/patterns/windows/preferences-windows.html` URL did not resolve; it is not cited as a fetched page.
- [PreferencesDialog API][preferences-dialog]: available since 1.5. Use it for host preferences, not as the multi-panel application shell.
- [PreferencesWindow API][preferences-window]: replacement/deprecation guidance; it remains usable at the 1.5 floor, so deprecation alone is not a reason to rewrite every existing page.
- [HIG writing style][writing-style]: plain task vocabulary, control-specific capitalization and ellipses.
- [HIG sidebars][sidebars]: navigation between panel locations.

[cc-users-page.ui]: https://raw.githubusercontent.com/GNOME/gnome-control-center/gnome-46/panels/system/users/cc-users-page.ui
[cc-user-page.ui]: https://raw.githubusercontent.com/GNOME/gnome-control-center/gnome-46/panels/system/users/cc-user-page.ui
[boxed-lists]: https://developer.gnome.org/hig/patterns/containers/boxed-lists.html
[dialogs]: https://developer.gnome.org/hig/patterns/feedback/dialogs.html
[windows]: https://developer.gnome.org/hig/patterns/containers/windows.html
[preferences-dialog]: https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/class.PreferencesDialog.html
[preferences-window]: https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/class.PreferencesWindow.html
[writing-style]: https://developer.gnome.org/hig/guidelines/writing-style.html
[sidebars]: https://developer.gnome.org/hig/patterns/nav/sidebars.html
