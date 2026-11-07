/* SPDX-License-Identifier: MIT
 *
 * relay_pool_private.h - relay_pool.c internals shared with its tests.
 *
 * Not installed. Dispatcher watch registration exposed for deterministic
 * control-frame/lifetime tests on a single NostrRelay.
 */

#ifndef SIGNET_RELAY_POOL_PRIVATE_H
#define SIGNET_RELAY_POOL_PRIVATE_H

#include "signet/relay_pool.h"

#include <nostr-relay.h>

/* Add a generation-scoped AUTH watch without displacing publish watches.
 * An accepted OK schedules the complete stored filter on the default context.
 * The pool owns the watch, callback references and deferred source. */
void signet_relay_pool_watch_auth_ok(SignetRelayPool *rp, NostrRelay *relay,
                                     const char *auth_event_id,
                                     const char *relay_url);

/* Add a publish watch. The dispatcher removes the watch, not its own callback;
 * duplicate/concurrent OKs cannot fire a watch twice. Stop aborts pending watches. */
void signet_relay_pool_watch_publish_ok(SignetRelayPool *rp, NostrRelay *relay, const char *event_id,
                                        SignetPublishOkCallback cb, void *user_data);

/* NIP-46 replies: already signed bytes, sequential failover to other connected
 * relays on reject/deadline; no request execution or signing in this path. */
int signet_relay_pool_publish_reply(SignetRelayPool *rp, const char *event_json);
int signet_relay_pool_publish_selected_ack(SignetRelayPool *rp, const char *event_json,
                                           const char *relay_url,
                                           SignetPublishOkCallback cb, void *user_data);

#endif /* SIGNET_RELAY_POOL_PRIVATE_H */
