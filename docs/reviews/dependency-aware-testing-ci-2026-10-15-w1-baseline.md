# W1 operational baseline — dependency-aware testing

**Tracker:** `nostrc-0cfsg.5` (left in progress). **Phase:** W1 audit only; partial baseline, not integration acceptance.
**Observed:** 2026-10-15 UTC. **Audited checkout:** `9b8dc253cbc75f35783b6928d35fb59618025cb8`.
**Plan:** [dependency-aware testing](../plans/dependency-aware-testing-ci-2026-10-15.md), historical source baseline `d25102afa480`, plan commit `5d19e80f8`.

No workflow, local gate, selector, CMake, production, GitHub setting, schedule or documented default is changed by this report. Siblings `.1`/`.2` own selector/inventory and realization/runtime implementation; their work is not integrated or validated here. W11/W12 remain NOT RUN. Documentation-only: **no component version bump**. No merge or push is authorized for this phase.

## 1. Source delta and ownership inputs

`git log --oneline 5d19e80f8..9b8dc253c` identifies `f9e8edfaf`, `762689fac`, `07f3d537e`, `c815b3f3a`, `fa4a4cf00`, `9b8dc253c`. Only the two bead/plan commits separate the historical source baseline from the plan commit. The implementation design is unchanged; these are concrete inventory/reference updates:

| Source at audited checkout | Finding / selection consequence |
|---|---|
| `gnome/groundhog/CMakeLists.txt:1-19,198,856-860,1597-1615` | Base `0.12.0` plus `GROUNDHOG_PRERELEASE=alpha8` produces `0.12.0-alpha8` for the application and About/test compilation. `groundhog-version` now checks the full version. About/model/GUI and app-process coverage remain distinct; this is not evidence that an entire app target is isolated from MLS. |
| `gnome/groundhog/cmake/CheckPackagingVersion.cmake:1-31` and registration above | New `groundhog-packaging-version` runs CMake script mode. Runtime inputs are **all four** of `flake.nix`, `packaging/archlinux/groundhog/PKGBUILD`, `packaging/rpm/groundhog.spec`, `packaging/debian-desktop/changelog`, plus the script and configured version. The RPM check also validates prerelease Release, and Debian uses `~`. These external runtime files must not disappear from realization/impact ownership. |
| `.github/workflows/groundhog-release.yml:46-62` | Source-archive job rejects a `groundhog-v*` suffix unequal to the checked-out CMake full version; non-tag dispatch gets `0.0.0-<sha>`. This narrow guard is already implemented, **not** W10 broad exact-tag test validation or complete version-source/release aggregation. |
| `packaging/rpm/groundhog.spec:42-45,182-188` | Shared Groundhog RPM now supplies Grotto's `gstreamer-video-1.0`/`zbar` build dependencies and packages Control1 XML, autostart template, symbolic SVG, and daemon configuration examples. Grotto dependency/install consumers already exist at `apps/grotto/CMakeLists.txt:98-102,343-344,531-565`. Shared package ownership reaches Grotto, not just Groundhog. No packaging build/install acceptance was run here. |

Older plan line references into Groundhog shift with this delta (for example its final isolation pass is now near the end of a 3743-line file). Prefer symbols and the concrete references above; no metadata changes in sibling scopes are made here.

## 2. Live GitHub state (read-only)

Authenticated `gh 2.96.0`, account `chebizarro`, could read the public `chebizarro/nostrc` repository, protections, rules, workflows, runners, checks and completed jobs. No auth denial was encountered. Token values are not retained here.

| GET/query | Observed result |
|---|---|
| `repos/chebizarro/nostrc` | Default branch `master`; merge, squash and rebase methods enabled. This does not imply a merge queue. |
| `repos/chebizarro/nostrc/rulesets?includes_parents=true` | HTTP 200, `[]` (no repository/inherited rulesets returned). |
| `repos/chebizarro/nostrc/branches/master/protection` | HTTP 404, **`Branch not protected`**. |
| `repos/chebizarro/nostrc/branches/main/protection` | HTTP 404, **`Branch not found`**, not an auth failure. |
| `repos/chebizarro/nostrc/rules/branches/master` | HTTP 200, `[]`. |
| GraphQL `repository.mergeQueue(branch:"master")`, `branchProtectionRules(first:100)` | `mergeQueue: null`, `nodes: []`. No configured master merge queue/protection was returned. |
| `repos/chebizarro/nostrc/actions/runners` | HTTP 200, `total_count: 0`, `runners: []`. This is the repository self-hosted runner list, **not** a hosted-runner availability test. |
| `repos/chebizarro/nostrc/actions/workflows?per_page=100` | 20 workflow records, all marked active; only 19 YAML workflows are tracked at audited HEAD. Historical `a5-symbols.yml` has no file at HEAD or live master (contents GET: 404); last returned run `37860831482`, 2026-10-08, SHA `1ab831da1aa3d481e8ee942c7ebc7af1d440aede`. An active API record alone is not an active source workflow. |

**Current required-status identities:** none returned by the audited rules/protection APIs. Workflow-blocking below means failures fail that workflow, **not** branch-enforced required checks. W11 still requires owner-approved protections and PR/merge-group evidence; the absence of legacy requirements does not authorize silently enabling a new requirement.

Actual check-runs were read at `fa4a4cf000e8` and `cb25ace72045` (`commits/<sha>/check-runs?per_page=100`, app `github-actions`). Examples of exact names: `static-checks`, `groundhog`, `groundhog-sanitizers` (job receipts), `ubuntu-24.04 / native`, `ubuntu-latest / native`, `ubuntu-24.04 / asan-ubsan`, `ubuntu-24.04 / tsan (ADVISORY, non-gating)`, `ubuntu-latest • asan_ubsan`, `macos-latest • asan_ubsan`, `signet • Debug • passkeys=ON • sqlcipher=OFF • hooks=ON`, `Source archive`, `Fedora package (aarch64)`, `Publish pre-release`. Multiple runs at one SHA can return repeated names and different publication conclusions; do not flatten them into one passing receipt. `Dependency gate` does not exist in this audited source.

## 3. Current workflow/job/profile contract

Paths below are under `.github/workflows/`; line numbers refer to audited HEAD. Names containing braces describe source matrix templates, not invented required-check identities. All jobs are workflow-blocking unless exceptions are stated. Coarse/path-filtered triggers remain unchanged. This is a source-level inventory, **not** an enumeration of realized CTest registrations or all skipped tests.

| Workflow / job IDs and check-name template | Profiles and scope / provenance |
|---|---|
| `nostrc-ci.yml` `build-and-test`: `{os} • asan_ubsan` | Ubuntu/macOS root builds; explicit libnostr audit then broad CTest and Linux short fuzz. Matrix `:52-63`, execution `:117-178`. General ASan disables leak detection (`:63`); short fuzz enables it. Explicit exclusions below. |
| `libgo-ci.yml` `build-and-test`: `{os} • {sanitizer}`; `gofiber-linux-sanitizers`: `linux • gofiber • {sanitizer}`; `fuzz-driver`: `{os} • fuzz-driver` | Ubuntu/macOS × ASan/UBSan, TSan, TSan MPMC; Linux fibers × ASan/UBSan, TSan. First two configure standalone **libgo**, fuzz-driver configures **root**. `:36-47,85-125,127-183,197-252`. |
| `libhanami-ci.yml` `build-and-test`: `{os} • libhanami` | Ubuntu/macOS focused options and `libhanami` label; `:45-51,89-103`. |
| `libnostr-gi.yml` `generate-gir`: `{os} • GIR/typelib` | Ubuntu/macOS generation/build contract and uploaded GIR/typelib, not ordinary unit coverage; `:40-46,82-105`. |
| `groundhog-ci.yml` `groundhog`, `groundhog-sanitizers` | Ubuntu 24.04 Debug and ASan/UBSan; controlled display/private bus, serial perf, nak, lifetime/keyring cases. Normal lane explicitly enables MDK011 and runs White Noise voice separately from ordinary tests; sanitizer lane is display-free. `:73-122,164-300,323-449`. |
| `grotto-ci.yml` `signer`: `{runner} / {mode}` | Ubuntu 24.04/latest native; Ubuntu 24.04 ASan/UBSan; **advisory TSan** suffix. Native signer plus desktop quarantine, alternate Meson frontend using CMake prerequisites, deb inspection, private-bus Shell units. `:36-51,140-239`. No live Shell/manual acceptance implied. |
| `nip55l-libsecret-ci.yml` `refusing-session` | Ubuntu 24.04 pinned container/private-bus refusing-session contract; `:36-74`. |
| `nostr-homed-ci.yml` `build-and-test`, `systemd-opensession` | Ubuntu 24.04 runner, privileged Ubuntu/Debian Bookworm/Fedora 40/Arch containers; focused unit tests blocking, some integration best-effort. Separate systemd job condition restricts PR execution, and systemd test step is advisory. `:41-88,132-221`. |
| `signet-ci.yml` `build-and-test`: `signet • {build_type} • passkeys={...} • sqlcipher={...} • hooks={...}` | Ubuntu, four Debug passkey/SQLCipher combinations (ON/OFF uses hooks ON), plus Release ON/ON; CMake tests and Meson test-list parity. `:23-44,67-102`. |
| `static-checks.yml` `static-checks` | Ubuntu 24.04; current static/checker/shell regressions; `:23` onward. Not an implemented selector gate. |
| `gnostr-appimage.yml` `build-appimage`: `Build and smoke test gnostr AppImage ({arch})` | Native x86_64/aarch64 Ubuntu 24.04 runners, package/install smoke; push/PR/manual/reusable triggers. `:3-40,42-52`. |
| `windows-build.yml` `build`: `Build Windows Installer`; `release`: `Create Release` | `windows-latest` MSYS2/MinGW/NSIS installer/portable artifacts, legacy `v*` publication only. No unit-test claim. `:19-58,70-72,255-259`. |
| `bp-matrix.yml` `bp-matrix` | Ubuntu short capacity matrix; **main-only** push, PR/manual; `:3-44,61-89`. |
| `bp-matrix-nightly.yml` `nightly-matrix` | Ubuntu extended consume-us matrix; **manual-only**, not a running nightly schedule; `:3-8,23-87`. |
| `grotto-fuzz.yml` `fuzz-test`: `Fuzz {target}`; `coverage-report`: `Generate coverage report` | Ubuntu nip49/bip39/json-event; PR/manual/**weekly**; coverage scheduled/manual after fuzz. Coverage commands best-effort, not proof fuzz passed. `:17-54,94-119,150-213`. |
| `marmot-mdk-interop.yml` `mdk-interop` | Ubuntu 24.04 live Docker 0.8 compatibility plus ordinary Marmot vectors; manual/**daily 03:17 UTC**; `:9-20,42-60`. |
| `marmot-mdk011-interop.yml` `mdk011-adopted-matrix`: `MDK 0.11 adopted matrix` | Ubuntu 24.04 separate adopted Docker matrix, real GUI/private bus, exact incompatibility exception; manual/**daily 03:47 UTC**; `:15-29,55-70`. |
| `release.yml` `detect-component`, `build-source`, `build-linux`, `build-linux-daemon-tcp`, `build-appimage-gnostr`, `build-snap-gnostr`, `build-snap-signer`, `build-snap-daemon`, `build-flatpak`, `build-flatpak-gnostr`, `build-macos-signer-dmg`, `build-macos-tarball`, `create-release` | Existing global/component tag/manual routes only; Ubuntu native x86_64/aarch64 packages, reusable AppImage, macOS DMG/tarball. Component conditions at `:93-102,184-194,251-325,366-484`. **Publication ORs artifact successes** at `:343-350`; W10 correction remains outstanding. |
| `groundhog-release.yml` `source`, `deb`, `rpm`, `arch`, `flatpak`, `nix`, `release` | Groundhog tags/manual dry run. Source; Debian 13/Ubuntu 24.04/26.04 × two native architectures; Fedora/Arch/Flatpak/Nix × two. `:37-62,71-252`; publication requires source success and no failed/cancelled dependency at `:262-267`. Not a broad test-validation receipt or W10 all-applicable gate. |

### Exceptions that must survive migration

- Grotto TSan is job-level `continue-on-error` (`grotto-ci.yml:37-40`). A green advisory job does not promote it to blocking. Pinned-GLib suppression issue `nostrc-wls0w` is open; it was not reproduced here.
- Grotto's native desktop baseline **executes** three exact quarantined tests; `scripts/grotto-ci-quarantine.tsv:3-8` also records macOS widget churn and macOS-ASan NIP55L exclusions. Beads `nostrc-q1974`, `nostrc-pf4gd`, `nostrc-otxhq`, `nostrc-x6arp`, `nostrc-nfnw` are all **open** via `bd show ... --json`. Sanitizer reports still block; retries/quarantine are not clean first-attempt passes (`grotto-ci.yml:201-215`). No behavioral revalidation here.
- `libgo-ci.yml:108-125,176-183`: TSan excludes `W4$`; macOS ASan/UBSan excludes `GoChannelStressTest`. ASan `detect_leaks=0` is explicit. The matrix's `nostrc-75rv` reference is a closed bead, not new execution evidence. Do not turn these existing gaps into selector-certified coverage.
- `nostrc-ci.yml:153-159`: audit tests run explicitly and again in broad CTest; exclusions are `test_json_envelope|concurrency_subscription_shutdown_asan|MockRelayUnit`. Do not count duplicate executions as separate logical coverage.
- `nostr-homed-ci.yml:142,150,183,217-221`: secrets/FUSE/warm-cache cases use `|| true` or capability conditions; systemd step has `continue-on-error: true`. Their workflow success does not establish blocking integration acceptance.
- `grotto-fuzz.yml:200-213`: best-effort coverage tooling. Manual filter-step exit is not whole-job cancellation; future target filtering remains W8 work.
- `gnome/groundhog/CMakeLists.txt:2570-2581,2681-2687`: only the named MDK 0.9 incompatibility probe is expected exit-77/XFAIL. Other adopted/control or GUI-without-display skips cannot satisfy new blocking acceptance. No live MDK case was executed in this audit.

## 4. Local execution and tool/runner limits

Current `scripts/pre-push:19-85` defaults to static pushed-tree checks on detached trees, iterating refs; `NOSTRC_FULL_GATE=1` opts into the broad legacy gate. `AGENTS.md` still separately mandates broad build/test verification before pushes. That documented-policy/script distinction remains unchanged; this phase does **not** advertise proposed `test-plan.py`/`test-gate.sh` as available. Neither script, `tests/selection`, nor the descriptor helper exists in audited HEAD (`git ls-tree -r HEAD -- ...`). Broad gate host configuration uses private libsecret/Secret Service backends (`scripts/full-gate.sh:244-271`); none was launched here.

Host observed: macOS **27.0.1 arm64**, Git 2.54.0, Python 3.14.8, CMake/CTest 4.3.2, Ninja 1.13.2, Apple clang 21.0.0 (`gcc` is the same Apple clang, **not Linux GCC**), Meson 1.11.1, pkg-config 3.0.7, D-Bus 1.16.2, nak 0.19.7. Docker CLI/server 29.8.2, server `linux/aarch64`, answered read-only `docker info`. Xvfb is installed at `/opt/X11/bin/Xvfb`; **no display was started or tested**. Nix and PyYAML are absent; PyYAML is not required by the proposed stdlib selector.

`pkg-config --modversion gtk4 libadwaita-1 libsecret-1 sqlcipher gstreamer-1.0 gstreamer-video-1.0 zbar` returned respectively `4.24.1`, `1.10.0`, `0.21.8.2`, `3.53.3`, `1.28.7`, `1.28.7`, `0.23.93`. Presence does not establish a complete supported profile, minimum CMake 3.22 compatibility, real desktop acceptance, Linux GCC behavior or container privilege capability.

Hosted Linux x86_64, Linux ARM and macOS labels have completed job receipts below. Windows run [38704238367](https://github.com/chebizarro/nostrc/actions/runs/38704238367), 2026-10-15 at `d25102afa480acc82730bf3211e5bf306233f12b`, returned `Build Windows Installer: success`, label `windows-latest`; `Create Release: skipped` on a branch push. These establish observed prior execution, **not guaranteed future capacity**. Privileged homed/FUSE execution was not sampled; no hosted jobs were launched.

Setup ran `bd onboard`, `bd prime`, `scripts/install-hooks.sh` (reported already installed), and marked `.5` in progress. `bd dolt pull` failed: **`fetch from origin/main: Error 1105: no remote`**. Bead reads/updates work; remote Dolt synchronization does not. No direct `.beads` edit or settings repair was attempted.

## 5. Small timing evidence — no savings claim

Read-only Actions run/job metadata, latest successful run per sampled workflow, attempt 1. Not selected representative PRs; different SHAs/events/caches. Job sum is `sum(completed_at - started_at)`; elapsed is `updated_at - run_started_at`. Neither is billed runner-minutes, pure critical path, or a controlled clean/incremental baseline. Workflow success is an API conclusion, **not** proof of first-attempt test success/no quarantine.

| Run receipt | Event / exact SHA | Jobs / summed job seconds / elapsed seconds |
|---|---|---|
| [Grotto 38777093076](https://github.com/chebizarro/nostrc/actions/runs/38777093076) | Tag push `groundhog-v0.12.0-alpha8`, `fa4a4cf000e8982733d9ba6c9ddca4fd5fad4f88` | 4 / **2285 / 2296**; native 24.04 761s, native latest 745s, ASan/UBSan 473s, advisory TSan 306s |
| [Groundhog 38772393313](https://github.com/chebizarro/nostrc/actions/runs/38772393313) | Master push, `07f3d537e92cdd08850cc0599e45974c6aa9dcbd` | 2 / **2224 / 1535**; ordinary 1531s, sanitizers 693s |
| [Core 38787290152](https://github.com/chebizarro/nostrc/actions/runs/38787290152) | Master push, `cb25ace720454d90575d258ff025649bfc347715` (newer than checkout) | 2 / **929 / 490**; Ubuntu 486s, macOS 443s |
| [Groundhog release 38777093209](https://github.com/chebizarro/nostrc/actions/runs/38777093209) | Tag push, `fa4a4cf000e8982733d9ba6c9ddca4fd5fad4f88` | 16 / **3486 / 410**; all package matrix jobs successful, native Ubuntu 24.04/24.04-arm |

Useful step spans (not exhaustive phase totals): Grotto native 24.04 configure/build/signer = **12/164/125s**, desktop baseline **146s**, alternate frontend **8s**, deb build/inspect **132s**; Groundhog configure/build/ordinary controlled-display step = **19/122/661s**; sanitizer configure/build/run = **14/349/223s**. Setup, interop, retries and caches cannot be reconstructed reliably from these coarse spans without further receipts/log analysis. Configured/built target counts, primary/fixture counts and per-case outcomes were **not measured**.

A separate script-only local sample at 23:48 UTC ran five times, sequentially, at audited HEAD:

```sh
cmake -DVERSION=0.12.0-alpha8 -DROOT="$PWD" \
  -P gnome/groundhog/cmake/CheckPackagingVersion.cmake
```

All five exited 0: `packaging files name 0.12.0-alpha8`. Python `time.perf_counter` subprocess spans (seconds, rounded): **0.0186, 0.0058, 0.0055, 0.0053, 0.0052**; median **0.0055**, range **0.0052–0.0186**, exclusive-quartile IQR **0.0070** (unrounded samples used). OS cache state uncontrolled. This proves only the current script/version-file agreement; no configure, build, registered CTest/fixture run, package or desktop test occurred. It is **not** one of the representative workload baseline conditions.

### Required baseline and rollout evidence still outstanding (tracked by `.5` / epic)

The plan's seven conditions remain unmeasured: Grotto-only, isolated Groundhog UI, MLS, shared core, shared store/media, graph change, unknown path. For **each** clean/incremental condition collect at least five samples, exact tested SHA/profile/compiler/tools/configure fingerprint, cache state, configured/built targets, primary/fixture/expensive execution counts, separate setup/configure/build/test durations, retries and actual aggregate runner cost/critical path; report median/range/IQR. Compare legacy and shadow at the same source identity. No 30%/25% savings or 10% regression target is established here.

Shadow count **0/20 representative PRs**, broad replacement count **0/2 successful integration runs**, release replacement dry run **NOT RUN**, selected-skip/cancellation and merge-group acceptance **NOT RUN**. Existing legacy historical runs do not count toward those gates. External consumers of artifact names/manual entrypoints have not been enumerated; preserve names and callers until checked. Supported-profile registration/input audits and optional-component coverage-gap records await integration. **No default cutover, requirement removal, workflow rename, or schedule removal is authorized by W1 evidence.**

## 6. Reproduction and handoff

Source commands: `git log --oneline 5d19e80f8..9b8dc253c`, `git diff 5d19e80f8..9b8dc253c -- .github/workflows/groundhog-release.yml gnome/groundhog/CMakeLists.txt gnome/groundhog/cmake/CheckPackagingVersion.cmake packaging/rpm/groundhog.spec`, `git ls-tree -r --name-only HEAD -- scripts/test-plan.py scripts/test-gate.sh cmake/NostrcTestInventory.cmake tests/selection`.

GitHub observations are reproducible with `gh api --method GET '<endpoint from §2>'`; job receipts with `gh api --method GET repos/chebizarro/nostrc/actions/runs/<run-id>/jobs?per_page=100`; timing samples were selected using `actions/workflows/<filename>/runs?status=success&per_page=1`. GraphQL query:

```sh
gh api graphql -f query='{ repository(owner:"chebizarro",name:"nostrc") {
  mergeQueue(branch:"master") { id }
  branchProtectionRules(first:100) { nodes {
    pattern requiresStatusChecks requiredStatusCheckContexts
  } }
} }'
```

Raw transient API JSON/timing data was kept under `/tmp/nostrc-0cfsg5-w1`; it is not a durable artifact dependency. This report preserves bounded results, exact SHAs and public run IDs; fresh queries can change. No secret material or complete build logs are committed.

Validation for this docs-only phase: `git diff --check`, local document-link/reference checks, and the five script-mode probes above. No full/destructive build, live MDK, public relay contact, desktop/keychain test, Docker container launch, protection mutation, merge or push. Leave `.5` open for parent-directed integration verification and later documentation/default work.
