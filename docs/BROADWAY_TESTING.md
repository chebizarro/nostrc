# Broadway Testing with Playwright MCP

This document describes how to test the gnostr GTK4 application using the Broadway backend and Playwright MCP.

## Overview

The Broadway backend allows GTK4 applications to run in a web browser. Combined with the Playwright MCP (Model Context Protocol) available in Windsurf, this provides a lightweight testing solution without requiring local Playwright installation.

## Quick Start

### Running gnostr in Broadway

```bash
# From project root
./tools/run-broadway.sh

# Or with custom port
BROADWAY_PORT=9090 ./tools/run-broadway.sh
```

The script will:

1. Check if Broadway daemon is already running (persistent across runs)
2. Start Broadway daemon if not running (detached, survives gnostr exit)
3. Launch gnostr with the Broadway backend
4. **Broadway daemon persists after gnostr exits** (for rebuild/debug cycles)

**Benefits of Persistent Daemon:**

- ✅ Playwright MCP stays connected across gnostr rebuilds
- ✅ No need to reconnect browser preview after each rebuild
- ✅ Faster iteration: rebuild → run → test (no reconnection delay)

**Stopping the Broadway Daemon:**

```bash
# When done testing, stop the persistent daemon
./tools/stop-broadway.sh
```

### 2. Access the UI

Open your browser to `http://127.0.0.1:8080` to see the gnostr UI.

### 3. Automated Testing with Playwright MCP

The Playwright MCP server (available in Windsurf) can interact with the Broadway UI:

- Navigate pages
- Click buttons and UI elements
- Type text
- Take screenshots
- Capture accessibility snapshots
- Inspect network requests
- Read console logs

## Configuration

### Environment Variables

- `BROADWAY_PORT` - HTTP port for Broadway server (default: 8080)
- `BROADWAY_DISPLAY` - X11 display number (default: 5)
- `GNOSTR_BIN` - Path to gnostr binary (default: `build/apps/gnostr/gnostr`)
- `BUILD_DIR` - Build directory (default: `build`)

### Example: Custom Port

```bash
BROADWAY_PORT=9090 ./tools/run-broadway.sh
```

## Test Scenarios

Test scenarios are documented in `docs/test-scenarios/`:

- **`broadway-smoke-tests.md`** - Basic UI smoke tests
  - Timeline visibility
  - Note content display
  - Reply mode switching
  - Relay manager dialog

- **`broadway-blossom-tests.md`** - Blossom media upload tests
  - Attach button functionality
  - Upload progress
  - URL insertion
  - Multi-server support (pending)
  - Error handling

## Playwright MCP Capabilities

The Playwright MCP provides these tools:

- `browser_navigate` - Navigate to URL
- `browser_click` - Click elements
- `browser_type` - Type text
- `browser_snapshot` - Capture accessibility tree
- `browser_take_screenshot` - Take screenshots
- `browser_console_messages` - Read console logs
- `browser_network_requests` - Inspect network activity
- `browser_evaluate` - Run JavaScript
- `browser_wait_for` - Wait for conditions

## Example Test Flow

```bash
# Terminal 1: Start gnostr with Broadway
./tools/run-broadway.sh

# Terminal 2 / Windsurf: Use Playwright MCP
# Navigate to Broadway UI
mcp0_browser_navigate(url="http://127.0.0.1:8080")

# Take snapshot to see UI structure
mcp0_browser_snapshot()

# Click a button
mcp0_browser_click(element="Manage Relays button", ref="...")

# Type in composer
mcp0_browser_type(element="Composer", ref="...", text="Hello Nostr!")

# Take screenshot
mcp0_browser_take_screenshot(filename="test-result.png")
```

## Advantages Over Local Playwright

- ✅ No `node_modules` bloat (~500MB saved)
- ✅ No package.json/lock files to maintain
- ✅ No Playwright version management
- ✅ MCP provides all automation capabilities
- ✅ Simpler repository structure
- ✅ AI assistant can run tests directly

## Troubleshooting

### Broadway server won't start

Check if port is already in use:
```bash
lsof -i :8080
```

Use a different port:
```bash
BROADWAY_PORT=9090 ./tools/run-broadway.sh
```

### gnostr binary not found

Build the project first:
```bash
cmake -B build -S .
cmake --build build
```

Or specify the binary location:
```bash
GNOSTR_BIN=/path/to/gnostr ./tools/run-broadway.sh
```

### Broadway daemon not found

Install GTK4 development tools:
```bash
# Debian/Ubuntu
sudo apt-get install libgtk-4-dev

# Fedora
sudo dnf install gtk4-devel

# Arch
sudo pacman -S gtk4
```

## References

- [GTK Broadway Backend](https://docs.gtk.org/gtk4/broadway.html)
- [Playwright MCP Documentation](https://github.com/microsoft/playwright-mcp)
- Test scenarios: `docs/test-scenarios/`
