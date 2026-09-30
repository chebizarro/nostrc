/*
 * SPDX-License-Identifier: MIT
 *
 * Test seam for GNostrSubscription delivery (nostrc-dha5).
 *
 * Built into nostr-gobject only when GNOSTR_TESTING is defined (BUILD_TESTING
 * builds). Not installed and not part of the API: tests include it from
 * nostr-gobject/src.
 */
#ifndef NOSTR_SUBSCRIPTION_TEST_HOOKS_H
#define NOSTR_SUBSCRIPTION_TEST_HOOKS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GNostrSubscription GNostrSubscription;

/* Events waiting for the main loop; *eose_queued (nullable) is set to
 * whether an EOSE marker is waiting too. */
guint gnostr_subscription_test_queued(GNostrSubscription *self, gboolean *eose_queued);

G_END_DECLS

#endif /* NOSTR_SUBSCRIPTION_TEST_HOOKS_H */
