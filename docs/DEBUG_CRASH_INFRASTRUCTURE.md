# Crash Debugging Infrastructure

This document describes the debugging infrastructure for reproducing and diagnosing heap corruption crashes in gnostr.

## Quick Start

```bash
# 1. Run with channel debug logging
NOSTR_CHAN_DEBUG=1 ./gnostr

# 2. Run with quarantine mode (channels never freed - proves UAF)
NOSTR_CHAN_QUARANTINE=1 ./gnostr

# 3. Run with glibc malloc debugging (catches corruption earlier)
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165
./gnostr

# 4. Full debug mode (all of the above)
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165
NOSTR_CHAN_DEBUG=1 NOSTR_CHAN_QUARANTINE=1 ./gnostr

# 5. Pin to single CPU + disable ASLR for determinism
taskset -c 0 setarch "$(uname -m)" -R ./gnostr
```

## Components

### 1. Stress Scroll Mode (`GNOSTR_STRESS_SCROLL=1`)

**Purpose:** Turn "scrolling triggers crash" into a deterministic, reproducible test.

**How it works:**
- Creates a ~200Hz scroll loop that rapidly adjusts the timeline scroll position
- Bounces between top and bottom of the timeline
- Triggers high-frequency model invalidations, widget disposal, and signal emission
- Logs progress every 1000 iterations

**Files:**
- `apps/gnostr/src/ui/stress_scroll.h`
- `apps/gnostr/src/ui/stress_scroll.c`

**Output:**
```
[STRESS_SCROLL] Stress scroll mode ENABLED
[STRESS_SCROLL] Interval: 5ms, Step: 200px
[STRESS_SCROLL] Starting stress scroll test...
[STRESS_SCROLL] iterations=1000 bounces=5 rate=198.2/s value=4200
```

### 2. Channel Debug Mode (`NOSTR_CHAN_DEBUG=1`)

**Purpose:** Add detailed logging and validation to GoChannel operations.

**Features:**
- Logs every channel create/unref with pointer and ref count
- Validates magic number at every entrypoint
- Detects double-free attempts
- Tracks allocation IDs for correlation

**Files:**
- `libgo/include/channel_debug.h`
- `libgo/src/channel_debug.c`
- `libgo/src/channel.c` (modified)

**Output:**
```
[GO_CHAN_DEBUG] Debug mode ENABLED
[GO_CHAN_DEBUG] CREATE chan=0x55a1b2c3d4e0 alloc_id=42 cap=16
[GO_CHAN_DEBUG] UNREF chan=0x55a1b2c3d4e0 refs=2->1
[GO_CHAN_DEBUG] UNREF chan=0x55a1b2c3d4e0 refs=1->0
```

### 3. Quarantine Mode (`NOSTR_CHAN_QUARANTINE=1`)

**Purpose:** Prove whether crashes are caused by use-after-free in channels.

**How it works:**
- Channels are NEVER freed, only poisoned
- Magic number set to `0xDEADBEEF` (FREED)
- Hot fields filled with `0xA5` poison bytes
- Leaked channel count logged periodically

**Interpretation:**
- If crash **disappears** with quarantine: Bug is UAF in channel memory
- If crash **persists**: Bug is elsewhere (GTK, other primitives)

**Output:**
```
[GO_CHAN_DEBUG] Quarantine mode ENABLED (channels will never be freed)
[GO_CHAN_DEBUG] QUARANTINE: chan=0x55a1b2c3d4e0 poisoned but NOT freed
[GO_CHAN_DEBUG] Quarantine: 100 channels leaked (intentionally)
```

### 4. Magic Number Validation

**Existing:** `GO_CHANNEL_MAGIC = 0xC4A77E10`

**New states:**
- `GO_CHAN_MAGIC_ALIVE = 0xC4A77E10` - Channel is valid
- `GO_CHAN_MAGIC_CLOSING = 0xC4A77E11` - Channel is being destroyed
- `GO_CHAN_MAGIC_FREED = 0xDEADBEEF` - Channel has been freed

**Behavior:**
- Access to FREED channel → immediate abort with stack trace
- Access to CLOSING channel → warning (may be legitimate briefly)

## Debugging Workflow

### Step 1: Reproduce with Stress Scroll

```bash
GNOSTR_STRESS_SCROLL=1 ./gnostr
```

Wait for crash. Note the iteration count when it crashes.

### Step 2: Enable Channel Debug

```bash
GNOSTR_STRESS_SCROLL=1 NOSTR_CHAN_DEBUG=1 ./gnostr 2>&1 | tee crash.log
```

Look for:
- `FATAL: Use-after-free!` - Direct evidence of UAF
- `FATAL: Invalid magic` - Corrupted channel pointer
- `FATAL: Double-free detected` - Over-unref bug

### Step 3: Confirm with Quarantine

```bash
GNOSTR_STRESS_SCROLL=1 NOSTR_CHAN_QUARANTINE=1 ./gnostr
```

If crash disappears, the bug is in channel lifetime management.

### Step 4: Use glibc Malloc Debugging

```bash
export GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.tcache_max=0
export MALLOC_CHECK_=3
export MALLOC_PERTURB_=165
GNOSTR_STRESS_SCROLL=1 ./gnostr
```

This often turns "crashes later in GTK" into "crashes immediately at the bad free/write".

### Step 5: Pin to Single CPU

```bash
taskset -c 0 setarch "$(uname -m)" -R ./gnostr --stress-scroll
```

Reduces timing variability for more reproducible crashes.

## Phase Isolation

Use these env vars to isolate which subsystem is causing corruption:

```bash
# Test 1: Network on, UI updates off
GNOSTR_DISABLE_UI_UPDATES=1 ./gnostr

# Test 2: Network off, UI updates on (with cached data)
GNOSTR_DISABLE_NETWORK=1 ./gnostr

# Test 3: Single relay (reduce concurrency)
GNOSTR_SINGLE_RELAY=wss://relay.damus.io ./gnostr

# Test 4: Serialized relay connections
GNOSTR_SERIALIZE_RELAYS=1 ./gnostr
```

**Interpretation:**
- Crash with (1) only → Bug is in network/channel code, not UI
- Crash with (2) only → Bug is in UI/GTK code
- Crash disappears with single relay → Race condition between relays
- Crash disappears with serialized → Concurrent connection race

## Environment Variables Summary

| Variable | Purpose |
|----------|---------|
| `GNOSTR_STRESS_SCROLL=1` | Enable scroll stress test |
| `GNOSTR_DISABLE_UI_UPDATES=1` | Suppress list model signals |
| `GNOSTR_DISABLE_NETWORK=1` | No websocket connections |
| `GNOSTR_SINGLE_RELAY=<url>` | Use only this relay |
| `GNOSTR_SERIALIZE_RELAYS=1` | Connect relays one at a time |
| `NOSTR_CHAN_DEBUG=1` | Enable channel debug logging |
| `NOSTR_CHAN_QUARANTINE=1` | Never free channels (leak intentionally) |
| `NOSTR_SPIN_ITERS=N` | Channel spin iterations before parking |
| `NOSTR_SPIN_US=N` | Microseconds per spin iteration |

## Interpreting Results

### Crash in `malloc_consolidate()` or `free()`
→ Heap corruption, likely UAF. Enable quarantine to confirm.

### Crash with "invalid magic"
→ Channel pointer is garbage or freed. Check ref counting.

### Crash in GTK/GLib with quarantine enabled
→ Bug is NOT in channel lifetime. Look at:
- GTK widget disposal
- GObject ref counting
- Signal handler lifetime

### No crash with quarantine
→ Bug IS in channel lifetime. Implement proper sync_state refcounting per the recommendations.

## Next Steps if UAF Confirmed

1. **Split lifetime domains**: Move mutex/waiter state into separately refcounted `sync_state` struct
2. **Hard barrier at close**: Wait for `waiter_count == 0` before any cleanup
3. **Immortal sync storage**: As diagnostic, allocate sync state from never-freed arena

See the original recommendations for detailed implementation guidance.
