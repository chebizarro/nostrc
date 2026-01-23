#!/bin/bash
# =============================================================================
# bundle-gtk.sh - Bundle GTK4 and dependencies into macOS app bundle
# =============================================================================
#
# This script bundles all GTK4, GLib, libadwaita, and related dependencies
# into a macOS .app bundle, making it fully self-contained.
#
# Usage:
#   ./bundle-gtk.sh --app-bundle "path/to/App.app" [options]
#
# Options:
#   --app-bundle PATH   Path to the .app bundle (required)
#   --homebrew PATH     Homebrew prefix (default: auto-detect)
#   --verbose           Show detailed output
#   --help              Show this help message
#
# This handles:
#   - Dynamic libraries (dylibs) and their dependencies
#   - GLib typelibs (for GObject introspection)
#   - GIO modules (for networking)
#   - GdkPixbuf loaders (for image formats)
#   - GTK4 settings and themes
#   - Adwaita icons
#

set -euo pipefail

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------

VERBOSE=false
HOMEBREW_PREFIX=""
APP_BUNDLE=""

# -----------------------------------------------------------------------------
# Helper functions
# -----------------------------------------------------------------------------

log() {
    echo "[$(date '+%H:%M:%S')] $*"
}

debug() {
    if [[ "${VERBOSE}" == "true" ]]; then
        echo "  [DEBUG] $*"
    fi
}

error() {
    echo "[ERROR] $*" >&2
    exit 1
}

usage() {
    head -n 25 "$0" | grep -E '^#' | sed 's/^# *//'
    exit 0
}

# Detect Homebrew prefix
detect_homebrew() {
    if [[ -n "${HOMEBREW_PREFIX}" ]]; then
        return
    fi

    if [[ -d "/opt/homebrew" ]]; then
        HOMEBREW_PREFIX="/opt/homebrew"
    elif [[ -d "/usr/local/Homebrew" ]]; then
        HOMEBREW_PREFIX="/usr/local"
    else
        error "Homebrew not found. Install from https://brew.sh"
    fi

    debug "Homebrew prefix: ${HOMEBREW_PREFIX}"
}

# -----------------------------------------------------------------------------
# Parse arguments
# -----------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app-bundle)
            APP_BUNDLE="$2"
            shift 2
            ;;
        --homebrew)
            HOMEBREW_PREFIX="$2"
            shift 2
            ;;
        --verbose|-v)
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

# Validate arguments
if [[ -z "${APP_BUNDLE}" ]]; then
    error "Missing required argument: --app-bundle"
fi

if [[ ! -d "${APP_BUNDLE}" ]]; then
    error "App bundle not found: ${APP_BUNDLE}"
fi

detect_homebrew

# -----------------------------------------------------------------------------
# Setup paths
# -----------------------------------------------------------------------------

CONTENTS="${APP_BUNDLE}/Contents"
MACOS="${CONTENTS}/MacOS"
FRAMEWORKS="${CONTENTS}/Frameworks"
RESOURCES="${CONTENTS}/Resources"

# Create directories
mkdir -p "${FRAMEWORKS}"
mkdir -p "${RESOURCES}/lib"
mkdir -p "${RESOURCES}/share"

log "Bundling GTK resources into: ${APP_BUNDLE}"

# -----------------------------------------------------------------------------
# Bundle GdkPixbuf loaders
# -----------------------------------------------------------------------------

bundle_pixbuf_loaders() {
    log "Bundling GdkPixbuf loaders..."

    local PIXBUF_DIR="${HOMEBREW_PREFIX}/lib/gdk-pixbuf-2.0/2.10.0"
    local DEST_DIR="${RESOURCES}/lib/gdk-pixbuf-2.0/2.10.0"

    if [[ ! -d "${PIXBUF_DIR}" ]]; then
        log "  Warning: GdkPixbuf directory not found, skipping"
        return
    fi

    mkdir -p "${DEST_DIR}/loaders"

    # Copy loaders
    if [[ -d "${PIXBUF_DIR}/loaders" ]]; then
        for loader in "${PIXBUF_DIR}/loaders/"*.so; do
            if [[ -f "${loader}" ]]; then
                local name=$(basename "${loader}")
                debug "Copying loader: ${name}"
                cp "${loader}" "${DEST_DIR}/loaders/"
            fi
        done
    fi

    # Generate loaders.cache for bundle location
    if command -v gdk-pixbuf-query-loaders &>/dev/null; then
        log "  Generating loaders.cache..."
        GDK_PIXBUF_MODULEDIR="${DEST_DIR}/loaders" \
            gdk-pixbuf-query-loaders > "${DEST_DIR}/loaders.cache" 2>/dev/null || true

        # Fix paths in cache to use @executable_path
        if [[ -f "${DEST_DIR}/loaders.cache" ]]; then
            sed -i '' "s|${DEST_DIR}|@executable_path/../Resources/lib/gdk-pixbuf-2.0/2.10.0|g" \
                "${DEST_DIR}/loaders.cache" 2>/dev/null || true
        fi
    fi
}

# -----------------------------------------------------------------------------
# Bundle GIO modules
# -----------------------------------------------------------------------------

bundle_gio_modules() {
    log "Bundling GIO modules..."

    local GIO_DIR="${HOMEBREW_PREFIX}/lib/gio/modules"
    local DEST_DIR="${RESOURCES}/lib/gio/modules"

    if [[ ! -d "${GIO_DIR}" ]]; then
        log "  Warning: GIO modules directory not found, skipping"
        return
    fi

    mkdir -p "${DEST_DIR}"

    for module in "${GIO_DIR}/"*.so "${GIO_DIR}/"*.dylib; do
        if [[ -f "${module}" ]]; then
            local name=$(basename "${module}")
            debug "Copying GIO module: ${name}"
            cp "${module}" "${DEST_DIR}/"
        fi
    done

    # Compile GIO module cache
    if command -v gio-querymodules &>/dev/null; then
        log "  Compiling GIO module cache..."
        gio-querymodules "${DEST_DIR}" 2>/dev/null || true
    fi
}

# -----------------------------------------------------------------------------
# Bundle GLib schemas
# -----------------------------------------------------------------------------

bundle_glib_schemas() {
    log "Bundling GLib schemas..."

    local SCHEMA_DIR="${RESOURCES}/share/glib-2.0/schemas"
    mkdir -p "${SCHEMA_DIR}"

    # Copy system schemas if they exist
    local SYS_SCHEMAS="${HOMEBREW_PREFIX}/share/glib-2.0/schemas"
    if [[ -d "${SYS_SCHEMAS}" ]]; then
        for schema in "${SYS_SCHEMAS}/"*.xml "${SYS_SCHEMAS}/"*.override; do
            if [[ -f "${schema}" ]]; then
                cp "${schema}" "${SCHEMA_DIR}/" 2>/dev/null || true
            fi
        done
    fi

    # Compile schemas
    if command -v glib-compile-schemas &>/dev/null; then
        log "  Compiling GLib schemas..."
        glib-compile-schemas "${SCHEMA_DIR}" 2>/dev/null || true
    fi
}

# -----------------------------------------------------------------------------
# Bundle Adwaita icons
# -----------------------------------------------------------------------------

bundle_adwaita_icons() {
    log "Bundling Adwaita icons..."

    local ICON_DIR="${HOMEBREW_PREFIX}/share/icons/Adwaita"
    local DEST_DIR="${RESOURCES}/share/icons/Adwaita"

    if [[ ! -d "${ICON_DIR}" ]]; then
        log "  Warning: Adwaita icons not found, skipping"
        return
    fi

    mkdir -p "${DEST_DIR}"

    # Copy icon theme
    cp -R "${ICON_DIR}/"* "${DEST_DIR}/" 2>/dev/null || true

    # Update icon cache
    if command -v gtk4-update-icon-cache &>/dev/null; then
        log "  Updating icon cache..."
        gtk4-update-icon-cache -f -t "${DEST_DIR}" 2>/dev/null || true
    elif command -v gtk-update-icon-cache &>/dev/null; then
        gtk-update-icon-cache -f -t "${DEST_DIR}" 2>/dev/null || true
    fi
}

# -----------------------------------------------------------------------------
# Bundle hicolor icons
# -----------------------------------------------------------------------------

bundle_hicolor_icons() {
    log "Bundling hicolor icons..."

    local ICON_DIR="${HOMEBREW_PREFIX}/share/icons/hicolor"
    local DEST_DIR="${RESOURCES}/share/icons/hicolor"

    if [[ ! -d "${ICON_DIR}" ]]; then
        log "  Warning: hicolor icons not found, creating minimal"
        mkdir -p "${DEST_DIR}"
        return
    fi

    mkdir -p "${DEST_DIR}"
    cp -R "${ICON_DIR}/"* "${DEST_DIR}/" 2>/dev/null || true
}

# -----------------------------------------------------------------------------
# Bundle GTK4 settings and themes
# -----------------------------------------------------------------------------

bundle_gtk_settings() {
    log "Bundling GTK4 settings..."

    local GTK_DIR="${HOMEBREW_PREFIX}/share/gtk-4.0"
    local DEST_DIR="${RESOURCES}/share/gtk-4.0"

    if [[ -d "${GTK_DIR}" ]]; then
        mkdir -p "${DEST_DIR}"
        cp -R "${GTK_DIR}/"* "${DEST_DIR}/" 2>/dev/null || true
    fi

    # Create default settings.ini
    mkdir -p "${RESOURCES}/share/gtk-4.0"
    cat > "${RESOURCES}/share/gtk-4.0/settings.ini" << 'EOF'
[Settings]
gtk-application-prefer-dark-theme=0
gtk-font-name=SF Pro Display 13
gtk-icon-theme-name=Adwaita
gtk-theme-name=Adwaita
gtk-enable-animations=true
EOF
}

# -----------------------------------------------------------------------------
# Bundle GObject introspection typelibs (if using introspection)
# -----------------------------------------------------------------------------

bundle_typelibs() {
    log "Bundling GObject typelibs..."

    local TYPELIB_DIR="${HOMEBREW_PREFIX}/lib/girepository-1.0"
    local DEST_DIR="${RESOURCES}/lib/girepository-1.0"

    if [[ ! -d "${TYPELIB_DIR}" ]]; then
        log "  Warning: Typelib directory not found, skipping"
        return
    fi

    mkdir -p "${DEST_DIR}"

    # Only copy essential typelibs
    local ESSENTIAL_TYPELIBS=(
        "GLib-2.0.typelib"
        "GObject-2.0.typelib"
        "Gio-2.0.typelib"
        "GdkPixbuf-2.0.typelib"
        "Gtk-4.0.typelib"
        "Gdk-4.0.typelib"
        "Gsk-4.0.typelib"
        "Pango-1.0.typelib"
        "PangoCairo-1.0.typelib"
        "Adw-1.typelib"
        "cairo-1.0.typelib"
        "Graphene-1.0.typelib"
    )

    for typelib in "${ESSENTIAL_TYPELIBS[@]}"; do
        if [[ -f "${TYPELIB_DIR}/${typelib}" ]]; then
            debug "Copying typelib: ${typelib}"
            cp "${TYPELIB_DIR}/${typelib}" "${DEST_DIR}/"
        fi
    done
}

# -----------------------------------------------------------------------------
# Fix library paths in Frameworks
# -----------------------------------------------------------------------------

fix_framework_paths() {
    log "Fixing library paths in Frameworks..."

    for dylib in "${FRAMEWORKS}/"*.dylib "${FRAMEWORKS}/"*.so; do
        if [[ ! -f "${dylib}" ]]; then
            continue
        fi

        local name=$(basename "${dylib}")
        debug "Processing: ${name}"

        # Set the install name
        install_name_tool -id "@executable_path/../Frameworks/${name}" "${dylib}" 2>/dev/null || true

        # Fix references to other bundled libraries
        local deps
        deps=$(otool -L "${dylib}" 2>/dev/null | grep -E "^\s+/" | awk '{print $1}' | \
            grep -v '^/System/' | grep -v '^/usr/lib/' || true)

        for dep in ${deps}; do
            local dep_name=$(basename "${dep}")
            if [[ -f "${FRAMEWORKS}/${dep_name}" ]]; then
                install_name_tool -change "${dep}" "@executable_path/../Frameworks/${dep_name}" "${dylib}" 2>/dev/null || true
            fi
        done
    done
}

# -----------------------------------------------------------------------------
# Fix library paths in Resources
# -----------------------------------------------------------------------------

fix_resource_lib_paths() {
    log "Fixing library paths in Resources..."

    # Fix paths in GdkPixbuf loaders
    for loader in "${RESOURCES}/lib/gdk-pixbuf-2.0/2.10.0/loaders/"*.so; do
        if [[ -f "${loader}" ]]; then
            local deps
            deps=$(otool -L "${loader}" 2>/dev/null | grep -E "^\s+/" | awk '{print $1}' | \
                grep -v '^/System/' | grep -v '^/usr/lib/' || true)

            for dep in ${deps}; do
                local dep_name=$(basename "${dep}")
                if [[ -f "${FRAMEWORKS}/${dep_name}" ]]; then
                    install_name_tool -change "${dep}" "@executable_path/../Frameworks/${dep_name}" "${loader}" 2>/dev/null || true
                fi
            done
        fi
    done

    # Fix paths in GIO modules
    for module in "${RESOURCES}/lib/gio/modules/"*.so "${RESOURCES}/lib/gio/modules/"*.dylib; do
        if [[ -f "${module}" ]]; then
            local deps
            deps=$(otool -L "${module}" 2>/dev/null | grep -E "^\s+/" | awk '{print $1}' | \
                grep -v '^/System/' | grep -v '^/usr/lib/' || true)

            for dep in ${deps}; do
                local dep_name=$(basename "${dep}")
                if [[ -f "${FRAMEWORKS}/${dep_name}" ]]; then
                    install_name_tool -change "${dep}" "@executable_path/../Frameworks/${dep_name}" "${module}" 2>/dev/null || true
                fi
            done
        fi
    done
}

# -----------------------------------------------------------------------------
# Update launcher script with GTK environment
# -----------------------------------------------------------------------------

update_launcher() {
    log "Updating launcher script..."

    local LAUNCHER="${MACOS}/gnostr-signer-launcher"

    cat > "${LAUNCHER}" << 'LAUNCHER_EOF'
#!/bin/bash
# =============================================================================
# Gnostr Signer Launcher
# =============================================================================
# Sets up GTK4/GLib environment and launches the application.

BUNDLE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CONTENTS="${BUNDLE_DIR}/Contents"
RESOURCES="${CONTENTS}/Resources"
FRAMEWORKS="${CONTENTS}/Frameworks"

# Library paths
export DYLD_FALLBACK_LIBRARY_PATH="${FRAMEWORKS}:${DYLD_FALLBACK_LIBRARY_PATH}"

# GLib/GTK resource paths
export XDG_DATA_DIRS="${RESOURCES}/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GSETTINGS_SCHEMA_DIR="${RESOURCES}/share/glib-2.0/schemas"

# GdkPixbuf loaders
export GDK_PIXBUF_MODULE_FILE="${RESOURCES}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GDK_PIXBUF_MODULEDIR="${RESOURCES}/lib/gdk-pixbuf-2.0/2.10.0/loaders"

# GIO modules
export GIO_MODULE_DIR="${RESOURCES}/lib/gio/modules"

# GTK settings
export GTK_DATA_PREFIX="${RESOURCES}"
export GTK_EXE_PREFIX="${RESOURCES}"
export GTK_PATH="${RESOURCES}"

# GObject introspection
export GI_TYPELIB_PATH="${RESOURCES}/lib/girepository-1.0"

# Icon theme
export GTK_ICON_THEME="Adwaita"

# Font configuration (use system fonts)
export FONTCONFIG_FILE="/opt/homebrew/etc/fonts/fonts.conf"
if [[ ! -f "${FONTCONFIG_FILE}" ]]; then
    export FONTCONFIG_FILE="/usr/local/etc/fonts/fonts.conf"
fi

# Launch the actual executable
exec "${CONTENTS}/MacOS/gnostr-signer" "$@"
LAUNCHER_EOF

    chmod +x "${LAUNCHER}"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

log "Starting GTK bundling process..."

bundle_pixbuf_loaders
bundle_gio_modules
bundle_glib_schemas
bundle_adwaita_icons
bundle_hicolor_icons
bundle_gtk_settings
bundle_typelibs
fix_framework_paths
fix_resource_lib_paths
update_launcher

log "GTK bundling complete!"

# Summary
echo ""
echo "Bundle contents:"
du -sh "${FRAMEWORKS}" 2>/dev/null || echo "  Frameworks: (empty)"
du -sh "${RESOURCES}/lib" 2>/dev/null || echo "  Resources/lib: (empty)"
du -sh "${RESOURCES}/share" 2>/dev/null || echo "  Resources/share: (empty)"
