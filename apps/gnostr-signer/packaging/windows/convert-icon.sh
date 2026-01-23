#!/bin/bash
# =============================================================================
# convert-icon.sh - Convert SVG icon to Windows ICO format
# =============================================================================
#
# Converts the application SVG icon to Windows ICO format for use in the
# installer and executable.
#
# Prerequisites:
#   - ImageMagick (convert) or Inkscape
#   - icotool (from icoutils package)
#
# Usage:
#   ./convert-icon.sh
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${SCRIPT_DIR}/../.."
SVG_ICON="${APP_DIR}/data/icons/hicolor/scalable/apps/org.gnostr.Signer.svg"
OUTPUT_DIR="${APP_DIR}/data/icons"
OUTPUT_ICO="${OUTPUT_DIR}/gnostr-signer.ico"

if [[ ! -f "${SVG_ICON}" ]]; then
    echo "Error: SVG icon not found: ${SVG_ICON}"
    exit 1
fi

echo "Converting SVG to ICO..."

# Create temp directory for PNGs
TEMP_DIR=$(mktemp -d)
trap "rm -rf ${TEMP_DIR}" EXIT

# Icon sizes for Windows ICO (16, 24, 32, 48, 64, 128, 256)
SIZES=(16 24 32 48 64 128 256)

# Convert SVG to multiple PNG sizes
if command -v rsvg-convert &>/dev/null; then
    echo "Using rsvg-convert..."
    for size in "${SIZES[@]}"; do
        rsvg-convert -w ${size} -h ${size} "${SVG_ICON}" -o "${TEMP_DIR}/icon_${size}.png"
        echo "  Generated ${size}x${size}"
    done
elif command -v convert &>/dev/null; then
    echo "Using ImageMagick..."
    for size in "${SIZES[@]}"; do
        convert -background none -density 300 "${SVG_ICON}" -resize ${size}x${size} "${TEMP_DIR}/icon_${size}.png"
        echo "  Generated ${size}x${size}"
    done
elif command -v inkscape &>/dev/null; then
    echo "Using Inkscape..."
    for size in "${SIZES[@]}"; do
        inkscape --export-type=png --export-filename="${TEMP_DIR}/icon_${size}.png" -w ${size} -h ${size} "${SVG_ICON}" 2>/dev/null
        echo "  Generated ${size}x${size}"
    done
else
    echo "Error: No SVG converter found. Install one of:"
    echo "  - librsvg (rsvg-convert)"
    echo "  - ImageMagick (convert)"
    echo "  - Inkscape"
    exit 1
fi

# Create ICO file
if command -v icotool &>/dev/null; then
    echo "Creating ICO with icotool..."
    icotool -c -o "${OUTPUT_ICO}" "${TEMP_DIR}"/icon_*.png
elif command -v convert &>/dev/null; then
    echo "Creating ICO with ImageMagick..."
    convert "${TEMP_DIR}"/icon_*.png "${OUTPUT_ICO}"
else
    echo "Error: Cannot create ICO file. Install one of:"
    echo "  - icoutils (icotool)"
    echo "  - ImageMagick (convert)"
    exit 1
fi

echo "Created: ${OUTPUT_ICO}"

# Also copy to Windows packaging directory for easy access
cp "${OUTPUT_ICO}" "${SCRIPT_DIR}/" 2>/dev/null || true

# Generate PNG icons for various sizes (for Windows installer welcome/finish pages)
for size in 150 164; do
    if command -v rsvg-convert &>/dev/null; then
        rsvg-convert -w ${size} -h 314 "${SVG_ICON}" -o "${OUTPUT_DIR}/gnostr-signer-${size}x314.png"
    fi
done

echo "Icon conversion complete!"
