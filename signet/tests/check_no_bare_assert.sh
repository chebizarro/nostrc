#!/bin/sh
# SPDX-License-Identifier: MIT
#
# fp-3126 guard: fail if a signet test uses bare assert().
#
# The repo builds Release, which defines NDEBUG, under which assert(expr)
# expands to ((void)0) and the expression -- including any side-effecting
# call inside it -- is never evaluated. That silently turned the whole
# signet C suite into a no-op. Tests must use CHECK() from test_check.h,
# which always evaluates.
#
# Usage: check_no_bare_assert.sh <tests-source-dir>

set -eu

dir="${1:-$(dirname "$0")}"

# Match `assert(` only when not preceded by an identifier character, so
# g_assert(, verify_assertion(, assert_payload_...( etc. do not trip it.
# Skip test_check.h (which documents the hazard) and comment lines.
hits=$(
  grep -nE '(^|[^_A-Za-z0-9])assert\(' \
    "$dir"/*.c "$dir"/*.h "$dir"/phase0/*.c 2>/dev/null \
  | grep -v '/test_check\.h:' \
  | grep -vE ':[0-9]+:[[:space:]]*(\*|//|/\*)' \
  || true
)

if [ -n "$hits" ]; then
  printf '\nFAIL: bare assert() found in the signet test suite.\n' >&2
  printf 'Under the Release build (-DNDEBUG) these are compiled out entirely,\n' >&2
  printf 'taking any side-effecting call inside them with it. Use CHECK() from\n' >&2
  printf 'test_check.h instead -- it always evaluates. See fp-3126.\n\n' >&2
  printf '%s\n' "$hits" >&2
  exit 1
fi

echo "OK: no bare assert() in signet tests"
exit 0
