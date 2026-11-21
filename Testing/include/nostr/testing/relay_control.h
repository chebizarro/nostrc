/* Test-only control-frame injection contract. Not installed with libnostr.
 * Lets integration tests hold callback delivery boundaries without depending
 * on libnostr's private relay layout. Wire transport is tested separately. */
#ifndef NOSTR_TESTING_RELAY_CONTROL_H
#define NOSTR_TESTING_RELAY_CONTROL_H

#include <nostr-relay.h>
#include <nostr-envelope.h>

#ifdef __cplusplus
extern "C" {
#endif

void nostr_relay_dispatch_control_envelope(NostrRelay *relay,
                                          NostrEnvelope *envelope);

#ifdef __cplusplus
}
#endif
#endif
