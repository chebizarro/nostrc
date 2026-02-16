---
name: gtk-inspector
description: >
  Using GTK Inspector for debugging widget trees, CSS styling, GObject lifecycle,
  signal handlers, layout constraints, and rendering performance in gnostr and
  other nostrc GTK4 apps. Covers both interactive use via Broadway/Playwright
  and programmatic use in tests and debug builds.
allowed-tools: "Bash,Read,mcp__playwright__*,mcp__RepoPrompt__*"
version: "1.0.0"
---

# GTK Inspector Debugging

GTK Inspector is a built-in interactive debugger for GTK4 applications. It provides
real-time introspection of widget trees, CSS styling, GObject properties, signal
connections, layout measurements, accessibility, and rendering performance — all
without recompilation.

When combined with Broadway + Playwright, the LLM can interact with the inspector
programmatically: clicking buttons, reading widget properties, and diagnosing issues
in a deterministic loop.

## Quick Start

### Launch with Inspector

```bash
# Method 1: Environment variable (opens Inspector window on launch)
GTK_DEBUG=interactive GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build/apps/gnostr \
  build/apps/gnostr/gnostr

# Method 2: Keyboard shortcut (open at runtime)
# Press Ctrl+Shift+I (or Ctrl+Shift+D) in the running app

# Method 3: Programmatic (in code or GDB)
# gtk_window_set_interactive_debugging(TRUE);
```

### Architecture with Broadway

```
┌────────────────────────────────────┐
│ Broadway Browser Tab               │
│                                    │
│ ┌──────────────┐ ┌──────────────┐ │
│ │ App Window   │ │ Inspector    │ │
│ │              │ │ Window       │ │
│ │ ┌──────────┐ │ │ ┌──────────┐ │ │
│ │ │ Timeline │ │ │ │ Objects  │ │ │
│ │ │ Cards    │ │ │ │ CSS      │ │ │
│ │ │ Composer │ │ │ │ Visual   │ │ │
│ │ │ ...      │ │ │ │ A11y     │ │ │
│ │ └──────────┘ │ │ │ Stats    │ │ │
│ └──────────────┘ │ └──────────┘ │ │
│                  └──────────────┘ │
│  ▲ Playwright MCP can interact    │
│    with BOTH windows              │
└────────────────────────────────────┘
```

The Inspector is a **separate GtkWindow** rendered in the same Broadway session.
Playwright sees both windows and can interact with either — take snapshots of the
inspector to read widget properties, click inspector buttons to navigate the tree,
and use screenshots to see visual overlays.

## Inspector Panels

### 1. Objects Panel — Widget Tree & Properties

The **primary panel** for debugging widget hierarchy and GObject state.

**What it shows:**
- Complete widget tree (every `GtkWidget` in the application)
- Selected widget's GObject properties (name, value, type, flags)
- Property change notification (live updates when properties change)
- CSS classes applied to the selected widget
- Widget state flags (sensitive, visible, focusable, etc.)

**How to use it for nostrc debugging:**

```
# Via Broadway + Playwright:
# 1. Open Inspector
# 2. Take accessibility snapshot — Inspector panel labels are accessible
browser_snapshot()

# 3. Look for the Objects panel in the inspector window
# 4. Navigate to find the problem widget
# The object tree mirrors the GtkWidget containment hierarchy
```

**Key things to check:**
- **Widget sizing:** Select a widget → check `width-request`, `height-request`,
  `hexpand`, `vexpand`, `halign`, `valign` properties
- **Visibility:** Check `visible`, `sensitive`, `can-focus` state flags
- **Model binding:** For `GtkListView`, check `model` property to see the
  `GtkSelectionModel` and its underlying `GListModel`
- **Factory state:** For `GtkListItemFactory`, check bound items

**Debugging GnostrTimelineView sizing (blocker #3):**
```
1. Launch app with Inspector
2. In Inspector Objects panel, expand:
   GnostrMainWindow → GtkBox → GtkScrolledWindow → GtkListView
3. Select the GtkListView
4. Check properties:
   - vexpand: should be TRUE
   - hexpand: should be TRUE
   - If a child NoteCardRow is visible, select it
   - Check its height-request, vexpand, natural size
5. Take screenshot to see the visual highlight overlay
   (Inspector highlights the selected widget with a blue border)
```

### 2. CSS Panel — Live Style Editing

**What it shows:**
- All CSS rules applied to the selected widget
- Rule specificity and source (theme, app CSS, Inspector override)
- Computed style values
- CSS node tree (GTK's internal CSS model)

**How to use it:**

The Inspector's CSS panel has a **live CSS editor**. You can type CSS rules and
see them applied immediately — without rebuilding.

```
# Via Broadway:
# 1. Open Inspector → CSS panel
# 2. Type CSS to test sizing fixes:

/* Force timeline cards to respect max height */
row.note-card {
  max-height: 400px;
  overflow: hidden;
}

/* Debug: highlight all expanding widgets */
* {
  outline: 1px solid rgba(255, 0, 0, 0.3);
}

/* Debug: show widget borders to find sizing issues */
box, scrolledwindow, listview {
  border: 1px dashed blue;
}
```

**CSS debugging workflow for widget sizing:**
```
1. Add debug outlines via Inspector CSS panel
2. Screenshot → identify which widget expands incorrectly
3. Select that widget in Objects panel → check hexpand/vexpand
4. Test a fix in Inspector CSS panel (live)
5. Once working, copy the CSS rule to the app's stylesheet:
   apps/gnostr/data/ui/styles/gnostr.css
6. Rebuild and verify
```

**App CSS files for reference:**
| App | CSS file | Purpose |
|-----|----------|---------|
| gnostr | `apps/gnostr/data/ui/styles/gnostr.css` | App theme |
| gnostr-signer | `apps/gnostr-signer/data/css/app.css` | Signer theme |
| gnostr-signer | `apps/gnostr-signer/data/css/high-contrast.css` | Accessibility |

### 3. Visual Panel — Layout & Rendering Debug

**What it shows:**
- Widget bounds (margins, borders, padding, content — like browser DevTools box model)
- Baseline alignment
- Layout constraints
- Clip regions
- Widget allocation (actual pixel position + size)

**How to use it for sizing issues (blocker #3):**

```
1. Select a NoteCardRow in the Objects panel
2. Switch to Visual panel
3. Read the box model:
   - Margin: outside spacing
   - Border: drawn border
   - Padding: inside spacing
   - Content: actual content area
4. Compare content area size to the widget's natural size
5. If content area exceeds the ScrolledWindow, the widget
   is not properly constrained
```

**What to look for:**
- Content size vastly larger than allocation → widget not respecting parent size
- Zero margins/padding when you expect spacing → CSS not applied
- Baseline misalignment → mixed text/icon widgets without baseline sync

### 4. Accessibility Panel — Widget Semantics

**What it shows:**
- Accessible role (button, label, list, listitem, etc.)
- Accessible properties (label, description, value)
- Accessible states (checked, selected, disabled, etc.)
- Accessible relations (labelledby, describedby, etc.)

**Why it matters for LLM debugging:**
Playwright's `browser_snapshot()` returns an **accessibility tree** — the same data
this panel shows. If a widget isn't showing up in Playwright snapshots, this panel
tells you why (missing accessible role, invisible to AT, etc.).

### 5. Statistics Panel — Rendering Performance

**What it shows:**
- Frame rate (FPS)
- Frame times (per-frame rendering cost)
- Number of rendered nodes
- Texture memory usage
- CSS node count

**How to use it for latency debugging (blocker #5):**

```
1. Open Inspector → Statistics panel
2. Scroll the timeline rapidly
3. Watch frame times:
   - Consistent <16ms → smooth (60fps)
   - Spikes >50ms → main-thread stall (correlate with NDB violations)
   - Steady >33ms → too much rendering work per frame
4. Watch texture memory:
   - Growing without bound → media cache not evicting
   - Spiking during scroll → too many textures created per frame
```

### 6. Recorder Panel — Frame-by-Frame Analysis

**What it shows:**
- Records render node trees for each frame
- Allows frame-by-frame playback
- Shows exactly what was rendered and why
- Identifies redundant redraws

**How to use it:**

```
1. Open Inspector → Recorder tab
2. Click "Record"
3. Perform the problematic action (scroll, load timeline, etc.)
4. Click "Stop"
5. Step through frames:
   - Identify frames with excessive render nodes
   - Find widgets being redrawn unnecessarily
   - Spot layout invalidation cascades
```

## Using Inspector via Playwright (LLM-Actionable)

The Inspector window appears in the Broadway session alongside the app window.
Playwright can interact with both. Here's how the LLM can use it:

### Step 1: Identify the Inspector window

```
# After launching with GTK_DEBUG=interactive:
browser_snapshot()

# The snapshot will show TWO windows:
# - The app window (GNostr, GNostr Signer, etc.)
# - The Inspector window ("GTK Inspector")
# Each has accessible refs you can click
```

### Step 2: Navigate the Inspector

```
# Click the "Objects" tab in the inspector
browser_click(element="Objects", ref="eNN")

# Take a snapshot to see the widget tree
browser_snapshot()

# Click to expand a widget in the tree
browser_click(element="GnostrMainWindow", ref="eNN")
browser_click(element="GtkBox", ref="eNN")
browser_click(element="GtkScrolledWindow", ref="eNN")

# Take a snapshot to read the selected widget's properties
browser_snapshot()
```

### Step 3: Read widget properties

```
# After selecting a widget, the properties panel shows:
browser_snapshot()
# Look for property rows like:
#   width-request: -1
#   height-request: -1
#   hexpand: TRUE
#   vexpand: TRUE
#   visible: TRUE
#   css-classes: ["note-card", "card"]
```

### Step 4: Use the "Pick Widget" tool

```
# Click the crosshair/target icon in the Inspector toolbar
browser_click(element="Pick a widget", ref="eNN")

# Now click on a widget in the APP window
browser_click(element="<target widget in app>", ref="eNN")

# The Inspector jumps to that widget in the tree
browser_snapshot()  # Read its properties
```

### Step 5: Live CSS editing

```
# Click the "CSS" tab
browser_click(element="CSS", ref="eNN")

# Click the CSS text area
browser_click(element="<CSS editor area>", ref="eNN")

# Type CSS rules
browser_type(element="<CSS editor>", ref="eNN",
  text="row.note-card { max-height: 400px; }")

# Take screenshot to see effect immediately
browser_take_screenshot(filename="css-test.png")
```

## Programmatic Inspector Usage

### In test code

```c
#include <gtk/gtk.h>

// Enable inspector programmatically
gtk_window_set_interactive_debugging(TRUE);

// Alternatively, for specific widget inspection in tests:
void debug_widget_tree(GtkWidget *root) {
    GtkWidget *child = gtk_widget_get_first_child(root);
    while (child) {
        int min_w, nat_w, min_h, nat_h;
        gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1,
                           &min_w, &nat_w, NULL, NULL);
        gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, nat_w,
                           &min_h, &nat_h, NULL, NULL);
        
        g_print("Widget %s: %s  min=%dx%d nat=%dx%d alloc=%dx%d\n",
                G_OBJECT_TYPE_NAME(child),
                gtk_widget_get_css_name(child),
                min_w, min_h, nat_w, nat_h,
                gtk_widget_get_width(child),
                gtk_widget_get_height(child));
        
        // Recurse
        debug_widget_tree(child);
        child = gtk_widget_get_next_sibling(child);
    }
}
```

### In GDB/LLDB sessions

```bash
# Break on a widget method, then inspect:
gdb -batch \
  -ex "break nostr_gtk_note_card_row_prepare_for_bind" \
  -ex "run" \
  -ex "call gtk_window_set_interactive_debugging(1)" \
  -ex "continue" \
  --args build-debug/apps/gnostr/gnostr

# Or inspect a specific widget's properties:
gdb -ex "break factory_bind_cb" \
    -ex "run" \
    -ex "call (void)g_object_get_data(row, \"css-name\")" \
    -ex "call (int)gtk_widget_get_width(row)" \
    -ex "call (int)gtk_widget_get_height(row)" \
    -ex "call (int)gtk_widget_get_hexpand(row)" \
    -ex "continue" \
    --args build-debug/apps/gnostr/gnostr
```

## GObject Lifecycle Debugging with Inspector

The Inspector's Objects panel shows **all live GObjects** — not just widgets.
This is invaluable for debugging reference count issues (blocker #2).

### Finding leaked objects

```
1. Open Inspector → Objects panel
2. Use the search/filter to find your type (e.g., "NoteCardRow")
3. Count instances:
   - After scrolling away from a timeline, the old NoteCardRow
     instances should be finalized
   - If the count keeps growing → leak
4. Select an instance → check "References" in properties:
   - ref_count value
   - Who holds references (if g_object_ref tracking is enabled)
```

### Signal handler debugging

```
1. Select a widget in the Objects panel
2. Look at the "Signals" section in the properties pane
3. It lists ALL connected signal handlers:
   - Signal name (e.g., "notify::profile")
   - Handler function pointer
   - Connected object / user_data
   - Whether it's currently blocked
4. After unbind, the handler list should be EMPTY
   (or only contain handlers from the widget itself)
5. If a handler remains after unbind → it will cause UAF on next emit
```

### Property change monitoring

```
1. Select a GObject in the Objects panel
2. Properties update LIVE as the app runs
3. Useful for debugging:
   - Watch "ref-count" changing (shouldn't grow unbounded)
   - Watch "visible" toggling (layout thrashing?)
   - Watch model "n-items" (growing without bound?)
   - Watch "content" property on event items (large strings?)
```

## Debugging Scenarios with Inspector

### Scenario 1: NoteCardRow expanding beyond container

**Problem:** Timeline cards grow to enormous heights with long content.

```
1. Launch: GTK_DEBUG=interactive GDK_BACKEND=broadway ...
2. browser_snapshot() → find Inspector window
3. In Inspector, click "Pick Widget" tool
4. Click on the oversized card in the app
5. Inspector selects the NoteCardRow
6. Read properties:
   - height-request: -1 (no explicit constraint)
   - vexpand: TRUE (expanding to fill!)
   - Check parent ScrolledWindow max-content-height
7. Switch to Visual panel → read actual allocation
8. Switch to CSS panel → add test constraint:
   row.note-card { max-height: 400px; overflow: hidden; }
9. Screenshot → verify the fix works
10. Apply to gnostr.css and rebuild
```

### Scenario 2: Signal handler leak causing crash

**Problem:** Segfault after rapid scrolling (recycling).

```
1. Launch with Inspector + ASan build
2. Scroll slowly → select a NoteCardRow in Inspector
3. Count signal handlers in the Signals section
4. Scroll rapidly (items recycle) → select same row position
5. Count signal handlers again:
   - If count is HIGHER → handlers not disconnected on unbind
   - Note which signal names are accumulating
6. Cross-reference with factory_unbind_cb:
   - Which handler IDs are being tracked?
   - Which are being disconnected?
   - The difference = the leak
```

### Scenario 3: Memory growth during scroll

**Problem:** RSS grows without bound during timeline scrolling.

```
1. Launch with Inspector
2. Open Statistics panel
3. Note starting texture memory and CSS node count
4. Scroll the timeline for 30 seconds
5. Check Statistics:
   - Texture memory growing? → media cache not evicting
   - CSS node count growing? → widgets not being recycled
   - Frame times increasing? → render tree growing
6. Switch to Objects panel
7. Search for "NoteCardRow" → count instances
   - More than visible + small buffer? → leak
8. Search for "GdkTexture" → count instances
   - More than IMAGE_CACHE_SIZE? → cache broken
```

### Scenario 4: Layout performance analysis

**Problem:** UI stutters during layout.

```
1. Launch with Inspector
2. Open Recorder panel
3. Start recording
4. Perform the slow action (open timeline, scroll, resize)
5. Stop recording
6. Step through frames:
   - Frames with >1000 render nodes → too complex
   - Multiple consecutive frames with full-window redraws
     → layout invalidation cascade
   - Look for widgets being redrawn that shouldn't be
     (e.g., header bar redrawn on every scroll)
7. Identify the widget causing invalidation
8. Fix: add `gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN)`
   or restructure the container hierarchy
```

## Inspector Environment Variables

| Variable | Value | Effect |
|----------|-------|--------|
| `GTK_DEBUG` | `interactive` | Opens Inspector window on app launch |
| `GTK_DEBUG` | `actions` | Log all GAction activations |
| `GTK_DEBUG` | `layout` | Log layout measurements and allocations |
| `GTK_DEBUG` | `snapshot` | Log render snapshot operations |
| `GTK_DEBUG` | `css` | Log CSS style computation |
| `GTK_DEBUG` | `builder` | Log GtkBuilder UI template loading |
| `GTK_DEBUG` | `size-request` | Log size request/measure calls |
| `GTK_DEBUG` | `interactive,layout` | Combine: Inspector + layout logging |

Multiple values are comma-separated:
```bash
GTK_DEBUG=interactive,layout,size-request GDK_BACKEND=broadway ...
```

## Inspector Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+I` | Toggle Inspector window |
| `Ctrl+Shift+D` | Toggle Inspector (alternate) |

These work in the app window — press them in the Broadway browser tab.

## Limitations & Workarounds

| Limitation | Workaround |
|-----------|-----------|
| Inspector adds overhead | Only use for debugging, not perf measurement |
| Recorder generates large data | Record short (2-3 second) segments |
| Pick Widget may not work via Broadway click events | Use the Objects tree to navigate manually |
| CSS editor has no undo | Keep track of your test CSS, paste from notes |
| Some properties are read-only | Use GDB to call `g_object_set()` at runtime |
| Inspector can't show finalized objects | Use `G_DEBUG=instance-count` to track types |

## Combining with Other Debug Tools

### Inspector + ASan (memory debugging)

```bash
# Build with ASan
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DGNOSTR_ENABLE_ASAN=ON

# Launch with Inspector + ASan
GTK_DEBUG=interactive GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build-asan/apps/gnostr \
  build-asan/apps/gnostr/gnostr 2>&1 | tee /tmp/asan.log &

# Use Inspector to trigger the bug path, ASan catches the violation
```

### Inspector + NDB Violation Detection

```bash
# Build with GNOSTR_TESTING
cmake -B build-test -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON

# Launch with Inspector
GTK_DEBUG=interactive GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build-test/apps/gnostr \
  build-test/apps/gnostr/gnostr 2>&1 | tee /tmp/violations.log &

# Inspector tells you WHICH widget is slow
# Violation log tells you WHY (NDB on main thread)
```

### Inspector + GDB

```bash
# Run under GDB with Inspector
gdb -ex "run" --args env \
  GTK_DEBUG=interactive GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  build-debug/apps/gnostr/gnostr

# When you see an issue in Inspector:
# Ctrl+C to break into GDB
# Inspect the widget in detail:
(gdb) call (int)gtk_widget_get_width($selected_widget)
(gdb) call (int)gtk_widget_get_height($selected_widget)
(gdb) call (void)g_object_get(widget, "hexpand", &expand, NULL)
```

## Related

- [`skills/broadway-debug/SKILL.md`](../broadway-debug/SKILL.md) — Broadway + Playwright setup
- [`skills/gdb-debug/SKILL.md`](../gdb-debug/SKILL.md) — GDB/LLDB debugging
- [`skills/closed-loop-debug/SKILL.md`](../closed-loop-debug/SKILL.md) — Full debug workflow
- [`docs/TESTING.md`](../../docs/TESTING.md) — Test suite documentation
- [GTK Inspector docs](https://docs.gtk.org/gtk4/running.html#interactive-debugging) — Official GTK4 reference
