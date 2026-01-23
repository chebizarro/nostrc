#!/bin/bash
# =============================================================================
# create-dmg.sh - Build macOS .app bundle and DMG installer for Gnostr Signer
# =============================================================================
#
# Usage:
#   ./create-dmg.sh [options]
#
# Options:
#   --build-dir DIR       Build directory containing compiled binary (default: ../../build)
#   --version VERSION     Application version (default: extracted from build or "1.0.0")
#   --output-dir DIR      Where to place the final DMG (default: ./dist)
#   --sign IDENTITY       Developer ID for code signing (optional)
#   --notarize            Submit for Apple notarization (requires --sign)
#   --skip-dmg            Only build .app bundle, skip DMG creation
#   --help                Show this help message
#
# Environment variables:
#   DEVELOPER_ID          Code signing identity (overridden by --sign)
#   APPLE_ID              Apple ID for notarization
#   APPLE_PASSWORD        App-specific password for notarization
#   APPLE_TEAM_ID         Apple Developer Team ID
#
# Example:
#   ./create-dmg.sh --version 0.5.0 --sign "Developer ID Application: ..."
#

set -euo pipefail

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${SCRIPT_DIR}/../.."
PROJECT_ROOT="${APP_DIR}/../.."

# Application metadata
APP_NAME="Gnostr Signer"
APP_BUNDLE_NAME="Gnostr Signer.app"
APP_IDENTIFIER="org.gnostr.Signer"
EXECUTABLE_NAME="gnostr-signer"
DAEMON_NAME="gnostr-signer-daemon"

# Default paths
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
OUTPUT_DIR="${OUTPUT_DIR:-${SCRIPT_DIR}/dist}"
VERSION="${VERSION:-1.0.0}"

# Signing configuration
DEVELOPER_ID="${DEVELOPER_ID:-}"
NOTARIZE=false
SKIP_DMG=false

# DMG configuration
DMG_NAME="GnostrSigner"
DMG_VOLUME_NAME="Gnostr Signer"
DMG_BACKGROUND=""  # Placeholder for background image
DMG_ICON_SIZE=128
DMG_TEXT_SIZE=14
DMG_WINDOW_WIDTH=600
DMG_WINDOW_HEIGHT=400

# -----------------------------------------------------------------------------
# Helper functions
# -----------------------------------------------------------------------------

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

error() {
    log "ERROR: $*" >&2
    exit 1
}

warn() {
    log "WARNING: $*" >&2
}

usage() {
    head -n 30 "$0" | grep -E '^#' | sed 's/^# *//'
    exit 0
}

check_dependency() {
    if ! command -v "$1" &>/dev/null; then
        error "Required command not found: $1"
    fi
}

# -----------------------------------------------------------------------------
# Parse arguments
# -----------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --version)
            VERSION="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --sign)
            DEVELOPER_ID="$2"
            shift 2
            ;;
        --notarize)
            NOTARIZE=true
            shift
            ;;
        --skip-dmg)
            SKIP_DMG=true
            shift
            ;;
        --help|-h)
            usage
            ;;
        *)
            error "Unknown option: $1"
            ;;
    esac
done

# -----------------------------------------------------------------------------
# Validate environment
# -----------------------------------------------------------------------------

log "Validating build environment..."

# Check for required build artifacts
if [[ ! -d "${BUILD_DIR}" ]]; then
    error "Build directory not found: ${BUILD_DIR}"
fi

BINARY_PATH="${BUILD_DIR}/apps/gnostr-signer/${EXECUTABLE_NAME}"
if [[ ! -f "${BINARY_PATH}" ]]; then
    # Try alternative paths
    BINARY_PATH="${BUILD_DIR}/${EXECUTABLE_NAME}"
    if [[ ! -f "${BINARY_PATH}" ]]; then
        error "Binary not found. Expected at: ${BUILD_DIR}/apps/gnostr-signer/${EXECUTABLE_NAME}"
    fi
fi

DAEMON_PATH="${BUILD_DIR}/apps/gnostr-signer/${DAEMON_NAME}"
if [[ ! -f "${DAEMON_PATH}" ]]; then
    DAEMON_PATH="${BUILD_DIR}/${DAEMON_NAME}"
    if [[ ! -f "${DAEMON_PATH}" ]]; then
        warn "Daemon binary not found: ${DAEMON_NAME}"
        DAEMON_PATH=""
    fi
fi

# Check for required tools
check_dependency "otool"
check_dependency "install_name_tool"
if [[ "${SKIP_DMG}" != "true" ]]; then
    check_dependency "hdiutil"
fi

log "Using binary: ${BINARY_PATH}"
log "Version: ${VERSION}"

# -----------------------------------------------------------------------------
# Create output directory
# -----------------------------------------------------------------------------

mkdir -p "${OUTPUT_DIR}"

# -----------------------------------------------------------------------------
# Build .app bundle structure
# -----------------------------------------------------------------------------

APP_BUNDLE="${OUTPUT_DIR}/${APP_BUNDLE_NAME}"

log "Creating application bundle: ${APP_BUNDLE}"

# Clean any existing bundle
rm -rf "${APP_BUNDLE}"

# Create bundle structure
# macOS .app bundle structure:
#   Gnostr Signer.app/
#   Contents/
#     Info.plist
#     MacOS/
#       gnostr-signer (main executable)
#       gnostr-signer-daemon (optional)
#     Resources/
#       AppIcon.icns
#       *.lproj/ (localizations)
#       share/ (GLib schemas, etc.)
#     Frameworks/ (bundled dylibs)

mkdir -p "${APP_BUNDLE}/Contents/MacOS"
mkdir -p "${APP_BUNDLE}/Contents/Resources"
mkdir -p "${APP_BUNDLE}/Contents/Frameworks"
mkdir -p "${APP_BUNDLE}/Contents/Resources/share/glib-2.0/schemas"
mkdir -p "${APP_BUNDLE}/Contents/Resources/share/icons/hicolor/scalable/apps"

# -----------------------------------------------------------------------------
# Copy Info.plist
# -----------------------------------------------------------------------------

log "Generating Info.plist..."

INFO_PLIST_TEMPLATE="${SCRIPT_DIR}/Info.plist.in"
INFO_PLIST="${APP_BUNDLE}/Contents/Info.plist"

if [[ -f "${INFO_PLIST_TEMPLATE}" ]]; then
    sed "s/@VERSION@/${VERSION}/g" "${INFO_PLIST_TEMPLATE}" > "${INFO_PLIST}"
else
    error "Info.plist template not found: ${INFO_PLIST_TEMPLATE}"
fi

# -----------------------------------------------------------------------------
# Copy executables
# -----------------------------------------------------------------------------

log "Copying executables..."

cp "${BINARY_PATH}" "${APP_BUNDLE}/Contents/MacOS/${EXECUTABLE_NAME}"
chmod +x "${APP_BUNDLE}/Contents/MacOS/${EXECUTABLE_NAME}"

if [[ -n "${DAEMON_PATH}" && -f "${DAEMON_PATH}" ]]; then
    cp "${DAEMON_PATH}" "${APP_BUNDLE}/Contents/MacOS/${DAEMON_NAME}"
    chmod +x "${APP_BUNDLE}/Contents/MacOS/${DAEMON_NAME}"
fi

# -----------------------------------------------------------------------------
# Bundle dynamic libraries
# -----------------------------------------------------------------------------

log "Bundling dynamic libraries..."

bundle_dylibs() {
    local binary="$1"
    local frameworks_dir="${APP_BUNDLE}/Contents/Frameworks"

    # Get list of non-system dylibs
    local dylibs
    dylibs=$(otool -L "${binary}" 2>/dev/null | \
        grep -E '^\s+/' | \
        awk '{print $1}' | \
        grep -v '^/System/' | \
        grep -v '^/usr/lib/' | \
        grep -v '@executable_path' | \
        grep -v '@loader_path' | \
        grep -v '@rpath' || true)

    for dylib in ${dylibs}; do
        if [[ -f "${dylib}" ]]; then
            local dylib_name
            dylib_name=$(basename "${dylib}")
            local target="${frameworks_dir}/${dylib_name}"

            if [[ ! -f "${target}" ]]; then
                log "  Bundling: ${dylib_name}"
                cp "${dylib}" "${target}"
                chmod 644 "${target}"

                # Recursively bundle dependencies of this dylib
                bundle_dylibs "${target}"
            fi

            # Update reference in binary
            install_name_tool -change "${dylib}" "@executable_path/../Frameworks/${dylib_name}" "${binary}" 2>/dev/null || true
        fi
    done

    # Update @rpath references
    local rpath_dylibs
    rpath_dylibs=$(otool -L "${binary}" 2>/dev/null | \
        grep -E '^\s+@rpath/' | \
        awk '{print $1}' || true)

    for rpath_ref in ${rpath_dylibs}; do
        local dylib_name
        dylib_name=$(basename "${rpath_ref}")

        # Try common Homebrew locations
        local found_path=""
        for prefix in "/opt/homebrew/lib" "/usr/local/lib" "/opt/homebrew/opt/*/lib" "/usr/local/opt/*/lib"; do
            # Use glob expansion
            for candidate in ${prefix}/${dylib_name}; do
                if [[ -f "${candidate}" ]]; then
                    found_path="${candidate}"
                    break 2
                fi
            done
        done

        if [[ -n "${found_path}" && -f "${found_path}" ]]; then
            local target="${frameworks_dir}/${dylib_name}"
            if [[ ! -f "${target}" ]]; then
                log "  Bundling (rpath): ${dylib_name}"
                cp "${found_path}" "${target}"
                chmod 644 "${target}"
                bundle_dylibs "${target}"
            fi
            install_name_tool -change "${rpath_ref}" "@executable_path/../Frameworks/${dylib_name}" "${binary}" 2>/dev/null || true
        fi
    done
}

bundle_dylibs "${APP_BUNDLE}/Contents/MacOS/${EXECUTABLE_NAME}"
if [[ -f "${APP_BUNDLE}/Contents/MacOS/${DAEMON_NAME}" ]]; then
    bundle_dylibs "${APP_BUNDLE}/Contents/MacOS/${DAEMON_NAME}"
fi

# Fix dylib install names to use @loader_path
for dylib in "${APP_BUNDLE}/Contents/Frameworks/"*.dylib; do
    if [[ -f "${dylib}" ]]; then
        local name
        name=$(basename "${dylib}")
        install_name_tool -id "@executable_path/../Frameworks/${name}" "${dylib}" 2>/dev/null || true
    fi
done

# -----------------------------------------------------------------------------
# Copy resources
# -----------------------------------------------------------------------------

log "Copying resources..."

# Copy icon
ICON_SVG="${APP_DIR}/data/icons/hicolor/scalable/apps/org.gnostr.Signer.svg"
ICON_ICNS="${APP_BUNDLE}/Contents/Resources/AppIcon.icns"

if [[ -f "${ICON_SVG}" ]]; then
    # Try to convert SVG to ICNS
    if command -v rsvg-convert &>/dev/null && command -v iconutil &>/dev/null; then
        log "  Converting SVG to ICNS..."
        ICONSET_DIR=$(mktemp -d)/AppIcon.iconset
        mkdir -p "${ICONSET_DIR}"

        for size in 16 32 64 128 256 512; do
            rsvg-convert -w ${size} -h ${size} "${ICON_SVG}" -o "${ICONSET_DIR}/icon_${size}x${size}.png"
            rsvg-convert -w $((size*2)) -h $((size*2)) "${ICON_SVG}" -o "${ICONSET_DIR}/icon_${size}x${size}@2x.png"
        done

        iconutil -c icns -o "${ICON_ICNS}" "${ICONSET_DIR}"
        rm -rf "$(dirname "${ICONSET_DIR}")"
    else
        warn "Cannot convert SVG to ICNS (missing rsvg-convert or iconutil)"
        # Copy SVG as fallback
        cp "${ICON_SVG}" "${APP_BUNDLE}/Contents/Resources/share/icons/hicolor/scalable/apps/"
    fi
elif [[ -f "${APP_DIR}/data/icons/AppIcon.icns" ]]; then
    cp "${APP_DIR}/data/icons/AppIcon.icns" "${ICON_ICNS}"
fi

# Copy GSettings schema
SCHEMA_FILE="${APP_DIR}/data/org.gnostr.Signer.gschema.xml"
if [[ -f "${SCHEMA_FILE}" ]]; then
    log "  Copying GSettings schema..."
    cp "${SCHEMA_FILE}" "${APP_BUNDLE}/Contents/Resources/share/glib-2.0/schemas/"

    # Compile schemas if glib-compile-schemas is available
    if command -v glib-compile-schemas &>/dev/null; then
        log "  Compiling GSettings schemas..."
        glib-compile-schemas "${APP_BUNDLE}/Contents/Resources/share/glib-2.0/schemas/"
    fi
fi

# Copy CSS
if [[ -d "${APP_DIR}/data/css" ]]; then
    log "  Copying CSS resources..."
    mkdir -p "${APP_BUNDLE}/Contents/Resources/share/gnostr-signer/css"
    cp -r "${APP_DIR}/data/css/"* "${APP_BUNDLE}/Contents/Resources/share/gnostr-signer/css/"
fi

# Create launcher script that sets up GLib environment
log "Creating launcher wrapper..."

LAUNCHER="${APP_BUNDLE}/Contents/MacOS/gnostr-signer-launcher"
cat > "${LAUNCHER}" << 'LAUNCHER_EOF'
#!/bin/bash
# Launcher script for Gnostr Signer
# Sets up environment for GTK/GLib resources

BUNDLE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
RESOURCES_DIR="${BUNDLE_DIR}/Contents/Resources"
FRAMEWORKS_DIR="${BUNDLE_DIR}/Contents/Frameworks"

# Set up library paths
export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS_DIR}:${DYLD_FALLBACK_LIBRARY_PATH}"

# Set up GLib/GTK resources
export GSETTINGS_SCHEMA_DIR="${RESOURCES_DIR}/share/glib-2.0/schemas"
export XDG_DATA_DIRS="${RESOURCES_DIR}/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

# Launch the actual executable
exec "${BUNDLE_DIR}/Contents/MacOS/gnostr-signer" "$@"
LAUNCHER_EOF

chmod +x "${LAUNCHER}"

# -----------------------------------------------------------------------------
# Code signing
# -----------------------------------------------------------------------------

if [[ -n "${DEVELOPER_ID}" ]]; then
    log "Code signing with identity: ${DEVELOPER_ID}"

    ENTITLEMENTS="${SCRIPT_DIR}/entitlements.plist"
    if [[ "${NOTARIZE}" == "true" ]]; then
        ENTITLEMENTS="${SCRIPT_DIR}/entitlements-release.plist"
    fi

    # Sign frameworks first
    for dylib in "${APP_BUNDLE}/Contents/Frameworks/"*; do
        if [[ -f "${dylib}" ]]; then
            log "  Signing: $(basename "${dylib}")"
            codesign --force --options runtime --timestamp \
                --sign "${DEVELOPER_ID}" \
                "${dylib}"
        fi
    done

    # Sign executables
    for binary in "${APP_BUNDLE}/Contents/MacOS/"*; do
        if [[ -x "${binary}" ]]; then
            log "  Signing: $(basename "${binary}")"
            codesign --force --options runtime --timestamp \
                --sign "${DEVELOPER_ID}" \
                --entitlements "${ENTITLEMENTS}" \
                "${binary}"
        fi
    done

    # Sign the bundle
    log "  Signing bundle..."
    codesign --force --options runtime --timestamp \
        --sign "${DEVELOPER_ID}" \
        --entitlements "${ENTITLEMENTS}" \
        "${APP_BUNDLE}"

    # Verify signature
    log "Verifying signature..."
    codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"
else
    log "Skipping code signing (no identity provided)"

    # Ad-hoc sign for local testing
    log "Applying ad-hoc signature..."
    codesign --force --deep --sign - "${APP_BUNDLE}"
fi

# -----------------------------------------------------------------------------
# Create DMG
# -----------------------------------------------------------------------------

if [[ "${SKIP_DMG}" == "true" ]]; then
    log "Skipping DMG creation (--skip-dmg)"
    log "Application bundle created: ${APP_BUNDLE}"
    exit 0
fi

log "Creating DMG installer..."

DMG_PATH="${OUTPUT_DIR}/${DMG_NAME}-${VERSION}.dmg"
DMG_TEMP="${OUTPUT_DIR}/${DMG_NAME}-temp.dmg"

# Remove existing DMG
rm -f "${DMG_PATH}" "${DMG_TEMP}"

# Create temporary DMG
log "  Creating temporary DMG..."
hdiutil create -srcfolder "${APP_BUNDLE}" \
    -volname "${DMG_VOLUME_NAME}" \
    -fs HFS+ \
    -fsargs "-c c=64,a=16,e=16" \
    -format UDRW \
    "${DMG_TEMP}"

# Mount temporary DMG
log "  Mounting DMG for customization..."
MOUNT_DIR=$(hdiutil attach -readwrite -noverify -noautoopen "${DMG_TEMP}" | \
    grep '/Volumes/' | \
    awk '{print $3}')

if [[ -z "${MOUNT_DIR}" ]]; then
    error "Failed to mount DMG"
fi

# Create Applications symlink
log "  Creating Applications symlink..."
ln -sf /Applications "${MOUNT_DIR}/Applications"

# Copy background image if available
if [[ -n "${DMG_BACKGROUND}" && -f "${DMG_BACKGROUND}" ]]; then
    log "  Setting background image..."
    mkdir -p "${MOUNT_DIR}/.background"
    cp "${DMG_BACKGROUND}" "${MOUNT_DIR}/.background/background.png"
    BACKGROUND_CLAUSE="set background picture of theViewOptions to file \".background:background.png\""
else
    BACKGROUND_CLAUSE=""
fi

# Set DMG window appearance using AppleScript
log "  Configuring DMG appearance..."
osascript <<APPLESCRIPT
tell application "Finder"
    tell disk "${DMG_VOLUME_NAME}"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set the bounds of container window to {100, 100, $((100 + DMG_WINDOW_WIDTH)), $((100 + DMG_WINDOW_HEIGHT))}
        set theViewOptions to the icon view options of container window
        set arrangement of theViewOptions to not arranged
        set icon size of theViewOptions to ${DMG_ICON_SIZE}
        set text size of theViewOptions to ${DMG_TEXT_SIZE}
        ${BACKGROUND_CLAUSE}
        set position of item "${APP_BUNDLE_NAME}" of container window to {150, 200}
        set position of item "Applications" of container window to {450, 200}
        update without registering applications
        delay 2
        close
    end tell
end tell
APPLESCRIPT

# Finalize DMG
log "  Finalizing DMG..."
sync
hdiutil detach "${MOUNT_DIR}"

# Convert to compressed read-only DMG
log "  Compressing DMG..."
hdiutil convert "${DMG_TEMP}" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "${DMG_PATH}"

rm -f "${DMG_TEMP}"

# -----------------------------------------------------------------------------
# Notarization
# -----------------------------------------------------------------------------

if [[ "${NOTARIZE}" == "true" && -n "${DEVELOPER_ID}" ]]; then
    log "Submitting for notarization..."

    if [[ -z "${APPLE_ID:-}" || -z "${APPLE_PASSWORD:-}" || -z "${APPLE_TEAM_ID:-}" ]]; then
        error "Notarization requires APPLE_ID, APPLE_PASSWORD, and APPLE_TEAM_ID environment variables"
    fi

    # Submit for notarization
    xcrun notarytool submit "${DMG_PATH}" \
        --apple-id "${APPLE_ID}" \
        --password "${APPLE_PASSWORD}" \
        --team-id "${APPLE_TEAM_ID}" \
        --wait

    # Staple the notarization ticket
    log "Stapling notarization ticket..."
    xcrun stapler staple "${DMG_PATH}"

    # Verify notarization
    log "Verifying notarization..."
    spctl --assess --type open --context context:primary-signature --verbose "${DMG_PATH}"
fi

# -----------------------------------------------------------------------------
# Done
# -----------------------------------------------------------------------------

log "Build complete!"
log "  Application bundle: ${APP_BUNDLE}"
if [[ "${SKIP_DMG}" != "true" ]]; then
    log "  DMG installer: ${DMG_PATH}"

    # Calculate DMG size
    DMG_SIZE=$(du -h "${DMG_PATH}" | cut -f1)
    log "  DMG size: ${DMG_SIZE}"
fi

# Print SHA256 checksum
if [[ "${SKIP_DMG}" != "true" ]]; then
    log "SHA256 checksum:"
    shasum -a 256 "${DMG_PATH}"
fi
