# Agent Instructions

This project uses **bd** (beads) for issue tracking. Run `bd onboard` to get started.

## Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --status in_progress  # Claim work
bd close <id>         # Complete work
bd sync               # Sync with git
```

## Setup

Install git hooks before starting work:
```bash
scripts/install-hooks.sh
```

This installs the repository pre-push hook. Its delivery policy is static checks
plus dependency-aware affected execution, with an explicit broad override.

**Dependency-aware delivery policy — owner reset, 2026-10-25:**

The delivery acceptance contract is exactly these three outcomes:

1. **Correct affected-test selection.** Select checks from changed inputs and
   downstream consumers, including relevant compatibility checks. Build their
   prerequisites without selecting unrelated prerequisite tests. Explain inclusion,
   omission and conservative broad fallback for unknown paths or incomplete
   relevant dependency information.
2. **Operational defaults.** Local development, pre-push and PR checks default to
   affected selection. Master/integration, release and manual runs are broad;
   an explicit full override remains available. There is no scheduled/nightly tier.
3. **Focused verification.** Representative changes demonstrate relevant inclusion
   and unrelated omission, including compatibility where relevant. Failures,
   cancellations, unavailable prerequisites and unexecuted checks remain honest;
   planning, skips and partial diagnostics are not passing execution.

These replace the former W11/W12 delivery blockers: no PR/broad-run quotas,
seven-class/five-sample baselines, savings thresholds, benchmarks, universal
custody/profile parity, exhaustive baseline repairs or new acceptance tasks.
Existing execution safety guards still apply to the routes that use them; this
reset does not turn failed or incomplete qualification receipts into success.
Prior qualification/profile hardening and unrelated bugs are nonblocking
follow-ups, not prerequisites to the three outcomes. Parent verification of the
integrated delivery remains necessary; this docs change alone proves no cutover.

Use `scripts/test-gate.sh` for affected checks, `--explain` for plan-only output,
and `--broad` for broad selection. `--profile` is a deliberately partial run,
not whole-profile or release proof. `NOSTRC_FULL_GATE=1 git push` retains the full
override. Missing required capabilities must fail visibly, not silently drop
selected checks. A bypass must remain explicit and cannot be called verification.

See [TESTING](docs/TESTING.md#dependency-aware-delivery-policy--2026-10-25) for
commands and evidence limits, and the [plan execution index](docs/plans/dependency-aware-testing-ci-2026-10-15.md#execution-index)
for W11/W12 and task ownership. Historical implementation restrictions and
source-specific receipts are retained in TESTING and the
[acceptance audit](docs/reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md);
their obsolete delivery blockers are superseded, not claimed satisfied.


---

## Component Versioning Policy

The independently releasable components tracked in `VERSION_MANIFEST.md` use
[Semantic Versioning](https://semver.org/) and component-prefixed tags. Prerelease
tags may append a SemVer prerelease identifier, for example
`gnostr-v0.1.0-preview`; their authoritative version sources retain the base
version (`0.1.0`), while the manifest records the full prerelease version. Any change
to a tracked component must assess its version impact before merge. A breaking
change must not land without the corresponding version-source update.

Public surfaces include installed headers and ABI, exported functions/types,
CMake or pkg-config contracts, plugin APIs, supported CLI flags or
machine-readable output, and documented configuration, storage, or wire formats.
For each affected component, apply the highest bump required by the change:

- **MAJOR**: For a stable component (`1.0.0` or later), make any
  backward-incompatible public-surface change. Examples include removing or
  renaming an API, changing a function signature or public struct layout,
  changing documented behavior in a way that breaks callers, or requiring an
  incompatible config/storage/wire migration.
- **MINOR**: Add backward-compatible public functionality, add an optional
  protocol/format capability, or deprecate (without removing) a public API. For
  a `0.x` component, which is explicitly in initial development, use a MINOR
  bump for a breaking public-surface change and call out the incompatibility in
  the release notes.
- **PATCH**: Make a backward-compatible bug or security fix, performance
  improvement, packaging/build correction, or other shipped change that adds no
  incompatible behavior or public API.
- **No bump**: Purely internal, test-only, or documentation-only changes that do
  not alter a shipped artifact. State that assessment in the bead or review
  summary.

Reset lower-order fields after a bump and apply one bump per component, not one
repository-wide bump. If a change affects multiple components, evaluate and bump
each independently.

### Updating versions and the manifest

Until release automation is implemented, version maintenance is deliberately
manual because several components have duplicate CMake/Meson version sources and
`libnostr` and `libgo` do not yet have authoritative version declarations.

1. Identify every affected component and the required bump using the criteria
   above.
2. Update every authoritative source listed for that component in
   `VERSION_MANIFEST.md`; duplicated CMake and Meson values must remain equal.
3. In the same commit, update the manifest's **Declared version**. Do not change
   **Latest release** until that exact version has a matching published
   `<component>-v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]` tag. For a prerelease,
   keep **Declared version** at the base `MAJOR.MINOR.PATCH` and record the full
   prerelease version under **Latest release**.
4. During a release, verify the declared version against all listed sources and
   existing tags, create/push the component tag, then update **Latest release**
   and **Release tag** in the manifest.
5. Mention all version decisions (including “no bump”) in the bead and peer
   review request.

The planned release-tagging script must validate source/manifest agreement and
update the release columns automatically. Until it lands, the checklist above
is the required process.

---

## Pre-Push Requirements

Use the affected gate for the change being pushed, plus the hook's static checks.
A blanket clean root build/full CTest is **not** a prerequisite for every push.
The gate must build selected checks' prerequisites and execute selected checks:

```bash
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --explain  # plan only
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA"
```

The hook validates detached pushed trees, not dirty caller files. Without explicit
`--head`, `scripts/test-gate.sh` uses the working-change snapshot for developer
feedback. A selected build/test failure blocks success; report it honestly and
fix relevant delivery defects rather than omitting the check. Unrelated baseline
bugs stay tracked separately and are not a mandate to repair the whole repository.

The pre-push hook (`scripts/pre-push`) retains static checks. For each
pushed commit it runs `git diff --check` over the pushed range and, on the
pushed tree in a temporary worktree, the static checks CI would otherwise
report late: `scripts/check-unsequenced-args.py` (no call may modify a variable
in one argument and use it in another; x86_64 GCC evaluates arguments right to
left), `scripts/check-gui-test-gdk-frame.py`, `scripts/check-linux-ci-packages.py`
and `gnome/groundhog/tests/check_privacy.py`. `NOSTRC_SKIP_PRE_PUSH=1` skips
them (announced, not verification). The affected gate supplies dependency-aware
execution; broad master/release/manual routes and the full override remain distinct.
`.40` and `.41` own implementation of this policy; their source and focused results,
not this documentation commit, establish that the defaults are operational.

The explicit full override is `NOSTRC_FULL_GATE=1 git push`;
`scripts/full-gate.sh` retains its full-gate adapter role. Broad selection must
not silently narrow to affected-only coverage. Platform/capability restrictions,
resource locks, private services and sanitizer safeguards remain in force.
A skip/bypass is incomplete verification, not a broad pass. Leaks in Groundhog
are fixed, not suppressed; `lsan.supp` is only for leaks in other libraries,
each naming its bead.

### Focused verification

Verify affected inclusion and unrelated omission with representative selector,
adapter or workflow tests appropriate to the change. Relevant compatibility tests
belong in affected coverage; unrelated expensive suites do not. `--explain` alone
is not execution. No fixed sample count, benchmark or exhaustive baseline repair
is required. Retain failures and report the exact verification scope.

For deliberately broad local verification use `scripts/test-gate.sh --broad` or
`NOSTRC_FULL_GATE=1 git push`; a profile restriction still limits coverage.

## Landing the Plane (Session Completion)

**When ending a work session**, follow the assigned landing boundary. For an
explicitly local-only delegated task, commit locally, update its bead and hand the
commit to the parent; do not push or merge. Otherwise complete the steps below,
including a successful push.

**MANDATORY WORKFLOW (when landing/pushing is assigned, not local-only handoff):**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd sync
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES (when landing/pushing is assigned):**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds

---

## Enforcement

Violations of the pre-push requirements result in immediate revert and reassignment.

The pre-push hook blocks pushes that fail its static checks; CI (path-scoped per component) verifies builds and tests.


<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

## Session Completion

**When ending a work session**, follow the assigned landing boundary. For an
explicitly local-only delegated task, commit locally, update its bead and hand the
commit to the parent; do not push or merge. Otherwise complete the steps below,
including a successful push.

**MANDATORY WORKFLOW (when landing/pushing is assigned, not local-only handoff):**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES (when landing/pushing is assigned):**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->

## Developer Keychains (MANDATORY)

A developer's macOS default keychain and search list were once left pointing at
a throwaway test keychain under `/tmp` (nostrc-2hmd): everything apps saved for
days went to a file a reboot then deleted, and the login keychain had to be
restored by hand.

- **Never** run `security default-keychain -s`, `security list-keychains -s`,
  `security login-keychain -s`, `security delete-keychain` or
  `security set-keychain-settings` on a developer machine, and never call
  `SecKeychainSetDefault`, `SecKeychainSetSearchList` or
  `SecKeychainSetDomainSearchList`. Not for isolation, not temporarily.
- A test that needs a keychain creates one with `SecKeychainCreate` in a
  temporary directory and addresses it **by reference** (`kSecUseKeychain`,
  `kSecMatchSearchList`), wrapped in `tests/common/nostrc-test-keychain-guard.h`
  so it proves the default keychain and search list are unchanged.
- Smoke and manual test builds on macOS are configured with
  `-DNIP55L_SECRET_BACKEND=libsecret -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`
  and run against a throwaway Secret Service on a private bus; they never
  store test keys in the login keychain.
- `scripts/check-macos-keychain.sh` (read-only) runs in the opt-in full gate
  (`NOSTRC_FULL_GATE=1`, `scripts/full-gate.sh`) before and after the tests and blocks the push if the configuration is not
  the login keychain. If it fails, **stop and tell the owner**; only the
  machine's owner repairs it. Never choose "Reset To Defaults" in a keychain
  dialog.

## Banned Relays

**NEVER add `relay.damus.io` (or `wss://relay.damus.io`) anywhere in this codebase** — not in code, defaults, configs, docs, examples, or tests. It has been deliberately purged due to unreliability. Do not reintroduce it under any circumstances, even as an example URL. Use `wss://nos.lol` or `wss://relay.nostr.band` instead.

(The only remaining occurrences are in vendored `third_party/nostrdb/testdata/` fixtures, which contain signed events that cannot be modified without breaking signatures. Do not add new ones.)

## Nostr Protocol Guidelines

This project is Nostr-native — keep it event-driven, not request/response or polling:

- Subscribe with `REQ` filters and react to `EVENT`/`EOSE`; never poll relays on a timer or close a subscription after an arbitrary delay.
- Check every publish: don't ignore `OK`/`OK false`, `CLOSED` reasons, or `AUTH` (NIP-42) challenges — a successful websocket send is not a successful publish.
- Backfill with `since`/`until`/`limit` and a clear catch-up → live boundary instead of sleeping and hoping for history.
- Scope filters tightly (kinds/authors/tags) rather than pulling broad sets and filtering client-side.
- Dedupe by `event.id` across relays/reconnects; handlers must be idempotent on replay.
- Probe relay capabilities with NIP-11 instead of hardcoding single-relay assumptions; reconnect with backoff and re-issue subscriptions.
- Before merging Nostr-touching changes, run a **Nostr Protocol Review** (the `nostr-protocol-smells` skill / `nostr-protocol-review` workflow) to catch polling, timeout-closes, ignored relay frames, and other protocol smells.

## Git Worktree & Branch Hygiene

- Once a feature branch's changes are merged into `main`/`master`, remove the worktree and delete the branch. Don't leave merged worktrees or stale branches lying around.
- Before removing a worktree, run `git status` inside it to confirm there's nothing uncommitted you'd lose — commit, stash, or discard first.
- Clean up with `git worktree remove <path>` and `git branch -d <branch>` (add `git push origin --delete <branch>` if the branch was pushed).
