#!/usr/bin/env bash
#
# Run gnostr with proper GSettings schema directory for development.
#
# Usage:
#   ./apps/gnostr/run-gnostr.sh          # Run from project root
#   ./apps/gnostr/run-gnostr.sh --help   # Pass args to gnostr
#
# The script automatically locates the build directory and sets
# GSETTINGS_SCHEMA_DIR so that the compiled schemas are found.

set -euo pipefail

# Find the directory where this script lives
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Determine build directory - check common locations
BUILD_DIR=""
for candidate in "${PROJECT_ROOT}/build" "${PROJECT_ROOT}/cmake-build-debug" "${PROJECT_ROOT}/cmake-build-release"; do
    if [[ -d "${candidate}/apps/gnostr" ]]; then
        BUILD_DIR="${candidate}"
        break
    fi
done

if [[ -z "${BUILD_DIR}" ]]; then
    echo "Error: Could not find build directory with apps/gnostr." >&2
    echo "Please build the project first:" >&2
    echo "  cmake -B build && cmake --build build" >&2
    exit 1
fi

GNOSTR_BIN="${BUILD_DIR}/apps/gnostr/gnostr"
SCHEMA_DIR="${BUILD_DIR}/apps/gnostr"

if [[ ! -x "${GNOSTR_BIN}" ]]; then
    echo "Error: gnostr binary not found at ${GNOSTR_BIN}" >&2
    echo "Please build the project first:" >&2
    echo "  cmake --build ${BUILD_DIR} --target gnostr" >&2
    exit 1
fi

if [[ ! -f "${SCHEMA_DIR}/gschemas.compiled" ]]; then
    echo "Error: Compiled schemas not found at ${SCHEMA_DIR}/gschemas.compiled" >&2
    echo "Please rebuild to generate schemas:" >&2
    echo "  cmake --build ${BUILD_DIR} --target gnostr-schemas" >&2
    exit 1
fi

# Export the schema directory and run gnostr
export GSETTINGS_SCHEMA_DIR="${SCHEMA_DIR}"
exec "${GNOSTR_BIN}" "$@"
