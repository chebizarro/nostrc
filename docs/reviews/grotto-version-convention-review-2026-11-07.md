# Grotto version convention: behavior, packaging and release review

Date: 2026-11-07. Review task: `nostrc-6yifh`; implementation: `nostrc-u6v55.74`.

## Boundary and verdict

Reviewed exactly `git diff de2a40876...ee01bd5b0` on local branch `grotto/version-convention`:

- Base: `de2a408766773369407bbd5ef831ccc53f0f1727`; reviewed HEAD: `ee01bd5b0c81267ce82e75aaa7ef2ebf0e8747ac`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-19caa654-grotto-version-convention-24e6e5cf`.
- Scope: owner-selected **0.1.1-alpha1**, Groundhog's base/prerelease/full-version convention, application naming, CMake/Meson and package metadata agreement, release-tag refusal, catalogs and the requested focused ARM64 checks. Groundhog was compared at the same reviewed HEAD.
- No implementation, tests, catalogs, version sources or workflows were changed by this review. Only this report is committed, locally; no push, merge, tag creation or publication.

**Verdict:** the full-version runtime wiring works, the tag guard rejects mismatches, and all seven requested CTest checks plus two isolation fixtures pass. It is **not a clean pass against the requested convention/cleanup**: the new manifest expectation contradicts the documented base-version declaration convention, and shipped package/manpage copy still contains Preview branding. These are finite version-ledger/packaging issues, not evidence of a signing or authorization regression. The separate `nostrc-p4bcf` failure is confirmed on master and is not a new blocker.

Evidence labels: **confirmed/executed** means run during this review; **confirmed/static** means verified in source and its owning consumer. S2 denotes a version/verification contract defect; S3 denotes packaging/copy polish, not exploit severity. File references are repository-relative at the reviewed HEAD. Container/Xvfb and local diagnostic evidence are not hosted CI, native macOS/Windows packaging, desktop owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/static and executed — Should-fix: the manifest agreement check enforces a different convention from Groundhog and AGENTS.md

**Where:** `VERSION_MANIFEST.md:35`; `apps/grotto/cmake/CheckPackagingVersion.cmake:26`; comparison: `VERSION_MANIFEST.md:30`, `gnome/groundhog/CMakeLists.txt:2,11-15`; policy: `AGENTS.md:123-127`.

Grotto's **Declared version** is changed from the numeric base to `0.1.1-alpha1`, and the new packaging check requires that exact full string. Groundhog retains **Declared version 0.12.1** while its full display/package version is `0.12.1-alpha8`. The repository instructions likewise say that, for prereleases, **Declared version retains the base version**. The owner's full runtime version `0.1.1-alpha1` does not itself require changing that ledger convention. Keeping Latest release as Unreleased and Release tag empty is correct: nothing has been published.

**Failure scenario:** maintaining the documented base declaration (`0.1.1`) now makes `grotto-packaging-version` fail even while CMake's base, full display string and all package versions remain correct. In an ignored diagnostic copy, changing only this manifest cell to `0.1.1` caused the unchanged production check to exit 1 at line 26, demanding `0.1.1-alpha1`. This is a newly introduced convention/test mismatch, not a failed binary-version test. Reconcile the manifest cell and its checker with the base-version rule, retaining the owner-selected full string in runtime/package surfaces; alternatively, an explicit owner-approved policy exception would need to be documented. No such reconciliation was made here.

### F2 — S3, confirmed/static and executed search — Should-fix: user-visible Preview packaging copy and old manual version titles remain

**Where:** `apps/grotto/packaging/archlinux/PKGBUILD:7`; `apps/grotto/packaging/rpm/grotto.spec:8`; `apps/grotto/packaging/debian/control:53`; `apps/grotto/packaging/debian/grotto.1:1-3`; `apps/grotto/packaging/debian/grotto-daemon.1:1-3`; `packaging/archlinux/groundhog/PKGBUILD:81`. Debian installs both manuals through `apps/grotto/packaging/debian/grotto.manpages:1-2`.

The literal tracked search `git grep -n -F '(Preview)' -- apps/grotto packaging data po` returns seven lines: six package-summary/manual lines above, plus the non-application-name documentation heading `apps/grotto/docs/pairing.md:1`. The package summaries still end in `(Preview)`. Both manual NAME sections do too, and their `.TH` titles still advertise **Grotto Preview 0.1.0**. About, desktop Name, metainfo name and all five translated `Grotto` entries are clean; this is not a claim that those requested fields were missed.

**Failure scenario:** a user sees Preview in the installed package description or `man grotto`, and the manual identifies an old version while the executable reports `0.1.1-alpha1`. These strings all predate the diff: this is incomplete requested cleanup, not a newly introduced branding regression. Reconcile the shipped Grotto descriptions/manual titles with the chosen name/version. The documentation heading is listed for transparency, not treated as an application-name defect. No copy was changed by this review.

## Groundhog comparison and regression checks

| Surface | Source comparison and result |
|---|---|
| Base + prerelease + full string | Groundhog `CMakeLists.txt:1-15` and Grotto `apps/grotto/CMakeLists.txt:1-18` have the same numeric `project(VERSION ...)`, suffix variable, conditional hyphen append and full display version. Different minimum CMake versions are pre-existing build requirements, not a convention defect. |
| Compile definitions and About | Groundhog defines its full-version macro on the executable/About targets (`gnome/groundhog/CMakeLists.txt:209,1616`) and consumes it with the plain name in `src/ui/gh-about-dialog.c:62-64`. Grotto defines the full macro directory-wide (`apps/grotto/CMakeLists.txt:18`) and consumes it with plain Grotto in `src/main_app.c:73-75`; both GUI and daemon inherit it. About is a source-confirmed trace, not a live About-dialog inspection. |
| CLI and daemon | Grotto's real `main_app.c:810-814` prints the macro; `daemon/main_daemon.c:43,305-306` does the same. Both newly built binaries actually report `0.1.1-alpha1`. |
| Version/agreement tests | Grotto `CMakeLists.txt:847-854` mirrors Groundhog `CMakeLists.txt:868-874`. Grotto's checker additionally covers Meson, metainfo, manifest, Snap and platform scripts, and checks RPM's numeric Version. Both are source-agreement checks, not native package builds. F1 is the manifest-specific divergence. |
| Debian and RPM | Groundhog's changelog starts `0.12.1~alpha8-1`; Grotto's starts `0.1.1~alpha1-1`. Both use upstream full version plus an RPM Release beginning with `0.` for prereleases. Grotto correctly has Version `0.1.1`, Release `0.1.alpha1`. Groundhog's existing spec has upstream `0.12.1-alpha8` but Version `0.12.0` (`packaging/rpm/groundhog.spec:15,19-20`); this unrelated mismatch was filed as `nostrc-gsj76`, not copied into Grotto or made a Grotto merge prerequisite. |
| Release guard | Grotto `.github/workflows/release.yml:45-56` mirrors the extraction/comparison at Groundhog `.github/workflows/groundhog-release.yml:95-109`, but runs earlier in detect-component. The actual extracted Grotto shell body accepts `grotto-v0.1.1-alpha1` and exits 1 for three mismatches. This was a local dry-run, not an Actions run. |
| Meson and other packages | `apps/grotto/meson.build:8,298` supplies the full version to the alternate GUI frontend. The agreement checks pass for metainfo, Debian, RPM, Arch, Nix, Snap, macOS and Windows inputs. Arch separates the full archive version from hyphen-free pkgver. No Meson executable, Nix/Snap package or native installer was built during this review. |
| Name and identity | About, canonical desktop and metainfo use plain Grotto. Desktop/metainfo IDs remain `org.nostr.Grotto`; the reviewed diff changes no application ID. Catalog translations of `Grotto` agree, including Japanese and Persian. Residual package/manual copy is F2. |
| Apple numeric bundle fields | Both actual substitution expressions (`apps/grotto/CMakeLists.txt:769-772`, `packaging/macos/create-dmg.sh:218`) were executed in isolation and parsed as plists: CFBundleShortVersionString and CFBundleVersion are both `0.1.1`, name Grotto, identifier `org.nostr.Grotto`. Keeping the short version numeric follows [Apple's required three-integer format](https://developer.apple.com/help/glossary/version-number/); the full prerelease remains in the binary and DMG version argument. This is not a signed/notarized bundle build. |
| Panels and release status | No `gnome/grotto-panels` source changes occur in the diff; its manifest row remains `0.1.0`. No published release/tag columns were advanced for Grotto. |

## Verification actually run

Linux image: `nostrc-linux-ci:arm64`, image ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; container reports `aarch64`. A fresh task-owned build volume was used, with this worktree mounted read-only at `/src`. CMake Debug configured with apps/signer tests/native host/libvault/TCP test IPC enabled; GNostr, Groundhog, relayd and Signet disabled; libsecret selected. The Groundhog store-backend setting was unused because Groundhog was disabled. Only `grotto`, `grotto-daemon`, `test-ui-layout` and `test-desktop-activation` plus their build prerequisites were built, with `--parallel 1`. No aggregate/full-suite test target was run.

| Command/check | Actual result |
|---|---|
| `xvfb-run -a ctest --test-dir /build --parallel 1 --no-tests=error -R '^(grotto-version\|grotto-packaging-version\|signer/(version\|ui-layout\|ui-accessibility\|blueprint-drift\|desktop-contract))$' --output-on-failure` | PASS, **9/9** including `grotto-test-homes` and `grotto-tests-test-homes`; seven requested checks all passed, none skipped; 166.02 seconds. |
| Newly built GUI/daemon `--version` | PASS: `Grotto 0.1.1-alpha1`; `grotto-daemon version 0.1.1-alpha1`. |
| `desktop-file-validate apps/grotto/data/org.nostr.Grotto.desktop`; `appstreamcli validate --no-net apps/grotto/data/org.nostr.Grotto.metainfo.xml` | Both exit 0; AppStream reports successful validation with one pedantic hint. Also covered by the passing desktop-contract check. |
| `msgfmt --check --check-format -o /dev/null <catalog>` and fuzzy-flag search | PASS for all five Grotto `.po` catalogs and the panels Spanish catalog; zero fuzzy flags. Grotto POT also has no fuzzy flag. This does not claim complete translations of every unrelated entry. |
| `dpkg --compare-versions 0.1.1~alpha1 lt 0.1.1`, and complete package versions `0.1.1~alpha1-1 lt 0.1.1-1` | Both PASS; `dpkg-parsechangelog` reports `0.1.1~alpha1-1`. |
| RPM ordering | `rpmdev-vercmp`/Ubuntu `rpmdevtools` unavailable. Installed container-only `rpm`/`python3-rpm`; real librpm 4.18.2 `rpm.labelCompare` returns **-1** for both `0.1.1~alpha1 < 0.1.1` and actual spec EVR `0:0.1.1-0.1.alpha1 < 0:0.1.1-1`. No RPM build or host installation. |
| Extracted, unmodified workflow guard body | Matching alpha1 tag name exits **0**; `grotto-v0.1.1`, `grotto-v0.1.1-alpha2`, `grotto-v0.2.0` each exit **1**, with expected mismatch messages. No refs created. |
| Actual CMake/script plist substitutions, followed by `plistlib` parsing | PASS: both numeric fields `0.1.1` on both paths. Diagnostic extraction only; no native bundle, signing or notarization. |
| Literal tracked Preview search over the requested paths | **Does not meet the zero-user-visible-leftovers request**: six packaging/manual hits and one documentation heading, as F2. |
| Isolated base-declared manifest probe | Expected FAIL, exit 1: unchanged checker requires full prerelease in the cell; confirms F1. No tracked file changed. |
| Host `PYTHONPATH=tests/selection python3 -m unittest test_release_plan` | **14/15 PASS**, one FAIL: `ReleaseTests.test_checked_in_version_sources_match_independent_manifest`, `25 != 24`. All three added Grotto release tests pass. Not a green module result. |
| The same single count test at reviewed HEAD and master | FAIL `25 != 24` on both. Master was exactly `d6ea3b79e646ac58f7154a87c37596a9a86343a7`, executed from a task-owned local sparse clone under this worktree's ignored cache, not by modifying/checking out the main worktree. Confirms `nostrc-p4bcf`; master traceback is at line 141, task traceback at line 170. |
| Production `.github/workflows/dependency_contract.py release-identity --sha ee01bd5b0c81267ce82e75aaa7ef2ebf0e8747ac --component grotto --publisher main --dry-run` with explicit local diagnostic Actions environment | Final exit 0, verifies base `0.1.1`, `publish=false`. This is an untagged branch dry-run: output version is the workflow's synthetic `0.0.0.gitee01bd5b`, not evidence of a published/full-version tag. Initial invocations correctly refused missing workflow-SHA/output environment; their logs remain. |
| `git diff --check de2a40876...ee01bd5b0` | PASS. |

Receipts are retained in ignored `.cache/grotto-version-review/evidence/`: `configure.log`, `build.log`, `ctest.log`, `LastTest.log`, `binaries.log`, `desktop.log`, `appstream.log`, `diagnostics.log`, `preview-grep.log`, `manifest-base-probe.log`, `release-module.log`, `release-count-{head,master}.log`, `release-identity*.log` and dependency-install logs. The initial sparse-clone `add --no-cone` setup command was unsupported; the subsequent actual baseline test needed only the checked-out scripts/inventory and committed source reads and reached the stated count assertion. This setup error was not counted as a test pass. `xdotool` and RPM diagnostics were installed only in the task-owned disposable container. No developer keychain commands were run. The task-owned container/build volume, clean temporary baseline clone and isolated probe copy were removed; receipts and diagnostic scripts are preserved.

The full suite, broad gate, operational selector suite, sanitizers, actual Meson build, native desktop/Orca, native macOS/Windows packages, live relays/keyring, hosted workflow execution and release publication were **not run**. No guard, suppression, policy, timeout or test was weakened. Advisory Jev `find-lines`/`filter-search` lookups narrowed source reads; the CLI lookup missed its initial line range and was resolved by direct literal search. All finding claims were checked against source, not inferred from Jev. Lookup usage for this review: 22 calls, 35 API requests, 196,892 input tokens, approximately $0.008267; not test or acceptance evidence.

## Version and delivery decision

This report is documentation-only: **no bump** for Grotto, grotto-panels, the independently versioned native host or any other manifest component. The implementation's version/packaging corrections are PATCH-class within the owner's unreleased numeric base **0.1.1**, with chosen full display/package version **0.1.1-alpha1**; grotto-panels remains **0.1.0**, with no shipped change. F1 concerns representation of that base in the ledger, not a request to alter the chosen full runtime version or publish a release. No version source or release column was edited by the reviewer.

## Merge recommendation

**Request the finite F1/F2 reconciliations before merging unchanged.** Preserve the working full-version propagation, unchanged application ID, numeric Apple fields, prerelease package ordering and fail-closed tag guard. Align the manifest declaration/check with the documented Groundhog convention and finish the shipped packaging/manual naming cleanup, then rerun their directly affected version/branding checks. The requested ARM64 checks already pass. Keep `nostrc-p4bcf` and unrelated Groundhog `nostrc-gsj76` separate; neither justifies a full-suite expansion, baseline repair or a new release-qualification programme. This review makes no implementation fixes and is delivered only as a local report commit.

## Re-check (9135187b6)

Date: 2026-11-07. Re-check task: `nostrc-5tqzu`. Reviewed implementation `9135187b6774f217d7cbf7cd72423dce3925786e` on the same local branch/worktree, limited to F1/F2 and their previously requested serial checks. This section updates the findings' status and merge recommendation; the original observations above remain historical evidence. No implementation fixes or push were made.

### F1 — Resolved, confirmed/static and executed

`VERSION_MANIFEST.md:35` now declares numeric base **0.1.1**, and `apps/grotto/cmake/CheckPackagingVersion.cmake:27` expects `BASE_VERSION`. The owner-selected runtime/package string remains **0.1.1-alpha1**. Repeated the original isolated manifest-cell diagnostic against the production checker: a copied manifest declaring **0.1.1 passes, exit 0**. Changing only that copied cell to **0.1.1-alpha1 fails, exit 1**, with the expected base-version requirement. No tracked cell was edited. This resolves the Groundhog/AGENTS.md convention mismatch without changing the full display version or advancing unpublished release columns.

### F2 — Partially resolved; one shipped description remains, confirmed/static and executed metadata probe

The originally cited summaries and manual NAME sections are clean. CMake now configures `grotto.1` and `grotto-daemon.1` from `.1.in` templates (`apps/grotto/CMakeLists.txt:22-25`). The real `dh_installman -pgrotto` command, in an isolated fixture using the actual Debian control/changelog/manpages inputs and the newly CMake-generated manuals, installed both manuals with **Grotto 0.1.1-alpha1** titles and no Preview text or unexpanded token. This verifies the new `obj-*/apps/grotto/*.1` install inputs, not just template substitution.

**Remaining where:** `packaging/debian-desktop/control:89`, in the `Package: grotto` long Description: **“Grotto is Preview 0.1.0; installed-desktop release acceptance remains pending.”** This is current package metadata, unlike a historical changelog or the documentation headings. Both `scripts/build-desktop-deb.sh:31` and `.github/workflows/groundhog-release.yml:162` copy this recipe into the build's `debian/` directory. A real `dpkg-gencontrol -pgrotto -Pdebian/grotto -O` invocation with the actual split-package control/changelog in a metadata-only fixture reproduced the stale sentence in the generated binary control Description. Its undefined shlibs/misc substitution warnings are expected because no complete package build was attempted; they do not prevent reproducing the Description.

**Failure scenario:** the Groundhog-bundled Grotto Debian package still advertises Preview 0.1.0 in package-manager details despite its clean short summary and the corrected standalone Grotto version/manuals. The literal `(Preview)` search now finds only `apps/grotto/docs/pairing.md:1`, but a case-insensitive `preview` search exposes this unparenthesized long-description case. The passing desktop-contract assertion at `apps/grotto/tests/test-desktop-contract.py:103-105` checks parenthesized wording in the standalone recipe files, not this split-package description. F2 therefore remains open only for this missed metadata sentence. Preserve the release-acceptance caveat when reconciling its obsolete name/version wording; do not treat cleanup as release acceptance. No source was fixed here.

### Verification repeated

Fresh `nostrc-linux-ci:arm64` container, same image ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`, reporting `aarch64`; read-only source mount and a fresh task-owned build volume. Repeated the initial review's CMake configuration and targeted build with `--parallel 1`, then executed checks serially. Container-only dependencies included xdotool, RPM diagnostics and debhelper; no developer keychain commands were run.

| Check | Actual result |
|---|---|
| Same seven-check `xvfb-run -a ctest --test-dir /build --parallel 1 --no-tests=error` selection: `grotto-version`, `grotto-packaging-version`, `signer/version`, `signer/ui-layout`, `signer/ui-accessibility`, `signer/blueprint-drift`, `signer/desktop-contract` | **PASS 9/9** including both isolation fixtures; no skips; 165.38 seconds. |
| Newly built GUI and daemon `--version` | Both show **0.1.1-alpha1**. |
| `desktop-file-validate`; `appstreamcli validate --no-net` on canonical inputs | Both exit 0; AppStream retains one non-failing pedantic hint. |
| Repeated manifest-cell diagnostic | Numeric base accepted; full prerelease cell refused as expected; F1 resolved. |
| Real `dh_installman` fixture using production inputs | Both generated manuals installed with **Grotto 0.1.1-alpha1** titles, no Preview/token residue. |
| Literal `(Preview)` grep over `apps/grotto packaging data po`, plus case-insensitive grep across standalone/split packaging, canonical desktop/metainfo and `flake.nix` | Literal search: only the previously excluded pairing documentation heading. Broader search: the current split Debian long Description above remains; **F2 cleanup is not complete**. Other matches are documentation/history or legitimate operation/link previews. |
| Real split-package `dpkg-gencontrol` metadata fixture | Command exits 0 and **reproduces the remaining F2 sentence**. Not a complete `.deb` build/install or release proof. |
| Repeated version ordering, catalogs, unchanged tag-guard and plist diagnostics | PASS: Debian prerelease sorts before final; librpm 4.18.2 returns -1 for both tested comparisons (`rpmdev-vercmp` unavailable); all five Grotto catalogs plus panels Spanish pass `msgfmt --check --check-format`, no fuzzy flags; matching alpha1 guard exits 0 and the same three mismatches exit 1; both plist paths retain numeric **0.1.1**. |
| Host `PYTHONPATH=tests/selection python3 -m unittest test_release_plan` | **14/15 pass**; all three Grotto cases pass. The sole failure remains `25 != 24` in `nostrc-p4bcf`; master confirmation is the original review's evidence, not a fresh master run. |
| `git diff --check 9452e538a...9135187b6` | PASS. |

New receipts and probe scripts are retained under ignored `.cache/grotto-version-recheck-9135187b6/`, especially `evidence/{ctest,manifest-base,manifest-full-negative,manual-install,split-control,preview-literal,preview-packaging-casefold,diagnostics,release-module}.log`. The task-owned container, build volume and fixture copies were removed after verification. No full suite, broad gate, native package build, hosted workflow, live service, tag creation, merge or publication was performed. This report append is documentation-only: no component bump; Grotto stays base **0.1.1** / full **0.1.1-alpha1**, grotto-panels **0.1.0**.

**Final verdict at 9135187b6: request changes for the single remaining F2 split-Debian description. F1 and the generated-manual portion of F2 are resolved, and all requested serial ARM64 checks pass.** No new audit or broader qualification is required; the unrelated baseline issues remain separate.
