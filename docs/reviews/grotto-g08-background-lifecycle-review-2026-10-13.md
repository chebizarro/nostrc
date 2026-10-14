# Grotto G08 — Background / Desktop Lifecycle Security Review

- **Date:** 2026-10-13
- **Target:** `origin/grotto/g08-background` @ `167709f8a` vs `master` (single commit "feat(grotto): add background and desktop activation lifecycle", bead `nostrc-u6v55.9`)
- **Spec:** `docs/plans/grotto-signer-ux-2026-10-12.md` §5.6 (full), §5.1 "Foreground rules", §5.8 (pause/lock), §5.10 (migration v1), §5.11 "Packaging", §3.4 (portal facts); §9 rows `signer/background`, `signer/desktop-contract`
- **Mode:** read-only review, no code changes. Line numbers refer to the branch tip.

**Verdicts:**
- **CONFIRMED**: shown directly by the code (and by a test where noted).
- **PLAUSIBLE**: the code path is certain, but the real-world impact depends on portal or session-manager behaviour that hasn't been run here.

## 1. Scope

Covered: (1) hold accounting with G07's `approval-controller`; (2) Background portal usage; (3) autostart and migration v1; (4) URI handling and the `--new-identity`/`--settings` actions; (5) quit, lock and the StatusNotifier; (6) consistency of the `.desktop`, metainfo and service files; (7) coverage gaps against §9.

Not covered: the notification content of G07, G06 pairing, the Shell extension (G13), daemon internals, and UI layout.

Files read: `apps/grotto/src/background.{c,h}`, the `main_app.c` diff and its surrounding activation/shutdown code, the `approval-controller.c` hold/lock paths, `signer-window.c` (quit, new-profile), `gn-status-notifier.c`, `data/*.desktop*`, `*.service.in`, `metainfo.xml`, `gschema.xml`, the CMake/Meson install rules, `tests/test-background.c`, `tests/test-desktop-activation.c` and `tests/test-desktop-contract.py`.

## 2. Findings (ranked)

| # | Sev | Verdict | Area | Location |
|---|---|---|---|---|
| H1 | High | CONFIRMED | Pause/resume | `main_app.c:499-508, 973`; `test-desktop-activation.c:120` |
| H2 | High | CONFIRMED | Pause on lock | `main_app.c:509-519, 577-583`; `approval-controller.c:1178-1181` |
| M1 | Medium | PLAUSIBLE | Holds / service mode | `background.c:241-249, 188-215`; `main_app.c:419, 439-444`; `approval-controller.c:536-557` |
| M2 | Medium | CONFIRMED (mapping) / PLAUSIBLE (impact) | Portal denial vs absence | `background.c:130-131, 145` |
| M3 | Medium | PLAUSIBLE | Portal autostart | `background.c:180-182` |
| M4 | Medium | CONFIRMED | Portal autostart cleanup | `background.c:128, 203-208` |
| M5 | Medium | CONFIRMED | URI / activation windows | `main_app.c:324-333, 768-775, 683-697` |
| L1 | Low | PLAUSIBLE | Native vs portal autostart path | `background.c:74-75, 177-185, 200` |
| L2 | Low | CONFIRMED | Autostart before disclosure | `background.c:73-77, 200, 206` |
| L3 | Low | CONFIRMED | `--new-identity` idempotence | `main_app.c:95-102`; `signer-window.c:621-625` |
| L4 | Low | CONFIRMED | Notifier / SetStatus churn | `main_app.c:525-548`; `background.c:106-115`; `gn-status-notifier.c` `set_presentation` |
| L5 | Low | CONFIRMED | Notifier stuck Passive | `main_app.c:622, 595-607, 448-451` |
| L6 | Low | CONFIRMED | Autostart template drift | `background.c:87-96`; `data/org.nostr.Grotto-autostart.desktop.in` |
| L7 | Low | PLAUSIBLE | Relocatable native installs | `background.c:91`; `main_app.c:440`; CMake/Meson `GROTTO_EXECUTABLE` |
| L8 | Low | CONFIRMED | Metainfo contract | `data/org.nostr.Grotto.metainfo.xml`; `test-desktop-contract.py:40-45` |

### H1 — `app.pause-signing` is a D-Bus-exported toggle that resumes signing for any session caller (CONFIRMED)

`on_pause_signing` (`main_app.c:505-508`) calls `set_signing_paused(!signing_paused)`. It is a plain `GActionEntry` app action (`main_app.c:973`), so GApplication exports it on `org.nostr.Grotto` through `org.gtk.Actions` and `org.freedesktop.Application.ActivateAction`. Any same-session process can activate it, and if Grotto isn't running, D-Bus activation starts it first. Grotto is the trusted approver, so it issues `Control1.SetPaused(false)` with no confirmation, no check of `desktop_locked`, and no user gesture.

**How it fails:** the user presses **Pause signing**, or the screen locks with `pause-on-screen-lock` on. A local process then calls `gdbus call --session -d org.nostr.Grotto -o /org/nostr/Grotto -m org.freedesktop.Application.ActivateAction pause-signing [] {}`. The daemon un-pauses and remembered permissions start signing again, even while the desktop is still locked.

**Why it matters:**
- It launders the approver-only gate on `SetPaused(false)` (§5.12 Control1 rule).
- It breaks "resumes only through authenticated management UI" (§5.8).
- It breaks "never auto-resumes without the approver path" (the review brief, item 5).

The test codifies the toggle: `test-desktop-activation.c:120` uses `pause-signing` to *resume*.

**Fix:**
- Make `pause-signing` one-way: it only ever sends `SetPaused(true)`.
- Add a separate `resume-signing` action that only presents a confirmation in the management window (refused or deferred while `desktop_locked`). The spec's §5.12 `ActivateAction("resume-signing")` relies on that confirmation, not on the GUI acting silently.
- Bind the menu and indicator items to the right action.
- Change the test so an activated `pause-signing` can never produce `SetPaused(false)`.

### H2 — `pause-on-screen-lock` only works while the approval UI process is alive and on GNOME's ScreenSaver name (CONFIRMED)

The only enforcement is `screen_lock_changed` and `control_appeared` in `main_app.c:509-519, 577-583`. These are driven by G07's `org.gnome.ScreenSaver` `ActiveChanged`/`GetActive` subscription (`approval-controller.c:644, 1178-1181`). A grep shows no daemon-side lock policy (`pause-on-screen-lock` appears only in `main_app.c` and the schema). The setting therefore does nothing when:

- **The user quit the UI.** After Ctrl+Q, which the menu copy and README describe as "authorised signing continues", nothing pauses on lock.
- **Background mode is off, denied (DENIED), or unavailable in a sandbox.** The service instance exits once G07's holds drain, so later locks are unobserved.
- **The desktop isn't GNOME.** KDE and others use `org.freedesktop.ScreenSaver`, so nothing is ever observed.
- **Service-mode exit race.** See M1.

Meanwhile the Settings row promises "Pause signing on screen lock" (`main_app.c:299-303`). Spec §5.8 says to "move enforcement to the daemon service". As written, a security setting can be silently ineffective, and nothing in the UI says so.

**Fix (pick one, explicitly):**
- **Preferred:** the daemon owns lock observation and the policy, via a `Control1` property or `bunker.ini`-style persisted desired state.
- **Interim:**
  - Treat `pause-on-screen-lock=true` as requiring a continuous hold, independent of `run-in-background`.
  - Have Ctrl+Q/Quit warn while it is enabled.
  - Show "unavailable on this desktop" when no supported lock service is present.

### M1 — Service-mode instance can exit before the background hold exists (PLAUSIBLE)

The startup sequence:

1. `on_dbus_connected` drops `setup_held` (`main_app.c:419`).
2. It then creates `GrottoBackground` (`:439`).
3. The background object takes no hold until the first `appeared`/`vanished` callback runs `refresh()`. That callback comes from `g_bus_watch_name_on_connection(... AUTO_START ...)` (`background.c:247`), which needs `StartServiceByName` plus `GetNameOwner` round-trips.
4. Only then does `refresh()` take the temporary hold, which becomes the continuous hold (`background.c:210-211`).

The only hold in between is G07's bootstrap → `GetStatus` → `ListRequests` chain (`approval-controller.c:536-557, 430-456`). Grotto sets no inactivity timeout (grep finds none). The `on_startup` hold also means GLib's own grace timer for idle service-mode instances is never armed. So if G07 finishes with no pending requests before the portal watch reports, `use_count` reaches 0 and the instance exits with `run-in-background=true`.

That ordering is most likely at login: the `launch-on-login` case, when `xdg-desktop-portal` is still being auto-started. Launch-on-login then quietly does nothing. The same window also kills H2's interim mitigation.

**Fix:** have `grotto_background_new` take the `temporary` slot immediately and release it in the first `appeared`/`vanished` handler (after `refresh`). Alternatively, set a short `g_application_set_inactivity_timeout` for service mode.

### M2 — Portal errors and non-success responses are treated as "explicit denial" (CONFIRMED mapping, PLAUSIBLE impact)

Two paths produce `DENIED`, which the UI reports as "Desktop denied background permission":

- **`response()`** (`background.c:130-131`): anything other than `code == 0 && background == true`. That includes code 1 (user dismissed the dialog) and code 2 (request ended another way, e.g. backend failure).
- **`request_done()`** (`background.c:145`): any D-Bus error on `RequestBackground`, such as a timeout, `NotSupported`, or a portal refusing a host app with no app id.

`DENIED` releases the continuous hold even on a native install (`background.c:42-43`). Portal *absence* on native keeps residence (`UNAVAILABLE && native`), but a portal that is present and errors removes it. That inverts the spec's distinction: absence is nonfatal, and only explicit denial disables continuous mode (§5.6). It also makes native behaviour depend on how a given portal version treats unsandboxed callers.

**Fix:**
- `DENIED` only for `code == 0 && !background`. Code 1 is arguably a denial too; decide and document.
- Code 2 and D-Bus errors map to `UNAVAILABLE`, so native keeps its hold.
- Add tests for each.

### M3 — Portal autostart requested with `dbus-activatable=TRUE` can open a window at login (PLAUSIBLE)

`version_done` sends `commandline=[grotto, --gapplication-service]` and also `dbus-activatable=TRUE` (`background.c:180-182`). The portal then writes `DBusActivatable=true` into the sandbox's autostart entry.

GDesktopAppInfo-based launchers (including gnome-session's non-systemd path) launch D-Bus-activatable entries with `org.freedesktop.Application.Activate`, not `Exec`. In Grotto that runs `on_activate` (`main_app.c:625-732`), which creates a window, contradicting "Start without opening a window" (`main_app.c:302`) and §5.6. `systemd-xdg-autostart-generator` uses `Exec`, so the result depends on the session manager.

Request-scoped activation does not depend on this flag; it uses `org.nostr.Grotto.service`.

**Fix:** send `dbus-activatable=FALSE` and assert it in `test-background.c`.

### M4 — A stale portal autostart entry is never cleaned up across restarts (CONFIRMED)

`portal_autostart` lives only in process memory: it starts FALSE and is set from the most recent Response (`background.c:128`). The cleanup request needs `!native && owner && portal_autostart && disclosed` in the same process (`background.c:205`). Cleanup is missed if:

- the user turns background or launch-on-login off while the portal is absent;
- the process exits before the clearing Response arrives;
- the keys are changed outside Grotto (dconf-editor, `gsettings`).

In each case the portal-written login entry survives, and every later start sees `portal_autostart=FALSE` and skips cleanup. Grotto then launches at every login despite `launch-on-login=false`, which breaks the "disabling background disables launch-on-login" contract in effect.

**Fix:** in sandboxed mode, once per process whenever the portal is present and `!(enabled && launch)`, issue one `RequestBackground(autostart=false)`. That call is idempotent. Alternatively, persist a "portal autostart requested" flag. Add a restart test.

### M5 — Opening a URI during onboarding creates a second window that bypasses onboarding (CONFIRMED)

`open_pairing_uri` (`main_app.c:768-775`) calls `present_pending`. When no `SignerWindow` exists, `present_pending` (`main_app.c:324-333`) calls `g_application_activate`. On first run that presents the onboarding assistant (`main_app.c:683-697`) and still finds no `SignerWindow`, so it constructs one with `signer_window_new`.

A `bunker:`/`nostrconnect:` link clicked before onboarding finishes therefore leaves onboarding open next to a full management window. The same happens through `review-request`/`deny-request` activation. That breaks "repeated activation presents the existing management/onboarding window" (§5.1), and the link-driven path skips identity setup.

The URI handling itself is safe:
- No parsing and no relay contact (CONFIRMED, `main_app.c:768-775`).
- The URI is never logged (CONFIRMED, `main_app.c:786`).
- No `nostr:` claim (CONFIRMED, `.desktop:16`).

**Fix:** if `management_window()` is an `OnboardingAssistant`, present it, and queue the review/pairing intent until `on_onboarding_finished`.

### L1 — Native autostart file shares the portal's autostart path (PLAUSIBLE)

The native entry is `$XDG_CONFIG_HOME/autostart/org.nostr.Grotto.desktop` (`background.c:74-75`). That is the same `<app-id>.desktop` path `xdg-desktop-portal` manages for an app it identifies as `org.nostr.Grotto`. On native installs `refresh()` writes the file (`:200`) and then sends `RequestBackground(autostart=false)` (`:177-185`).

If the portal resolves a host app id (newer portals do this from the systemd scope or the host registry) and honours `autostart=false` by unlinking, it deletes Grotto's native entry. Launch-on-login then silently undoes itself. This needs checking against portal 1.18 and 1.21 host-app handling.

**Fix:** give the native entry a distinct basename, e.g. `org.nostr.Grotto-login.desktop`, and update the cleanup paths to match.

### L2 — Native login entry is written before disclosure (CONFIRMED)

`grotto_background_sync_autostart` gates only on `native && run-in-background && launch-on-login` (`background.c:76-77`), not on `background-disclosure-accepted`. Migration v1 copies a legacy `autostart=true` into `launch-on-login`. On first run, `refresh()` writes the login entry (`:200`) even though background stays `DISABLED` until disclosure (`:206-207`).

The login-launched service then exits once G07's holds drain. It doesn't accept anything, but it is a login launch with no disclosed residence, and it contradicts the README's "Launch on login defaults off".

**Fix:** include the disclosure flag in the native gate.

### L3 — `--new-identity` is window-idempotent but not sheet-idempotent (CONFIRMED)

`on_app_new_profile` (`main_app.c:95-102`) correctly reuses the existing window. But `signer_window_show_new_profile` (`signer-window.c:621-625`) presents a new `SheetCreateProfile` on every call, so repeating the desktop action stacks sheets. On a fresh window it also stacks on top of the disclosure dialog: `on_activate:724` presents the dialog, then the action presents the sheet. `--settings` is idempotent.

**Fix:** reuse or focus an already-visible create sheet.

### L4 — Every pending-list change triggers `SetStatus` and notifier signals (CONFIRMED)

`refresh_indicator` runs on every pending `items-changed` and every status change (`main_app.c:525-548`). Each time it:

- calls `gn_status_notifier_set_presentation`, which unconditionally emits `NewTitle` and `NewIcon`;
- calls `grotto_background_set_status`, which issues a `SetStatus` D-Bus call even when the text is unchanged (`background.c:106-115`).

The content is generic and compliant: the three strings are fixed, there is no count, identity or app name, and `SetStatus` is skipped below portal v2. The issue is only churn during request bursts.

**Fix:** cache the last message and presentation, and emit only on change.

### L5 — The StatusNotifier can stay Passive forever (CONFIRMED)

The notifier starts Passive (`main_app.c:622`), and only the Shell-name watch, set up in `start_desktop_observation` after `on_dbus_connected`, can make it Active.

If the deferred bus connection fails or hits the 5 s `cancel_setup` (`main_app.c:448-451`), the indicator never appears on KDE or Ubuntu, even though `org.nostr.Shell.Signer1` is absent. Hide/restore otherwise works and is tested in one direction (`test-desktop-activation.c:77-83`).

Passive is "hidden" on Ubuntu's AppIndicator extension, but KDE puts Passive items in its overflow list rather than removing them. That matches the spec's GNOME-only intent and is noted here, not counted as a defect.

### L6 — Two sources of truth for the login entry (CONFIRMED)

`data/org.nostr.Grotto-autostart.desktop.in` is configured and installed to `$datadir/grotto` by both build systems, but the runtime never reads it. `background.c:87-96` builds its own key file. The contract test checks only the template (`test-desktop-contract.py:27-30`), and `test-background.c:241-246` checks only that `--gapplication-service` appears in the generated file.

**Fix:** either copy the installed template at runtime, or drop the template and have the contract test assert the generated entry (`NoDisplay`, no `DBusActivatable`, `Exec`).

### L7 — Native `Exec` is a compile-time absolute path (PLAUSIBLE)

The native entry's `Exec` uses `GROTTO_EXECUTABLE`, set at build time to `<prefix>/bin/grotto` (`background.c:91`; CMake/Meson). Native mode is anything without `/.flatpak-info` or `$SNAP` (`main_app.c:440`), which includes relocatable bundles such as an AppImage. For those, the login entry points at a path that won't exist at the next login.

**Fix:** for bundles, derive `Exec` from `$APPIMAGE` or `/proc/self/exe`, or disable native login when not installed under the prefix.

### L8 — Metainfo isn't covered by the desktop contract (CONFIRMED)

`signer/desktop-contract` only checks that the metainfo path appears in both build files. AppStream still says `<name>Grotto</name>`, not "Grotto (Preview)" (§5.11). It has no `<provides>` for the `bunker`/`nostrconnect`/`grotto` handlers or the `org.nostr.Grotto` user D-Bus name, and the build runs no `appstreamcli validate`.

The `.desktop` file, the service files and the actions are consistent with the code:
- With `DBusActivatable=true`, the `new-identity` and `settings` desktop actions map to the identically named app actions (`main_app.c:971-972`).
- The `Exec` fallbacks match the registered main options (`main_app.c:957-960`).
- Both CMake and Meson install `org.nostr.Grotto.service` with `--gapplication-service`.
- The symbolic icon is installed by both build systems.

## 3. Hold accounting proof (item 1)

All hold and release calls added or affected by G08:

| Site | Hold | Release |
|---|---|---|
| `main_app.c:615` `setup_held` | `on_startup` | `on_dbus_connected:419`, or `on_shutdown:399` (flag-guarded; exactly one fires) |
| `background.c:35-39` `held()` | single call site for `g_application_hold` | single call site for `g_application_release` |
| → slot `continuous` | `state()` when `REGISTERED`, or `UNAVAILABLE && native` (`:42-43`) | `state()` on any other state; `grotto_background_stop:256` |
| → slot `temporary` | `refresh:211` (REQUESTING only) | `cancel_request:55` (every refresh, appear, vanish, stop, version failure, request failure); `response:132` |
| G07 `hold()`/`release()` | counted `s->holds` | per-request/fetch release; `stop` drains (`approval-controller.c:1113-1114`) |

**The invariant holds.** Each background slot is a boolean, and `held()` changes the GApplication use count only on a FALSE↔TRUE edge. So each slot holds at most one reference, and every hold is matched by exactly one release on the opposite edge. `grotto_background_stop` (from `on_shutdown:400`, or `dispose`) forces both slots FALSE. G07's counter is independent, so the two are never double-held.

Scenario traces:

- **Portal absent:** `UNAVAILABLE`. Native → one continuous hold; sandbox → none. Tested.
- **Portal denial:** `DENIED`, no continuous hold; the temporary hold is released in `response`. Tested. Note that errors also land here (M2).
- **Enable → disable → enable:** each settings change runs `refresh` → `cancel_request` (temporary released) → `DISABLED` (continuous released) → `REQUESTING` (temporary held) → Response → `REGISTERED` (continuous held). Balanced, but the full cycle is not tested.
- **Daemon/portal vanish and reappear:** `vanished` → `UNAVAILABLE` → `appeared` → `REQUESTING` → `REGISTERED`. Balanced and tested (`test_granted`).
- **Quit:** `g_application_quit` ignores holds; `shutdown` stops the background object and the G07 controller and releases everything. Tested only behind DISPLAY (§5 below).
- **Late Response after cancel:** a request-path mismatch plus `stopped` means it can't take a hold again. Tested (`test_cancel`).

Two notes:

1. **The "continuous" hold isn't literally continuous.** `refresh()` releases it in `state(REQUESTING)` *before* taking `temporary` (`background.c:210-211`). The use count dips to zero synchronously, which is safe because the loop doesn't iterate in between. But a re-request that then fails with an error drops residence (M2). Taking `temporary` before calling `state()` would be clearer.
2. **The only real gap is before the first watch callback (M1),** not a leak.

## 4. Other items checked and found compliant

- **Portal calls:** async and cancellable (`s->cancel` is passed to `Properties.Get`, `RequestBackground` and `SetStatus`). The predicted handle path is subscribed before the call, and a mismatched reply re-subscribes (`background.c:147-153`). Stale generations are ignored. `Request.Close` is sent on cancel.
- **`SetStatus`:** only when `REGISTERED && version >= 2`, using the three fixed strings (`background.c:108-111`, test-enforced).
- **Request-scoped activation after denial:** independent of `GrottoBackground`; it goes through `org.nostr.Grotto.service` and `--gapplication-service`.
- **Native autostart:** runs `"<bindir>/grotto" --gapplication-service`, no `DBusActivatable`, `NoDisplay=true`. Native always sends portal `autostart=false` and the sandbox unlinks its in-sandbox native file, so there is no double native+portal autostart (but see L1).
- **Disable background → launch off** (`refresh:192-198`, plus the native branch at `settings_changed:219-222`). **Disable launch → background unchanged.**
- **Migration v1:** copies an explicit `autostart` only when `launch-on-login` has no user value. It sets `run-in-background=true` only from `start-minimized=true`, otherwise leaving the default true. It forces launch off when background is off. `start-minimized` is read only here: the contract test greps `src/` for any other `"start-minimized"`/`"autostart"` use, and `settings_manager` accessors were removed.
- **Ctrl+Q / indicator Quit:** both reach `g_application_quit`. At shutdown G07 resolves pending requests locally as `UNAVAILABLE`, withdrawing their notifications and the summary, and makes no deny call to the daemon. Nothing sticky blocks the next `StartServiceByName` summon. Ctrl+W destroys the window and does not hide it.
- **Pause on lock:** pauses only on a positively observed lock (`main_app.c:509-513`; `approval-controller.c:1136-1138` comment). It re-applies when the daemon reappears while locked (`:580-581`) and never resumes on unlock (test lines 114-119). H1 and H2 are the exceptions.
- **StatusNotifier:** Passive while `org.nostr.Shell.Signer1` is owned and Active once it vanishes (tested). The symbolic icon is used.

## 5. Coverage gaps vs §9

**`signer/desktop-contract` passes without its dynamic half.** When there is no `DISPLAY` or private bus, the worker exits 77 and `test-desktop-contract.py:54-58` prints "skipped" and returns success. That means:

- windowless `--gapplication-service`;
- hold release on quit;
- action and URI window idempotence;
- Shell-name suppression;
- pause on lock

are all unverified in headless CI while the row shows green. The §9 row asks for "activation services" behaviour, not just static files. **Fix:** register the dynamic worker as its own CTest entry with `SKIP_RETURN_CODE 77`, so a skip is reported as a skip.

Missing tests (each maps to a finding or a §9 requirement):

1. `pause-signing` activated over D-Bus must never send `SetPaused(false)` (H1). The current test asserts the opposite.
2. Pause-on-lock effectiveness after Ctrl+Q or with background disabled, or a test that asserts the disclosed limitation (H2).
3. Service-mode start where G07 reconciles before the portal watch reports; the instance must stay alive while `run-in-background` is on (M1).
4. `RequestBackground` D-Bus error, Response code 1 and Response code 2 → expected state and hold count on native and sandbox (M2).
5. `dbus-activatable` value and `commandline[0]` asserted for the sandbox (M3). Only `commandline[1]` is checked (`test-background.c:63-64`).
6. Stale portal autostart cleanup across a restart (M4).
7. A full enable → disable → enable cycle asserting `get_holds()` at each step, and DENIED followed by a successful request-scoped activation (§9 "portal denial").
8. `bunker:` open during onboarding (M5), and `--new-identity` twice (L3).
9. URI secret never logged. Install a log writer and assert `never-log-this` never appears. Today only the window count is checked (`test-desktop-activation.c:98-103`).
10. Quit with ≥1 pending request: no `Deny`/`ApproveRequest` reaches the fake `Control1`, and notifications are withdrawn. The fixture has no pending requests, so the `g_assert_not_reached` guard is never exercised.
11. `org.nostr.Shell.Signer1` reappearing → Passive again (only the vanish direction is tested).
12. Native entry not written before disclosure (L2); `appstreamcli validate` (L8).

## 6. Recommendations (priority order)

1. **H1:** make `pause-signing` one-way and add a confirmation-gated `resume-signing`; fix the test.
2. **H2:** decide where lock enforcement lives. Either the daemon, or a GUI interim that holds the app, warns on quit, and discloses when the desktop is unsupported. Make the Settings copy honest about the preconditions.
3. **M1:** take a temporary hold from construction until the first portal-watch callback.
4. **M2:** map only an explicit `background=false` (code 0) to `DENIED`; errors and code 2 → `UNAVAILABLE`.
5. **M3 and M4:** send `dbus-activatable=false`; clear portal autostart once per start when it shouldn't be enabled.
6. **M5:** route URI and review activation through the onboarding window when it is present.
7. Low items as convenience allows. Then close the §5 test gaps, starting with the headless skip-as-pass.
