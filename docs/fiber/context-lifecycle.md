# Context switch lifecycle

This page explains the lifecycle of a fiber context switch in `libgo`, from initialization to first entry and subsequent swaps.

## Pieces

- `libgo/fiber/context/context.h`
  - Declares `gof_context` and per-arch saved register layout.
- `libgo/fiber/context/context_asm.c`
  - `gof_ctx_init_bootstrap()` prepares a new fiber stack and sets the initial return address to the trampoline.
- Assembly backends
  - macOS: `context_arm64.S`, `context_x86_64.S`
  - Linux: `context_arm64_linux.S`, `context_x86_64_linux.S`
  - Provide `gof_trampoline` and `gof_ctx_swap`.

## Flow

1) Initialization (`gof_ctx_init_bootstrap`)

- Allocates/receives the fiber stack area and sets a 16-byte–aligned SP.
- Stores the entry function pointer and `void *arg` in the `gof_context` struct.
- Pushes an initial return address so that the first resume returns into `gof_trampoline`.

2) First run (`gof_trampoline`)

- Loads `entry` and `arg` from `gof_context`.
- Calls `entry(arg)`.
- If `entry` returns, the trampoline safely parks or exits the fiber (implementation-defined; typically returns to the scheduler).

3) Subsequent switches (`gof_ctx_swap`)

- Saves callee-saved registers and SP from the "from" fiber into its `gof_context`.
- Restores the callee-saved registers and SP of the "to" fiber from its `gof_context`.
- Returns into the previously saved location in the "to" fiber (which is either inside the fiber body or the trampoline on first run).

## Stack sketch (not to scale)

```
| higher addresses                         |
| ---------------------------------------- |
| guard page (no access)                   |
| fiber stack payload ...                  |
| saved return address -> gof_trampoline   | <- initial SP after bootstrap
| ...                                      |
| guard page (no access)                   |
| lower addresses                          |
```

## ABI-specific notes

- x86_64 (System V)
  - Saves/restores callee-saved GPRs: `rbx, rbp, r12–r15` and `rsp`.
  - Preserves red-zone rules (stack aligned to 16 bytes on calls).
  - Uses an ELF symbol name on Linux and Mach-O with leading underscore on macOS.
- arm64 (AAPCS64)
  - Saves/restores callee-saved GPRs: `x19–x28`, `fp`(x29), `lr`(x30), and `sp`.
  - Saves/restores callee-saved SIMD: `q8–q15`.
  - Maintains 16-byte SP alignment across calls.

## Related tests

- See `libgo/fiber/tests/` and the `GoFiber` ctest filter.
- `GoFiberStarvationTest` exercises fairness with frequent `gof_yield()`.
