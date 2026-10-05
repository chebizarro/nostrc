/* nostrc-test-keychain-guard.h — a test that uses a throwaway macOS keychain
 * proves it left the developer's keychain configuration alone (nostrc-2hmd).
 *
 * nostrc_test_keychain_guard_begin() records the user's default keychain and
 * search list; nostrc_test_keychain_guard_end() aborts the test if either has
 * changed. Read-only: nothing here writes the configuration. A test creates
 * its keychain with SecKeychainCreate (which does not add it to the search
 * list), addresses it by reference (kSecUseKeychain / kSecMatchSearchList) and
 * never calls SecKeychainSetDefault or SecKeychainSetSearchList, nor the
 * `security default-keychain -s` / `list-keychains -s` commands.
 * No-ops off macOS. */
#ifndef NOSTRC_TEST_KEYCHAIN_GUARD_H
#define NOSTRC_TEST_KEYCHAIN_GUARD_H

#ifdef __APPLE__
#include <Security/Security.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wunused-function"

static char nostrc_test_keychain_guard_before[4096];

static void
nostrc_test_keychain_guard_snapshot(char *out, size_t size)
{
  size_t used = 0;
  out[0] = '\0';
  SecKeychainRef def = NULL;
  char path[1024];
  UInt32 len = sizeof path - 1;
  if (SecKeychainCopyDefault(&def) == errSecSuccess && def &&
      SecKeychainGetPath(def, &len, path) == errSecSuccess) {
    path[len] = '\0';
    used += (size_t)snprintf(out + used, size - used, "default=%s;", path);
  } else {
    used += (size_t)snprintf(out + used, size - used, "default=<none>;");
  }
  if (def)
    CFRelease(def);
  CFArrayRef list = NULL;
  if (SecKeychainCopyDomainSearchList(kSecPreferencesDomainUser, &list) == errSecSuccess && list) {
    for (CFIndex i = 0; i < CFArrayGetCount(list) && used < size; i++) {
      SecKeychainRef kc = (SecKeychainRef)CFArrayGetValueAtIndex(list, i);
      len = sizeof path - 1;
      if (SecKeychainGetPath(kc, &len, path) == errSecSuccess) {
        path[len] = '\0';
        used += (size_t)snprintf(out + used, size - used, "list=%s;", path);
      }
    }
    CFRelease(list);
  }
}

static void
nostrc_test_keychain_guard_begin(void)
{
  nostrc_test_keychain_guard_snapshot(nostrc_test_keychain_guard_before,
                                      sizeof nostrc_test_keychain_guard_before);
}

static void
nostrc_test_keychain_guard_end(void)
{
  char after[sizeof nostrc_test_keychain_guard_before];
  nostrc_test_keychain_guard_snapshot(after, sizeof after);
  if (strcmp(nostrc_test_keychain_guard_before, after) != 0) {
    fprintf(stderr,
            "keychain guard: this test changed the developer's keychain configuration "
            "(nostrc-2hmd)\n  before: %s\n  after:  %s\n",
            nostrc_test_keychain_guard_before, after);
    abort();
  }
}

#pragma clang diagnostic pop
#else
static inline void nostrc_test_keychain_guard_begin(void) {}
static inline void nostrc_test_keychain_guard_end(void) {}
#endif

#endif /* NOSTRC_TEST_KEYCHAIN_GUARD_H */
