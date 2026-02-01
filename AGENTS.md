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

This installs the pre-push hook that enforces build verification.

---

## Pre-Push Requirements (MANDATORY)

Before pushing ANY commits, you MUST complete these checks:

### 1. Build Verification

The pre-push hook enforces this automatically, but you should verify locally:

```bash
# Clean build
rm -rf _build && cmake -B _build && cmake --build _build
```

**If build fails, DO NOT PUSH. Fix the issue first.**

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

The pre-push hook blocks pushes that fail build verification. Peer review is enforced by process - agents are expected to follow the workflow.

