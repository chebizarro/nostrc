# App display names: UX, caller identity and boundary review

Date: 2026-11-06. Review task: `nostrc-l0936`; implementation: `nostrc-u6v55.37`.

## Boundary and verdict

Reviewed exactly `git diff ca1a0b746...c2804d1dd` on `grotto/app-display-names`:

- Base: `ca1a0b746`; reviewed HEAD: `c2804d1dd1123a9374291b72a5364c04660ea49a`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-f2fe1631-grotto-app-display-names-19fc9c59`.
- Scope: panels-side GIO resolver; Connected Apps, Activity, app detail, pending rows and approval notifications; additive ABI, translations, Blueprint drift and the shared Nostr Settings host. No product source, tests, catalogs, templates or version declarations were changed by this review. Diagnostic copies and fixtures stayed outside tracked source.

**Verdict:** the requested focused checks pass and policy remains principal-keyed, but this is **not merge-ready unchanged**. Spoofable desktop metadata is presented alongside an unqualified verified-app claim, website callers lose their origin in notification titles, synchronous whole-catalog enumeration sits in row/filter callbacks, and notifications never receive the resolved icon. These are finite presentation/attribution corrections, not grounds for a new permission policy or a broader qualification programme.

Evidence labels: **confirmed/static** means traced through production callers; **confirmed/executed** means run during this review. S2 denotes security-relevant attribution or responsiveness defects, not a demonstrated authorization bypass; S3 denotes missing presentation functionality. Paths and lines refer to the reviewed HEAD. Private-bus/container evidence is not native desktop, hosted CI, owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed and static — Blocker: spoofable desktop names inherit the verified-app presentation

**Where:** `gnome/grotto-panels/src/grotto-app-info.c:25-34`; production consumer `apps/grotto/src/approval-controller.c:155-173,1094-1095,1104-1105`; unchanged verification wording at `apps/grotto/src/ui/approval_dialog.c:737-762,795-796,884-898`.

The resolver accepts a desktop ID without checking its executable, and accepts any absolute caller executable whose basename equals a desktop entry's bare `Exec` command. Neither comparison proves the application's desktop identity. The controller passes the resulting name into the approval headline; the dialog still labels a verified local executable as “Verified local app”, with no qualification that the newly resolved name/icon is unverified desktop metadata. `verified` proves the daemon's principal identification, not this name mapping. A comment in the resolver/public header does not communicate that distinction to the person granting access.

**Failure scenario:** a desktop entry named `Trusted Desktop App` has `Exec=true`; an unrelated caller runs `/tmp/review-other/true`. The resolver returns the trusted name and icon for `exe:/tmp/review-other/true`. A second diagnostic principal, `app:org.review.Trusted;exe:/tmp/review-other/unrelated`, also receives that name solely from the ID. The latter shape is possible because the systemd scope's app ID is process-chosen; production deliberately includes the executable in its principal for that reason (`nips/nip55l/src/glib/signer_caller.c:209-213,461-472`). It is not Flatpak attestation.

An isolated test-fixture copy linked to the **unchanged production approval-controller objects and panels DSO** delivered `exe:/tmp/review-other/true` through reconciliation and the notification backend: the actual title was `Trusted Desktop App`. The subsequent approval-dialog headline/“Verified local app” combination is source-confirmed, not a live dialog exploit reproduction. Raw principal details and principal-keyed permissions still prevent automatic grant inheritance; they do not make the prominent brand assertion trustworthy. Resolve bare commands to the actual executable before associating them, and visibly distinguish cosmetic names from verified principal evidence where a desktop ID is not attested. Preserve the existing approval decisions and policy keys.

### F2 — S2, confirmed/executed and static — Should-fix: website callers become the bridge app in notifications and pending titles

**Where:** `apps/grotto/src/approval-controller.c:169-173,223-229,1221-1231`; resolver fallback `gnome/grotto-panels/src/grotto-app-info.c:27-34`; origin construction `nips/nip55l/src/glib/signer_caller.c:398-409`.

`signer_caller_for_origin()` retains the bridge's executable while replacing the principal/app ID with the website origin. Previously `verified_client_name()` returned that app ID. It now synthesizes `app:<origin>;exe:<bridge>` and asks a desktop resolver to interpret it. No desktop ID matches an HTTPS origin, so executable matching wins; without a desktop match, the bridge basename still wins over the origin. The function has no website/`via` branch comparable to its NIP-46 privacy branch.

**Failure scenario:** `https://first.example` and `https://second.example` use the same permitted browser bridge. Both notifications identify the bridge's desktop name (or executable basename), not the requesting website. The host pending rows likewise use that name and only a countdown as subtitle, so same-operation requests no longer carry their formerly distinct origins there. Review retains the origin in its browser-provenance text/Details, and decisions remain request-ID-based; this is lost attribution before opening review, not crossed permission records.

Two diagnostic fixture cases used the real production controller with `principal_kind=website`, distinct HTTPS principals/app IDs, `via=exe:/usr/bin/true` and the same executable. The captured notification title was `Trusted Desktop App` in **both** cases. These are synthetic bridge snapshots delivered through the actual controller, not an end-to-end browser-extension run. Preserve website-origin attribution instead of routing that caller type through local desktop executable fallback; keep distinguishing principal/origin information visible in the pending row.

### F3 — S2, confirmed/executed and static — Should-fix: every row/filter lookup synchronously enumerates the desktop catalog

**Where:** `gnome/grotto-panels/src/grotto-app-info.c:22-40`; `gnome/grotto-panels/src/grotto-apps-panel.c:53-66,115-116`; `gnome/grotto-panels/src/grotto-activity-panel.c:27-35,42,72`; detail callback `gnome/grotto-panels/src/grotto-app-detail.c:545,564-565`.

Every resolver call obtains and walks the complete `GAppInfo` list, even for a custom label, an unmatched/remote principal or an Activity filter with empty search text. Connected Apps rebuilds all rows on model changes; Activity calls the resolver both while filtering each record and while binding visible rows, and refilters on search edits. These are synchronous GTK/main-context callers, without a cached index or worker boundary. GIO may cache underlying directory data: this finding is **not** a claim that every call physically reopens every desktop file. Its API nevertheless returns a newly allocated list of application references ([GIO documentation](https://docs.gtk.org/gio/type_func.AppInfo.get_all.html)).

**Failure scenario:** a user with a populated application catalog opens/refilters a long Activity list or receives an Apps snapshot; the UI thread repeatedly enumerates the same catalog before processing input. A bounded probe linked to the production DSO interposed only `g_app_info_get_all` to count calls. With 250 additional valid local desktop entries, 200 warm resolver calls made **200 enumerations and took 681.032 ms** (one-entry fixture: 2.486 ms). This is a synthetic workload demonstrating synchronous cost, not a measured production-window freeze or a universal latency prediction. Cache/index the cosmetic metadata outside per-row/filter work and invalidate on desktop-catalog changes; do not replace this with polling or expand the task into benchmarking.

### F4 — S3, confirmed/static and executed backend observation — Missing: the sign-request notification never receives the resolved icon

**Where:** `apps/grotto/src/approval-controller.c:223-235`; fixture coverage `apps/grotto/tests/test-approval-controller.c:471-480,553-556`.

The notification path explicitly passes `NULL` for the resolver's icon output and never calls `g_notification_set_icon()`. Other new consumers do use the returned `GIcon`. Thus the requested app-name-plus-icon notification is only name-enabled; the shell may fall back to Grotto's own icon. The existing notification tests capture title/body/actions but do not assert the caller icon.

**Failure scenario:** an unlocked local sign request resolves to an installed desktop entry with a distinct icon, but its notification carries no caller icon. The actual freedesktop `Notify` calls in all three controller diagnostics had an empty `app_icon` argument. Together with the absence of any icon setter, this confirms the missing plumbing, not a desktop-shell rendering test. Pass the resolved icon through [GNotification's icon API](https://docs.gtk.org/gio/method.Notification.set_icon.html) only where the existing lock/verification/privacy rules permit it; do not expose a caller icon on the locked generic notification or attest a spoofable identity through an icon.

## Owner-policy and regression checks

- **No resolved-name authority:** every production reference to the new helper is presentation or Activity text matching. Connected Apps stores `m->id` as row `principal` (`grotto-apps-panel.c:80-81`); detail locates snapshots by `self->principal` (`grotto-app-detail.c:512-514`). Set-label/trust/revoke/permission RPCs serialize `args->principal`, not the resolved name (`apps/grotto/src/grotto-panels-control1-service.c:309-333`). Approval dispatch uses the request ID (`approval-controller.c:855-861,973-976`). No grant, deny or pairing path was changed to key on a display name. No automatic authorization bypass was found.
- **Same-name stored apps:** Connected Apps now retains the raw ID in its two-line subtitle (`grotto-apps-panel.c:75-77`); Activity retains the principal beneath outcome/time (`grotto-activity-panel.c:79-80`); detail retains Verified As/Fingerprint (`grotto-app-detail.c:549-550`). Different principals sharing a desktop name remain separate records and distinguishable in those views. The management-widget test executed these principal-preservation assertions for one fixture, not a two-principal collision fixture. Pending/browser attribution is the exception in F2.
- **Unattested/remote safeguards:** the controller rejects `verified=false`, `principal_kind=claimed` and `claimed:` before resolution, and keeps NIP-46 generic (`approval-controller.c:159-168`). The daemon separately emits `app_id` and `claimed_app_id`, and derives verified from its caller object (`signer_approval_service.c:375-378,414-415`); caller-supplied `claimed_app_id` is not fed to the resolver. The requested controller suite passed its unverified, locked, NIP-46 and desktop cases. These guards do not attest desktop scope names or basename matches (F1).
- **Panels/host boundary:** the additive function changes no public structure, existing ABI or command contract (`include/grotto-panels-0/grotto-panels.h:17-22`). The only added I/O-shaped dependency is cosmetic GAppInfo lookup; no signer policy, secret store, launch, generic network or relay API was added. No principal-derived network request was found in the resolver/callers; this is static inspection, not packet-capture proof. No desktop entry was launched by the probes. Both no-secrets and Control1 checks passed.
- **Nostr Settings:** the unchanged host constructs the same Apps/Activity panel classes and attaches its host adapter (`gnome/nostr-settings/src/ui/nss-window.c:245-249`). It therefore receives the stored-app names/icons/principal subtitles and the same resolver cost, not a forked resolver or policy evaluator. This host wiring was source-checked; its executable/installed SDK consumer was not separately built or launched during this review.
- **HIG/plain text:** familiar app names over technical principals follow the [HIG terminology guidance](https://developer.gnome.org/hig/guidelines/writing-style.html#terminology); retaining the technical identity as secondary text is appropriate. Row markup is disabled, Activity uses plain label text, and app detail does not interpret the name as markup. The new strings are gettext-marked. No new wording/capitalization violation was identified independently of attribution findings. Passing layout/a11y fixtures is not an Orca or native-desktop review.
- **Blueprint/catalogs:** no `.blp`/`.ui` pair changes in the scoped diff. The pinned 0.22.2 drift check passed for production templates, including panels. `msgfmt --check --check-format` passed for all five Grotto catalogs and panels Spanish; no fuzzy entries in any of those catalogs or either POT. This does not assert that all translations are filled in.

## Verification actually run

Linux image: `nostrc-linux-ci:arm64`, image ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; `aarch64`, GTK 4.14.5, libadwaita 1.5.0, Blueprint 0.22.2. This worktree was mounted **read-only** at `/src`; a fresh `/build` lived only inside task-owned containers. `xdotool` was installed inside the disposable root container; builds/tests ran as `ci`. No developer keychain commands were run.

Configuration: `cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_APPS=ON -DBUILD_GNOSTR_APP=OFF -DBUILD_GROUNDHOG=OFF -DBUILD_TESTING=ON -DBUILD_SIGNER_TESTS=ON -DBUILD_NATIVE_HOST=ON -DNIP55L_SECRET_BACKEND=libsecret -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF -DENABLE_TCP_IPC=ON -DNIP55L_TEST_TRUST_ENV=ON`.

| Command/check | Actual result |
|---|---|
| `scripts/install-hooks.sh`; `bd onboard`; `bd prime` | Completed; review tracked/claimed as `nostrc-l0936` |
| `cmake --build /build --parallel 4 --target grotto test-grotto-panels-smoke test-grotto-panels-admission test-grotto-panels-control1 test-approval-controller test-approval-integration test-ui-layout` | PASS, fresh production Grotto/panels and requested test prerequisites; build parallelism is not parallel test execution |
| `xvfb-run -a ctest --test-dir /build -j1 -R "^(grotto-panels/(management-panels\|no-secrets\|control1-service)\|signer/(approval-controller\|approval-integration\|ui-layout\|ui-accessibility))$" --output-on-failure -V` | PASS, **8/8 including required home fixture**, 151.11 s, serial; seven explicitly requested checks all executed |
| Individual CTest results | management-panels 0.53 s; no-secrets 4.03 s; control1-service 0.02 s; home fixture 0.05 s; approval-controller 16.21 s; approval-integration 12.08 s; ui-layout 58.86 s; ui-accessibility 59.31 s |
| `python3 /src/apps/grotto/tests/test-blueprint-drift.py /src/apps/grotto` | PASS, all production templates and generated kind catalog |
| `msgfmt --check --check-format -o /dev/null <catalog>` for every Grotto/panels `.po`; fuzzy-marker scan of `.po`/`.pot` | PASS, six catalogs; no fuzzy entries |
| Resolver diagnostic linked to fresh production panels DSO | Confirmed the spoof/name-collision mappings and the 200 synchronous enumerations in F1–F3; not a product test addition |
| Isolated notification fixture linked to unchanged production controller objects/DSO, serial `/review/browser-first`, `/review/browser-second`, `/review/basename-spoof` | Three diagnostic cases executed; all observed `Trusted Desktop App` and empty notification icon, confirming F1/F2/F4. Their assertions deliberately capture the defect; their passing status does not mean correct product behavior |
| `git diff --check ca1a0b746...c2804d1dd` | PASS |

Receipts are retained at the worktree Git directory's `app-name-review/` (`container-command.json`, `equipped-run.log`, `container-exit.txt`, resolver/controller diagnostic scripts, sources and logs). The first container dependency install failed because the image defaults to non-root; the equipped run explicitly used root for apt, then `ci` for build/tests. The first resolver diagnostic compile used the wrong generated-header directory; it was corrected to `/build/apps/grotto/grotto-panels/generated`. An initial controller-diagnostic setup had a quoting error before compilation. These failed setup receipts remain alongside the successful executions; no production failure was suppressed or retried to green. Existing GLib environment-thread-safety, Xvfb/EGL and narrow-layout Adwaita warnings remain in the passing test log.

No full suite, broad/affected-gate execution, sanitizer run, Meson build, installed SDK acceptance, native desktop/Orca, browser-extension end-to-end, live relay/keyring or owner acceptance was run. `grotto-panels/smoke-host` was deliberately excluded per the accepted baseline `nostrc-qdgge`; its binary was built only to execute the separately selected management-widget case. Task-owned containers/image were removed after preserving the compact receipts; the unmerged worktree/branch is retained for coordinator integration.

## Version and delivery decision

This report is documentation-only: **no bump** for Grotto, grotto-panels or Nostr Settings. Per the owner's explicit unreleased-version direction, the reviewed implementation retains panels **0.1.0** and Grotto **0.1.1**. Their CMake/Meson declarations agree, and `VERSION_MANIFEST.md:34-35` records the additive resolver folded into unreleased panels 0.1.0. The public addition changes no existing structures or SONAME/ABI generation. No version source, release column or tag was changed by this review. Nostr Settings has no separate implementation diff here.

## Merge recommendation

**Request changes before merging unchanged.** Keep principal-keyed authorization and the names/icons/principal-secondary layout. Correct the spoofable-name verification presentation (F1), preserve website attribution (F2), move repeated desktop enumeration out of UI row/filter work (F3), and wire the requested unlocked notification icon without weakening privacy (F4). Rerun only the directly affected checks and targeted attribution/collision cases. The seven requested checks are green; they do not cover or negate these source/diagnostic-confirmed gaps. No fixes were made.
