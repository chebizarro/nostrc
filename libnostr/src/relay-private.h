#ifndef NOSTR_RELAY_PRIVATE_H
#define NOSTR_RELAY_PRIVATE_H

#include "event.h"
#include "wait_group.h"
#include <stdbool.h>

struct _RelayPrivate {
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
};

typedef struct _write_request {
    char *msg;
    GoChannel *answer;
} write_request;

typedef void (*ok_callback)(bool, char *);

#endif // NOSTR_RELAY_PRIVATE_H
