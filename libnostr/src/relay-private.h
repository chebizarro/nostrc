#ifndef NOSTR_RELAY_PRIVATE_H
#define NOSTR_RELAY_PRIVATE_H

#include "event.h"
#include <stdbool.h>

struct _RelayPrivate {
    nsync_mu *close_mutex;

    GoContext *connection_context;
    // cancel_func

    char *challenge;
    void (*notice_handler)(const char *);
    bool (*signature_checker)(NostrEvent);
    ConcurrentHashMap *ok_callbacks;
    GoChannel *write_queue;
    GoChannel *subscription_channel_close_queue;
};

struct write_request {
    char *msg;
    GoChannel answer;
} write_request;

#endif // NOSTR_RELAY_PRIVATE_H