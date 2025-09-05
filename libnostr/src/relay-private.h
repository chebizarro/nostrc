#ifndef NOSTR_RELAY_PRIVATE_H
#define NOSTR_RELAY_PRIVATE_H

#include "go.h"
#include <stdbool.h>

struct _NostrRelayPrivate {
    nsync_mu mutex;

    GoContext *connection_context;
    CancelFunc connection_context_cancel;
    // cancel_func

    char *challenge;
    void (*notice_handler)(const char *);
    bool (*custom_handler)(const char *);
    GoHashMap *ok_callbacks;
    GoChannel *write_queue;
    GoChannel *subscription_channel_close_queue;
    GoChannel *debug_raw; // optional: emits summary/raw strings for debugging
    GoWaitGroup workers;
    /* Security: invalid signature tracker (pubkey->counters/bans). Impl in relay.c */
    void *invalid_sig_head;
};

typedef struct _NostrRelayWriteRequest {
    char *msg;
    GoChannel *answer;
} NostrRelayWriteRequest;

typedef void (*NostrRelayOkCallback)(bool, char *);

#endif // NOSTR_RELAY_PRIVATE_H
