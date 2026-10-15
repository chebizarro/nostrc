# Grotto release readiness — G12, 2026-10-14

**Decision: keep Grotto (Preview) 0.1.0. NOT PROMOTED.** G12 delivers CI,
packaging, truthful metadata and an exact manual-acceptance handoff; it does not
claim an installed desktop was available. Removing Preview is a separate owner
decision after all final gates, not an automatic consequence of G00–G13 delivery.
No release tag, merge or release publication is authorized by this report.

Scope: `grotto/g12-packaging-ci`, base `5ef33513c` (G00–G11 + G13 integration),
plus G12 implementation checkpoint `304d1da0` and its evidence follow-up. G12
has completed its required local remediation and native reruns; delivery closure
is separate from promotion. Owner remediation decision: `.14.3`
must pass with leak detection and narrow source-proven third-party exceptions;
`.14.4` may remain open/advisory after one pinned-runtime attempt. This permits
G12 delivery closure, **not** promotion or a met TSan/hosted/manual gate.
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
| All named signer suites pass on the supported floor | New `grotto-ci.yml` runs signer/*, nip55l and native-host suites on ubuntu-24.04 and ubuntu-latest; asserts required registrations/no skipped tests, pinned Blueprint 0.22.2, ASAN+UBSAN and separate TSAN selection. Exact local outcomes below. | **NOT MET as a promotion gate.** Local native-floor and required ASAN evidence is bounded below; TSan is still failed/advisory and exact-SHA hosted required results are missing. No advisory/quarantined/skipped result counts as met. ubuntu-latest is not Ubuntu 26.04 desktop acceptance. |
| Ubuntu 24.04/26.04 manual evidence exists | `docs/testing/grotto-manual-acceptance.md` enumerates clean 24.04 stock GNOME, 24.04 Ubuntu session and 26.04, each step/expected result and owning bead. | **NOT RUN / NOT SATISFIED.** No real desktop. .14.1 full matrix, .9.1 stock-GNOME windowless first request, .7.1 camera/native unit, .13.1 GNOME 46/50 are open. |
| No reachable control is inert or advertises an unimplemented capability | G10 `3f80ac819`/`5ef33513c`: real templates/source inventories in both builds, `signer/ui-layout`, `ui-accessibility`, `blueprint-drift`, home/identity-model suites. Capability/version-skew UI is read-only when unavailable; hardware/multisig/delegation/social recovery excluded from production claims. | Bounded source/layout evidence, not blanket manual certification of every control. Physical camera, Orca, keyboard-only, RTL, 200% text/360px and live Shell remain NOT RUN. Medium/required-kind final membership still needs explicit owner sign-off. |
| Metadata/docs describe demonstrated behavior | Canonical metainfo/Desktop/About say Preview; no unsupported hardware claims; explicit same-user-malware limit, key-management caveat and signing≠publication. Real-template fixture captures committed under apps/grotto/data/screenshots; desktop-contract checks referenced PNGs. Deployment removes false production-ready/TCP/system-bus promises and documents migration/network/headless policy. | Source/automated evidence, with manual limitations stated. Screenshot remote URLs resolve only after landing on master. Flatpak/AppImage remain experimental and release-blocked, not demonstrated native equivalents. |

**Overall gate is not satisfied** even when all automated checks are green:
manual evidence and owner decisions cannot be manufactured by CI or source review.

## Independent critique disposition

The [commit-scoped independent critique](grotto-g12-release-readiness-critique-2026-10-14.md)
is retained unchanged and committed with this remediation, not rewritten as an
approval. Its confirmed dangling Nix overlay reference to the removed
`grotto-daemon-tcp` output is removed (no unsupported TCP package resurrected).
Nix evaluation/build is still **NOT RUN**. Its four Linux smoke skips are named
in the evidence table, with actual reasons and existing related beads. The
sanitizer, manual and hosted promotion limitations remain explicit below.

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
| macOS initial default build and full CTest | 435 functional selections: **430 executed/pass, 5 skipped** (test_nip5f_tcp, signer/qr-scanner, signer/onboarding, signer/ui-layout, signer/ui-accessibility); 1 serial performance selection executed/pass. This initial run predates final G12 corrections and is not their final gate. Skipped cases are not passed acceptance. |
| Final macOS build/CTest | Gate succeeded **after serial rerun**, not a clean first-run pass: fresh build/all default targets; 435 functional selections, **428 initially passed, 5 skipped, 2 initially failed then passed on exact-name serial rerun**. `test_nostr_gtk_nip34_issue_view` (`nostrc-kgjba`) and `test_nostr_gtk_thread_view_bounded` (`nostrc-4k8o0`) both hit `gdk_display_link_source_pause: source->paused == FALSE` (SIGTRAP). No assertion changes or quarantine; open follow-up bugs. One serial performance selection passed. Both GTK cases also passed three further serial repetitions each (`/tmp/g12-owner-macos-gtk-repeat.log`); this does not resolve their first failures. Logs `/tmp/g12-owner-macos-{configure,build,ctest}.log`; original output `_build/g12-gate/gate-history/20261015T004905Z-64655-first-run.log`. |
| Stock Ubuntu 24.04 real layout/captures | PASS, signer/ui-layout + fixture, GTK 4.14.5+ds-0ubuntu0.10 / Adwaita 1.5.0-1ubuntu2 / GLib 2.80.0, arm64. No patched GTK. See screenshot provenance README. |
| scripts/linux-gate.sh | Final PASS after fixture corrections: all default targets + Groundhog built; 550 smoke selections: **546 executed/pass, 4 skipped** (listed individually below); 1 serial performance selection executed/pass, no quarantine used (3m01s). Log `/tmp/g12-owner-linux-final.log`; full CTest output in the G12 volume `/work/ctest.log`. This is the smoke/perf subset, not every slow/known unbuilt gnostr test. |
| Linux smoke skip: `test_nip5f_tcp` | **SKIPPED, not passed**: no `tcp:` NOSTR_SIGNER_ENDPOINT / NOSTR_SIGNER_TOKEN configured. No dedicated skip bead found; real-transport coverage remains separately open in `nostrc-ew9qv`. Loopback tests do not count as this external TCP acceptance. |
| Linux smoke skip: `groundhog-nip46-nak` | **SKIPPED, not passed**: `nak` not on PATH in the local image. Related coverage bead `nostrc-p15n5.3` is closed; that historical result does not execute this run. |
| Linux smoke skip: `groundhog-nip46-inbox-nak` | **SKIPPED, not passed**: `nak` not on PATH. No dedicated skip bead found; same external-peer prerequisite as the NIP-46 regression family (`nostrc-p15n5.3`). |
| Linux smoke skip: `groundhog-nip46-pair-nak` | **SKIPPED, not passed**: `nak` not on PATH. `nostrc-otxhq` separately tracks its failure when executed; quarantine was **not** used to convert this skip. Hosted baseline installs pinned nak and rejects skips. |
| Full floor signer/nip55l/native-host selection | Final PASS: 56 selections, no CTest skips (103.44s). Includes crypto/D-Bus suites omitted from smoke. Logs/XML `/work/g12-owner-native-signer.{log,xml}` in the G12 volume. |
| Required signer ASAN+UBSAN+LSan | Remediation run PASS: **15 executed/pass, zero skips/quarantines**, 71.78s, `detect_leaks=1`. Only two GUI tests load six source-proven third-party function exceptions; controller/private-bus suites have no suppression. Three deliberate application/fixture leaks still fail with that file loaded. `/work/g12-owner-final-asan.{log,xml}` and `/work/g12-owner-lsan-counterfactuals/`. This is not an unsuppressed all-library leak-free claim; see exact exceptions below. |
| Plan's TSan promotion gate | **NOT MET / ADVISORY, not gating G12 delivery by explicit owner decision.** One checksum-pinned GLib 2.80.0 + instrumented nsync attempt: 6 selections, 4 pass (including fixtures), `signer/bunker` fails, `signer/bunker-e2e` times out (63.61s). Channel waiter/annotation report at `libgo/src/channel.c:43`; no race diagnosis or suppression. `.14.4` remains open. Job name says ADVISORY and only that matrix cell has `continue-on-error`; neither its tolerated result nor aggregate CI green can satisfy this gate. `/work/g12-tsan-owner-attempt.{log,xml}`. |
| Existing Groundhog sanitizer pattern | PASS: `scripts/linux-gate.sh --sanitizers` built Groundhog/required targets; all 1103 source inputs covered by SANITIZER_BUILD_PATHS; 65 CTest selections passed (62 required names plus fixtures), serial, 6m00s. `ab5794e91` required-test-set checking preserved. Existing third-party libsecret suppression unchanged; Grotto's GUI-only exception file is not loaded in this job. Log `/tmp/g12-owner-groundhog-sanitizers.log`. |
| Native package build / staged install | Final PASS after disabling unrelated libhanami configuration: real distro-hardening `dpkg-buildpackage` produced one GUI+daemon `grotto_0.1.0-1_arm64.deb` (plus optional dbgsym), dependencies/recommendations and contents inspected; desktop-contract + fixture rerun passes. Log `/work/g12-final-deb.log`, listing `/work/g12-final-deb-contents.txt`. Fixed GCC Release comparison diagnostic without weakening flags (`.14.2`). Alternate Meson GUI build passed; private-bus Shell unit tests passed 83 checks. RPM/Arch/Nix recipes were not target-native built or installed. |
| Static/CI contract checks | Final PASS: actionlint, Linux dependency checker/self-test, quarantine counterfactuals (known failure executes/records, unknown failure and ASAN/UBSAN/TSAN reports block), unsequenced-args, 46 GUI GDK frame sources, 21 privacy rules, Grotto name guard, shell syntax, read-only macOS keychain check and `git diff --check`. |
| Hosted floor/current required CI | **NOT MET.** No successful exact-remediation-SHA hosted native/ASAN matrix is claimed. Prior checkpoint [run 38659602695](https://github.com/chebizarro/nostrc/actions/runs/38659602695) has now completed: both native jobs succeeded, ASAN/UBSAN and TSan failed. Those native successes belong to `b9f2f92b5`, not this remediation SHA. Queued/in-progress is not passed. Advisory TSan cannot count as a met sanitizer gate even if the overall workflow turns green. |

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

### LSan ownership and narrowly scoped third-party exceptions

The real fixture leaks were fixed in `304d1da0`: controller snapshots remain
floating until consumed, retained delayed replies release their owned reference,
and GetApprovalInfo does not add a redundant ref. The nip55l contract now uses
the supervised private-bus fixture, disconnects the libsecret singleton, releases
connections and strictly tears the bus down. Assertions and failure cleanup stay
active. **Neither fixture suite loads any suppression.**

The earlier unsuppressed GUI failures remain in `/work/g12-final-asan.{log,xml}`;
they were diagnostic evidence, not proof everything was a harmless cache. Matching
stock Ubuntu debug symbols now identify the remaining ownership precisely:

| Literal function in `apps/grotto/tests/lsan.supp` | Source-proven third-party owner / open bead |
|---|---|
| `match_in_thread` | [Pango 1.52.1 font-map source](https://github.com/GNOME/pango/blob/1.52.1/pango/pangofc-fontmap.c): font matching stores the result in PangoFcPatterns; the map retains pattern/fontset caches. `nostrc-ydtdf`. |
| `pango_fc_font_map_load_fontset` | Same Pango source: loads patterns into fontset_hash and the recent-fontset cache, not Grotto allocations. `nostrc-ydtdf`. |
| `FcParsePattern` | [fontconfig 2.15.0 parser](https://github.com/fontconfig/fontconfig/blob/2.15.0/src/fcxml.c): parsed configuration patterns are retained by fontconfig, not the GUI. `nostrc-ydtdf`. |
| `gtk_path_bar_get_info_callback` | [GTK 4.14.5 path bar](https://github.com/GNOME/gtk/blob/4.14.5/gtk/gtkpathbar.c): callback allocates list nodes; set_file_finish replaces button_list after clear_buttons without freeing the previous nodes. This is a GTK ownership bug, not merely a cache. `nostrc-ojtu3`. |
| `gtk_file_system_model_got_enumerator` | [GTK filesystem model](https://github.com/GNOME/gtk/blob/4.14.5/gtk/gtkfilesystemmodel.c): frees errors only in the non-cancellation branch, losing cancelled errors. `nostrc-ojtu3`. |
| `query_info_async_thread` | The path-bar callback also never unrefs its GFileInfo result. [GLib 2.80 async worker](https://github.com/GNOME/glib/blob/2.80.0/gio/gfile.c) allocates/returns that object on a separate stack, so the consuming GTK callback cannot be matched there. `nostrc-ojtu3`. |

The last exception is deliberately restricted: the current Grotto sources and
these fixtures do not call/own asynchronous file-info queries. Reassess it if
that changes; the counterfactual script fails closed if Grotto/nip55l/GObject/GTK
wrapper C/header sources start using g_file_query_info_async. It is not permission
to hide an application query-result leak.
Only `signer/approval-integration` and `signer/onboarding` receive LSAN_OPTIONS,
and only for Linux ASAN builds. No module/path/wildcard, malloc, GObject or
GVariant allocator suppression; `detect_leaks=1` and all assertions remain.
This exception file is **not** used by the native or Groundhog sanitizer gates.

Symbolization was checked against installed build IDs: GTK
`1e35b540340590bad529753f6406528f4bf4f8cc`, GIO
`9ee169d35bbdc7d1097f0526cc86c0bcbf020bd1`, fontconfig
`80005a450f568b98f575ef85ff92e6122c0e2864`, PangoFT2
`ce8823f752d2569546c62307fddbc482e4fa5134`. Distro versions: GTK
4.14.5+ds-0ubuntu0.10, GLib 2.80.0-6ubuntu3.9, fontconfig 2.15.0-1.1ubuntu2,
Pango 1.52.1+ds-1build1, arm64. Docker/hosted ASAN install exact-version matching
Ubuntu dbgsym packages; runtime libraries/GTK are **not patched or replaced**.
Source-symbolized unsuppressed output: `/work/g12-symbolized-gui.log`.

Required ASAN/UBSAN/LSan now executes all **15 selections**, zero failures/skips.
The verbose log prints the used exception counts. Negative counterfactuals in
`scripts/test-grotto-lsan-suppressions.py` intentionally leak a GVariant graph,
GTestDBus fixture and application GFileInfo; **all three still fail** with the
GUI exception file loaded. Latest logs `/work/g12-owner-lsan-counterfactuals/*.log` are retained;
this is not a claim that every possible application leak is covered.

### TSan time-box disposition — promotion gate NOT MET

The one bounded attempt used checksum-verified, unmodified GLib 2.80.0 and
instrumented vendored nsync (compile-command check), with CMake RPATH selecting
private GLib only for tested binaries, not system schema tools. Of 6 selections,
4 pass (approval-service/cache plus fixtures), `signer/bunker` reports
`fiber_waiter_wake_one` (`libgo/src/channel.c:43`) against `fiber_waiter_wake_all`
(`:62`) during cancellation, and `signer/bunker-e2e` times out. Both accesses
report the same mutex; annotation interaction versus a production race is still
unresolved. No further core changes, report suppression or timeout inflation.
Logs `/work/g12-tsan-owner-attempt.{log,xml}` and linkage/compile evidence are
preserved. Earlier coarse-channel debugger trace `/tmp/g12-tsan-gdb.log` found
`nostr_connection_new:1284` waiting in go_select_timeout while its worker waited
in go_channel_send:1237.

Per the owner's explicit revised G12 contract, only the TSan matrix job is
**ADVISORY / continue-on-error** and `.14.4` remains open. This removes it as a
G12-delivery blocker; it **does not meet the plan's TSan promotion gate**. A
quarantined, skipped, advisory or tolerated failure is never a met release gate.

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
- `nostrc-ydtdf` / `nostrc-ojtu3`: source-proven Pango/fontconfig caches and
  GTK file-chooser ownership bugs; narrow GUI-only exceptions do not resolve upstream bugs.
- `nostrc-kgjba` / `nostrc-4k8o0`: first-run macOS GTK display-link SIGTRAPs;
  serial retries/repetitions passed, root cause remains open, no quarantine added.
- `nostrc-u6v55.14.4`: advisory TSan channel/annotation report + E2E timeout;
  instrumented runtime setup succeeded, plan promotion gate still NOT MET.
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
  not made mandatory by G12. The supplied independent critique is committed unchanged; a bounded remediation review found no further confirmed actionable items, not release approval.

## Version decision

Grotto stays **Preview 0.1.0**. PATCH-class packaging/build/copy corrections fold
into its initial unreleased version, no further bump; About/daemon now consume
the build version. Local and origin tag queries found no grotto-v*,
nip55l-v0.7.0*, nostr-gobject-v2.2.2* or nip46-v0.1.0* release tags on 2026-10-14.
No release columns or tags changed. CI/tests/docs and shared Grotto dependency
recipe corrections require no bump for nip55l, NIP-46, nostr-gobject, Groundhog,
native host or Shell extension. No monorepo-wide bump. Removing Preview remains
an explicit owner decision after the final promotion gate, never a packaging side effect.
