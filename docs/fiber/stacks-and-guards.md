# Stacks and Guard Pages

This page covers fiber stack allocation, guard pages, and safety considerations.

## Allocate/Free

- Implementation: `libgo/fiber/stack/stack.c`
- Fibers are allocated with a contiguous virtual region:
  - [Guard][Usable Stack][Guard]
  - Guard pages on both low and high ends are `PROT_NONE`.
- On POSIX: `mmap` is used for the region, guard pages set with `mprotect`.
- On Windows: `VirtualAlloc`/`VirtualProtect` are used.

## Sizing and Alignment

- Minimum stack size enforced (e.g., 16 KiB) and rounded to page size.
- Usable stack aligns to 16 bytes for ABI compliance (x86_64 and arm64).
- macOS x86_64: System red zone (128 bytes below `RSP`) is respected by aligning and placing the trampoline frame above the low guard.

## Overflow/Underflow Protection

- Any write into a guard page triggers an immediate fault, making stack bugs fail-fast.
- Overrun into high guard catches excessive growth; underrun into low guard catches corrupted prologue frames.

## Diagnostics

- ASan/UBSan builds are supported. Run:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DGOF_PORTABLE_CONTEXT=OFF -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON
cmake --build build-asan -j
ctest -j4 --output-on-failure
```

## Security Hardening

- User-provided stack sizes are validated and clamped to sane minimums.
- Future work: per-fiber stack watermarking and usage reporting to `gof_info.stack_used`.
