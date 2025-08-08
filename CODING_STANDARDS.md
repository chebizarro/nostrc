# Coding Standards

## Naming

- Types: `CamelCase` for public structs (e.g., `NostrEvent`), `snake_case` for internal types.
- Functions: `snake_case` with module prefix when helpful (e.g., `event_serialize`, `relay_connect`).
- Macros/consts: `UPPER_SNAKE_CASE`.
- Files: `snake_case.c/h` grouped by concept (e.g., `event.c`, `relay.h`).

## Layout & Formatting

- C standard: C11.
- Indent: 4 spaces, no tabs.
- Line length: aim ≤ 100 chars.
- Braces: K&R style.
- One header per C file with matching name when feasible.

## Comments & Docs

- Public headers: document each public function with brief description, parameters, return values, and ownership rules.
- Use `//` for single-line, `/* ... */` for blocks.
- Reference error codes from `libnostr/include/error_codes.h` where applicable.

## Project Organization

- Public headers in `include/` per module.
- Internal helpers are `static` in `.c` files; avoid leaking symbols.
- Tests in `tests/` with small focused executables.

## Git Workflow

- Branch naming: `feat/<topic>`, `fix/<issue>`, `docs/<area>`, `chore/<task>`.
- Commits: Conventional Commits style.
  - `feat: add relay backoff`
  - `fix: correct event signature verification`
  - `docs: update API for NostrEvent`
- PRs: include tests and doc updates.

## Error Handling & Memory

- Return negative error codes or `NULL` on failure; document expectations.
- Ensure each `create_*` has a corresponding `free_*`.
- Avoid global mutable state; when necessary, guard with `nsync` primitives.

## Testing

- Add tests alongside module introducing new behavior.
- Deterministic, no network in unit tests. For integration tests, gate behind CTest labels.

## Code Style Tools

- Recommend `clang-format` with a project `.clang-format` (to be added) using LLVM style with 4-space indents.
