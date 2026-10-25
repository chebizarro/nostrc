# Working in nostrc

## Scope and delivery

- Deliver the user's requested outcome. The current explicit user direction takes
  precedence over older plans, bead descriptions and historical acceptance notes.
  Those artifacts track work; they do not authorize additional requirements.
- Keep the work finite: identify the requested behavior, the files that own it and
  the focused checks needed to verify it. Do not turn a focused change into a
  repository-wide audit, baseline repair, benchmark programme or qualification matrix.
- Fix regressions caused by your change. File unrelated bugs and optional hardening
  as separate beads; do not make them new prerequisites for delivery. If a genuine
  blocker prevents the requested behavior, explain the concrete dependency rather
  than silently expanding scope or redefining completion.
- Verify the real entrypoints and downstream behavior you changed. A planner,
  isolated helper or mocked test passing does not prove its production caller is
  wired correctly. A closed bead is not evidence that the deliverable works.
- Report what actually ran and its result. Distinguish plan-only output, skipped
  checks, partial runs and unavailable environments from passing execution. Do not
  present local, simulated or diagnostic evidence as hosted, native or release proof.
- Stop when the agreed outcome is delivered and verified within the stated scope.
  Do not add acceptance criteria, repeat audits or invent further work to justify
  keeping the task open. Report any remaining limits and separately tracked bugs.

## Issue tracking and collaboration

Use **bd (Beads)** for all task tracking, not parallel markdown TODO lists,
TodoWrite or TaskCreate. Run `bd onboard` for setup and `bd prime` for current
command help; keep generated help out of this file.

```bash
bd ready                         # Find unblocked work
bd show <id>                     # Read scope and current state
bd create "Title" --type task --priority 2 --description "Scope and reason"
bd update <id> --claim           # Claim before editing
bd close <id> --reason "Result and verification"
```

Keep scope, progress, verification and version decisions in the bead. Use
`bd remember` for durable project knowledge, not new `MEMORY.md` files. Use the
tracker's supported commands; do not hand-edit its database or exports. Use only
configured synchronization routes; report unavailable synchronization rather than
inventing a remote or repeatedly retrying an unchanged failure.

When delegating, assign disjoint ownership and an explicit landing boundary.
Local-only children commit and hand their changes to the coordinator; they do not
push or merge. The coordinator reviews and integrates them. Preserve other agents'
work and the user's pre-existing changes, stashes and resources.

## Testing and CI

Install the repository hooks before starting work:

```bash
scripts/install-hooks.sh
```

**The push hook is static-only.** `scripts/pre-push` checks whitespace over the
pushed range and runs cheap static checks against detached pushed trees, not dirty
caller files. It must never configure, rebuild, execute tests or start containers.
`NOSTRC_FULL_GATE` and `NOSTRC_SELECTIVE_GATE` are inert in this hook.
`NOSTRC_SKIP_PRE_PUSH=1` is an explicit bypass, not verification.

Builds and tests belong in CI or explicitly invoked local commands:

```bash
scripts/test-gate.sh                                        # Affected working changes
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --explain  # Plan only
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA"          # Execute affected checks
scripts/test-gate.sh --broad                                 # Explicit broad local run
```

- Local test commands and PR CI default to affected selection. Master/integration,
  release and manual CI runs are broad. There is no scheduled/nightly tier.
- Select checks from changed inputs and downstream consumers, including relevant
  compatibility checks. Build prerequisites without selecting unrelated tests for
  those prerequisites. Explain inclusions, omissions and conservative broad fallback
  for unknown paths or incomplete relevant dependency information.
- Saving a `--plan` does not request attestation. `--execution attested` or explicit
  custody options select that separate route. `--profile` restricts coverage;
  combining it with `--broad` does not prove unrestricted broad coverage.
- Use focused verification appropriate to the change, not every suite in the repo.
  For ordinary selector/workflow/runner changes, use
  `python3 scripts/test-selection-operational.py`. Experimental discovery and
  namespace/custody diagnostics are opt-in, not ordinary delivery gates.
- Do not add sample quotas, benchmarks, universal profile parity or exhaustive
  baseline repair to acceptance. Representative checks must demonstrate relevant
  inclusion and unrelated omission, including compatibility when affected.
- Missing required capabilities must fail visibly, not silently drop selected
  checks. Preserve failures and cancellations; a skip or bypass is not a pass.
  Broad runs must not silently narrow their selected coverage.
- Keep platform restrictions, resource locks, private-service isolation and
  sanitizer safeguards on the routes that use them. Fix Groundhog leaks rather
  than suppressing them; `lsan.supp` is only for other libraries' leaks, each
  suppression naming its bead. Do not weaken guards to manufacture a green run.

See [TESTING](docs/TESTING.md#dependency-aware-commands) for route details and
capability limits. `scripts/full-gate.sh` is a separately invoked legacy adapter;
the push hook must never delegate to it. Build/test failures belong to the explicit
test command or CI run, not automatic push prerequisites.

## Component versioning

Assess every affected independently releasable component in
[VERSION_MANIFEST.md](VERSION_MANIFEST.md). Public surfaces include headers/ABI,
exported APIs, build/pkg-config contracts, plugins, CLI/output, documented
configuration, storage and wire formats. Apply the highest applicable
[SemVer](https://semver.org/) change per component:

- **MAJOR:** incompatible public change for a stable component (`1.0.0` or later).
- **MINOR:** backward-compatible public functionality or deprecation; also breaking
  changes to a `0.x` component, with the incompatibility called out in release notes.
- **PATCH:** backward-compatible shipped bug/security/performance or packaging/build
  correction without new public functionality.
- **No bump:** internal, test-only or documentation-only changes that do not alter a
  shipped artifact. Record this decision too.

Reset lower-order fields after a bump. Update every authoritative version source
listed in the manifest and its **Declared version** in the same commit; duplicate
CMake/Meson values must agree. Do not land a breaking change without its version
update. Mention each component's decision in the bead and review summary.

Release tags use `<component>-v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]`. Verify source,
manifest and tag agreement before publishing. Update **Latest release** and
**Release tag** only for the matching published tag, not merely a planned release.
For prereleases, authoritative sources and **Declared version** retain the base
version; release columns record the full prerelease version.

## Safety constraints

### Developer keychains

- Never run `security default-keychain -s`, `security list-keychains -s`,
  `security login-keychain -s`, `security delete-keychain` or
  `security set-keychain-settings` on a developer machine. Never call
  `SecKeychainSetDefault`, `SecKeychainSetSearchList` or
  `SecKeychainSetDomainSearchList`, even temporarily for test isolation.
- Tests needing a keychain create one with `SecKeychainCreate` in a temporary
  directory and address it by reference (`kSecUseKeychain`, `kSecMatchSearchList`).
  Use `tests/common/nostrc-test-keychain-guard.h` to prove the developer's default
  keychain and search list are unchanged.
- macOS smoke/manual builds use `-DNIP55L_SECRET_BACKEND=libsecret` and
  `-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service` with a throwaway Secret
  Service on a private bus, never the login keychain for test keys.
- `scripts/check-macos-keychain.sh` is read-only and runs before/after tests in the
  explicitly invoked legacy full gate. If it reports an invalid default/search
  list, stop and tell the owner. Only the owner repairs it; never choose
  "Reset To Defaults" in a keychain dialog.

### Banned Relays

**NEVER add `relay.damus.io` (or `wss://relay.damus.io`) anywhere in this codebase** — not in code, defaults, configs, docs, examples, or tests. It has been deliberately purged due to unreliability. Do not reintroduce it under any circumstances, even as an example URL. Use `wss://nos.lol` or `wss://relay.nostr.band` instead.

Existing signed fixtures in `third_party/nostrdb/testdata/` are the exception;
do not modify their signed events or add new occurrences.

### Nostr protocol behavior

- Keep relay behavior event-driven: `REQ` filters, `EVENT`/`EOSE` and an explicit
  catch-up/live boundary. Do not poll on timers, close subscriptions after arbitrary
  delays or sleep and hope that backfill is complete. Page with `since`/`until`/`limit`.
- Handle `OK`/`OK false`, `CLOSED` reasons and `AUTH` (NIP-42). A successful websocket
  send is not a successful publish.
- Scope filters tightly; deduplicate by `event.id` across relays/reconnects and make
  replay handling idempotent. Probe capabilities with NIP-11; reconnect with backoff
  and reissue subscriptions.
- Before merging Nostr protocol/relay changes, run the `nostr-protocol-smells` skill
  or `nostr-protocol-review` workflow. Unrelated edits do not require a protocol audit.

## Landing and cleanup

For a local-only delegated task, commit your scoped changes, update the bead and
hand the commit to the coordinator. Otherwise, when landing/pushing is assigned:

1. Run relevant checks, record their actual results and file unrelated follow-up
   bugs separately. Close only completed work; identify any genuine blocker.
2. Inspect `git status` and the diff. Commit only owned changes and tracker exports
   produced through the supported workflow; never sweep unrelated work into a commit.
3. Fetch and integrate upstream changes safely, then push. Do not blindly rebase,
   stash or discard the user's dirty work. Verify the pushed commit and upstream
   status before reporting delivery. A push failure is not completion: diagnose it,
   fix task-owned causes and retry; report persistent access/external blockers
   rather than bypassing checks or looping without new evidence.
4. After integration, remove task-owned worktrees and merged feature branches.
   Check each worktree's `git status` before `git worktree remove <path>`, then use
   `git branch -d <branch>` and, if pushed, `git push origin --delete <branch>`.
   Preserve uncommitted work and unmerged branches.
5. Remove only task-owned transient containers, volumes and build/test artifacts.
   Do not globally prune Docker, delete unrelated worktrees or clear others' stashes.
6. Give a short handoff: what changed, what was verified, the landed commit and any
   remaining bugs or limits. Do not substitute a new report or ledger for delivery.

## Maintaining this file

Keep this file a current operational guide, not a session log. Store task history
and evidence in beads and detailed reference material in the relevant docs. Do not
add dated resets, milestone IDs, incident narratives, duplicated tool-generated
instructions or speculative future requirements. Edit the existing owning section
instead of appending another policy block. Remove superseded instructions when
changing policy, and check that commands and cross-references still agree.
