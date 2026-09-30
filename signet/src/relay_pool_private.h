/* SPDX-License-Identifier: MIT
 *
 * relay_pool_private.h - relay_pool.c internals shared with its tests.
 *
 * Not installed. The per-relay OK callbacks that finish a NIP-42 AUTH and
 * that acknowledge a publish, exposed so tests can drive them on a single
 * NostrRelay (nostrc-tw7f).
 */

#ifndef SIGNET_RELAY_POOL_PRIVATE_H
#define SIGNET_RELAY_POOL_PRIVATE_H

#include "signet/relay_pool.h"

#include <nostr-relay.h>

/* Registers @relay's OK callback for a sent AUTH event @auth_event_id: an OK
 * true schedules @rp's post-AUTH re-subscribe on the default main context, an
 * OK false gives up; either removes the callback. @relay_url names the relay
 * in logs. The relay owns the registration's data (nostrc-tw7f). */
void signet_relay_pool_watch_auth_ok(SignetRelayPool *rp, NostrRelay *relay,
                                     const char *auth_event_id,
                                     const char *relay_url);

/* Registers @relay's OK callback for a published @event_id: the first OK for
 * it is passed to @cb, and the callback removes itself. The relay owns the
 * registration's data (nostrc-tw7f). */
void signet_relay_pool_watch_publish_ok(NostrRelay *relay, const char *event_id,
                                        SignetPublishOkCallback cb, void *user_data);

#endif /* SIGNET_RELAY_POOL_PRIVATE_H */
