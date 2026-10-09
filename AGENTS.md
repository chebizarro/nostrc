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

This installs the fast pre-push hook (static checks, no build; CI builds and tests).

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

## Pre-Push Requirements (MANDATORY)

Before pushing ANY commits, you MUST complete these checks:

### 1. Build Verification

Build and test what you touched before pushing:

```bash
rm -rf _build && cmake -B _build && cmake --build _build
```

**If build fails, DO NOT PUSH. Fix the issue first.**

The pre-push hook (`scripts/pre-push`) is fast (seconds, no build). For each
pushed commit it runs `git diff --check` over the pushed range and, on the
pushed tree in a temporary worktree, the static checks CI would otherwise
report late: `scripts/check-unsequenced-args.py` (no call may modify a variable
in one argument and use it in another; x86_64 GCC evaluates arguments right to
left), `scripts/check-gui-test-gdk-frame.py`, `scripts/check-linux-ci-packages.py`
and `gnome/groundhog/tests/check_privacy.py`. `NOSTRC_SKIP_PRE_PUSH=1` skips
them (announced). Full builds and tests run in CI, scoped per component by
workflow path filters.

The former full gate is kept as an explicit opt-in, `scripts/full-gate.sh`
(run it for a push with `NOSTRC_FULL_GATE=1 git push`): the macOS build and
full CTest run, in parallel a Linux stage (`scripts/linux-gate.sh`: GCC build
of every default target plus Groundhog in an Ubuntu 24.04 container, then a
smoke CTest subset; needs Docker, `NOSTRC_SKIP_LINUX_GATE=1` skips it,
`NOSTRC_GATE_AMD64=1` runs it under amd64 emulation), and a sanitizer stage
(`scripts/linux-gate.sh --sanitizers`) that reproduces `groundhog-ci.yml`'s
`groundhog-sanitizers` job as read by `scripts/sanitizer-gate-ci.py`, only
when the range touches its `SANITIZER_PATHS` (`NOSTRC_SKIP_SANITIZER_GATE=1`
skips it). Use it before pushing risky glibc/GCC-sensitive or leak-sensitive
Groundhog changes. Leaks in Groundhog are fixed, not suppressed; `lsan.supp`
is only for leaks in other libraries, each naming its bead.

### 2. Unit Tests

If touching code in `apps/`, `lib*/`, or `nips/`:

```bash
ctest --test-dir _build --output-on-failure
```

All tests must pass before push.

### 3. Peer Review (REQUIRED)

Before pushing, get another agent to review your changes:

1. **Create diff summary:**
   ```bash
   git diff HEAD~1 --stat
   git log -1 --pretty=format:"%s%n%b"
   ```

2. **Send for review:**
   ```bash
   gt mail send nostrc/<other-agent> -s "Review: <bead-id>" \
     -m "<description of changes and diff summary>" --type review
   ```

3. **Wait for approval** - Do NOT push until you receive APPROVED

4. **Push only after approval:**
   ```bash
   git push
   ```

### Reviewer Responsibilities

When you receive a review request:

1. Read the diff and description
2. Check for:
   - Code correctness
   - Obvious bugs or regressions
   - Pattern consistency with codebase
   - Missing error handling
3. Reply with either:
   - `APPROVED` - Author can push
   - `REQUEST CHANGES: <details>` - Author must address feedback

```bash
# Approve
gt mail send nostrc/<author> -s "Re: Review: <bead-id> APPROVED" -m "LGTM"

# Request changes
gt mail send nostrc/<author> -s "Re: Review: <bead-id>" -m "REQUEST CHANGES: <feedback>"
```

---

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

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

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds

---

## Enforcement

Violations of the pre-push requirements will result in:
1. **Broken builds**: Immediate revert and reassignment
2. **Skipped peer review**: Commit flagged for post-hoc review, pattern noted
3. **Repeated violations**: Escalation to Mayor

The pre-push hook blocks pushes that fail its static checks; CI (path-scoped per component) verifies builds and tests. Peer review is enforced by process - agents are expected to follow the workflow.


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

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

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

**CRITICAL RULES:**
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
