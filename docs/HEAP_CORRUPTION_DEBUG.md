# Heap Corruption Debugging Guide

This guide covers debugging Release-only heap corruption crashes (SIGSEGV, malloc errors) that don't reproduce under ASAN/Valgrind.

## Quick Start

```bash
# 1. Enable core dumps (one-time per session)
ulimit -c unlimited
sudo sysctl -w kernel.core_pattern=core.%e.%p.%t

# 2. Build the "relteeth" configuration
cmake --preset gnostr-relteeth
cmake --build --preset gnostr-relteeth -j

# 3. Run with heap tripwires
./run_and_dump.sh
```

Each crash produces a `bt.core.*.txt` file with full multi-thread backtrace.

---

## Build Configurations

### RelTeeth (Primary Debug Build)

"Release with teeth" - fast like Release but with debugging aids:

```bash
cmake --preset gnostr-relteeth
cmake --build --preset gnostr-relteeth -j
```

**Flags:**
- `-O2` - Release-level optimization (reproduces timing-sensitive bugs)
- `-g` - Debug symbols for readable backtraces
- `-fno-omit-frame-pointer` - Clean stack traces
- `-fno-strict-aliasing` - Disables aliasing UB exploitation
- `-fwrapv` - Signed overflow wraps instead of UB

**Binary:** `build/relteeth/apps/gnostr/gnostr`

### UBSan Release

Catches undefined behavior that often causes heap corruption:

```bash
cmake --preset gnostr-ubsan-release
cmake --build --preset gnostr-ubsan-release -j
```

UBSan often catches the *cause* of heap corruption (out-of-bounds, misaligned access) before the heap explodes.

---

## Scripts

### `tools/btcore.sh`

Extract backtrace from core dump:

```bash
tools/btcore.sh ./build/relteeth/apps/gnostr/gnostr core.gnostr.12345.1234567890
```

### `repro.sh`

Run with heap corruption detection:

```bash
./repro.sh
```

**Environment:**
- `MALLOC_CHECK_=3` - Abort on heap corruption
- `MALLOC_PERTURB_=165` - Fill freed memory with 0xA5 (catches use-after-free)
- Pinned to CPU 0, ASLR disabled for reproducibility

**Edit this file** to change binary path or arguments.

### `run_and_dump.sh`

Full iteration loop - run and capture backtrace:

```bash
./run_and_dump.sh
```

Produces `bt.core.*.txt` for each crash.

---

## Diagnostic Techniques

### 1. Allocator Swap

Different allocators often make "random" corruption become deterministic:

```bash
# jemalloc
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./build/relteeth/apps/gnostr/gnostr

# tcmalloc
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so.4 ./build/relteeth/apps/gnostr/gnostr
```

### 2. Strict Aliasing Test

If the bug disappears with `-fno-strict-aliasing` (relteeth build), you have type-punning UB:

```bash
# Build without -fno-strict-aliasing to test
cmake -B build/strict -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer"
```

Look for:
- `reinterpret_cast` / pointer casts between incompatible types
- Packed structs read through different pointer types
- Union type punning

### 3. Thread Sanitizer

If you suspect data races (one thread frees while another writes):

```bash
cmake --preset gnostr-debug-tsan
cmake --build --preset gnostr-debug-tsan -j
./build/debug-tsan/apps/gnostr/gnostr
```

---

## Common Release-Only Heap Corruption Causes

### 1. Strict Aliasing Violations

```c
// BAD: UB - optimizer may assume these don't alias
uint32_t *p = (uint32_t *)some_char_buffer;
*p = 0x12345678;
```

**Fix:** Use `memcpy` or `-fno-strict-aliasing`.

### 2. Data Races

```c
// Thread A
free(ptr);

// Thread B (racing)
ptr->field = value;  // Use-after-free!
```

**Fix:** Proper synchronization, atomic operations.

### 3. Signed Integer Overflow

```c
// BAD: UB in Release, optimizer may remove bounds check
int size = user_input * 4;  // Can overflow
if (size < 0) return;       // Optimizer removes this!
```

**Fix:** Use `-fwrapv` or unsigned types.

### 4. Uninitialized Memory

```c
int arr[10];
return arr[idx];  // UB if arr not initialized
```

Debug builds zero-initialize; Release doesn't.

---

## Iteration Workflow

1. **Run:** `./run_and_dump.sh`
2. **Crash:** Core dump created, backtrace saved
3. **Analyze:** Read `bt.core.*.txt`
4. **Hypothesize:** Form theory about root cause
5. **Instrument:** Add logging/assertions near suspect code
6. **Rebuild:** `cmake --build --preset gnostr-relteeth -j`
7. **Repeat**

### Speed Tips

- Use `ccache` for fast rebuilds:
  ```bash
  sudo apt install ccache
  export CMAKE_C_COMPILER_LAUNCHER=ccache
  export CMAKE_CXX_COMPILER_LAUNCHER=ccache
  ```

- Use Ninja (already default in presets)

---

## Checklist When Stuck

- [ ] Does UBSan build catch anything?
- [ ] Does TSan build show races?
- [ ] Does jemalloc/tcmalloc change behavior?
- [ ] Does `-fno-strict-aliasing` "fix" it? (indicates aliasing UB)
- [ ] Does `-O1` instead of `-O2` change behavior?
- [ ] Are there recent changes to threading code?
- [ ] Are there recent changes to memory management?
- [ ] Check `libgo/src/channel.c` for channel-related races
- [ ] Check `libnostr/src/connection.c` for websocket races

---

## Known Issues

See system-retrieved memories for known heap corruption patterns:
- LMDB reader slot exhaustion (rc=1003)
- GTK4 list item recycling race conditions
- Profile fetch thread explosion
- Websocket callback `conn->priv` races
