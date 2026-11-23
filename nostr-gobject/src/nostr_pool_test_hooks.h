#ifndef GNOSTR_POOL_TEST_HOOKS_H
#define GNOSTR_POOL_TEST_HOOKS_H
#include "select.h"
#include "nostr-gobject-export.h"
/* Private test seam: force a real EOSE receive before the greedy event drain. */
typedef GoSelectResult (*GNostrPoolSelectHook)(GoSelectCase *, size_t, uint64_t);
NOSTR_GOBJECT_API void gnostr_pool_test_set_select_hook(GNostrPoolSelectHook hook);
#endif
