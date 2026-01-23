# macOS Packaging for Gnostr Signer

This directory contains scripts and configuration files for building macOS application bundles and DMG installers for Gnostr Signer.

## Contents

- `Info.plist.in` - Application bundle metadata template
- `entitlements.plist` - Development entitlements (includes debugging support)
- `entitlements-release.plist` - Production entitlements (for App Store/notarization)
- `create-dmg.sh` - Build script for .app bundle and DMG installer

## Prerequisites

### Required Tools

Install via Homebrew:

```bash
brew install gtk4 libadwaita glib json-glib cmake pkg-config
```

### Optional Tools

For icon conversion (SVG to ICNS):

```bash
brew install librsvg
```

For code signing and notarization:
- Xcode Command Line Tools (`xcode-select --install`)
- Apple Developer ID certificate

## Building

### 1. Build the Application

First, build the main application using CMake:

```bash
# From the repository root
mkdir -p build && cd build
cmake .. -DBUILD_MACOS_BUNDLE=ON
make -j$(sysctl -n hw.ncpu)
```

### 2. Create Application Bundle

Create an unsigned .app bundle for local testing:

```bash
# Using CMake target
make macos-bundle

# Or directly with the script
cd apps/gnostr-signer/packaging/macos
./create-dmg.sh --build-dir ../../../../build --skip-dmg
```

The bundle will be created at `build/bundle/Gnostr Signer.app`.

### 3. Create DMG Installer

Create an unsigned DMG (for local distribution):

```bash
# Using CMake target
make macos-dmg

# Or directly
./create-dmg.sh --build-dir ../../../../build --version 1.0.0
```

### 4. Code Signing

For distribution outside the App Store, sign with a Developer ID:

```bash
# Set your Developer ID
export DEVELOPER_ID="Developer ID Application: Your Name (TEAM_ID)"

# Using CMake target
make macos-dmg-signed

# Or directly
./create-dmg.sh \
    --build-dir ../../../../build \
    --version 1.0.0 \
    --sign "${DEVELOPER_ID}"
```

### 5. Notarization

For Gatekeeper approval on macOS 10.15+:

```bash
# Set Apple credentials
export DEVELOPER_ID="Developer ID Application: Your Name (TEAM_ID)"
export APPLE_ID="your-apple-id@example.com"
export APPLE_PASSWORD="app-specific-password"  # Create at appleid.apple.com
export APPLE_TEAM_ID="YOUR_TEAM_ID"

# Using CMake target
make macos-dmg-notarized

# Or directly
./create-dmg.sh \
    --build-dir ../../../../build \
    --version 1.0.0 \
    --sign "${DEVELOPER_ID}" \
    --notarize
```

## Script Options

```
./create-dmg.sh [options]

Options:
  --build-dir DIR       Build directory containing compiled binary
  --version VERSION     Application version (default: 1.0.0)
  --output-dir DIR      Where to place the final DMG (default: ./dist)
  --sign IDENTITY       Developer ID for code signing
  --notarize            Submit for Apple notarization (requires --sign)
  --skip-dmg            Only build .app bundle, skip DMG creation
  --help                Show help message
```

## Entitlements

### Development (`entitlements.plist`)

Used during development with debugging support:
- Keychain access for secure key storage
- Network client/server for relay connections
- Inter-process communication for daemon
- `get-task-allow` for debugging with lldb

### Release (`entitlements-release.plist`)

Used for distribution builds:
- Same as development but without `get-task-allow`
- Required for notarization

## Hardened Runtime

The entitlements enable the following hardened runtime exceptions:

| Entitlement | Reason |
|-------------|--------|
| `cs.allow-unsigned-executable-memory` | GTK4/GLib requires dynamic memory execution |
| `cs.disable-library-validation` | Load Homebrew GTK/GLib dylibs |
| `cs.allow-dyld-environment-variables` | Debugging support |
| `network.client` | Connect to Nostr relays |
| `network.server` | Local IPC daemon |

## Info.plist Features

The bundle declares:

- **URL Schemes**: `nostr://` and `bunker://` protocol handlers
- **UTI Types**: Custom types for `.nostr` event files and `.nsec` key files
- **Retina Support**: `NSHighResolutionCapable` enabled
- **Category**: Utilities
- **Minimum macOS**: 11.0 (Big Sur)

## Troubleshooting

### Library Loading Issues

If the app fails to start due to missing libraries:

```bash
# Check what libraries are linked
otool -L "build/bundle/Gnostr Signer.app/Contents/MacOS/gnostr-signer"

# Verify bundled frameworks
ls -la "build/bundle/Gnostr Signer.app/Contents/Frameworks/"
```

### Code Signing Issues

```bash
# Verify signature
codesign -vvv --deep --strict "build/bundle/Gnostr Signer.app"

# Check entitlements
codesign -d --entitlements - "build/bundle/Gnostr Signer.app"
```

### Notarization Issues

```bash
# Check notarization log
xcrun notarytool log <submission-id> \
    --apple-id "${APPLE_ID}" \
    --password "${APPLE_PASSWORD}" \
    --team-id "${APPLE_TEAM_ID}"
```

## CI/CD Integration

For GitHub Actions or similar CI:

```yaml
- name: Build macOS DMG
  run: |
    mkdir -p build && cd build
    cmake .. -DBUILD_MACOS_BUNDLE=ON -DGNOSTR_SIGNER_VERSION=${{ github.ref_name }}
    make -j$(sysctl -n hw.ncpu)
    make macos-dmg

- name: Sign and Notarize
  if: startsWith(github.ref, 'refs/tags/')
  env:
    DEVELOPER_ID: ${{ secrets.DEVELOPER_ID }}
    APPLE_ID: ${{ secrets.APPLE_ID }}
    APPLE_PASSWORD: ${{ secrets.APPLE_PASSWORD }}
    APPLE_TEAM_ID: ${{ secrets.APPLE_TEAM_ID }}
  run: |
    cd build
    make macos-dmg-notarized

- name: Upload DMG
  uses: actions/upload-artifact@v3
  with:
    name: gnostr-signer-macos
    path: build/bundle/*.dmg
```

## Notes

- The script creates a self-contained bundle with all GTK/GLib libraries bundled
- Icons are converted from SVG using `rsvg-convert` if available
- GSettings schemas are compiled into the bundle
- The launcher script sets up proper environment variables for GLib resources
