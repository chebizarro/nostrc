#!/bin/bash
# =============================================================================
# build-msys2.sh - Build GNostr Signer on Windows using MSYS2/MinGW-w64
# =============================================================================
#
# This script builds GNostr Signer natively on Windows using the MSYS2 build
# environment with MinGW-w64 toolchain.
#
# Prerequisites:
#   1. Install MSYS2 from https://www.msys2.org/
#   2. Run this script from MSYS2 MinGW64 shell (NOT MSYS2 MSYS shell)
#   3. Run with: ./build-msys2.sh
#
# Usage:
#   ./build-msys2.sh [options]
#
# Options:
#   --install-deps    Install required MSYS2 packages first
#   --clean           Clean build directory before building
#   --release         Build in Release mode (default: Debug)
#   --package         Create installer package after build
#   --help            Show this help message
#
# Environment Variables:
#   MSYSTEM           Must be MINGW64 or UCRT64
#   BUILD_TYPE        Debug or Release (default: Debug)
#
# =============================================================================

set -euo pipefail

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${SCRIPT_DIR}/../.."
PROJECT_ROOT="${APP_DIR}/../.."

# Default options
INSTALL_DEPS=false
CLEAN_BUILD=false
BUILD_TYPE="${BUILD_TYPE:-Debug}"
CREATE_PACKAGE=false

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "${GREEN}[BUILD]${NC} $*"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $*" >&2
}

error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
    exit 1
}

usage() {
    head -n 30 "$0" | grep -E '^#' | sed 's/^# *//'
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps)
            INSTALL_DEPS=true
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --package)
            CREATE_PACKAGE=true
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
# Environment Validation
# -----------------------------------------------------------------------------

log "Checking build environment..."

# Verify we're in MSYS2 MinGW shell
if [[ -z "${MSYSTEM:-}" ]]; then
    error "This script must be run from MSYS2 MinGW64 or UCRT64 shell"
fi

if [[ "${MSYSTEM}" != "MINGW64" && "${MSYSTEM}" != "UCRT64" ]]; then
    error "Please run this script from MINGW64 or UCRT64 shell, not ${MSYSTEM}"
fi

log "Build environment: ${MSYSTEM}"
log "Build type: ${BUILD_TYPE}"

# -----------------------------------------------------------------------------
# Install Dependencies
# -----------------------------------------------------------------------------

install_dependencies() {
    log "Installing MSYS2 dependencies..."

    # Update package database
    pacman -Sy --noconfirm

    # Determine package prefix based on MSYSTEM
    local PREFIX
    if [[ "${MSYSTEM}" == "MINGW64" ]]; then
        PREFIX="mingw-w64-x86_64"
    else
        PREFIX="mingw-w64-ucrt-x86_64"
    fi

    # Core build tools
    local BUILD_TOOLS=(
        "base-devel"
        "${PREFIX}-toolchain"
        "${PREFIX}-cmake"
        "${PREFIX}-ninja"
        "${PREFIX}-pkgconf"
    )

    # GTK4 and dependencies
    local GTK_DEPS=(
        "${PREFIX}-gtk4"
        "${PREFIX}-libadwaita"
        "${PREFIX}-glib2"
        "${PREFIX}-json-glib"
        "${PREFIX}-gobject-introspection"
    )

    # Crypto libraries
    local CRYPTO_DEPS=(
        "${PREFIX}-libsodium"
        "${PREFIX}-libsecp256k1"
    )

    # Optional dependencies
    local OPTIONAL_DEPS=(
        "${PREFIX}-libsecret"
        "${PREFIX}-p11-kit"
        "${PREFIX}-nsync"
    )

    # Installer tools
    local INSTALLER_DEPS=(
        "${PREFIX}-nsis"
    )

    log "Installing build tools..."
    pacman -S --noconfirm --needed "${BUILD_TOOLS[@]}"

    log "Installing GTK4 dependencies..."
    pacman -S --noconfirm --needed "${GTK_DEPS[@]}"

    log "Installing crypto libraries..."
    pacman -S --noconfirm --needed "${CRYPTO_DEPS[@]}"

    log "Installing optional dependencies..."
    pacman -S --noconfirm --needed "${OPTIONAL_DEPS[@]}" || true

    log "Installing NSIS..."
    pacman -S --noconfirm --needed "${INSTALLER_DEPS[@]}"

    log "Dependencies installed successfully"
}

if [[ "${INSTALL_DEPS}" == "true" ]]; then
    install_dependencies
fi

# -----------------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------------

BUILD_DIR="${PROJECT_ROOT}/build-windows"

if [[ "${CLEAN_BUILD}" == "true" && -d "${BUILD_DIR}" ]]; then
    log "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

log "Configuring CMake..."
cd "${BUILD_DIR}"

cmake "${PROJECT_ROOT}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/install" \
    -DBUILD_TESTING=OFF \
    -DBUILD_SIGNER_TESTS=OFF \
    -DBUILD_NATIVE_HOST=ON \
    -DENABLE_TCP_IPC=OFF

log "Building..."
cmake --build . --parallel

log "Build completed successfully!"

# Verify outputs
if [[ -f "${BUILD_DIR}/apps/gnostr-signer/gnostr-signer.exe" ]]; then
    log "Main application: ${BUILD_DIR}/apps/gnostr-signer/gnostr-signer.exe"
else
    error "Build failed: gnostr-signer.exe not found"
fi

if [[ -f "${BUILD_DIR}/apps/gnostr-signer/gnostr-signer-daemon.exe" ]]; then
    log "Daemon: ${BUILD_DIR}/apps/gnostr-signer/gnostr-signer-daemon.exe"
fi

# -----------------------------------------------------------------------------
# Package
# -----------------------------------------------------------------------------

if [[ "${CREATE_PACKAGE}" == "true" ]]; then
    log "Creating installer package..."

    # First bundle dependencies
    "${SCRIPT_DIR}/bundle-deps.sh" --build-dir "${BUILD_DIR}"

    # Build NSIS installer
    cd "${SCRIPT_DIR}"
    makensis \
        -DVERSION="1.0.0" \
        -DARCH="x64" \
        -DBUILD_DIR="${BUILD_DIR}" \
        -DDEPS_DIR="${BUILD_DIR}/deps" \
        gnostr-signer.nsi

    log "Installer created: ${SCRIPT_DIR}/GNostrSigner-1.0.0-x64-setup.exe"
fi

log "Build process completed!"
