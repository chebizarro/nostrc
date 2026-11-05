# W6 manager role / Nostr Settings hosting — security and authority review

**Verdict: REQUEST CHANGES.** Two high-severity boundary violations and one medium-severity degraded-state defect. Direct Linux manager mutation refusals passed; that does not establish a safe read or presentation boundary.

## Scope and provenance

- Compared `master` **da2e4d62bb41537ad750e24892b415d603cb05e5** (also the merge base) with `origin/grotto/w6-nostr-settings-hosting` / worktree HEAD **b2c16b04d8fdee304ea13b9075348a639242adab**. Both local refs matched the requested pins; no fetch, merge, commit or push.
- Review and report performed in `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-99a5fb5c-grotto-w6-nostr-settings-hosting-ed0833d1`. Initial working tree was clean. This report is the only repository file created.
- Contract: design §b.4 and §c.4–c.6; Q1 means **reads plus cosmetic labels only**, not tiers; Q3 retains Grotto's existing privacy lock, not a Settings authorization mechanism (`docs/designs/grotto-settings-style-ux-2026-11-03.md:232-246,350-369,371-389,914-922`). The current request's prohibition on pending contents takes precedence over the branch's broad “read-only” request allowance.
- **Executed:** fresh Linux/aarch64 container build from a read-only mount of this worktree; real-host/private-bus test; adapter test; supplemental real-daemon authority/content and backend-unavailability probes. Host machine is macOS; this is **local container evidence**, not native GNOME-session, hosted CI, VM acceptance or release qualification.
- No product/test source remediation, dependency installation, hook installation, tracker mutation or keychain operation. Build/probe files were outside the repository.

## Findings, ranked by severity

### H1 — HIGH — Manager can enumerate unpublished pending request contents

**Confirmed: source and real-daemon execution.**

The new closed manager allowlist includes `ListRequests` (`apps/grotto/daemon/grotto_control.c:662-668`). Its dispatch returns the **same complete approver snapshots**, without a role-specific projection (`:108-117,739`).

Those snapshots contain `preview`, caller `reason`/`purpose`, flow information and `event_summary` (`nips/nip55l/src/glib/signer_approval_service.c:353-424`). `build_event_preview()` copies event content for kinds outside its private-kind exclusion list (`:574-598`); even an ordinary kind-1 note is unpublished at this point. MANAGEMENT summaries can also carry the exact unsigned management event, not merely inbox navigation metadata.

**Executed reproduction:** an executable authenticated as manager, with no approver path, submitted an unsigned kind-1 event containing `W6-UNPUBLISHED-CONTENT-REVIEW-SENTINEL`. An event-driven `ApprovalRequested` subscription established the pending boundary. Manager `ListRequests` returned the sentinel in **both** `preview` and `event_summary.preview`, for request `8069da5edac24c6357753d7067430757f4d92b40523b22ab4b826c112fb08c4f`. No approval or publication occurred; all nine decision modes were refused even for that live request.

The shared adapter's public-field projection/credential-key rejection occurs **after** the reply enters the manager process (`apps/grotto/src/grotto-panels-control1-service.c:50-123,182-188`); it cannot repair daemon confidentiality. Legacy `GetApprovalInfo` and `ListPendingRequests` were refused, but this newly accessible route bypasses that read boundary.

**Precondition/impact:** a legitimate or compromised trusted manager process plus any pending content-bearing request. This does **not** let an arbitrary Linux same-UID executable claim manager. It does expose private-before-publication request contents to the manager, contrary to this review's explicit contract. Secret/token text embedded in arbitrary preview/reason fields is a **plausible additional exposure**, not a demonstrated seed/token export.

**Required boundary:** manager inbox reads must be daemon-projected navigation/public metadata, not approver snapshots. Add a real pending-content negative assertion; the existing mount test has no such request.

### H2 — HIGH — “Cosmetic” labels become authoritative-looking approval names

**Confirmed by the production source chain; end-to-end spoofed notification rendering was not executed.**

Manager `SetClientLabel` reaches `grotto_clients_set_label()` (`apps/grotto/daemon/grotto_control.c:780-791`). The store updates only the label, with bounded text and existing-principal validation, not the principal/tier/grants (`apps/grotto/daemon/grotto_clients.c:198-202`). That storage separation is correct, but its downstream use is not:

1. The approval controller loads these labels from `ListClients` (`apps/grotto/src/approval-controller.c:501-525`).
2. `client_name()` returns the label **before inspecting verified identity** (`:100-115`).
3. An unlocked single-request notification uses it as the notification title, without a cosmetic-label qualifier or verified principal (`:165-177`).
4. Review presentation passes the same name to the approval dialog (`:1029,1035-1037`). For a verified local caller, the dialog displays “Verified local app” (`apps/grotto/src/ui/approval_dialog.c:604-628,638-651`) while its headline uses the manager-controlled name, e.g. “Grotto wants to sign a note” (`:725-743`). The true principal is in details (`:918-931`), not a distinct primary verified-name field.

**Precondition/impact:** an existing client record that the manager can relabel, followed by a request/review. A manager can assign another application's name or “Grotto” and influence the owner-facing identity cue, despite lacking approval permission. This is presentation/consent spoofing, **not** a direct change to daemon authorization.

W1's S1 transport wording remains intact: remote NIP-46 callers explicitly have an unverified app name (`approval_dialog.c:609-615`). It does not distinguish a local cosmetic label from the verified app identity, and the notification has no equivalent distinction. These consumers predate this diff; granting the weaker manager label-write authority makes them part of W6's new boundary.

**Required boundary:** render the local label explicitly as cosmetic and keep verified principal/app evidence distinct in reviews and notifications. The mount test's literal markup-label assertion proves text rendering, not identity non-spoofability (`gnome/nostr-settings/tests/test-panel-mount.c:162-169`).

### M1 — MEDIUM — Inventory failure is rendered as an empty, healthy identity inventory

**Confirmed static UI path and real-daemon backend-error execution; failing-backend GTK rendering was not executed.**

`ListIdentities` correctly reports `BackendUnavailable` on inventory failure (`apps/grotto/daemon/grotto_control.c:682-706`). But a non-status read failure only clears that model; it does not preserve an error state or change service availability (`apps/grotto/src/grotto-panels-control1-service.c:182-188`). Availability is determined by the separate successful status/role response (`:144-181`).

With no identity rows and `available == TRUE`, the real Identity panel says **“No Identity Yet”**, suggests creating/importing, and exposes its Add handoff (`gnome/grotto-panels/src/grotto-identity-panel.c:121-139`). The host banner still says security changes open in Grotto because the label capability remains true (`gnome/nostr-settings/src/ui/nss-window.c:175-183`). Thus “inventory unavailable” is indistinguishable from “inventory successfully empty.”

**Executed backend probe:** real daemon on a private bus with no Secret Service activation route and no fixture key: manager `ListIdentities` returned `org.nostr.Grotto.Control1.Error.BackendUnavailable`; subsequent `GetStatus` still reported manager and `set-client-label=true`. This reproduces the inputs to the static rendering defect, not a screenshot of it.

**Impact:** false absence/setup guidance during degradation; no demonstrated secret exposure or unauthorized creation (Add still hands off). Preserve per-model unavailable/error truth rather than interpreting failed reads as successful emptiness.

## 1. Authority boundary and exact permissions

### Principal attestation

**Linux manager attestation is not weaker than the executable approver path.** It requires a PID, non-unattested EXE/systemd-scope shape and the shared `exe_in()` verifier (`nips/nip55l/src/glib/signer_caller.c:337-353,366-394`). The verifier requires same UID, exact absolute executable path, and matching current device/inode. The compiled manager path is `<CMAKE_INSTALL_FULL_BINDIR>/nostr-settings` (`nips/nip55l/CMakeLists.txt:235-239`).

Credentials come from the D-Bus connection, not a supplied app ID; executable facts come from `/proc/<pid>/exe`; liveness is checked using a pidfd or a second bus PID query (`signer_caller.c:441-472,542-619`). A matching bus name, basename, copied binary at another path, or claimed app ID does not qualify. The real-host test executed arbitrary same-UID/name-owner rejection and invalidation after inode replacement (`test-panel-mount.c:197-203,262-270`).

**Answer:** an arbitrary same-UID Linux process cannot simply claim manager through an RPC argument or name ownership. This is executable identity, not a security boundary against arbitrary same-user code injection or a compromised/writable trusted installation; those stronger threats were not tested.

There is **no new UID-only manager fallback**: non-Linux manager returns false (`signer_caller.c:369-379`). The pre-existing approver UID-only compatibility fallback remains (`:383-394,582-592`); do not generalize this Linux separation to it. Test paths are environment-overridable only under `NIP55L_TEST_TRUST_ENV` (`:310-322`); the focused execution deliberately used that supported test route.

### Closed Control1 allowlist

Manager: **GetStatus, ListIdentities, ListClients, ListPermissions, ListRequests, ListActivity, SetClientLabel only** (`grotto_control.c:662-668`). All other current/future methods default to approver-only. Fresh identification precedes dispatch; manager takes precedence over an accidentally overlapping approver configuration (`:709-724`). Neither pause value is allowed to an identified manager. Ordinary non-manager same-UID callers retain the older coarse-status / pause-true exceptions.

| Refused operation | Executed proof |
|---|---|
| ApproveRequest / ApproveFlowRequest | Supplemental probe: all four decision/remember or encrypt/seals boolean combinations, TTL 60, legacy `org.nostr.Signer` interface; `Error.PermissionDenied` |
| DecideRequest modes | All nine: allow-once, allow, allow-24h, deny-once, block-operation, block-app, trust-routine, activate-saved, discard-saved; fake and live request IDs; `Control1.Error.NotAuthorized` |
| ExportIdentity / ExportRawIdentity | Canonical method loop; additionally ExportIdentity with mnemonic format; NotAuthorized |
| SetPaused(false), SetPaused(true) | Both denied |
| SetClientTrust / SetPermissions / RevokeClient | All denied |
| ConfigureBunker / CreatePairing / AcceptConnectURI / CancelPairing / ResetBunkerEndpoint | All denied |
| RotateIdentity / SetDefaultIdentityForLegacy | Both denied |
| Delete identity | Canonical method is **RemoveIdentity**, not DeleteIdentity; denied. Legacy **ClearKey** also refused, with GLib permission-denied encoded as an unmapped remote error |
| Remaining canonical privileged methods | StoreKeyWithMnemonic, ImportIdentity, SignProfileEvent, SelfTestIdentity, CancelIdentitySelfTest, GetIdentityMigration, ClearActivity: denied |

The XML-driven test covers **22** non-allowlisted Control1 methods (`test-panel-mount.c:51-82`; canonical declarations `apps/grotto/data/org.nostr.Grotto.Control1.xml:16-68`). Its generated empty but well-typed inputs prove denial before backend/semantic processing, not successful approver backend execution. The supplement covers the valid modes and legacy interface omitted by that loop. Legacy guards are at `nips/nip55l/src/glib/signer_service_g.c:246-256,297-323,500-511`.

### Read confidentiality

- **ListIdentities:** fixed npub/signability/default/unknown-state fields, no secret values (`grotto_control.c:682-700`).
- **ListClients / ListPermissions / ListActivity:** explicit metadata serializers; no built-in seed, nsec, pairing token or credential field found (`apps/grotto/daemon/grotto_clients.c:276-304`; `nips/nip55l/src/glib/signer_permissions.c:662-694`; `apps/grotto/src/event_history.c:850-856,1023-1035`).
- **GetStatus:** mnemonic-export and other custody capabilities are **booleans describing backend support**, not mnemonic values or permission to export (`grotto_control.c:150-171`). Caller-specific management capabilities are separate (`:670-680`). The probe verified that distinction. Status exposes relay/config/health information, not pairing credentials in the examined serializer.
- **ListRequests:** fails the requested no-contents boundary (H1). Consequently this review **cannot certify that manager reads never include secret material/request contents**. Free-form caller text also prevents treating a credential-key blacklist in the UI adapter as a confidentiality proof.

## 2. Hosting honesty and Grotto-owned handoffs

Positive source/execution evidence:

- Production `main.c:16` calls real `nss_window_new()`, which mounts Identity, Apps, Activity and Remote panels; the relay summary remains separate (`nss-window.c:242-253`; `nss-page-identity.c:1-39`). No duplicate keyring inventory is used by the replacement page.
- Missing daemon: no auto-start from status reads; owner loss cancels snapshots and clears all models/capabilities (`nss-widgets.c:24-28`; adapter `:193-223`). Real-host loss/restart and missing-Grotto toast assertions passed (`test-panel-mount.c:180-195`).
- Wrong/legacy role metadata fails the manager adapter closed, and it masks every command except label even if over-advertised (`adapter:144-176`). Adapter fixture checks passed (`gnome/grotto-panels/tests/test-control1.c:208-222`). Initial role refusal is therefore not silently upgraded to approver.
- Settings supplies **no** security command/profile hooks (`gnome/nostr-settings/src/ui/nss-grotto.c:34-45`); privileged panel commands require such host hooks (`gnome/grotto-panels/src/grotto-panel.c:105-112`). Remote configuration is insensitive and offers handoff; degraded/offline/error state names are real mappings (`grotto-remote-panel.c:18-22,99-119`).
- Handoffs call **org.freedesktop.Application.ActivateAction**, action **open-management**, on Grotto's own `org.nostr.Grotto` object, carrying only section/public target (`nss-grotto.c:13-32`). They do not reimplement backup, approval or permission flows.
- Grotto registers/handles that action; review delegates to its existing review path, other targets navigate (`apps/grotto/src/main_app.c:735-749,873`). Management navigation checks its existing privacy lock; Backup selects an identity and asks the user to choose Backup, not export (`apps/grotto/src/ui/signer-window.c:890-919`). No Settings “unlock” is invented.
- The real-host test captures the production activation wire, **not a real Grotto receiver**. Separate production-action navigation assertions exist (`apps/grotto/tests/test-approval-integration.c:408-424`), but that suite was **not run in this review**.

Limits: M1 breaks degraded inventory honesty. Actual locked-session/locked-Grotto handoff and degraded-bunker rendering were source-inspected, not exercised here. Role loss after an already successful snapshot does not itself emit a daemon signal; prompt UI capability invalidation after a refused command remains unproven (the inode test checks RPC denial, not refreshed GTK capability state).

## 3. Packaging/build and version decisions

- CMake requires the installed GrottoPanels SDK or adds the in-tree shared library, then links `GrottoPanels::Panels` publicly into Settings UI (`gnome/nostr-settings/CMakeLists.txt:14-20,75-93`).
- New Meson uses `grotto-panels-0 >=0.1.0`, the same Control1 adapter/shell/page sources and GTK 4.14/adwaita 1.5 pins (`gnome/nostr-settings/meson.build:2-28`). It installs the same desktop/service/schema assets and offers the same real-host source with an explicit test-daemon option (`:29-47`). Static production-source parity found; **Meson configure/build/test was not rerun here**. Unlike CMake, this file does not register the seven existing core suites, so test registration is not full parity.
- RPM explicitly adds panels BuildRequires/runtime Requires and system-panels configuration (`packaging/rpm/nostr-login.spec:69,648,748`). Debian runtime package naming is documented (`gnome/nostr-settings/README.md:17-20`), but this diff adds no standalone Settings Debian package recipe. No RPM/deb install or generated dependency-metadata verification was executed.
- Version decisions are explicit, not “artifact unchanged”: Grotto **MINOR-class folded into unreleased 0.1.1**; panels **PATCH-class folded into 0.1.0**; Settings **MINOR-class folded into 0.1.0**, with a previously missing manifest row; nip55l internal authorization implementation folded into **0.8.0** (`VERSION_MANIFEST.md:31,58-69`). Existing owner-directed unreleased-wave decisions are recorded; no release/tag columns were advanced.
- Values inspected agree: Grotto CMake/Meson 0.1.1 and deb/RPM 0.1.1; panels CMake/Meson and deb/RPM 0.1.0; Settings CMake/Meson 0.1.0; nip55l `signer_ops.h:28-31` 0.8.0. No version inconsistency found. This report is documentation-only and requires no component bump.

## 4. Checks actually run

Container image: existing `nostrc-grotto-g10-linux:arm64`, Ubuntu 24.04.5/aarch64, GTK **4.14.5**, libadwaita **1.5.0**; network disabled, source mounted read-only at `/src`. All tests used private buses/disposable configuration and fixture keys, not developer stores.

Focused CMake configuration:
```sh
cmake -S /src -B /tmp/w6-review -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_APPS=ON -DBUILD_GNOSTR_APP=OFF -DBUILD_GROUNDHOG=OFF \
  -DBUILD_TESTING=ON -DBUILD_TESTING_FRAMEWORK=OFF -DBUILD_SIGNER_TESTS=OFF \
  -DNIP55L_TEST_TRUST_ENV=ON -DENABLE_NOSTR_SETTINGS=ON -DBUILD_NATIVE_HOST=ON \
  -DGROTTO_WITH_PKCS11=OFF -DGROTTO_WITH_HW_WALLET=OFF \
  -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF -DBUILD_LIBHANAMI=OFF
cmake --build /tmp/w6-review --target test-nostr-settings-panel-mount -j4
ctest --test-dir /tmp/w6-review -R '^nostr-settings/panel-mount$' -V
cmake --build /tmp/w6-review --target nostr-settings test-grotto-panels-control1 -j4
ctest --test-dir /tmp/w6-review -R '^grotto-panels/control1-service$' --output-on-failure
```

| Check | Actual result |
|---|---|
| Focused configure; real daemon, shared panels, host test and Settings executable build | PASS |
| nostr-settings/panel-mount | **1/1 PASS**, 1.71 s; real `nss_window_new`, real daemon, private bus; rows, label signal, 22 forbidden methods plus pause=true, activation capture, missing Grotto, daemon loss/restart, inode replacement |
| grotto-panels/control1-service | **1/1 PASS**, 0.03 s; fixture adapter/role/capability checks, not a real daemon |
| Supplemental authority/content probe | PASS as a diagnostic: 22 Control1 denials, valid decision modes, legacy approvals/reads/ClearKey denied; **content-disclosure reproduction succeeded** (H1, not security acceptance) |
| Supplemental inventory probe | PASS as a diagnostic: real BackendUnavailable with manager status/label capability still true (M1 inputs) |
| git diff --check master..HEAD | PASS |
| Meson / package installation / full affected gate / hosted or desktop acceptance | NOT RUN |

Failures/limits retained rather than reported green:

- Initial broad signer-test configure failed: `apps/grotto/tests/CMakeLists.txt:599` requires absent `xdotool`. Focused reconfigure set **BUILD_SIGNER_TESTS=OFF**, leaving the real Settings mount test enabled. No fake xdotool, guard weakening or claim of signer-suite coverage.
- An initial login-shell container launch exited 1 without a usable test receipt. It is not counted as execution.
- Supplemental probe initially expected the wrong remote error spelling for legacy ClearKey. The operation was denied; the receipt contains GLib's unmapped I/O PermissionDenied code 14. The diagnostic expectation was corrected, not product code.
- First no-key inventory probe allowed standard session activation and obtained a genuinely empty Secret Service inventory. A second probe explicitly removed activation service directories to exercise actual backend unavailability. The first result is not claimed as failure reproduction.
- Xvfb/EGL DRI3 and missing portal diagnostics occurred during the passing host test; no native GPU/portal or visual acceptance is claimed.

Local receipts/probes retained outside the repository:
`/private/tmp/nostrc-w6-security-review-test.log`,
`/private/tmp/nostrc-w6-security-review-adapter.log`,
`/private/tmp/nostrc-w6-security-review-probe.log`,
`/private/tmp/nostrc-w6-security-review-probe-initial.log`,
`/private/tmp/nostrc-w6-security-review-inventory.log`,
`/private/tmp/nostrc-w6-security-review-inventory-initial.log`,
`/private/tmp/nostrc-w6-review-probe.py`,
`/private/tmp/nostrc-w6-inventory-probe.py`.
Supplemental probes authenticate a disposable Python executable via test-only manager configuration; they do not claim to be the installed Settings binary. The unmodified real-host test separately establishes that production host integration.

## 5. Remaining gaps / disposition

1. **H1 and H2 block the requested security boundary**, despite passing direct mutation-denial tests. Reads need a no-content contract and labels need an explicit non-authoritative presentation contract.
2. **M1 blocks degraded-state honesty.** Existing tests do not distinguish unavailable inventory from successful emptiness.
3. No end-to-end manager rename → real review/notification anti-spoof test, no locked-host/degraded-bunker real-host assertion, and no real-host UI assertion after role revocation. These are bounded coverage gaps in the requested behaviors, not a repository-wide test programme.
4. Backend custody/export success, production installed-path/package execution, Meson parity execution, native desktop behavior and release qualification remain outside the executed proof. No arbitrary same-UID manager takeover or direct manager approval/export success was found on Linux.

No fixes were made. No unrelated audits or new delivery prerequisites are proposed.

