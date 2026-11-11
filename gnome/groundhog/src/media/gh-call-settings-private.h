#ifndef GH_CALL_SETTINGS_PRIVATE_H
#define GH_CALL_SETTINGS_PRIVATE_H
#ifdef __APPLE__
#include <Security/Security.h>
/* Test-only injection. Retained; each async request snapshots its reference.
 * NULL restores the build's default backend. Never changes system Keychain
 * preferences. Tests must set NOSTRC_TEST_ISOLATION before doing any work. */
void gh_call_turn_password_set_test_keychain(SecKeychainRef keychain);
#endif
#endif
