/* SPDX-License-Identifier: MIT
 *
 * test_check.h - always-evaluated assertions for the signet test suite.
 *
 * Why this exists (fp-3126):
 *
 * The repo's CMake build is CMAKE_BUILD_TYPE=Release, which defines NDEBUG.
 * Under NDEBUG the C standard requires assert(expr) to expand to ((void)0) --
 * the expression is not evaluated at all. Signet's tests used bare assert()
 * for side-effecting calls, e.g.
 *
 *     assert(signet_store_put_agent(store, ...) == 0);
 *
 * so in the shipped build configuration the store was never populated, the
 * call never ran, and ctest reported PASS having tested nothing (or crashed
 * later on the NULLs the skipped setup left behind). A test that silently
 * evaluates to nothing is worse than no test.
 *
 * Use CHECK() instead of assert(). It always evaluates its expression, in
 * every build type, and reports file:line and the failing text before
 * exiting non-zero.
 *
 * Belt and braces: including this header also restores a working assert()
 * for the translation unit, so a stray assert() that slips past review
 * degrades to "non-idiomatic" rather than "silently deleted". The build
 * files additionally compile signet tests with -UNDEBUG, and the
 * `no_bare_assert` test fails the suite if a bare assert( reappears.
 */

#ifndef SIGNET_TESTS_TEST_CHECK_H
#define SIGNET_TESTS_TEST_CHECK_H

/* Must come before <assert.h>. assert.h is deliberately re-includable and
 * re-reads NDEBUG each time, so this works regardless of whether some other
 * header pulled it in first. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>

static inline void signet_test_check_failed(const char *file, int line,
                                            const char *expr) {
  fprintf(stderr, "\nFAIL %s:%d: %s\n", file, line, expr);
  fflush(stderr);
  exit(1);
}

/* Variadic so that expressions containing top-level commas still work. */
#define CHECK(...)                                                      \
  do {                                                                  \
    if (!(__VA_ARGS__))                                                 \
      signet_test_check_failed(__FILE__, __LINE__, #__VA_ARGS__);       \
  } while (0)

/* CHECK with a human-readable label instead of the stringified expression. */
#define CHECK_MSG(expr, msg)                                            \
  do {                                                                  \
    if (!(expr)) signet_test_check_failed(__FILE__, __LINE__, (msg));   \
  } while (0)

#endif /* SIGNET_TESTS_TEST_CHECK_H */
