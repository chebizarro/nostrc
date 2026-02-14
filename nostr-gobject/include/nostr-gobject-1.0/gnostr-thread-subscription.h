/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * gnostr-thread-subscription.h - Reactive thread subscription manager
 *
 * nostrc-pp64 (Epic 4): Manages subscriptions for a thread conversation,
 * routing events from the EventBus and nostrdb to consumers via GObject
 * signals. Replaces scattered one-shot relay queries with a unified
 * reactive subscription that handles real-time thread updates.
 */

#ifndef GNOSTR_THREAD_SUBSCRIPTION_H
#define GNOSTR_THREAD_SUBSCRIPTION_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_THREAD_SUBSCRIPTION (gnostr_thread_subscription_get_type())
G_DECLARE_FINAL_TYPE(GnostrThreadSubscription, gnostr_thread_subscription,
                     GNOSTR, THREAD_SUBSCRIPTION, GObject)

/**
 * GnostrThreadSubscription:
 *
 * Manages reactive subscriptions for a Nostr thread conversation.
 * Subscribes to the EventBus for kind:1 (notes), kind:7 (reactions),
 * and kind:1111 (NIP-22 comments) events referencing the thread root.
 * Also subscribes to nostrdb for local storage notifications.
 *
 * Signals:
 * - "reply-received" (const char *event_json) - new kind:1 reply arrived
 * - "reaction-received" (const char *event_json) - new kind:7 reaction
 * - "comment-received" (const char *event_json) - new kind:1111 NIP-22 comment
 * - "eose" () - initial batch complete (end of stored events)
 *
 * The subscription deduplicates events internally, so each event_json
 * is emitted at most once per signal type.
 */

/**
 * gnostr_thread_subscription_new:
 * @root_event_id: the 64-character hex event ID of the thread root
 *
 * Creates a new thread subscription for the given root event.
 * The subscription is not yet active; call
 * gnostr_thread_subscription_start() to begin receiving events.
 *
 * Returns: (transfer full): a new #GnostrThreadSubscription
 */
GnostrThreadSubscription *gnostr_thread_subscription_new(const char *root_event_id);

/**
 * gnostr_thread_subscription_start:
 * @self: a #GnostrThreadSubscription
 *
 * Starts the subscription. Sets up EventBus listeners for kind:1, kind:7,
 * and kind:1111 events, plus a nostrdb subscription for local storage
 * notifications. Events matching the thread root will be emitted as signals.
 *
 * Calling this on an already-active subscription is a no-op.
 */
void gnostr_thread_subscription_start(GnostrThreadSubscription *self);

/**
 * gnostr_thread_subscription_stop:
 * @self: a #GnostrThreadSubscription
 *
 * Stops the subscription. Unsubscribes from EventBus and nostrdb.
 * After this call, no more signals will be emitted.
 *
 * Calling this on an inactive subscription is a no-op.
 */
void gnostr_thread_subscription_stop(GnostrThreadSubscription *self);

/**
 * gnostr_thread_subscription_add_monitored_id:
 * @self: a #GnostrThreadSubscription
 * @event_id: a 64-character hex event ID
 *
 * Adds an additional event ID to monitor for replies and reactions.
 * This is useful for mid-thread focus events that may have their own
 * reply subtree not directly referencing the root.
 */
void gnostr_thread_subscription_add_monitored_id(GnostrThreadSubscription *self,
                                                  const char *event_id);

/**
 * gnostr_thread_subscription_get_root_id:
 * @self: a #GnostrThreadSubscription
 *
 * Returns: (transfer none): the hex event ID of the thread root
 */
const char *gnostr_thread_subscription_get_root_id(GnostrThreadSubscription *self);

/**
 * gnostr_thread_subscription_is_active:
 * @self: a #GnostrThreadSubscription
 *
 * Returns: %TRUE if the subscription is currently active
 */
gboolean gnostr_thread_subscription_is_active(GnostrThreadSubscription *self);

/**
 * gnostr_thread_subscription_get_seen_count:
 * @self: a #GnostrThreadSubscription
 *
 * Returns: the number of unique events seen by this subscription
 */
guint gnostr_thread_subscription_get_seen_count(GnostrThreadSubscription *self);

G_END_DECLS

#endif /* GNOSTR_THREAD_SUBSCRIPTION_H */
