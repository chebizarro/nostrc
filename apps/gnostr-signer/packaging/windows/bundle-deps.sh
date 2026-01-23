#!/bin/bash
# =============================================================================
# bundle-deps.sh - Bundle GTK4/libadwaita dependencies for Windows installer
# =============================================================================
#
# This script collects all DLL dependencies required to run GNostr Signer
# on Windows without requiring MSYS2 to be installed.
#
# Usage:
#   ./bundle-deps.sh [options]
#
# Options:
#   --build-dir DIR   Build directory containing executables
#   --output-dir DIR  Output directory for bundled deps (default: deps/)
#   --verbose         Show detailed output
#   --help            Show this help message
#
# Must be run from MSYS2 MinGW64 or UCRT64 shell.
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default paths
BUILD_DIR=""
OUTPUT_DIR="${SCRIPT_DIR}/deps"
VERBOSE=false

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[BUNDLE]${NC} $*"
}

debug() {
    if [[ "${VERBOSE}" == "true" ]]; then
        echo -e "${BLUE}[DEBUG]${NC} $*"
    fi
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $*" >&2
}

error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
    exit 1
}

usage() {
    head -n 25 "$0" | grep -E '^#' | sed 's/^# *//'
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --verbose)
            VERBOSE=true
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

# Validate environment
if [[ -z "${MSYSTEM:-}" ]]; then
    error "This script must be run from MSYS2 MinGW64 or UCRT64 shell"
fi

# Determine MSYS2 prefix
case "${MSYSTEM}" in
    MINGW64)
        MINGW_PREFIX="/mingw64"
        ;;
    UCRT64)
        MINGW_PREFIX="/ucrt64"
        ;;
    *)
        error "Unsupported MSYSTEM: ${MSYSTEM}. Use MINGW64 or UCRT64."
        ;;
esac

log "Using MSYS2 prefix: ${MINGW_PREFIX}"

# Validate build directory
if [[ -z "${BUILD_DIR}" ]]; then
    error "Build directory not specified. Use --build-dir"
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
    error "Build directory not found: ${BUILD_DIR}"
fi

# Find executables
EXE_PATH="${BUILD_DIR}/apps/gnostr-signer/gnostr-signer.exe"
DAEMON_PATH="${BUILD_DIR}/apps/gnostr-signer/gnostr-signer-daemon.exe"

if [[ ! -f "${EXE_PATH}" ]]; then
    error "Executable not found: ${EXE_PATH}"
fi

# Create output directories
log "Creating output directories..."
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/bin"
mkdir -p "${OUTPUT_DIR}/lib/gdk-pixbuf-2.0/2.10.0/loaders"
mkdir -p "${OUTPUT_DIR}/lib/gtk-4.0/4.0.0/media"
mkdir -p "${OUTPUT_DIR}/lib/gtk-4.0/4.0.0/printbackends"
mkdir -p "${OUTPUT_DIR}/share/glib-2.0/schemas"
mkdir -p "${OUTPUT_DIR}/share/gtk-4.0"
mkdir -p "${OUTPUT_DIR}/share/icons/Adwaita"
mkdir -p "${OUTPUT_DIR}/share/icons/hicolor"
mkdir -p "${OUTPUT_DIR}/share/locale"
mkdir -p "${OUTPUT_DIR}/share/libadwaita-1"

# -----------------------------------------------------------------------------
# Collect DLL dependencies recursively
# -----------------------------------------------------------------------------

declare -A COLLECTED_DLLS

collect_dlls() {
    local binary="$1"
    local binary_name
    binary_name=$(basename "${binary}")

    debug "Analyzing: ${binary_name}"

    # Get list of DLL dependencies using objdump
    local dlls
    dlls=$(objdump -p "${binary}" 2>/dev/null | grep "DLL Name:" | awk '{print $3}' || true)

    for dll in ${dlls}; do
        # Skip if already collected
        if [[ -n "${COLLECTED_DLLS[${dll}]:-}" ]]; then
            continue
        fi

        # Skip Windows system DLLs
        if echo "${dll}" | grep -qiE '^(kernel32|user32|gdi32|shell32|ole32|oleaut32|advapi32|ws2_32|msvcrt|ntdll|comctl32|comdlg32|shlwapi|version|secur32|crypt32|bcrypt|ncrypt|rpcrt4|imm32|usp10|dwrite|d2d1|d3d|dwmapi|uxtheme|msimg32|winmm|winspool|wldap32|api-ms-|ext-ms-|ucrtbase)'; then
            debug "  Skipping system DLL: ${dll}"
            continue
        fi

        # Find the DLL in MSYS2
        local dll_path="${MINGW_PREFIX}/bin/${dll}"
        if [[ -f "${dll_path}" ]]; then
            log "  Collecting: ${dll}"
            COLLECTED_DLLS["${dll}"]="${dll_path}"
            cp "${dll_path}" "${OUTPUT_DIR}/bin/"

            # Recursively collect dependencies
            collect_dlls "${dll_path}"
        else
            warn "  DLL not found in MSYS2: ${dll}"
        fi
    done
}

log "Collecting DLL dependencies..."
collect_dlls "${EXE_PATH}"
if [[ -f "${DAEMON_PATH}" ]]; then
    collect_dlls "${DAEMON_PATH}"
fi

log "Collected ${#COLLECTED_DLLS[@]} DLLs"

# -----------------------------------------------------------------------------
# Copy GLib utilities
# -----------------------------------------------------------------------------

log "Copying GLib utilities..."

GLIB_BINS=(
    "glib-compile-schemas.exe"
    "gspawn-win64-helper.exe"
    "gspawn-win64-helper-console.exe"
    "gdbus.exe"
    "gio.exe"
    "gresource.exe"
)

for bin in "${GLIB_BINS[@]}"; do
    src="${MINGW_PREFIX}/bin/${bin}"
    if [[ -f "${src}" ]]; then
        cp "${src}" "${OUTPUT_DIR}/bin/"
        debug "  Copied: ${bin}"
    fi
done

# GTK4 utilities
GTK_BINS=(
    "gtk4-update-icon-cache.exe"
    "gtk4-query-settings.exe"
)

for bin in "${GTK_BINS[@]}"; do
    src="${MINGW_PREFIX}/bin/${bin}"
    if [[ -f "${src}" ]]; then
        cp "${src}" "${OUTPUT_DIR}/bin/"
        debug "  Copied: ${bin}"
    fi
done

# -----------------------------------------------------------------------------
# Copy GDK-Pixbuf loaders
# -----------------------------------------------------------------------------

log "Copying GDK-Pixbuf loaders..."

PIXBUF_LOADERS_DIR="${MINGW_PREFIX}/lib/gdk-pixbuf-2.0/2.10.0/loaders"
if [[ -d "${PIXBUF_LOADERS_DIR}" ]]; then
    cp "${PIXBUF_LOADERS_DIR}"/*.dll "${OUTPUT_DIR}/lib/gdk-pixbuf-2.0/2.10.0/loaders/" 2>/dev/null || true

    # Generate loaders.cache
    "${MINGW_PREFIX}/bin/gdk-pixbuf-query-loaders.exe" \
        "${OUTPUT_DIR}/lib/gdk-pixbuf-2.0/2.10.0/loaders/"*.dll \
        > "${OUTPUT_DIR}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" 2>/dev/null || true

    # Fix paths in loaders.cache to be relative
    sed -i 's|.*/lib/|lib/|g' "${OUTPUT_DIR}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" || true
fi

# -----------------------------------------------------------------------------
# Copy GTK4 modules
# -----------------------------------------------------------------------------

log "Copying GTK4 modules..."

# Media backends
GTK4_MEDIA_DIR="${MINGW_PREFIX}/lib/gtk-4.0/4.0.0/media"
if [[ -d "${GTK4_MEDIA_DIR}" ]]; then
    cp "${GTK4_MEDIA_DIR}"/*.dll "${OUTPUT_DIR}/lib/gtk-4.0/4.0.0/media/" 2>/dev/null || true
fi

# Print backends
GTK4_PRINT_DIR="${MINGW_PREFIX}/lib/gtk-4.0/4.0.0/printbackends"
if [[ -d "${GTK4_PRINT_DIR}" ]]; then
    cp "${GTK4_PRINT_DIR}"/*.dll "${OUTPUT_DIR}/lib/gtk-4.0/4.0.0/printbackends/" 2>/dev/null || true
fi

# -----------------------------------------------------------------------------
# Copy GSettings schemas
# -----------------------------------------------------------------------------

log "Copying GSettings schemas..."

SCHEMAS_DIR="${MINGW_PREFIX}/share/glib-2.0/schemas"
if [[ -d "${SCHEMAS_DIR}" ]]; then
    # Copy essential GTK/GLib schemas
    for schema in org.gtk.* org.gnome.desktop.interface* org.freedesktop.*; do
        cp "${SCHEMAS_DIR}/${schema}" "${OUTPUT_DIR}/share/glib-2.0/schemas/" 2>/dev/null || true
    done

    # Copy schema overrides
    cp "${SCHEMAS_DIR}"/*.override "${OUTPUT_DIR}/share/glib-2.0/schemas/" 2>/dev/null || true
fi

# -----------------------------------------------------------------------------
# Copy Icon themes
# -----------------------------------------------------------------------------

log "Copying icon themes..."

# Adwaita icon theme (essential for GTK4)
ADWAITA_DIR="${MINGW_PREFIX}/share/icons/Adwaita"
if [[ -d "${ADWAITA_DIR}" ]]; then
    log "  Copying Adwaita icons..."
    cp -r "${ADWAITA_DIR}"/* "${OUTPUT_DIR}/share/icons/Adwaita/" 2>/dev/null || true
fi

# Hicolor (fallback theme)
HICOLOR_DIR="${MINGW_PREFIX}/share/icons/hicolor"
if [[ -d "${HICOLOR_DIR}" ]]; then
    log "  Copying hicolor icons..."
    cp -r "${HICOLOR_DIR}"/* "${OUTPUT_DIR}/share/icons/hicolor/" 2>/dev/null || true
fi

# -----------------------------------------------------------------------------
# Copy GTK4 settings
# -----------------------------------------------------------------------------

log "Copying GTK4 settings..."

GTK4_SHARE="${MINGW_PREFIX}/share/gtk-4.0"
if [[ -d "${GTK4_SHARE}" ]]; then
    cp -r "${GTK4_SHARE}"/* "${OUTPUT_DIR}/share/gtk-4.0/" 2>/dev/null || true
fi

# -----------------------------------------------------------------------------
# Copy libadwaita resources
# -----------------------------------------------------------------------------

log "Copying libadwaita resources..."

ADWAITA_SHARE="${MINGW_PREFIX}/share/libadwaita-1"
if [[ -d "${ADWAITA_SHARE}" ]]; then
    cp -r "${ADWAITA_SHARE}"/* "${OUTPUT_DIR}/share/libadwaita-1/" 2>/dev/null || true
fi

# -----------------------------------------------------------------------------
# Copy essential locale data
# -----------------------------------------------------------------------------

log "Copying locale data..."

# Copy common locales (add more as needed)
LOCALES=(en en_US en_GB de es fr it ja ko pt_BR ru zh_CN zh_TW)
LOCALE_DIR="${MINGW_PREFIX}/share/locale"

for locale in "${LOCALES[@]}"; do
    src="${LOCALE_DIR}/${locale}"
    if [[ -d "${src}" ]]; then
        mkdir -p "${OUTPUT_DIR}/share/locale/${locale}"
        # Only copy GTK/GLib message catalogs
        for domain in gtk40 gtk40-properties glib20 libadwaita; do
            mo_file="${src}/LC_MESSAGES/${domain}.mo"
            if [[ -f "${mo_file}" ]]; then
                mkdir -p "${OUTPUT_DIR}/share/locale/${locale}/LC_MESSAGES"
                cp "${mo_file}" "${OUTPUT_DIR}/share/locale/${locale}/LC_MESSAGES/"
            fi
        done
    fi
done

# -----------------------------------------------------------------------------
# Calculate bundle size
# -----------------------------------------------------------------------------

log "Calculating bundle size..."
BUNDLE_SIZE=$(du -sh "${OUTPUT_DIR}" | cut -f1)
DLL_COUNT=$(find "${OUTPUT_DIR}/bin" -name "*.dll" | wc -l)

log "Bundle complete!"
log "  Location: ${OUTPUT_DIR}"
log "  Total size: ${BUNDLE_SIZE}"
log "  DLLs bundled: ${DLL_COUNT}"

# List key DLLs
log "Key dependencies:"
for dll in libgtk-4 libadwaita-1 libglib-2.0 libgio-2.0 libgobject-2.0; do
    if [[ -f "${OUTPUT_DIR}/bin/${dll}"*.dll ]]; then
        log "  - ${dll}"
    fi
done
