#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
exec python3 "$(dirname "$0")/ops.py" preflight "$@"
