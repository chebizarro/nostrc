#!/usr/bin/env bash
# btcore.sh - Extract full multi-thread backtrace from a core dump
# Usage: tools/btcore.sh <binary> <core-file>
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <binary> <core-file>" >&2
  exit 1
fi

bin="$1"
core="$2"

if [[ ! -f "$bin" ]]; then
  echo "Error: binary '$bin' not found" >&2
  exit 1
fi

if [[ ! -f "$core" ]]; then
  echo "Error: core file '$core' not found" >&2
  exit 1
fi

gdb -q "$bin" "$core" \
  -ex "set pagination off" \
  -ex "echo === THREAD INFO ===\n" \
  -ex "info threads" \
  -ex "echo \n=== ALL THREAD BACKTRACES ===\n" \
  -ex "thread apply all bt full" \
  -ex "echo \n=== REGISTERS (current thread) ===\n" \
  -ex "info registers" \
  -ex "quit"
