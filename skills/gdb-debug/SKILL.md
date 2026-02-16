---
name: gdb-debug
description: >
  Debug memory errors, segfaults, and logic bugs in nostrc GTK applications using
  GDB (Linux) and LLDB (macOS). Covers ASan-assisted debugging, GObject reference
  counting, signal handler tracing, NDB transaction analysis, and structured
  workflows for LLM-driven crash diagnosis.
allowed-tools: "Bash,Read,mcp__RepoPrompt__*"
version: "1.0.0"
---

# GDB / LLDB Memory & Logic Debugging

Systematic debugger workflows for diagnosing segfaults, memory leaks, use-after-free,
and logic errors in the nostrc C/GTK4 stack. Designed for LLM agents who can run
`gdb`/`lldb` commands via Bash and interpret structured output.

## Prerequisites

- **Linux**: `gdb` installed, app built with `-g` (debug symbols)
- **macOS**: `lldb` installed (ships with Xcode CLT)
- Debug build: `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
- Optional: ASan build: `-DGNOSTR_ENABLE_ASAN=ON -DGNOSTR_ENABLE_UBSAN=ON`

## Build Configurations

### Debug (symbols, no optimization)

```bash
cmake -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-g3 -O0 -fno-omit-frame-pointer"
cmake --build build-debug
```

### ASan + Debug (catches UAF, leaks, buffer overflows)

```bash
cmake -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGNOSTR_ENABLE_ASAN=ON \
  -DGNOSTR_ENABLE_UBSAN=ON
cmake --build build-asan
```

### RelWithDebInfo (for profiling with symbols)

```bash
cmake -B build-rel \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rel
```

## GDB Workflows (Linux)

### Basic crash diagnosis

```bash
# Run app under GDB
gdb -ex run -ex bt -ex quit --args \
  build-debug/apps/gnostr/gnostr

# If it crashes, GDB prints backtrace automatically
# For interactive debugging:
gdb --args build-debug/apps/gnostr/gnostr
(gdb) run
# ... crash occurs ...
(gdb) bt full          # Full backtrace with locals
(gdb) info threads     # List all threads (important for GTK apps)
(gdb) thread apply all bt  # Backtrace ALL threads
```

### Running a specific test under GDB

```bash
gdb --args build-debug/apps/gnostr/tests/gnostr-test-ndb-main-thread-violations
(gdb) run
# On failure:
(gdb) bt
(gdb) info locals
(gdb) print *item     # Inspect GObject fields
```

### GDB batch mode (for LLM agents)

Non-interactive — run commands and capture output:

```bash
gdb -batch \
  -ex "set pagination off" \
  -ex "set print pretty on" \
  -ex "run" \
  -ex "bt full" \
  -ex "info threads" \
  -ex "thread apply all bt" \
  --args build-debug/apps/gnostr/gnostr 2>&1 | head -200
```

### Breakpoints for common crash patterns

```bash
gdb --args build-debug/apps/gnostr/gnostr
(gdb) # Break on GLib critical warnings (often precede crashes)
(gdb) break g_log_default_handler
(gdb) # Break on specific function
(gdb) break factory_unbind_cb
(gdb) # Break on NDB transaction opens (latency investigation)
(gdb) break storage_ndb_begin_query
(gdb) # Conditional: only break on main thread
(gdb) break storage_ndb_begin_query if g_thread_self() == g_main_context_get_thread_default()
(gdb) run
```

### GObject reference counting diagnosis

```bash
(gdb) # Print ref count of a GObject
(gdb) print ((GObject*)widget)->ref_count

# Watch for ref count going to 0 (finalization)
(gdb) watch ((GObject*)0x5555DEADBEEF)->ref_count
(gdb) condition 1 ((GObject*)0x5555DEADBEEF)->ref_count == 0

# Break on GObject dispose/finalize
(gdb) break g_object_unref if object == 0x5555DEADBEEF
```

### Signal handler investigation

```bash
(gdb) # List all signal handlers on a GObject
(gdb) call g_signal_list_ids(G_OBJECT_TYPE(item), &n_ids)
(gdb) # Check if a specific handler is connected
(gdb) print g_signal_handler_is_connected(item, handler_id)
```

### NDB transaction tracing

```bash
(gdb) # Set breakpoint on NDB transaction open
(gdb) break storage_ndb_begin_query
(gdb) commands
  bt 5
  continue
end
(gdb) run
# Every NDB transaction open prints a 5-frame backtrace
# Look for calls originating from the main thread GTK stack
```

### Watchpoints for memory corruption

```bash
(gdb) # Watch a specific memory location
(gdb) watch *(int*)0x5555DEADBEEF
(gdb) # Watch a struct field
(gdb) watch self->disposed
(gdb) # Hardware watchpoint on the GObject ref count
(gdb) watch -l ((GObject*)self)->ref_count
```

## LLDB Workflows (macOS)

### Basic crash diagnosis

```bash
# Run app under LLDB
lldb -- build-debug/apps/gnostr/gnostr
(lldb) run
# ... crash occurs ...
(lldb) bt all          # All threads backtrace
(lldb) frame variable  # Local variables
(lldb) thread list     # List threads
```

### LLDB batch mode (for LLM agents)

```bash
lldb -b \
  -o "run" \
  -o "bt all" \
  -o "thread list" \
  -k "bt all" \
  -k "quit" \
  -- build-debug/apps/gnostr/gnostr 2>&1 | head -200
```

### Breakpoints

```bash
(lldb) breakpoint set -n factory_unbind_cb
(lldb) breakpoint set -n storage_ndb_begin_query
(lldb) breakpoint set -n g_log_default_handler

# Conditional breakpoint
(lldb) breakpoint set -n storage_ndb_begin_query -c 'gn_test_on_main_thread'

# Break on GLib warnings
(lldb) breakpoint set -n g_logv
```

### GObject inspection

```bash
(lldb) # Print ref count
(lldb) expr ((GObject*)widget)->ref_count

# Print GType name
(lldb) expr (char*)g_type_name(G_OBJECT_TYPE(item))

# Check signal handler
(lldb) expr (int)g_signal_handler_is_connected(item, handler_id)
```

### Watchpoints

```bash
(lldb) watchpoint set expression -- ((GObject*)self)->ref_count
(lldb) watchpoint set variable self->disposed
```

## ASan Integration

### Interpreting ASan output

ASan reports look like:

```
=================================================================
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x...
READ of size 8 at 0x... thread T0
    #0 0x... in factory_bind_cb note-card-factory.c:245
    #1 0x... in gtk_list_item_factory_bind ...
    #2 0x... in gtk_list_view_activate_item ...

previously freed by thread T3:
    #0 0x... in g_object_unref ...
    #1 0x... in gn_nostr_event_item_finalize ...
    #2 0x... in on_sync_complete sync-service.c:89
```

**Reading the report**:
- **First block**: Where the crash happened (accessing freed memory)
- **Second block**: Where the memory was freed (the root cause)
- **Thread info**: T0 = main thread, T3 = worker — this is a threading bug

### ASan + GDB combo (Linux)

```bash
# ASan can break into GDB on error
ASAN_OPTIONS=abort_on_error=1 \
gdb -ex run -ex bt --args build-asan/apps/gnostr/gnostr
```

### ASan + LLDB combo (macOS)

```bash
ASAN_OPTIONS=abort_on_error=1 \
lldb -o run -k "bt all" -k quit \
  -- build-asan/apps/gnostr/gnostr
```

### Leak detection

```bash
# Linux only (LSAN is part of ASan)
ASAN_OPTIONS=detect_leaks=1 \
  build-asan/apps/gnostr/tests/gnostr-test-lifecycle-leaks
```

### Suppression files

For known leaks in third-party libraries:

```bash
# Create suppressions file
cat > /tmp/lsan.supp << 'EOF'
leak:libpango
leak:libfontconfig
leak:g_type_register_static
EOF

LSAN_OPTIONS=suppressions=/tmp/lsan.supp \
  build-asan/apps/gnostr/gnostr
```

## Structured Debug Workflows for LLM Agents

### Workflow 1: Diagnose a segfault

```
1. Reproduce:
   gdb -batch -ex run -ex "bt full" -ex "thread apply all bt" \
     --args build-debug/apps/gnostr/gnostr 2>&1

2. Parse the backtrace:
   - Identify the crashing function and line number
   - Read the source file at that line
   - Check if it's a NULL deref, UAF, or logic error

3. If UAF suspected, rebuild with ASan:
   cmake --build build-asan
   build-asan/apps/gnostr/gnostr 2>&1

4. ASan report shows:
   - WHERE it crashed (read/write of freed memory)
   - WHERE it was freed (the actual bug location)
   - WHICH thread freed it vs which thread accessed it

5. Fix the root cause (usually a missing g_object_ref, or
   a callback accessing freed user_data)
```

### Workflow 2: Track down a GObject leak

```
1. Run under Valgrind or ASan with leak detection:
   ASAN_OPTIONS=detect_leaks=1 \
     build-asan/apps/gnostr/tests/gnostr-test-lifecycle-leaks

2. If leak reported, check the allocation backtrace

3. In GDB, set a breakpoint on the constructor:
   (gdb) break gn_nostr_event_item_new
   (gdb) commands
     print $rax    # Return value = the new object pointer
     continue
   end

4. After the test, check if all objects were finalized:
   - Use gn_test_watch_object() / gn_test_assert_finalized()
   - Or check ref counts at test end
```

### Workflow 3: Find main-thread blocking

```
1. Run with NDB violation detection:
   build-debug/apps/gnostr/tests/gnostr-test-ndb-main-thread-violations

2. If violations detected, use GDB to get the full call chain:
   gdb --args build-debug/apps/gnostr/tests/gnostr-test-ndb-main-thread-violations
   (gdb) break gn_test_record_violation
   (gdb) commands
     bt 15
     continue
   end
   (gdb) run

3. Each backtrace shows exactly how the main thread reached
   the NDB transaction — trace back to find the entry point
   that should be async

4. Fix: Move the call to g_task_run_in_thread() or
   go_blocking_submit()
```

### Workflow 4: Debug signal handler lifetime

```
1. In GDB, break on signal connect and disconnect:
   (gdb) break g_signal_connect_data
   (gdb) break g_signal_handler_disconnect
   (gdb) # Track handler IDs being connected vs disconnected

2. Run the recycling stress test:
   gdb --args build-debug/nostr-gtk/tests/nostr-gtk-test-listview-recycle

3. On crash, check:
   (gdb) print handler_id    # The handler that wasn't disconnected
   (gdb) print item           # The source object
   (gdb) print row            # The user_data (possibly freed)
```

## GLib Debug Environment Variables

| Variable | Value | Purpose |
|----------|-------|---------|
| `G_DEBUG` | `fatal-warnings` | Abort on GLib warnings |
| `G_DEBUG` | `fatal-warnings,gc-friendly` | + GC-friendly for leak detection |
| `G_SLICE` | `always-malloc` | Disable slab allocator (needed for ASan) |
| `G_MESSAGES_DEBUG` | `all` | Print all debug messages |
| `G_MESSAGES_DEBUG` | `GLib-GObject` | Print only GObject messages |
| `GOBJECT_DEBUG` | `objects` | Track all GObject instances |
| `GOBJECT_DEBUG` | `signals` | Trace signal emissions |
| `GTK_DEBUG` | `actions` | Trace GTK actions |
| `GTK_DEBUG` | `layout` | Trace layout/sizing |

### Combining for maximum diagnostics

```bash
G_DEBUG=fatal-warnings,gc-friendly \
G_SLICE=always-malloc \
G_MESSAGES_DEBUG=all \
GOBJECT_DEBUG=objects \
gdb --args build-debug/apps/gnostr/gnostr
```

## Platform-Specific Notes

### Linux

- GDB is the primary debugger
- ASan/LSAN fully supported
- `/proc/self/maps` for memory mapping inspection
- `strace -e trace=mmap,brk` for allocation tracing
- Valgrind available as alternative: `valgrind --leak-check=full ./app`

### macOS

- LLDB is the primary debugger (GDB not well-supported on modern macOS)
- ASan supported via `-fsanitize=address` (Clang)
- LSAN has limited support on macOS — use `leaks` instead:
  ```bash
  MallocStackLogging=1 build-debug/apps/gnostr/gnostr &
  leaks --atExit -- $(pgrep gnostr)
  ```
- Instruments.app for profiling (Allocations, Leaks, Time Profiler)
- `malloc_history` for allocation tracking

## Related

- [`skills/broadway-debug/SKILL.md`](../broadway-debug/SKILL.md) — UI debugging via Broadway
- [`skills/closed-loop-debug/SKILL.md`](../closed-loop-debug/SKILL.md) — Full debug workflow
- [`docs/TESTING.md`](../../docs/TESTING.md) — Test suite documentation
