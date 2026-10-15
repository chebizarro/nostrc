# Grotto release readiness — G12, 2026-10-14

**Decision: keep Grotto (Preview) 0.1.0. NOT PROMOTED.** G12 delivers CI,
packaging, truthful metadata and an exact manual-acceptance handoff; it does not
claim an installed desktop was available. Removing Preview is a separate owner
decision after all final gates, not an automatic consequence of G00–G13 delivery.
No release tag, merge or release publication is authorized by this report.

Scope: `grotto/g12-packaging-ci`, base `5ef33513c` (G00–G11 + G13 integration),
plus G12 implementation checkpoint `304d1da0` and its evidence follow-up. G12
remains **OPEN**, blocked on `.14.3` / `.14.4`, not complete or promoted.
Independent review reports under `docs/reviews/` are
commit-scoped critiques; their original REQUEST CHANGES verdicts are not silently
rewritten as approvals. Follow-up commits are listed in the plan §10 execution
index. This report is a release-evidence evaluation, not that independent critique.

## Plan §10 final promotion gate, item by item

| Gate | Tree/test evidence | Assessment |
|---|---|---|
| Local, browser and remote requests use the common gate | G02 `9dcfd6e19`/`47a50253c`, G05 `3c3cbcfb8`/`2dd7468d5`; `nip55l_approval_service`, `nip55l_dbus_contract`, native-host E2E and real `signer/bunker-e2e`/`bunker-headless`. G11 `823925632`/`295a029b4` uses context on that path, not a separate policy engine. | Automated/source evidence; not a claim every real browser/NIP-5F transport permutation was tested. `nostrc-ew9qv` remains open for cross-transport coverage. |
| No unknown client or malformed request can sign | Approval-service `/approval/nip46-caller`, `/approval/validation-deadline`, `/approval/canonical-event-scope`, `/approval/policy-recheck`, `/approval/remote-headless`; bunker E2E wrong-client/stale/future/pause/revoke reply/signature accounting (`test-bunker-e2e.c:1037-1053`). Permissions/trust-tier suites assert exact-kind/deny/required-confirmation rules. | Specific negative cases proven, not a universal proof over all malformed input. No allow-all fallback added. Supported credential-bearing AUTH path policy still needs the owner (`nostrc-z6ps9`). |
| Pairing and permissions survive restart as specified | G03 `09749959f`/`7d3a98794`, G06 `26550a440`/`718ca898c`; `signer/permissions`, real `signer/bunker-e2e`, `signer/bunker-headless`; pending-ack admission and replay metadata persisted distinctly. | Automated fixture/restart evidence. Native Secret Service transport-key round-trip/non-enumeration and full installed-unit lifecycle are not replaced by a guarded file backend; manual .7.1/.14.1 remain open. |
| Pause actually stops remembered signing | Approval-service `/approval/pause-after-backend`, `/approval/deny-queued-execution`, `/approval/revoke-queued-scope`, `/approval/tier-lowered-queued-revision`, `/approval/expiry-queued-revision`; bunker E2E signature multiplicity; G08 `c48ebece0`, G11 invalidation `295a029b4`. | Deterministic queued/backend-boundary and remote evidence. Real logind/lock/unlock/confirmation and keyring prompt cancellation remain in the manual matrix. |
| All named signer suites pass on the supported floor | New `grotto-ci.yml` runs signer/*, nip55l and native-host suites on ubuntu-24.04 and ubuntu-latest; asserts required registrations/no skipped tests, pinned Blueprint 0.22.2, ASAN+UBSAN and separate TSAN selection. Exact local outcomes below. | Must use actual runs, not workflow presence, as green evidence. Hosted floor/current/sanitizer run links are required before promotion; ubuntu-latest is not a claim of Ubuntu 26.04 acceptance. |
| Ubuntu 24.04/26.04 manual evidence exists | `docs/testing/grotto-manual-acceptance.md` enumerates clean 24.04 stock GNOME, 24.04 Ubuntu session and 26.04, each step/expected result and owning bead. | **NOT RUN / NOT SATISFIED.** No real desktop. .14.1 full matrix, .9.1 stock-GNOME windowless first request, .7.1 camera/native unit, .13.1 GNOME 46/50 are open. |
| No reachable control is inert or advertises an unimplemented capability | G10 `3f80ac819`/`5ef33513c`: real templates/source inventories in both builds, `signer/ui-layout`, `ui-accessibility`, `blueprint-drift`, home/identity-model suites. Capability/version-skew UI is read-only when unavailable; hardware/multisig/delegation/social recovery excluded from production claims. | Bounded source/layout evidence, not blanket manual certification of every control. Physical camera, Orca, keyboard-only, RTL, 200% text/360px and live Shell remain NOT RUN. Medium/required-kind final membership still needs explicit owner sign-off. |
| Metadata/docs describe demonstrated behavior | Canonical metainfo/Desktop/About say Preview; no unsupported hardware claims; explicit same-user-malware limit, key-management caveat and signing≠publication. Real-template fixture captures committed under apps/grotto/data/screenshots; desktop-contract checks referenced PNGs. Deployment removes false production-ready/TCP/system-bus promises and documents migration/network/headless policy. | Source/automated evidence, with manual limitations stated. Screenshot remote URLs resolve only after landing on master. Flatpak/AppImage remain experimental and release-blocked, not demonstrated native equivalents. |

**Overall gate is not satisfied** even when all automated checks are green:
manual evidence and owner decisions cannot be manufactured by CI or source review.

## Atomic landing requirements

| Requirement | Evidence |
|---|---|
| Shared extraction + existing local/socket adaptations | G02 commits above; preserved legacy XML/contract plus transport-neutral submit/cancel, nip55l contract and approval-service tests |
| Scoped migration/evaluation + legacy revoke | G03 implementation/remediation together; permissions/trust-tiers and approval-service scoped-remember/revoke/write-failure/queued invalidation tests |
| Core async API + wrappers + Grotto adapter | G05 core/header/wrapper/adapter commits together; real loopback daemon/client tests, account key distinct from transport key |
| Blueprint + regenerated UI + resources | G10 implementation/fixes; pinned drift check + real layout tests, no hand-edited generated UI in G12 |
| Navigation/source subtraction in both builds | G10 source/resource inventories; G12 CMake and alternate Meson GUI build checks (Meson consumes CMake-built libraries/daemon) |
| Version consolidation with About/daemon/package use | G12 CMake GROTTO_VERSION and Meson project version feed About/daemon; desktop-contract checks 0.1.0 metadata/Debian/RPM/Arch agreement, manifest maintained in the same change |

## Automated and packaging evidence

Runs are serial on this host; no concurrent agent/build workload was requested.
Local macOS uses the supplied toolchain environment and explicit libsecret /
Secret Service backend options, never a developer keychain test vault.

| Gate | Result / scope |
|---|---|
| macOS initial default build and full CTest | PASS, 435 functional selections + 1 serial performance selection; 5 explicit platform/config skips (test_nip5f_tcp, signer/qr-scanner, signer/onboarding, signer/ui-layout, signer/ui-accessibility). This initial run predates the final G12 version/copy assertions and is not their final gate. |
| Final macOS build/CTest | PASS: fresh build tree (initial build moved to `/tmp/g12-macos-initial-build`), all default targets built; 435 functional selections + 1 serial performance selection passed, no quarantine used. Same five explicit platform/configuration skips as above. Logs `/tmp/g12-macos-final-{configure,build,ctest}.log`. |
| Stock Ubuntu 24.04 real layout/captures | PASS, signer/ui-layout + fixture, GTK 4.14.5+ds-0ubuntu0.10 / Adwaita 1.5.0-1ubuntu2 / GLib 2.80.0, arm64. No patched GTK. See screenshot provenance README. |
| scripts/linux-gate.sh | Final PASS after fixture corrections: all default targets + Groundhog built; 550 smoke selections + 1 serial performance selection passed, no quarantine used (2m55s). Log `/tmp/g12-linux-final.log`; full CTest output in the G12 volume `/work/ctest.log`. This is the smoke/perf subset, not every slow/known unbuilt gnostr test. |
| Full floor signer/nip55l/native-host selection | Final PASS: 56 selections, no CTest skips (102.55s). Includes crypto/D-Bus suites omitted from smoke. Logs/XML `/work/g12-final-signer.{log,xml}` in the G12 volume. |
| Signer ASAN+UBSAN / TSAN | **NOT GREEN.** Final ASAN+UBSAN+LSan: 15 selections, 13 pass, 2 fail (`signer/approval-integration`, `signer/onboarding`), no skips. Controller/private-bus ownership corrections pass; GUI exit-time allocations remain blocking (`nostrc-u6v55.14.3`). Instrumented GLib/nsync TSan passes approval-service/cache; the coarse TSan-only channel fallback stalls rendezvous, while `GO_TSAN_MPMC_SLOTS=ON` exposes a channel waiter/annotation report during cancellation (`.14.4`). Bunker/e2e fail; neither a confirmed production-race diagnosis nor green sanitizer acceptance is claimed. No sanitizer report is quarantined or suppressed. |
| Existing Groundhog sanitizer pattern | PASS: `scripts/linux-gate.sh --sanitizers` built Groundhog/required targets; all 1103 source inputs covered by SANITIZER_BUILD_PATHS; 65 CTest selections passed (62 required names plus fixtures), serial, 7m17s. `ab5794e91` required-test-set checking preserved. Existing third-party libsecret suppression unchanged; no G12 suppression added. Log `/tmp/g12-groundhog-sanitizers.log`. |
| Native package build / staged install | Final PASS after disabling unrelated libhanami configuration: real distro-hardening `dpkg-buildpackage` produced one GUI+daemon `grotto_0.1.0-1_arm64.deb` (plus optional dbgsym), dependencies/recommendations and contents inspected; desktop-contract + fixture rerun passes. Log `/work/g12-final-deb.log`, listing `/work/g12-final-deb-contents.txt`. Fixed GCC Release comparison diagnostic without weakening flags (`.14.2`). Alternate Meson GUI build passed; private-bus Shell unit tests passed 83 checks. RPM/Arch/Nix recipes were not target-native built or installed. |
| Static/CI contract checks | Final PASS: actionlint, Linux dependency checker/self-test, quarantine counterfactuals (known failure executes/records, unknown failure and ASAN/UBSAN/TSAN reports block), unsequenced-args, 46 GUI GDK frame sources, 21 privacy rules, Grotto name guard, shell syntax, read-only macOS keychain check and `git diff --check`. |
| Hosted floor/current CI | New workflow will run on the pushed branch; no successful hosted run is claimed by this document yet. |

### Named quarantines are not disabled tests

`scripts/grotto-ci-quarantine.tsv` is an opt-in exact-name/platform/bead registry.
Every selected test executes with unchanged assertions; a known non-sanitizer
failure keeps its entire output and is recorded in `gate-history/quarantined.tsv`.
The gate explicitly says green **with named quarantine**, not all-tests pass.
Unknown failures, missing/skipped execution and sanitizer reports cannot be
accepted through this registry. The native signer CI selection itself has **no**
quarantines; baseline Groundhog tests run in their own named evidence step.

- Linux `groundhog-nip29-service`: `nostrc-q1974`.
- Linux `groundhog-mls-service`: `nostrc-pf4gd`.
- Linux `groundhog-nip46-pair-nak`: `nostrc-otxhq`.
- macOS `test_nostr_gtk_widget_churn_leaks`: `nostrc-x6arp`.
- macOS ASAN-only `nip55l_dbus_contract`: `nostrc-nfnw` host fork/runtime case;
  **not** a Linux signer exclusion or permission to hide a sanitizer report.

These owning beads remain open. Historical nfnw failures are not blindly excluded
from today's Linux signer suites. Quarantine is not remediation or release proof.

### Blocking sanitizer handoff

- `.14.3`: `signer/approval-controller` and `nip55l_dbus_contract` now pass
  ASAN+UBSAN+LSan after fixture ownership/private-bus cleanup. GUI integration and
  onboarding functional assertions finish, then LSan reports exit-time allocations:
  Final full-selection reproduction: 184,165 bytes/6,148 allocations and
  172,720 bytes/5,841 allocations respectively on stock Noble arm64
  (`/work/g12-final-asan.{log,xml}`). Allocation counts vary between runs. Most direct allocation groups have fontconfig stacks;
  remaining groups involve GIO objects and GTK file-dialog cancellation. That is
  diagnostic evidence, **not** permission for a broad library suppression. No new
  suppression or leak-detection disablement was added.
- `.14.4`: the TSan-only private runtime uses checksum-verified, unmodified GLib
  2.80.0 and instrumented vendored nsync; native/ASAN use stock distro libraries.
  CMake RPATH selects the private libraries only for tested binaries, not system
  schema tools. Approval-service/cache pass. GDB with the coarse TSan channel path
  finds `nostr_connection_new:1284` waiting in `go_select_timeout` while the worker
  waits in `go_channel_send:1237`. The shipped slot-path option instead reports
  `fiber_waiter_wake_one` (`libgo/src/channel.c:43`) against `fiber_waiter_wake_all`
  (`:62`) during subscription cancellation; both report the same mutex, so native
  nsync/wrapper annotation interaction must be investigated before declaring a
  production race. Required bunker selections remain blocking, not quarantined.

These blockers keep G12 open until resolved and its sanitizer stages pass. Logs
are preserved in the G12 Docker volume: `/work/g12-gui-leaks.log`,
`/work/g12-tsan-mpmc.log`; the debugger trace is `/tmp/g12-tsan-gdb.log` on this host.

## Packaging and identity close-out

One native `grotto` deb includes GUI+daemon and recommends the existing Shell
extension. Component/native and desktop-bundle recipes require camera deps and
canonical assets; RPM no longer installs legacy AppImage desktop/appdata; Arch has
a standalone complete-source recipe; Nix uses Grotto's own 0.1.0 and attests the
wrapped ELF. Base unit stays D-Bus activated, namespace-free and localhost-only;
installed bunker/headless examples explicitly reset ExecStart/network denial.
Autostart is a template, not automatic login enablement. See packaging/README.md.

R3 `nostrc-0ulk`: painted icon `64cc2c985`, truthful canonical metainfo/screenshots,
Preview desktop/About and build-derived version now provide identity close-out.
R1/R2 (`nostrc-f7fm`/`nostrc-exnn`) are already closed; current `legacy_dirs.c` and
`signer/legacy-dirs` preserve the move, storage/wire IDs deliberately retained.
R4 `nostrc-wic1` was closed before the G08–G10 reviewed UX work. Rename-epic closure
is distinct from Preview promotion and does not close the manual acceptance beads.

Flatpak/AppImage outcome: **NOT ACCEPTED**. All five §5.11 gates remain open:
host caller attestation, trusted approver identification, Secret-portal custody,
activation ownership and browser-host behavior. Do not weaken path/inode checks.
`nostrc-h3kh` stays deferred; Snap only on demand. A Groundhog Flatpak talking to
a native signer is not a sandboxed-signer acceptance result.

## Open obligations and handoff

- Manual .14.1/.9.1/.7.1/.13.1: all real-session steps NOT RUN, exact instructions
  and per-step expected outcomes in docs/testing/grotto-manual-acceptance.md.
- `nostrc-9n2n9`: stock GTK workaround is landed and exercised; authenticated
  Launchpad/SRU filing URL still needs the owner. No ID fabricated, no GTK backport shipped.
- `nostrc-ew9qv`: real cross-transport, export-delivery/logging/retention coverage
  remains separate; not silently closed by a general CTest pass.
- `nostrc-z6ps9`: credential-bearing AUTH relay-path product contract undecided;
  do not silently strip an exact relay URL or claim opaque paths are token-free.
- G05 handoff's additional bunker race/custody scenarios remain bounded coverage
  limitations, not tests claimed run by G12. Physical installed custody is manual.
- `nostrc-u6v55.15`: owner-approved transition from shared-version legacy
  Groundhog Debian/Arch bundles to independently versioned Grotto packages. G12
  provides standalone 0.1.0 recipes without silently changing bundle upgrade policy.
- Owner sign-off for exact Medium/required-confirmation sets and distro target
  compatibility remains open in plan §12. External consumer context adoption is
  not made mandatory by G12. Independent critique runs on the pushed branch.

## Version decision

Grotto stays **Preview 0.1.0**. PATCH-class packaging/build/copy corrections fold
into its initial unreleased version, no further bump; About/daemon now consume
the build version. Local and origin tag queries found no grotto-v*,
nip55l-v0.7.0*, nostr-gobject-v2.2.2* or nip46-v0.1.0* release tags on 2026-10-14.
No release columns or tags changed. CI/tests/docs and shared Grotto dependency
recipe corrections require no bump for nip55l, NIP-46, nostr-gobject, Groundhog,
native host or Shell extension. No monorepo-wide bump. Removing Preview remains
an explicit owner decision after the final promotion gate, never a packaging side effect.
