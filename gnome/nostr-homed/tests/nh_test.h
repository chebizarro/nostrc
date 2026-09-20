/* Always-on test assertion for nostr-homed tests.
 *
 * Unlike assert(), NH_CHECK() is never compiled out under NDEBUG. These tests
 * historically placed their work inside assert() (e.g. NH_CHECK(build(...)==0)),
 * so a build type that defines NDEBUG (Release/RelWithDebInfo) silently turned
 * the suite into a no-op — and some tests then dereferenced uninitialised state.
 * NH_CHECK evaluates its expression exactly once and aborts on failure with a
 * diagnostic, regardless of build type. See beads nostrc-rb0e.16.
 */
#ifndef NH_TEST_H
#define NH_TEST_H

#include <stdio.h>
#include <stdlib.h>

#define NH_CHECK(expr)                                                     \
  do {                                                                     \
    if (!(expr)) {                                                         \
      fprintf(stderr, "NH_CHECK failed: %s (%s:%d)\n", #expr,             \
              __FILE__, __LINE__);                                         \
      abort();                                                             \
    }                                                                      \
  } while (0)

#endif /* NH_TEST_H */
