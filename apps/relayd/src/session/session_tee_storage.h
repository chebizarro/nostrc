/*
 * session_tee_storage — a NostrStorage that forwards every call to the
 * session relay's real store (nostrdb) and, on the write path, then hands
 * the stored event to a hook (the upstream federation outbox, bead
 * nostrc-7d96).
 *
 * Order on put_event: the inner store first, then the hook (durable
 * enqueue). A queued row therefore always refers to an event held locally,
 * and nothing can be forwarded for an event the local store refused. A
 * failing hook fails the put, so the client gets OK false and retries —
 * an event is never acknowledged to a local app without being queued
 * upstream (when the forwarding contract says it should be). The relay
 * core does not change: it sees an ordinary NostrStorage.
 */
#ifndef NSR_SESSION_TEE_STORAGE_H
#define NSR_SESSION_TEE_STORAGE_H

#include "nostr-event.h"
#include "nostr-storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*NsrTeeOfferFn)(void *user_data, NostrEvent *ev);

/* @inner: opened store, borrowed (the caller closes it after freeing the
 * tee). open/close on the tee are no-ops. Returns NULL on OOM. */
NostrStorage *nsr_tee_storage_new(NostrStorage *inner, NsrTeeOfferFn offer, void *user_data);
void nsr_tee_storage_free(NostrStorage *tee);

#ifdef __cplusplus
}
#endif

#endif /* NSR_SESSION_TEE_STORAGE_H */
