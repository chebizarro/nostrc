#include <stdatomic.h>
#include <errno.h>
#include "nostr-init.h"

static atomic_int g_init_refcnt = 0;

int nostr_global_init(void)
{
  int expected;
  // Fast path: if already initialized, just increment
  expected = atomic_load(&g_init_refcnt);
  while (expected > 0) {
    if (atomic_compare_exchange_weak(&g_init_refcnt, &expected, expected + 1))
      return 0;
  }

  // We need to transition 0 -> 1 and perform one-time inits
  expected = 0;
  if (!atomic_compare_exchange_strong(&g_init_refcnt, &expected, 1)) {
    // Lost race; someone else initialized. Increment again for us.
    atomic_fetch_add(&g_init_refcnt, 1);
    return 0;
  }

  /* External crypto integration hooks previously used here have been removed. */

  return 0;
}

void nostr_global_cleanup(void)
{
  int prev = atomic_load(&g_init_refcnt);
  if (prev <= 0)
    return; // not initialized or already cleaned
  prev = atomic_fetch_sub(&g_init_refcnt, 1);
  if (prev == 1) {
    /* External crypto integration hooks previously used here have been removed. */
  }
}

#ifndef NOSTR_DISABLE_AUTO_INIT
__attribute__((constructor)) static void __nostr_ctor(void)
{
  (void)nostr_global_init();
}

__attribute__((destructor)) static void __nostr_dtor(void)
{
  nostr_global_cleanup();
}
#endif
