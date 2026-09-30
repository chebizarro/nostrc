/*
 * SPDX-License-Identifier: MIT
 *
 * Test seams for GNostrRelay teardown (nostrc-flp7).
 *
 * Built into nostr-gobject only when GNOSTR_TESTING is defined (BUILD_TESTING
 * builds). Not installed and not part of the API: tests include it from
 * nostr-gobject/src.
 */
#ifndef NOSTR_RELAY_TEST_HOOKS_H
#define NOSTR_RELAY_TEST_HOOKS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    /* First statement of the core relay's state / AUTH / OK callback, on the
     * libnostr thread that invokes it, before the callback reads its data. */
    GNOSTR_RELAY_TEST_POINT_CORE_STATE,
    GNOSTR_RELAY_TEST_POINT_CORE_AUTH,
    GNOSTR_RELAY_TEST_POINT_CORE_OK,
    /* First statement of finalize: the reference count is already zero and
     * the relay is still in the URL registry. */
    GNOSTR_RELAY_TEST_POINT_FINALIZE,
} GNostrRelayTestPoint;

/* Called on the thread that reaches @point. It may block to hold that thread
 * there, e.g. to finalize the relay while a worker is inside a callback. */
typedef void (*GNostrRelayTestHook)(GNostrRelayTestPoint point, gpointer hook_data);

/* Installs @hook (NULL removes it) for every relay in the process. */
void gnostr_relay_test_set_hook(GNostrRelayTestHook hook, gpointer hook_data);

/* Number of core-callback data blocks currently alive in the process. */
gint gnostr_relay_test_live_callback_data(void);

G_END_DECLS

#endif /* NOSTR_RELAY_TEST_HOOKS_H */
