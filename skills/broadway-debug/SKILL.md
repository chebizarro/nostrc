---
name: broadway-debug
description: >
  Closed-loop UI debugging for GTK4 applications using the Broadway HTML5 backend
  and Playwright MCP. Covers persistent browser sessions across rebuilds, GTK test
  utilities for widget manipulation, accessibility snapshot inspection, and rapid
  iteration on visual/interaction bugs in gnostr, gnostr-signer, and other nostrc
  GTK apps.
allowed-tools: "Bash,Read,mcp__playwright__*,mcp__RepoPrompt__*"
version: "1.0.0"
---

# Broadway + Playwright UI Debug Loop

Debug GTK4 application UI in a browser using the Broadway backend with Playwright MCP
for automated inspection, interaction, and screenshot capture — all without leaving
the agent session.

## Prerequisites

- GTK4 with Broadway support (`gtk4-broadwayd` in PATH)
- Built application binary (e.g. `build/apps/gnostr/gnostr`)
- Playwright MCP server connected
- Linux: X11 or headless (Broadway runs its own HTTP server)
- macOS: Works natively (Broadway is cross-platform in GTK4)

## Architecture

```
┌──────────────────────┐    HTTP     ┌───────────────────┐
│  GTK4 App            │◄──────────►│  Broadway Daemon   │
│  (GDK_BACKEND=       │    :8080   │  (gtk4-broadwayd)  │
│   broadway)          │            └────────┬──────────┘
└──────────────────────┘                     │ WebSocket
                                             ▼
                                    ┌───────────────────┐
                                    │  Browser Tab       │
                                    │  (Playwright MCP   │
                                    │   connected)       │
                                    └───────────────────┘
```

**Key insight**: The Broadway daemon is a **separate process** from the app.
It persists across app rebuilds/restarts, so the browser tab and Playwright
connection stay alive while you rebuild and relaunch the app.

## Quick Start

### 1. Start the persistent Broadway daemon

```bash
./scripts/run-broadway.sh
# Or with custom port
BROADWAY_PORT=9090 ./scripts/run-broadway.sh
```

### 2. Connect Playwright to the Broadway UI

```
# Via Playwright MCP
browser_navigate(url="http://127.0.0.1:8080")
```

### 3. The debug loop

```
   ┌─── Inspect UI (snapshot / screenshot) ◄──────────────────┐
   │                                                           │
   ▼                                                           │
   Identify issue ──► Edit code ──► Rebuild ──► Relaunch app ──┘
                                       │
                                 (daemon stays,
                                  browser stays)
```

## Maintaining a Persistent Session Across Rebuilds

The **critical workflow optimization**: the Broadway daemon (`gtk4-broadwayd`)
and browser connection persist independently of the app process.

### Rebuild cycle (no reconnection needed)

```bash
# 1. App is running, Playwright is connected
# 2. Kill the app (Ctrl+C or process signal)
# 3. Rebuild
cmake --build build --target gnostr

# 4. Relaunch — Broadway daemon is still running, browser still connected
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build/apps/gnostr \
  build/apps/gnostr/gnostr

# 5. Playwright MCP is still connected — just take a new snapshot
browser_snapshot()
```

### For gnostr-signer

```bash
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build/apps/gnostr-signer \
  build/apps/gnostr-signer/gnostr-signer
```

### Multiple apps on different displays

```bash
# App 1 on display :5, port 8080
BROADWAY_DISPLAY=5 BROADWAY_PORT=8080 ./scripts/run-broadway.sh

# App 2 on display :6, port 8081 (in another terminal)
BROADWAY_DISPLAY=6 BROADWAY_PORT=8081 \
  GNOSTR_BIN=build/apps/gnostr-signer/gnostr-signer \
  ./scripts/run-broadway.sh
```

## Inspecting the UI

### Accessibility Snapshots (preferred for LLM interaction)

```
browser_snapshot()
```

Returns a structured accessibility tree with ref IDs for every widget.
This is the **primary way to understand the UI structure** — it gives you
semantic labels, roles, and states that map directly to GTK widget properties.

Example output:
```
- window "GNostr" [focused]
  - box
    - headerbar
      - button "Manage Relays" [ref=e3]
      - button "Settings" [ref=e4]
    - scrolledwindow
      - listview "Timeline List" [ref=e12]
        - listitem
          - box
            - label "alice" [ref=e15]
            - label "Hello Nostr!" [ref=e16]
            - button "Note Reply" [ref=e17]
```

### Screenshots

```
browser_take_screenshot(filename="current-state.png")
```

Use after visual changes to verify layout, sizing, and rendering.

### Console / Network

```
browser_console_messages()    # GTK debug output forwarded here
browser_network_requests()    # WebSocket frames between Broadway and browser
```

## Manipulating Widgets

### Via Playwright (DOM-level interaction)

```
# Click a button by accessibility label
browser_click(element="Manage Relays button", ref="e3")

# Type into a text entry
browser_type(element="Composer", ref="e20", text="Hello world")

# Scroll a list
browser_evaluate(expression="document.querySelector('[ref=e12]').scrollTop = 500")
```

### Via GTK Test Utilities (code-level, for automated tests)

GTK4 provides test utilities that operate at the widget level, bypassing
DOM/Broadway entirely. Use these in C test code for deterministic results:

```c
#include <gtk/gtk.h>

// Initialize GTK in test mode (required)
gtk_test_init(&argc, &argv, NULL);

// Find widgets by type in a container
GtkWidget *btn = gtk_test_find_widget(
    container, "Manage Relays", GTK_TYPE_BUTTON);

// Simulate a button click
gtk_test_widget_send_key(btn, GDK_KEY_Return, 0);

// Wait for all pending redraws to complete
gtk_test_widget_wait_for_draw(widget);

// Measure widget after layout
int min_w, nat_w, min_h, nat_h;
gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1,
                   &min_w, &nat_w, NULL, NULL);
gtk_widget_measure(widget, GTK_ORIENTATION_VERTICAL, nat_w,
                   &min_h, &nat_h, NULL, NULL);

// Get accessible properties
GtkAccessible *acc = GTK_ACCESSIBLE(widget);
// Check state, properties via ATK bridge
```

### Hybrid approach: GTK Inspector via Broadway

Enable the GTK Inspector in the Broadway session for interactive debugging:

```bash
GTK_DEBUG=interactive GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  build/apps/gnostr/gnostr
```

The Inspector appears as a separate window in the Broadway session. Use
Playwright to interact with both the app window and the inspector.

> **📖 For comprehensive Inspector guidance** — widget tree navigation, CSS live
> editing, GObject lifecycle debugging, signal handler inspection, rendering
> performance analysis, and LLM-actionable workflows — see
> [`skills/gtk-inspector/SKILL.md`](../gtk-inspector/SKILL.md).

## Common Debug Scenarios

### Scenario 1: Widget sizing regression

```
1. browser_navigate(url="http://127.0.0.1:8080")
2. browser_snapshot()  → find the timeline list ref
3. browser_take_screenshot(filename="before.png")
4. Edit the widget CSS/layout code
5. cmake --build build --target gnostr
6. Relaunch app (daemon persists)
7. browser_snapshot()  → compare structure
8. browser_take_screenshot(filename="after.png")
9. Read both screenshots to compare
```

### Scenario 2: Interaction flow debugging

```
1. browser_snapshot()  → find the compose button ref
2. browser_click(element="Compose", ref="eNN")
3. browser_snapshot()  → verify composer opened
4. browser_type(element="Composer Text", ref="eNN", text="Test note")
5. browser_snapshot()  → verify text entered
6. browser_click(element="Post", ref="eNN")
7. browser_snapshot()  → verify composer closed, note appeared
```

### Scenario 3: Startup latency observation

```
1. Kill existing app
2. Relaunch with timing:
   time GDK_BACKEND=broadway ... build/apps/gnostr/gnostr &
3. Immediately start polling:
   browser_snapshot()  → may show loading state
4. Wait 2s, snapshot again → should show content
5. If content not loaded, the startup path is blocking
```

## Environment Variables Reference

| Variable | Default | Purpose |
|----------|---------|---------|
| `BROADWAY_PORT` | `8080` | HTTP port for Broadway |
| `BROADWAY_DISPLAY` | `5` | Virtual display number |
| `GNOSTR_BIN` | `build/apps/gnostr/gnostr` | App binary path |
| `BUILD_DIR` | `build` | Build output directory |
| `GDK_BACKEND` | (system) | Set to `broadway` for HTML5 mode |
| `GTK_DEBUG` | (none) | Set to `interactive` for GTK Inspector |
| `GSETTINGS_SCHEMA_DIR` | (system) | Path to compiled GSettings schemas |

## Stopping the Daemon

```bash
./scripts/stop-broadway.sh
# Or manually:
kill $(cat /tmp/broadway-5.pid)
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "Port 8080 in use" | `BROADWAY_PORT=9090 ./scripts/run-broadway.sh` |
| App crashes on launch | Check `GSETTINGS_SCHEMA_DIR` points to compiled schemas |
| Browser shows blank | App may have crashed — check terminal, relaunch |
| Playwright can't find elements | Take `browser_snapshot()` first to get ref IDs |
| Inspector won't open | Ensure `GTK_DEBUG=interactive` is set before launch |
| Stale daemon | `./scripts/stop-broadway.sh` then restart |

## Related

- [`skills/gtk-inspector/SKILL.md`](../gtk-inspector/SKILL.md) — **GTK Inspector debugging (comprehensive)**
- [`docs/BROADWAY_TESTING.md`](../../docs/BROADWAY_TESTING.md) — Overview and test scenarios
- [`docs/test-scenarios/`](../../docs/test-scenarios/) — Playwright test scripts
- [`skills/gdb-debug/SKILL.md`](../gdb-debug/SKILL.md) — Debugger integration for crashes
- [`skills/closed-loop-debug/SKILL.md`](../closed-loop-debug/SKILL.md) — Full debug workflow
