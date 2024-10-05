#!/bin/bash

# Check if a NIP name is provided
if [ -z "$1" ]; then
  echo "Usage: $0 <nip_name>"
  exit 1
fi

NIP_NAME=$1

# Define directories and files
BASE_DIR=$NIP_NAME
INCLUDE_DIR="$BASE_DIR/include"
SRC_DIR="$BASE_DIR/src"
EXAMPLES_DIR="$BASE_DIR/examples"
TESTS_DIR="$BASE_DIR/tests"

# Create directories
mkdir -p "$INCLUDE_DIR" "$SRC_DIR" "$EXAMPLES_DIR" "$TESTS_DIR"

# Create header and source files for the NIP
HEADER_FILE="$INCLUDE_DIR/${NIP_NAME}.h"
SOURCE_FILE="$SRC_DIR/${NIP_NAME}.c"
EXAMPLES_FILE="$EXAMPLES_DIR/example.c"


# Create the header file
touch "$HEADER_FILE"

# Create the source file
touch "$SOURCE_FILE"

# Create the source file
touch "$EXAMPLES_FILE"

# Print success message
echo "Created directories and files for NIP-$NIP_NAME"
