# Grotto G12 — focused release-readiness critique

## Context / Scope

- Compared `master` **5ef33513c99f29e60d914dcef3a245fce618f8a0** with pushed `origin/grotto/g12-packaging-ci` **b9f2f92b507fa7ff7b044b2199c9e7d87c92a9b1** (66 changed files). All repository line references below refer to the latter commit.
- Specification: `docs/plans/grotto-signer-ux-2026-10-12.md` §5.11, §8–10 and §12. Limited to promotion evidence, CI, packaging, AppStream, manual handoff, versions/rename closure and the two sanitizer sub-beads. This is not a fresh general security/protocol or UX audit.
- Read-only implementation review. Only this report is added; no source, issue state, commit, push or merge is changed. Concurrent work appeared during review: an **uncommitted** workflow edit making TSan advisory and an untracked `apps/grotto/tests/lsan.supp`. Both are preserved and **excluded** from this commit-scoped verdict; statements about gating/suppressions describe the pushed SHA only.
- **Verdict: REQUEST CHANGES for completed G12 / release readiness.** Keeping **Grotto (Preview) 0.1.0, not promoted**, and leaving G12 open is honest. There is one confirmed native-recipe defect and one smaller evidence-reporting correction. The existing unsatisfied gates are real blockers, not newly discovered proof of production vulnerabilities.

## Findings — severity ranked

### P1 — Confirmed release/G12 blocker: required sanitizer acceptance is genuinely red

**Evidence:** `docs/reviews/grotto-release-readiness-2026-10-14.md:57,83-107`; plan `:491,612-616`; `.github/workflows/grotto-ci.yml:47-53,101-105,120-148`.

The preserved Docker-volume logs substantiate the report:

- `/work/g12-final-asan.log` / `.xml`: **15 selections, two failures**, `signer/approval-integration` and `signer/onboarding`. LSan summaries are **184,165 bytes / 6,148 allocations** and **172,720 / 5,841**, matching the readiness document. Controller/private-bus fixture repairs do not make the remaining GUI exit diagnostics green.
- `/work/g12-tsan-mpmc.log`: approval-service/cache pass, but `signer/bunker` fails and `signer/bunker-e2e` times out; the report names `libgo/src/channel.c:43` (`fiber_waiter_wake_one`). The separate coarse-channel debugger evidence also exists at `/tmp/g12-tsan-gdb.log`.
- Read-only `bd show` confirms **nostrc-u6v55.14.3** and **.14.4** are open and explicitly retain the no-suppression/no-relaxed-assertion obligations.

**Confirmed:** required instrumented runs fail. **Plausible, not established:** that every residual allocation is harmless third-party cache, or that the TSan report is a production race rather than synchronization-annotation interaction. Fontconfig-heavy stacks and two reports naming the same mutex are diagnostic leads, not either exoneration or a race diagnosis.

**Disposition:** under the reviewed §8/§9/§10 contract, both must gate **G12 completion and promotion**, not merely sit in a tracker behind green CI. They need not prevent preserving/merging an explicitly incomplete Preview checkpoint if the owner separately permits that; such a landing is not successful G12 acceptance. An owner-approved advisory TSan policy would require a consistent, explicit contract/evidence change, with sanitizer acceptance still marked unmet; it cannot turn failed TSan into a met promotion gate. The uncommitted advisory edit is not reviewed here.

### P1 — Confirmed release blocker: installed acceptance and hosted green evidence do not yet exist

**Evidence:** readiness `:25-31,61,134-149`; manual acceptance `:3-7,15-24,100-110`; plan `:557,624,639-646`.

The full three-session desktop matrix remains **NOT RUN**. Xvfb, fake portals/Control1, a camera-frame fixture and a package archive cannot establish live GNOME activation, physical camera, keyring cancellation, Shell 46/50, Orca or combined RTL/large-text/narrow-screen behavior. The cited open acceptance beads preserve these boundaries.

For the exact reviewed SHA, the hosted run was still incomplete during review: [Grotto Preview CI run 38659602695](https://github.com/chebizarro/nostrc/actions/runs/38659602695) had the floor-native job in progress and other matrix jobs queued. No green hosted result is inferred. This is a time-bounded observation, not a prediction of the eventual run.

**Disposition:** these remain promotion gates. The readiness document does **not** falsely mark them met. Manual obligations may remain separately tracked while Preview implementation work lands; do not close them or remove Preview based on this review.

### P2 — Confirmed packaging defect: Nix overlay still exports the removed TCP derivation

**Evidence:** `flake.nix:100-101,110-115`, especially `:114`; compare deletion of the old `packages.grotto-daemon-tcp` derivation with retained overlay expression `self.packages.${final.system}.grotto-daemon-tcp`. The packaging documentation says that output was removed (`apps/grotto/packaging/README.md:29-32`).

The overlay still exposes an attribute whose value references a nonexistent package. Forcing `pkgs.grotto-daemon-tcp` through the overlay therefore fails with a missing attribute rather than reflecting the documented removal. This inconsistency is introduced by this branch; master still had the target derivation.

**Recommendation:** remove the stale overlay entry along with the output, not resurrect an unsupported TCP package. **Source-confirmed; Nix evaluation/build was not run.** This does not imply that `#grotto` itself necessarily fails, since Nix attributes are lazy. The packaging correction can fold into the same unreleased 0.1.0 decision.

### P3 — Confirmed evidence omission: the 550-selection Linux smoke run includes four skips

**Evidence:** readiness `:55` describes “550 smoke selections + 1 serial performance selection passed” without naming skips, unlike the explicit macOS accounting at `:52-53`. The preserved `nostrc-grotto-g12-linux` volume's `/work/ctest.log:1105-1108,1255-1259` lists:

- `test_nip5f_tcp`
- `groundhog-nip46-nak`
- `groundhog-nip46-inbox-nak`
- `groundhog-nip46-pair-nak`

**Recommendation:** say **550 selected, four skipped**, list their actual skip reasons, and retain the distinction from the separately green **56-selection signer/nip55l/native-host run with no skips**. “No quarantine used” is true but not equivalent to every selected test having executed. This omission does **not** invalidate the separate signer-floor result or demonstrate a false promotion decision; it understates the smoke run's limitations.

## Promotion-gate honesty — each §10 item

The eight rows in readiness `:21-28` are qualified assessments, not eight unconditional PASS labels. No row was found claiming installed manual acceptance or a quarantined failure as promotion success.

| Gate | Independent evidence check | Assessment |
|---|---|---|
| Local/browser/remote share one gate (`:21`) | Local service submits to `signer_approval_service_submit` (`nips/nip55l/src/glib/signer_service_g.c:149-172`); NIP-5F calls `signer_gate_submit` (`signer_nip5f.c:87`); the native host calls the signer over D-Bus (`apps/grotto/native-host/nm_provider_nip07.c:125-141`); bunker requests use `signer_gate_submit_full` (`apps/grotto/src/bunker_service.c:529-541`). Named approval/contract/native-host/bunker suites exist. | Real source/test evidence, correctly bounded by the open cross-transport coverage obligation. Not an installed-browser certification. |
| No unknown/malformed signing (`:22`) | `/approval/nip46-caller` checks constructor/approver exclusion (`test_signer_approval_service.c:72-81`); validation rejects `{}` (`:512-521`); policy recheck denies changed authority (`:546-557`); headless request fails before custody (`:584-595`). Bunker E2E asserts stale/future/wrong-client rejection (`test-bunker-e2e.c:814-820`) and reply/signature accounting (`:1037-1053`). Registrations exist (`test_signer_approval_service.c:749-772`). | Tests assert the cited negative cases. Constructor/canonicalization tests alone are not a universal malformed-input proof; the report explicitly avoids that claim. AUTH policy remains open. |
| Pairing/permissions survive restart (`:23`) | Exact-kind grant and restarted evaluator assertion (`nips/nip55l/tests/test_signer_permissions.c:40-49`); migration/restart equality and legacy re-sanitization (`:99-140`); real daemon restart, offer invalidation, restored client relay/identity and replay rejection (`test-bunker-e2e.c:839-856`). | Genuine fixture/restart coverage. Guarded transport-key storage is not installed Secret Service custody proof, as the report acknowledges. |
| Pause stops remembered signing (`:24`) | After-backend barrier asserts no delivered result, typed paused error, no resurrection/grant (`test_signer_approval_service.c:418-448`). Queued deny/revoke/tier/expiry tests are registered at `:754-761`; the build enables their `NIP55L_TEST_TRUST_ENV` test seam (`nips/nip55l/CMakeLists.txt:215-221,391-399`). Remote cached success is refused after pause (`test-bunker-e2e.c:832-835`). | Real deterministic evidence, not a sleep-based claim. It does not prove real logind/lock-screen/keyring behavior. |
| Named suites pass on supported floor (`:25`) | Required CTest names exist; preserved final floor XML/log records 56 selections, zero failures/skips, 102.55s. Workflow covers floor/current and separate sanitizer selections. | Native floor evidence is real. Full promotion assessment is **not met** while hosted/sanitizer acceptance is absent/red. Mac skips are not used as Linux evidence. |
| Ubuntu 24.04/26.04 manual evidence (`:26`) | Three platforms, per-step expectations, ownership and all NOT RUN statuses exist in the handoff. | Correctly **NOT SATISFIED**. |
| No inert/unsupported reachable controls (`:27`) | Real production-source GUI test core and UI tests are registered (`apps/grotto/tests/CMakeLists.txt:513-521,585-607`); tests load templates, exercise focus and capture widgets (`test-ui-layout.c:250-265,392-410`). Dormant-feature claims are absent from reviewed metainfo. | Real bounded layout/source evidence. Not every-control live usability certification; the report retains that qualification and owner policy decisions. |
| Metadata/docs describe demonstrated behavior (`:28`) | Metainfo names Preview and states custody limits/signing≠publication (`org.nostr.Grotto.metainfo.xml:6,16-37`); desktop-contract asserts wording, MIME and image references (`test-desktop-contract.py:26-62`). Actual committed PNGs were visually inspected. | Substantially supported, with fixture and sandbox limits explicit. Master screenshot URLs are not yet branch-publication evidence. See AppStream section. |

### Commits and atomic landing evidence

All commit IDs cited in the readiness report and the plan's G00–G13 execution index were resolved as commits and verified as ancestors of **b9f2f92b5**, including `304d1da0`, `64cc2c985`, `ab5794e91`, and the implementation/remediation pairs. No nonexistent or off-branch evidence commit was found. Historical review verdicts are not rewritten into approvals.

Readiness `:35-42` tracks the six atomic requirements without claiming manual certification: G02 service plus adapters; G03 migration/evaluator/revoke; G05 core/header/GLib wrapper/Grotto integration; G10 Blueprint/generated UI/resources; production inventory subtraction in both builds; and G12 About/daemon/package version consumption. The real GUI test core takes production sources, not duplicate lookalike widgets. Alternate Meson evidence is explicitly GUI-only and consumes CMake-built daemon/libraries, not an independently verified daemon build.

## CI fidelity

**Qualified pass for the reviewed workflow's design; not a claim the hosted matrix has passed.**

- **Runner/display fidelity:** `grotto-ci.yml:45-48` selects `ubuntu-24.04` and `ubuntu-latest` native jobs plus floor ASan/UBSan and TSan. `:118-127` uses full signer/nip55l/native-host selection natively and explicit narrower sanitizer sets. `:139-149` checks required registrations, uses `--no-tests=error`, runs under **`dbus-run-session -- xvfb-run`**, serializes tests, preserves pipe failures and rejects CTest skips/Not Run. The real layout worker exits 77 on missing display (`test-ui-layout.c:383-393`); that becomes a rejected CTest skip, not green display acceptance. `ubuntu-latest` is a rolling runner, **not proof of a distinct Ubuntu 26.04 desktop**.
- **Real sanitizer failures gate:** env settings retain leaks and halt on findings (`:49-53`); Debug/global instrumentation is wired in root CMake (`CMakeLists.txt:129-176,288-317`). TSan uses checksum-pinned unmodified private GLib (`scripts/grotto-tsan-runtime.sh:9-21`), vendored nsync instrumentation and runtime-linkage checks (`grotto-ci.yml:88-105,128-137`). The pushed job has no `continue-on-error`. Actual local findings demonstrate that these stages are not synthetic always-green checks. No new suppressions or leak disablement were found.
- **Named quarantine is executed, not excluded:** native baseline step selects `groundhog-nip29-service` / `nostrc-q1974`, `groundhog-mls-service` / `nostrc-pf4gd`, and `groundhog-nip46-pair-nak` / `nostrc-otxhq` (`:162-175`; `scripts/grotto-ci-quarantine.tsv:3-5`). The helper runs CTest first with display/bus and no-empty-selection protection (`scripts/linux-gate-smoke.sh:99-108,260-281`), forbids skips and sanitizer reports, then records exact-name/platform/bead failures (`:283-318`). Unknown failures remain fatal. No quarantines apply to the native signer selection. Mac-only `test_nostr_gtk_widget_churn_leaks` / `nostrc-x6arp` and macOS-ASan `nip55l_dbus_contract` / `nostrc-nfnw` are explicitly registry-scoped, not Linux signer exclusions (`quarantine.tsv:6-8`).
- **Dependencies agree now:** workflow `:64-72`, Dockerfile `:15-24,62-64` and standalone Debian `control:5-25` include the required zbar/QR/GStreamer development packages. Linux CMake makes camera dependencies REQUIRED (`apps/grotto/CMakeLists.txt:84-99`), so missing camera headers do not quietly downgrade the GUI. Debian runtime base/good plugins are explicit (`control:39-40`). The dependency checker requires the signer packages and mirrors CI to Docker (`scripts/check-linux-ci-packages.py:51-55,87-111`). It does not itself parse Debian Build-Depends; current agreement was manually cross-checked, not inferred from its PASS alone.
- **Limits:** required names protect against an empty/missing planned suite, not every future internal subtest being present. The package step builds/lists the deb (`:181-192`); it is not an installed user-session test. Missing upload artifacts only warn (`:213`), but do not neutralize prior test failures.

## Packaging and AppStream

### Native payload

**Deb payload confirmed** by inspection of the preserved `grotto_0.1.0-1_arm64.deb`, not just an install-list assertion. It contains GUI, daemon, native messaging host, both D-Bus activation services, Control1 XML, schema, user unit, both network/headless examples, autostart **template**, canonical desktop/metainfo, eight raster icon sizes and symbolic icon. This agrees with `apps/grotto/packaging/debian/grotto.install:1-16` and the desktop contract. Debian has one binary package stanza, recommends the Shell extension and Secret Service provider (`control:32-44`), and does not enable login startup (`rules:28-29`).

Configured services point at `/usr/bin/grotto --gapplication-service` and `/usr/bin/grotto-daemon`; the autostart template uses the same installed GUI. Both ELF RUNPATHs are **`$ORIGIN/../lib/grotto`**, with private JSON library present. No `/work/src`, `/work/build`, reviewer-home or `/tmp/g12` strings were found in the two stripped executables; inspected installed text assets contain no staging/host path or literal test-key environment assignment. This is a bounded archive inspection, not a universal secret scan or proof of installed activation/attestation.

The base unit remains D-Bus activated/localhost-only, with no ExecReload (`daemon/packaging/systemd/user/grotto-daemon.service:11-19,65-68`); examples ship as documentation, not automatically enabled policy. Actual systemd network/caller behavior remains manual. RPM/Arch/Nix target-native builds/installs are **not demonstrated**, honestly stated in readiness `:59` and packaging README `:35-42`; the Nix overlay defect above remains actionable. The shared Groundhog bundle transition is separately owned by `.15`, not concealed as an independent-version upgrade.

Flatpak and AppImage are clearly experimental/release-blocked. All five §5.11 gates—host caller attestation, trusted approver, Secret-portal custody, activation ownership and browser-host behavior—are spelled out (`apps/grotto/packaging/README.md:48-52`; `flatpak/README.md:8-29`; `appimage/README.md:1-12`). No sandbox launch is presented as acceptance, and no relaxed path/inode trust is prescribed.

### AppStream and screenshots

Metainfo says **Grotto (Preview)**, development **0.1.0**, and describes implemented signing/permissions/pairing/identity/activity paths, not OC-3 hardware/TPM/Secure-Enclave/PKCS#11/multisig/delegation/social-recovery features (`metainfo.xml:6,16-37,68-79`). It states the **same-user-malware limitation**, explicit key-management exception and **signing is not publication**. Installed-desktop/accessibility acceptance remains explicitly pending. No physical-camera or live Shell acceptance claim is smuggled into that copy.

All 12 PNGs were visually inspected. They show the actual Home/Client/Permissions/Connect/Settings templates and contain no visible seed, nsec, pairing QR/token, private payload or operator identity. They are **not literally identity-free**: fixture labels and a public npub appear, including full fixture npub on dark Permissions. Source derives that npub from the fixed disposable scalar 1 (`test-ui-layout.c:398`), supplies `https://jank.example` and synthetic count 117 (`:119-125`), and captures real widgets (`:260-265`). Thus this is not a real-identity/private-key disclosure; if “no identities/keys” is intended to forbid even synthetic public identifiers, these screenshots do not meet that stricter publication requirement.

Provenance explicitly distinguishes fixtures from installed/manual captures (`data/screenshots/README.md:3-12,21-31`). AppStream references only three existing PNGs, checked by `test-desktop-contract.py:55-59`; remote URLs point to **master**, so they are not yet demonstrated to resolve for this pushed branch. That landing dependency is already disclosed, not falsely reported complete. Fixture screenshots do not satisfy manual D4's onboarding/review/backup/degraded screenshots.

## Manual acceptance coverage

**Full §9 matrix covered as an unrun handoff**, with per-row action, expected result and bead mapping:

- Three clean-session columns and environment/evidence record: `grotto-manual-acceptance.md:11-32`.
- First-request activation, notification privacy/DND/denial, portal/autostart, close/Quit/logout/upgrade, keyring cancellation: **A1–A9**, `:46-54`.
- Separate local identities, browser origins, interleaved remote clients, pairing/replay/ack failures, physical camera, revoke/expiry/pause/resume, unit/network policy, relay outage/live configuration, headless and migration: **B1–B10**, `:60-69`.
- Live Shell 46/50, AppIndicator, owner/signal lifecycle and keyboard/Orca: **C1–C5**, `:73-82`.
- Keyboard-only, Orca, RTL + 200% text + 360px combinations, light/dark and degraded screenshots: **D1–D4**, `:86-98`.

Owners `.14.1`, `.9.1`, `.13.1`, `.7.1` are named at `:34-40`; no row is marked executed. This is adequate coverage documentation, **not actual acceptance**. Cross-transport coverage, AUTH product policy and authenticated upstream filing remain explicitly separate at `:100-110`.

## Version / manifest and rename close-out

**Qualified pass; no stray version bump found in this branch.**

- Grotto authoritative CMake/Meson values remain **0.1.0** (`apps/grotto/CMakeLists.txt:4`, `meson.build:8`); About and daemon consume the build version. Manifest lists duplicate package sources including Arch/Nix (`VERSION_MANIFEST.md:23`), while desktop-contract checks the principal values (`test-desktop-contract.py:78-88`). Checked macOS/Windows defaults also remain 0.1.0. No release-column change is made.
- The G00–G13 ledger records decisions for Grotto, nip55l, NIP-46, nostr-gobject, libnostr, Groundhog, native host and Shell extension, including no-bump/test-only effects and affected gnostr/nostr-seal fixtures (`VERSION_MANIFEST.md:39-83`). G12 separately records the nip55l hardened-build comparison fix, test/runtime seams, shared recipe dependencies and other-component no-bump decisions (`:44-47`). G13's **single 0.4.0 → 0.5.0 MINOR** predates this diff and is not another G12 bump (`:39-40`).
- Local and fresh read-only origin tag queries found no `grotto-v*`, `nip55l-v0.7.0*`, `nostr-gobject-v2.2.2*` or `nip46-v0.1.0*` release tags. The recorded folding into unreleased versions is not contradicted by these checks. This is not permission to fold after a release is tagged.
- **R3 `nostrc-0ulk` closure is justified for identity delivery:** painted-icon commit exists, Preview desktop/metainfo/About are present, real-template screenshots exist and build-derived version is checked. **Rename epic `nostrc-8otj` closure is justified for its bounded R1–R4 scope**, not for release promotion. Read-only tracker checks confirm the R1/R2/R4 closures; mechanical rename commit `7dfb8d433` exists. Migration guard tests cover old/new/both/neither/symlink cases (`test-legacy-dirs.c:112-116`). Preserved wire/custody identifiers and internal Gnostr types are deliberate compatibility choices, not incomplete product branding (`docs/designs/grotto-rename-plan-2026-10-05.md:57-79`).

## Verification performed / Recommendations

Performed during this review:

- Resolved cited commits and checked ancestry; inspected pinned diff, test registrations/assertions, preserved host/Docker logs and the real deb; visually inspected all screenshots; queried relevant beads read-only and checked exact release tags locally/on origin.
- PASS: actionlint on a copy of the **pushed** workflow; dependency checker plus self-test; all five quarantine counterfactuals (known failure executes/records; unknown/ASan/UBSan/TSan fail); old-name guard.
- PASS: existing-build `ctest -R '^signer/(desktop-contract|blueprint-drift|legacy-dirs)$' --no-tests=error` — **four selections including fixture**. On macOS the desktop contract is static/headless and is **not** Linux GUI activation acceptance.
- Independently found the preserved 56-selection native-floor pass, matching GUI leak counts, TSan failure/timeout, successful alternate Meson link and **83** Shell unit checks; Groundhog sanitizer log reports **65** passed selections. The initial default build and broad static-check claims were not all independently rerun. Local logs are real corroboration but not immutable SHA-attested hosted results.

Before declaring completed G12: resolve the required sanitizer blockers under the approved contract, remove the dangling Nix overlay export, and correct Linux smoke skip accounting. Before promotion: obtain successful exact-SHA hosted required jobs, execute/attach the full installed matrix and owner policy/promotion decisions, and verify public screenshot availability. Keep unbuilt distro recipes, sandbox gates and shared-bundle transition explicitly bounded. No implementation changes or new scope are requested by this report.

**REQUEST CHANGES — release/G12 acceptance, not an allegation of dishonest Preview promotion.**
