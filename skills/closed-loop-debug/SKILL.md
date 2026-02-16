---
name: closed-loop-debug
description: >
  Complete closed-loop debugging workflow for LLM agents working on nostrc GTK
  applications. Covers the full cycle: identify bug → write/run test → debug with
  GDB/LLDB → fix code → rebuild → verify via Broadway UI → iterate. Integrates
  the Broadway and GDB skills into a unified methodology that an LLM can execute
  deterministically.
allowed-tools: "Bash,Read,mcp__playwright__*,mcp__RepoPrompt__*"
version: "1.0.0"
---

# Closed-Loop Debug Workflow for nostrc GTK Apps

A deterministic methodology for LLM agents to identify, reproduce, diagnose, fix,
and verify bugs in gnostr, gnostr-signer, and other GTK4 applications in the nostrc
stack — without human intervention between iterations.

## The Loop

```
    ┌──────────────────────────────────────────────────────────┐
    │                                                          │
    ▼                                                          │
┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐
│ IDENTIFY │───►│ REPRO   │───►│ DIAGNOSE│───►│  FIX    │───►│ VERIFY  │
│ (test/   │    │ (test + │    │ (GDB/   │    │ (edit   │    │ (test + │
│  report) │    │  ASan)  │    │  LLDB/  │    │  code)  │    │  Bway)  │──┐
└─────────┘    └─────────┘    │  ASan)  │    └─────────┘    └─────────┘  │
                               └─────────┘                        │       │
                                                                  │ PASS  │ FAIL
                                                                  ▼       │
                                                               DONE ◄─────┘
```

Each phase uses specific tools and produces structured output that feeds the next phase.

## Phase 1: IDENTIFY — Find the Bug

### From a failing test

```bash
# Run the full test suite — find what fails
cd build && ctest --output-on-failure 2>&1 | tail -50

# Run a specific test category
ctest -R nostr_gtk --output-on-failure
ctest -R ndb-main-thread --output-on-failure
```

### From an ASan report

```bash
# Build with sanitizers
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON
cmake --build build-asan

# Run app or test — ASan reports are deterministic
ASAN_OPTIONS=detect_leaks=1 \
  build-asan/apps/gnostr/gnostr 2>&1 | tee /tmp/asan-report.txt
```

### From Broadway UI observation

```bash
# Start Broadway daemon (persists across rebuilds)
./skills/broadway-debug/scripts/run-broadway.sh

# Connect Playwright, take snapshot
# browser_navigate(url="http://127.0.0.1:8080")
# browser_snapshot()
# browser_take_screenshot(filename="bug-state.png")
```

### From a beads issue

```bash
bd ready                    # Find unblocked work
bd show <issue-id>          # Get context + reproduction steps
bd update <id> --status in_progress
```

**Output of this phase**: A clear statement of what's wrong, ideally with a
reproduction path (test command, ASan trace, or UI steps).

## Phase 2: REPRODUCE — Make It Deterministic

The goal is to have a **single command** that demonstrates the bug every time.

### For crashes / memory errors

```bash
# Best: existing test that exercises the path
build-asan/apps/gnostr/tests/gnostr-test-ndb-main-thread-violations

# If no test exists, write a minimal one:
# 1. Read the relevant source code
# 2. Create a test file using the testkit
# 3. Add it to CMakeLists.txt
# 4. Build and run
```

### For UI bugs

```bash
# 1. Start app in Broadway
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build/apps/gnostr \
  build/apps/gnostr/gnostr &

# 2. Use Playwright to reach the bug state
# browser_navigate(url="http://127.0.0.1:8080")
# browser_click(element="...", ref="eNN")
# browser_snapshot()  → capture the broken state

# 3. Document the repro steps for the fix/verify cycle
```

### For latency / performance bugs

```bash
# Use the NDB violation detection tests
build/apps/gnostr/tests/gnostr-test-ndb-main-thread-violations 2>&1

# Or the real-component bind latency test
xvfb-run -a build/apps/gnostr/tests/gnostr-test-real-bind-latency 2>&1
```

**Output**: A single command that fails consistently.

## Phase 3: DIAGNOSE — Find the Root Cause

### Strategy selection

| Bug Type | Primary Tool | Secondary Tool | Inspector Panel |
|----------|-------------|----------------|-----------------|
| Segfault | ASan build | GDB/LLDB `bt full` | — |
| Use-after-free | ASan (shows alloc+free) | GDB watchpoint | Objects (signal handlers) |
| Memory leak | LSAN / Valgrind | GObject ref count tracing | Objects (instance count) |
| Main-thread blocking | NDB violation test | GDB breakpoint on `storage_ndb_begin_query` | Statistics (frame times) |
| Widget sizing | GTK Inspector Visual panel | `gtk_widget_measure()` in test | **Visual + CSS** |
| Signal handler bug | GTK Inspector Objects panel | GDB break on `g_signal_*` | **Objects (Signals section)** |
| Latency | Heartbeat test | GDB + NDB violation count | Statistics + Recorder |

### GDB diagnosis (Linux)

```bash
# Run the failing test under GDB in batch mode
gdb -batch \
  -ex "set pagination off" \
  -ex "set print pretty on" \
  -ex "run" \
  -ex "bt full" \
  -ex "info threads" \
  -ex "thread apply all bt 10" \
  --args build-debug/apps/gnostr/tests/FAILING_TEST 2>&1
```

### LLDB diagnosis (macOS)

```bash
lldb -b \
  -o "run" \
  -o "bt all" \
  -o "thread list" \
  -k "bt all" \
  -k "quit" \
  -- build-debug/apps/gnostr/tests/FAILING_TEST 2>&1
```

### ASan diagnosis (cross-platform)

```bash
# ASan output is self-diagnosing — it tells you:
# 1. What happened (heap-use-after-free, stack-buffer-overflow, etc.)
# 2. Where it happened (the read/write that crashed)
# 3. Where the memory was allocated
# 4. Where the memory was freed (for UAF)
# 5. Which threads were involved
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
  build-asan/apps/gnostr/tests/FAILING_TEST 2>&1
```

**Output**: The specific function, line number, and root cause mechanism.

## Phase 4: FIX — Apply the Change

### Common fix patterns

**Use-after-free in callback**:
```c
// BEFORE (bug): callback uses freed user_data
g_signal_connect(source, "notify::profile", G_CALLBACK(on_profile), row);

// AFTER (fix): weak reference guards the callback
g_object_weak_ref(G_OBJECT(row), (GWeakNotify)invalidate_row_ref, ctx);
// OR: track handler ID and disconnect on unbind
self->profile_handler_id = g_signal_connect(...);
// In unbind:
if (self->profile_handler_id) {
    g_signal_handler_disconnect(item, self->profile_handler_id);
    self->profile_handler_id = 0;
}
```

**Main-thread NDB transaction**:
```c
// BEFORE (bug): NDB query on main thread
const char *content = storage_ndb_get_content(key);

// AFTER (fix): offload to worker thread
static void query_in_thread(GTask *task, ...) {
    const char *content = storage_ndb_get_content(key);
    g_task_return_pointer(task, g_strdup(content), g_free);
}
static void on_query_done(GObject *src, GAsyncResult *res, gpointer data) {
    char *content = g_task_propagate_pointer(G_TASK(res), NULL);
    // Update widget on main thread
}
g_task_run_in_thread(task, query_in_thread);
```

**Memory leak (missing unref)**:
```c
// BEFORE (bug): ownership not transferred
GObject *obj = g_object_new(MY_TYPE, NULL);
some_function(obj);  // Does not take ownership
// obj leaked!

// AFTER (fix): use g_autoptr or explicit unref
g_autoptr(GObject) obj = g_object_new(MY_TYPE, NULL);
some_function(obj);
// Automatically unreffed at scope exit
```

### Apply the edit

```
# Use apply_edits with verbose=true to see the diff
apply_edits(path="src/model/gn-nostr-event-model.c",
  search="old code...", replace="new code...", verbose=true)
```

## Phase 5: VERIFY — Confirm the Fix

### Step 1: Rebuild

```bash
cmake --build build-debug
# AND the ASan build if diagnosing memory issues
cmake --build build-asan
```

### Step 2: Run the reproducing test

```bash
# The SAME command from Phase 2 — must now pass
build-debug/apps/gnostr/tests/FAILING_TEST
# Or under ASan:
build-asan/apps/gnostr/tests/FAILING_TEST
```

### Step 3: Run the full test suite (no regressions)

```bash
cd build-debug && ctest --output-on-failure
```

### Step 4: Visual verification via Broadway (if UI-related)

```bash
# App is already running in Broadway? Kill and relaunch:
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build-debug/apps/gnostr \
  build-debug/apps/gnostr/gnostr &

# Playwright is still connected — verify:
# browser_snapshot()
# browser_take_screenshot(filename="after-fix.png")
```

### Step 5: Decision

- **PASS**: All tests green, UI looks correct → proceed to commit
- **FAIL**: Loop back to Phase 3 (diagnose) with new information

## Complete Example: Fixing a Recycling Crash

```
=== IDENTIFY ===
Running: ctest -R listview_recycle --output-on-failure
Result: FAIL — "profile notification after unbind" test crashes

=== REPRODUCE ===
Command: build-asan/nostr-gtk/tests/nostr-gtk-test-listview-recycle
ASan output:
  heap-use-after-free in on_profile_changed (note-card-factory.c:312)
  freed by factory_unbind_cb (note-card-factory.c:280)

=== DIAGNOSE ===
Reading note-card-factory.c:312 — the `notify::profile` handler
fires AFTER unbind. The handler_id was disconnected, but there's a
SECOND handler connected in on_ncf_row_mapped_tier2() that uses a
different signal name and wasn't tracked.

Root cause: `on_ncf_row_mapped_tier2` connects `notify::profile`
on the ITEM with ROW as user_data, but `factory_unbind_cb` only
disconnects handlers tracked in `row->profile_handler_id` — the
tier-2 handler has a different ID stored in a local variable that's
lost when the stack frame exits.

=== FIX ===
Store the tier-2 handler ID in the row struct.
Disconnect it in factory_unbind_cb.

apply_edits(path="nostr-gtk/src/note-card-factory.c", ...)

=== VERIFY ===
cmake --build build-asan
build-asan/nostr-gtk/tests/nostr-gtk-test-listview-recycle  → PASS ✅
cd build-asan && ctest --output-on-failure  → ALL PASS ✅
```

## App-Specific Commands

### gnostr

```bash
# Debug build
build-debug/apps/gnostr/gnostr

# Broadway
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build-debug/apps/gnostr \
  build-debug/apps/gnostr/gnostr

# Tests
cd build-debug && ctest -R gnostr --output-on-failure
```

### gnostr-signer

```bash
# Debug build
build-debug/apps/gnostr-signer/gnostr-signer

# Broadway
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build-debug/apps/gnostr-signer \
  build-debug/apps/gnostr-signer/gnostr-signer

# Tests
cd build-debug && ctest -R signer --output-on-failure
```

### Any GTK app in the stack

The same patterns apply to any GTK4 app built with the nostrc build system:

```bash
# Pattern:
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build-debug/apps/<APP_NAME> \
  build-debug/apps/<APP_NAME>/<BINARY>
```

## Iteration Tracking

For multi-iteration debug sessions, use beads to track progress:

```bash
# Start work on an issue
bd update <id> --status in_progress

# Add notes as you iterate
bd update <id> --note "Iteration 1: ASan shows UAF in factory_unbind_cb"
bd update <id> --note "Iteration 2: Found missing handler disconnect in tier-2 path"
bd update <id> --note "Iteration 3: Fix applied, all tests pass"

# Close when verified
bd close <id> --reason "Fixed handler disconnect in note-card-factory.c"
bd sync
```

## Parallel Debugging

For complex bugs that span multiple components:

### Terminal layout

```
┌─────────────────────┬─────────────────────┐
│ Terminal 1:         │ Terminal 2:         │
│ Broadway app        │ GDB session         │
│ (visual feedback)   │ (crash diagnosis)   │
├─────────────────────┼─────────────────────┤
│ Terminal 3:         │ Terminal 4:         │
│ Test runner         │ Code editor         │
│ (regression check)  │ (apply fixes)       │
└─────────────────────┴─────────────────────┘
```

### LLM agent workflow (single terminal)

```bash
# Background Broadway daemon (started once)
./skills/broadway-debug/scripts/run-broadway.sh &

# Foreground: iterate on test/fix cycle
while ! build-debug/tests/FAILING_TEST; do
  # Read ASan output, identify fix, apply edit, rebuild
  cmake --build build-debug --target FAILING_TEST
done

# Visual verification
# browser_snapshot()
# browser_take_screenshot(filename="verified.png")
```

## GTK Test Utilities Reference

For writing deterministic widget tests (used in Phase 2):

| Function | Purpose |
|----------|---------|
| `gtk_test_init(&argc, &argv, NULL)` | Initialize GTK in test mode |
| `gtk_test_widget_wait_for_draw(w)` | Wait for pending redraws |
| `gtk_widget_measure(w, orient, for_size, ...)` | Measure widget dimensions |
| `gtk_test_accessible_assert_role(w, role)` | Verify accessibility role |
| `gtk_test_accessible_assert_property(w, ...)` | Verify accessible properties |
| `g_test_add_func(path, func)` | Register a test case |
| `g_test_run()` | Run all registered tests |

## Related

- [`skills/broadway-debug/SKILL.md`](../broadway-debug/SKILL.md) — Broadway + Playwright details
- [`skills/gtk-inspector/SKILL.md`](../gtk-inspector/SKILL.md) — **GTK Inspector debugging (widget tree, CSS, GObject lifecycle, signals)**
- [`skills/gdb-debug/SKILL.md`](../gdb-debug/SKILL.md) — GDB/LLDB reference
- [`docs/TESTING.md`](../../docs/TESTING.md) — Full test suite documentation
- [`AGENTS.md`](../../AGENTS.md) — Agent workflow and commit policy
