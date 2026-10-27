#!/bin/bash
# =============================================================================
# bundle-deps.sh - Bundle GTK4/libadwaita dependencies for Windows installer
# =============================================================================
#
# This script collects all DLL dependencies required to run Grotto
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
# The DLL set is the import closure of the bundled executables and modules.
# Imports resolve against DLLs built in --build-dir, then the MSYS2 prefix,
# then Windows System32; anything unresolved fails the bundle.
# OBJDUMP and SYSTEM32_DIR override the PE reader and system directory.
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
EXE_PATH="${BUILD_DIR}/apps/grotto/grotto.exe"
DAEMON_PATH="${BUILD_DIR}/apps/grotto/grotto-daemon.exe"

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
# Collect DLL dependencies recursively (import closure)
# -----------------------------------------------------------------------------

OBJDUMP="${OBJDUMP:-objdump}"
if [[ -z "${SYSTEM32_DIR:-}" ]]; then
    if [[ -n "${SYSTEMROOT:-}" ]] && command -v cygpath >/dev/null 2>&1; then
        SYSTEM32_DIR="$(cygpath -u "${SYSTEMROOT}")/System32"
    else
        SYSTEM32_DIR="/c/Windows/System32"
    fi
fi

# Index DLLs by lower-cased name (Windows resolves imports case-insensitively).
# Project libraries built as DLLs (e.g. libnostr-json.dll) exist only in the
# build tree, so it is searched before the MSYS2 prefix.
declare -A BUILD_DLLS=() MINGW_DLLS=() COLLECTED_DLLS=() UNRESOLVED_DLLS=()

while IFS= read -r -d '' f; do
    key="$(basename "${f}")"; key="${key,,}"
    if [[ -n "${BUILD_DLLS[${key}]:-}" ]] && ! cmp -s "${BUILD_DLLS[${key}]}" "${f}"; then
        error "Ambiguous build-tree DLL ${key}: ${BUILD_DLLS[${key}]} and ${f}"
    fi
    BUILD_DLLS["${key}"]="${f}"
done < <(find "${BUILD_DIR}" -type f -iname '*.dll' -print0)

while IFS= read -r -d '' f; do
    key="$(basename "${f}")"; MINGW_DLLS["${key,,}"]="${f}"
done < <(find "${MINGW_PREFIX}/bin" -maxdepth 1 -type f -iname '*.dll' -print0)

is_system_dll() {
    local dll="$1"
    [[ "${dll,,}" =~ ^(kernel32|user32|gdi32|shell32|ole32|oleaut32|advapi32|ws2_32|msvcrt|ntdll|comctl32|comdlg32|shlwapi|version|secur32|crypt32|bcrypt|ncrypt|rpcrt4|imm32|usp10|dwrite|d2d1|d3d|dwmapi|uxtheme|msimg32|winmm|winspool|wldap32|api-ms-|ext-ms-|ucrtbase) ]] \
        || [[ -f "${SYSTEM32_DIR}/${dll}" ]]
}

collect_dlls() {
    local binary="$1"
    local binary_name
    binary_name=$(basename "${binary}")

    debug "Analyzing: ${binary_name}"

    local dlls
    dlls=$("${OBJDUMP}" -p "${binary}" | awk '/DLL Name:/ {print $3}') \
        || error "Cannot read imports of ${binary} with ${OBJDUMP}"

    local dll key dll_path
    for dll in ${dlls}; do
        key="${dll,,}"
        if [[ -n "${COLLECTED_DLLS[${key}]:-}" ]]; then
            continue
        fi

        dll_path="${BUILD_DLLS[${key}]:-${MINGW_DLLS[${key}]:-}}"
        if [[ -n "${dll_path}" ]]; then
            log "  Collecting: ${dll}"
            COLLECTED_DLLS["${key}"]="${dll_path}"
            cp "${dll_path}" "${OUTPUT_DIR}/bin/"
            collect_dlls "${dll_path}"
        elif is_system_dll "${dll}"; then
            debug "  Skipping system DLL: ${dll}"
        else
            UNRESOLVED_DLLS["${key}"]="${dll} (imported by ${binary_name})"
        fi
    done
}

require_resolved() {
    if [[ ${#UNRESOLVED_DLLS[@]} -gt 0 ]]; then
        for entry in "${UNRESOLVED_DLLS[@]}"; do
            warn "  Unresolved DLL: ${entry}"
        done
        error "${#UNRESOLVED_DLLS[@]} imported DLL(s) not found in ${BUILD_DIR}, ${MINGW_PREFIX}/bin or ${SYSTEM32_DIR}"
    fi
}

log "Collecting DLL dependencies..."
collect_dlls "${EXE_PATH}"
if [[ -f "${DAEMON_PATH}" ]]; then
    collect_dlls "${DAEMON_PATH}"
fi
require_resolved

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

# Bundled helpers and loadable modules need their own import closures too.
log "Collecting dependencies of bundled helpers and modules..."
for module in "${OUTPUT_DIR}"/bin/*.exe \
              "${OUTPUT_DIR}"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll \
              "${OUTPUT_DIR}"/lib/gtk-4.0/4.0.0/media/*.dll \
              "${OUTPUT_DIR}"/lib/gtk-4.0/4.0.0/printbackends/*.dll; do
    if [[ -f "${module}" ]]; then
        collect_dlls "${module}"
    fi
done
require_resolved

log "Collected ${#COLLECTED_DLLS[@]} DLLs in total"

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
