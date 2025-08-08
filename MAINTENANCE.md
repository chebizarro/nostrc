# Documentation Maintenance

## When to Update

- API changes: any modification to public headers in `libnostr/include/`, `libgo/include/`, `libjson/include/`.
- Build changes: CMake options, dependencies, or target/link changes.
- New modules: new NIP directories under `nips/` or new libraries.
- Testing changes: add/remove tests or change how to run tests.
- Release: update versioning/packaging steps.

## What to Update

- README.md: Quick Start, install, usage examples, list of NIPs.
- ARCHITECTURE.md: component diagram, data flow, module map.
- API.md: new/changed public functions and types.
- DEVELOPMENT.md: local setup, troubleshooting, flags.
- CODING_STANDARDS.md: conventions and workflow.
- DEPLOYMENT.md: CI/CD, packaging, environment configuration.
- DATABASE.md: only if a database is introduced.
- .ai-context/AI_CONTEXT.md: conventions/patterns, domain terms.

## Process

1. Open a branch: `docs/<area>`.
2. Make code changes with accompanying doc edits in the same PR where feasible.
3. Run build and tests locally; include examples if they changed.
4. Have at least one reviewer verify technical accuracy.
5. Merge via PR; avoid committing directly to main.

## Consistency Checklist

- Terminology matches across files (Event, Relay, Subscription, NIP, etc.).
- Code snippets compile and reflect current APIs.
- CMake examples use actual targets and dependencies.
- Cross-references: ensure file paths and header names are correct.

## Automation Ideas

- Add a CI job to fail if public headers changed without modifications to API.md.
- Lint code snippets in docs using a small build in CI.
