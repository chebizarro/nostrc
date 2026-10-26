#!/bin/bash

# Check if a NIP name is provided
if [ -z "$1" ]; then
  echo "Usage: $0 <nip_name>"
  exit 1
fi

set -eu
NIP_NAME=$1
if [[ ! "$NIP_NAME" =~ ^[a-zA-Z][a-zA-Z0-9_-]*$ ]]; then
  echo "Invalid NIP name: $NIP_NAME" >&2
  exit 1
fi
NIP_NAMESPACE=${NIP_NAME//-/_}
UPPER_NIP_NAME=$(echo "$NIP_NAMESPACE" | tr '[:lower:]' '[:upper:]')
if [ -e "$NIP_NAME" ]; then
  echo "Refusing to overwrite existing package: $NIP_NAME" >&2
  exit 1
fi

# Define directories and files
BASE_DIR=$NIP_NAME
INCLUDE_DIR="$BASE_DIR/include/nostr/$NIP_NAMESPACE"
SRC_DIR="$BASE_DIR/src"
EXAMPLES_DIR="$BASE_DIR/examples"
TESTS_DIR="$BASE_DIR/tests"

# Create directories
mkdir -p "$INCLUDE_DIR" "$SRC_DIR" "$EXAMPLES_DIR" "$TESTS_DIR"

# Create header and source files for the NIP
HEADER_FILE="$INCLUDE_DIR/${NIP_NAME}.h"
SOURCE_FILE="$SRC_DIR/${NIP_NAME}.c"
EXAMPLES_FILE="$EXAMPLES_DIR/example.c"


# Public declarations use this package's export macro.
cat > "$HEADER_FILE" <<EOF
#ifndef NOSTR_${UPPER_NIP_NAME}_H
#define NOSTR_${UPPER_NIP_NAME}_H

#include <nostr/$NIP_NAMESPACE/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prefix public function declarations with NOSTR_${UPPER_NIP_NAME}_API. */

#ifdef __cplusplus
}
#endif
#endif
EOF

# Create the source file
echo "#include <nostr/$NIP_NAMESPACE/$NIP_NAME.h>" > "$SOURCE_FILE"

# Create the source file
touch "$EXAMPLES_FILE"

# Print success message
echo "Created directories and files for NIP-$NIP_NAME"
