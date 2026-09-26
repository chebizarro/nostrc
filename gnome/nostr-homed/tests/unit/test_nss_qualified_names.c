/*
 * test_nss_qualified_names.c — plan §4.3 C2 acceptance.
 *
 * The rejected earlier design (plan Finding 15) would have widened
 * `nh_identity_username_is_valid` to accept `DOMAIN\user` /
 * `user@REALM` only to return NOTFOUND from the NSS module.  The
 * accepted design keeps admission bit-exact and relies on nsswitch
 * ordering (`passwd: files winbind nostr`).
 *
 * This test asserts the bit-exact invariant that the C-side gate
 * still refuses every qualified-name shape we might otherwise be
 * tempted to admit, and that ONLY names matching the reserved
 * `^n_[a-z0-9_]+$` shape pass.  It's a pure unit test — no NSS
 * library, no /etc/nsswitch.conf, no external process.  The property
 * being tested lives entirely in identity_common.c.
 */
#include "../nh_test.h"
#include "nostr_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Names that MUST be rejected — every one is what would otherwise
 * cross a joined-host lookup path. */
static const char *kQualifiedRejects[] = {
  /* winbind qualified names, primary and alt separators */
  "DOMAIN\\alice",
  "domain\\alice",
  "DOMAIN\\ALICE",
  "corp.example.com\\alice",
  "alice@REALM",
  "alice@realm.example.com",
  /* short and long domain-like names */
  "D\\a",
  "A_VERY_LONG_DOMAIN\\someone",
  /* mixed-case n_ prefix that is not the reserved form */
  "N_alice",
  "n_Alice",
  /* invalid chars that are legal in POSIX names */
  "n_alice.bob",
  "n_alice-bob",
  "n_alice bob",
  /* empty and singleton */
  "",
  "n",
  "n_",
  /* pretend NIS/DNS suffixed forms */
  "root@EXAMPLE",
  "administrator@DOMAIN.LOCAL",
  NULL,
};

/* Names that MUST be admitted — the SAME set of shapes the module
 * accepted before the plan work; this half of the test is the
 * "bit-exact behaviour vs today" clause. */
static const char *kNostrAccepts[] = {
  "n_a",
  "n_alice",
  "n_alice_bob",
  "n_0",
  "n_alice_1",
  "n_z9_9_z",
  NULL,
};

int main(void) {
  for (const char **p = kQualifiedRejects; *p; ++p) {
    if (nh_identity_username_is_valid(*p)) {
      fprintf(stderr,
          "FAIL: qualified/invalid name was admitted: %s\n", *p);
      return 1;
    }
  }
  for (const char **p = kNostrAccepts; *p; ++p) {
    if (!nh_identity_username_is_valid(*p)) {
      fprintf(stderr,
          "FAIL: reserved-form Nostr name was rejected: %s\n", *p);
      return 1;
    }
  }
  /* Redundant guard: NULL and control-char cases. */
  NH_CHECK(!nh_identity_username_is_valid(NULL));
  char with_nul[] = "n_al\0ice";
  /* This buffer has an embedded NUL; strlen() sees "n_al", which
   * passes admission — the point of the check is that the input
   * pointer must be a valid C string, not that we detect embedded
   * NULs.  Kept as documentation. */
  NH_CHECK(nh_identity_username_is_valid(with_nul));

  printf("PASS: nss qualified-name admission bit-exact "
         "(%zu rejects, %zu accepts)\n",
         sizeof kQualifiedRejects / sizeof *kQualifiedRejects - 1,
         sizeof kNostrAccepts / sizeof *kNostrAccepts - 1);
  return 0;
}
