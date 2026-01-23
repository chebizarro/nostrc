#!/bin/bash
# =============================================================================
# generate-icns.sh - Generate macOS ICNS icon from SVG
# =============================================================================
#
# Creates AppIcon.icns from the SVG icon for use in the macOS app bundle.
#
# Usage:
#   ./generate-icns.sh [--svg PATH] [--output PATH]
#
# Options:
#   --svg PATH      Path to source SVG (default: ../../data/icons/hicolor/scalable/apps/org.gnostr.Signer.svg)
#   --output PATH   Output ICNS path (default: ./AppIcon.icns)
#   --help          Show this help message
#
# Requirements:
#   - librsvg (rsvg-convert): brew install librsvg
#   - iconutil (comes with Xcode Command Line Tools)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_SVG="${SCRIPT_DIR}/../../data/icons/hicolor/scalable/apps/org.gnostr.Signer.svg"

# Default paths
SVG_PATH="${DEFAULT_SVG}"
OUTPUT_PATH="${SCRIPT_DIR}/AppIcon.icns"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --svg)
            SVG_PATH="$2"
            shift 2
            ;;
        --output)
            OUTPUT_PATH="$2"
            shift 2
            ;;
        --help|-h)
            head -n 20 "$0" | grep -E '^#' | sed 's/^# *//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

# Check prerequisites
if ! command -v rsvg-convert &>/dev/null; then
    echo "Error: rsvg-convert not found. Install with: brew install librsvg" >&2
    exit 1
fi

if ! command -v iconutil &>/dev/null; then
    echo "Error: iconutil not found. Install Xcode Command Line Tools: xcode-select --install" >&2
    exit 1
fi

if [[ ! -f "${SVG_PATH}" ]]; then
    echo "Error: SVG file not found: ${SVG_PATH}" >&2
    exit 1
fi

echo "Generating ICNS from: ${SVG_PATH}"
echo "Output: ${OUTPUT_PATH}"

# Create temporary iconset directory
ICONSET_DIR=$(mktemp -d)/AppIcon.iconset
mkdir -p "${ICONSET_DIR}"

# Generate all required sizes for macOS icons
# macOS requires these specific sizes in the iconset:
# - icon_16x16.png, icon_16x16@2x.png (32x32)
# - icon_32x32.png, icon_32x32@2x.png (64x64)
# - icon_128x128.png, icon_128x128@2x.png (256x256)
# - icon_256x256.png, icon_256x256@2x.png (512x512)
# - icon_512x512.png, icon_512x512@2x.png (1024x1024)

echo "Generating PNG sizes..."

for size in 16 32 128 256 512; do
    echo "  ${size}x${size}"
    rsvg-convert -w ${size} -h ${size} "${SVG_PATH}" -o "${ICONSET_DIR}/icon_${size}x${size}.png"

    # @2x version (Retina)
    retina_size=$((size * 2))
    echo "  ${size}x${size}@2x (${retina_size}x${retina_size})"
    rsvg-convert -w ${retina_size} -h ${retina_size} "${SVG_PATH}" -o "${ICONSET_DIR}/icon_${size}x${size}@2x.png"
done

# Convert iconset to ICNS
echo "Converting to ICNS..."
iconutil -c icns -o "${OUTPUT_PATH}" "${ICONSET_DIR}"

# Cleanup
rm -rf "$(dirname "${ICONSET_DIR}")"

# Verify output
if [[ -f "${OUTPUT_PATH}" ]]; then
    SIZE=$(du -h "${OUTPUT_PATH}" | cut -f1)
    echo "Successfully created: ${OUTPUT_PATH} (${SIZE})"
else
    echo "Error: Failed to create ICNS file" >&2
    exit 1
fi
