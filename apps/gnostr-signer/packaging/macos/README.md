# macOS Packaging for Gnostr Signer

This directory contains scripts and configuration files for building macOS application bundles and DMG installers for Gnostr Signer.

## Contents

| File | Description |
|------|-------------|
| `Info.plist.in` | Application bundle metadata template |
| `entitlements.plist` | Development entitlements (includes debugging support) |
| `entitlements-release.plist` | Production entitlements (for notarization) |
| `create-dmg.sh` | Main build script for .app bundle and DMG installer |
| `bundle-gtk.sh` | GTK4/GLib resource bundler (loaders, icons, schemas) |
| `generate-icns.sh` | Icon converter (SVG to ICNS) |
| `org.gnostr.Signer.daemon.plist` | launchd plist for daemon auto-start |

## Prerequisites

### Required Dependencies (Homebrew)

```bash
# Core dependencies
brew install gtk4 libadwaita glib json-glib

# Build tools
brew install cmake pkg-config

# Optional: for better crypto support
brew install libsodium libsecp256k1

# Optional: for PKCS#11 HSM support
brew install p11-kit
```

### Optional Tools

For icon conversion (SVG to ICNS):

```bash
brew install librsvg
```

For code signing and notarization:
- Xcode Command Line Tools: `xcode-select --install`
- Apple Developer ID certificate (paid Apple Developer Program membership)

## Quick Start

```bash
# 1. Build the application
cd /path/to/repository
mkdir -p build && cd build
cmake .. -DBUILD_MACOS_BUNDLE=ON
make -j$(sysctl -n hw.ncpu)

# 2. Create .app bundle (unsigned, for local testing)
make macos-bundle

# 3. Create DMG installer (unsigned)
make macos-dmg
```

The outputs will be in `build/bundle/`:
- `Gnostr Signer.app` - Application bundle
- `GnostrSigner-1.0.0.dmg` - DMG installer

## Build Instructions

### 1. Build the Application

```bash
# From the repository root
mkdir -p build && cd build
cmake .. -DBUILD_MACOS_BUNDLE=ON -DGNOSTR_SIGNER_VERSION=1.0.0
make -j$(sysctl -n hw.ncpu)
```

CMake options:
- `-DBUILD_MACOS_BUNDLE=ON` - Enable macOS bundle targets
- `-DGNOSTR_SIGNER_VERSION=X.Y.Z` - Set version number
- `-DGNOSTR_SIGNER_WITH_PKCS11=ON` - Enable PKCS#11 HSM support

### 2. Create Application Bundle Only

```bash
# Using CMake target
make macos-bundle

# Or directly with the script
cd apps/gnostr-signer/packaging/macos
./create-dmg.sh --build-dir ../../../../build --skip-dmg
```

### 3. Create DMG Installer

```bash
# Using CMake target
make macos-dmg

# Or directly
./create-dmg.sh --build-dir ../../../../build --version 1.0.0
```

### 4. Generate App Icon

If you need to regenerate the ICNS icon from SVG:

```bash
cd apps/gnostr-signer/packaging/macos
./generate-icns.sh
# Creates AppIcon.icns from the SVG source
```

## Code Signing

### Developer ID Signing (for distribution outside App Store)

```bash
# Set your Developer ID
export DEVELOPER_ID="Developer ID Application: Your Name (TEAM_ID)"

# Build signed DMG
make macos-dmg-signed

# Or directly
./create-dmg.sh \
    --build-dir ../../../../build \
    --version 1.0.0 \
    --sign "${DEVELOPER_ID}"
```

### Finding Your Developer ID

```bash
# List available signing identities
security find-identity -v -p codesigning

# Look for "Developer ID Application: ..."
```

### Ad-hoc Signing (local testing only)

Unsigned builds are automatically ad-hoc signed, which allows them to run on your own machine but not on other machines without disabling Gatekeeper.

## Notarization

Notarization is required for distribution on macOS 10.15+ (Catalina and later) to pass Gatekeeper without warnings.

### Prerequisites

1. Apple Developer Program membership (paid)
2. Developer ID Application certificate
3. App-specific password for your Apple ID

### Create App-Specific Password

1. Go to https://appleid.apple.com
2. Sign in and navigate to "App-Specific Passwords"
3. Generate a new password for "Gnostr Signer Notarization"

### Notarize the DMG

```bash
# Set credentials
export DEVELOPER_ID="Developer ID Application: Your Name (TEAM_ID)"
export APPLE_ID="your-apple-id@example.com"
export APPLE_PASSWORD="xxxx-xxxx-xxxx-xxxx"  # App-specific password
export APPLE_TEAM_ID="YOUR_TEAM_ID"

# Build notarized DMG
make macos-dmg-notarized

# Or directly
./create-dmg.sh \
    --build-dir ../../../../build \
    --version 1.0.0 \
    --sign "${DEVELOPER_ID}" \
    --notarize
```

### Using Keychain for Credentials

For security, store credentials in Keychain:

```bash
# Store notarization credentials
xcrun notarytool store-credentials "gnostr-signer-notarize" \
    --apple-id "your-apple-id@example.com" \
    --password "xxxx-xxxx-xxxx-xxxx" \
    --team-id "YOUR_TEAM_ID"

# Then use --keychain-profile instead of individual credentials
xcrun notarytool submit GnostrSigner-1.0.0.dmg \
    --keychain-profile "gnostr-signer-notarize" \
    --wait
```

## Script Options

### create-dmg.sh

```
./create-dmg.sh [options]

Options:
  --build-dir DIR       Build directory containing compiled binary
  --version VERSION     Application version (default: 1.0.0)
  --output-dir DIR      Output directory (default: ./dist)
  --sign IDENTITY       Developer ID for code signing
  --notarize            Submit for Apple notarization (requires --sign)
  --skip-dmg            Only build .app bundle, skip DMG creation
  --help                Show help message
```

### bundle-gtk.sh

```
./bundle-gtk.sh [options]

Options:
  --app-bundle PATH     Path to the .app bundle (required)
  --homebrew PATH       Homebrew prefix (default: auto-detect)
  --verbose             Show detailed output
  --help                Show help message
```

### generate-icns.sh

```
./generate-icns.sh [options]

Options:
  --svg PATH            Source SVG file
  --output PATH         Output ICNS file (default: ./AppIcon.icns)
  --help                Show help message
```

## Daemon Auto-Start

To have the signing daemon start automatically on login:

```bash
# Install launchd plist
cp org.gnostr.Signer.daemon.plist ~/Library/LaunchAgents/

# Load the daemon
launchctl load ~/Library/LaunchAgents/org.gnostr.Signer.daemon.plist

# Check status
launchctl list | grep gnostr

# View logs
tail -f /tmp/gnostr-signer-daemon.out.log
```

To unload:

```bash
launchctl unload ~/Library/LaunchAgents/org.gnostr.Signer.daemon.plist
rm ~/Library/LaunchAgents/org.gnostr.Signer.daemon.plist
```

## Bundle Structure

The generated .app bundle has this structure:

```
Gnostr Signer.app/
  Contents/
    Info.plist              # Bundle metadata
    MacOS/
      gnostr-signer         # Main executable
      gnostr-signer-daemon  # Background daemon
      gnostr-signer-launcher # Launcher script (sets up environment)
    Frameworks/             # Bundled dylibs (GTK4, GLib, etc.)
    Resources/
      AppIcon.icns          # Application icon
      share/
        glib-2.0/schemas/   # GSettings schemas
        icons/              # Adwaita and hicolor icons
        gtk-4.0/            # GTK settings
      lib/
        gdk-pixbuf-2.0/     # Image loaders
        gio/modules/        # GIO modules
        girepository-1.0/   # GObject typelibs
```

## Entitlements

### Development (`entitlements.plist`)

Includes all capabilities plus debugging support:
- Keychain access for secure key storage
- Network client/server for relay connections
- Inter-process communication for daemon
- `get-task-allow` for debugging with lldb
- DYLD environment variables for debugging

### Release (`entitlements-release.plist`)

Same as development but:
- Removes `get-task-allow` (required for notarization)
- Removes `allow-dyld-environment-variables`

### Hardened Runtime Exceptions

| Entitlement | Reason |
|-------------|--------|
| `cs.allow-unsigned-executable-memory` | GTK4/GLib requires dynamic memory execution |
| `cs.disable-library-validation` | Load bundled GTK/GLib dylibs |
| `network.client` | Connect to Nostr relays |
| `network.server` | Local IPC daemon |
| `keychain-access-groups` | Store private keys securely |

## Info.plist Features

The bundle declares:

- **URL Schemes**: `nostr://` and `bunker://` protocol handlers
- **UTI Types**: Custom types for `.nostr` event files and `.nsec` key files
- **Retina Support**: `NSHighResolutionCapable` enabled
- **Category**: Utilities (`public.app-category.utilities`)
- **Minimum macOS**: 11.0 (Big Sur)

## Troubleshooting

### Application Crashes on Launch

Check library loading:

```bash
# Check linked libraries
otool -L "Gnostr Signer.app/Contents/MacOS/gnostr-signer"

# Check bundled frameworks
ls -la "Gnostr Signer.app/Contents/Frameworks/"

# Run from terminal to see errors
"Gnostr Signer.app/Contents/MacOS/gnostr-signer-launcher"
```

### GTK/GLib Errors

```bash
# Check GSettings schema
gsettings --schemadir "Gnostr Signer.app/Contents/Resources/share/glib-2.0/schemas" \
    list-schemas | grep gnostr

# Check GdkPixbuf loaders
GDK_PIXBUF_MODULE_FILE="Gnostr Signer.app/Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" \
    gdk-pixbuf-query-loaders
```

### Code Signing Issues

```bash
# Verify signature
codesign -vvv --deep --strict "Gnostr Signer.app"

# Check entitlements
codesign -d --entitlements - "Gnostr Signer.app"

# Re-sign if needed
codesign --force --deep --sign - "Gnostr Signer.app"
```

### Notarization Issues

```bash
# Check submission status
xcrun notarytool history \
    --apple-id "${APPLE_ID}" \
    --password "${APPLE_PASSWORD}" \
    --team-id "${APPLE_TEAM_ID}"

# Get detailed log for a submission
xcrun notarytool log <submission-id> \
    --apple-id "${APPLE_ID}" \
    --password "${APPLE_PASSWORD}" \
    --team-id "${APPLE_TEAM_ID}"
```

### Gatekeeper Issues

```bash
# Check Gatekeeper assessment
spctl --assess --verbose "Gnostr Signer.app"
spctl --assess --type open --context context:primary-signature "GnostrSigner-1.0.0.dmg"

# If notarization ticket is missing
xcrun stapler validate "GnostrSigner-1.0.0.dmg"
```

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Build macOS

on:
  push:
    tags:
      - 'v*'

jobs:
  build-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install Dependencies
        run: |
          brew install gtk4 libadwaita glib json-glib cmake pkg-config librsvg

      - name: Build
        run: |
          mkdir -p build && cd build
          cmake .. -DBUILD_MACOS_BUNDLE=ON -DGNOSTR_SIGNER_VERSION=${{ github.ref_name }}
          make -j$(sysctl -n hw.ncpu)

      - name: Create DMG
        run: |
          cd build
          make macos-dmg

      - name: Sign and Notarize
        env:
          DEVELOPER_ID: ${{ secrets.MACOS_DEVELOPER_ID }}
          APPLE_ID: ${{ secrets.APPLE_ID }}
          APPLE_PASSWORD: ${{ secrets.APPLE_PASSWORD }}
          APPLE_TEAM_ID: ${{ secrets.APPLE_TEAM_ID }}
        run: |
          cd build
          make macos-dmg-notarized

      - name: Upload DMG
        uses: actions/upload-artifact@v4
        with:
          name: gnostr-signer-macos-${{ github.ref_name }}
          path: build/bundle/*.dmg

      - name: Upload to Release
        uses: softprops/action-gh-release@v1
        with:
          files: build/bundle/*.dmg
```

### Required GitHub Secrets

- `MACOS_DEVELOPER_ID` - Full Developer ID string (e.g., "Developer ID Application: Name (TEAM)")
- `APPLE_ID` - Apple ID email
- `APPLE_PASSWORD` - App-specific password
- `APPLE_TEAM_ID` - Apple Developer Team ID

## Notes

- The bundle is fully self-contained with all GTK/GLib libraries
- Icons are converted from SVG using `rsvg-convert` and `iconutil`
- GSettings schemas are compiled into the bundle
- The launcher script sets up all required environment variables
- The daemon can run independently or be managed by launchd
- Universal binaries (Intel + Apple Silicon) require building on both architectures and using `lipo`
