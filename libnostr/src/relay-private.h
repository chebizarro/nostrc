#ifndef NOSTR_RELAY_PRIVATE_H
#define NOSTR_RELAY_PRIVATE_H

#include "go.h"
#include <stdbool.h>
#include <stdint.h>

/* Connection state enum - also defined in nostr-relay.h with same guard */
#ifndef NOSTR_RELAY_CONNECTION_STATE_DEFINED
#define NOSTR_RELAY_CONNECTION_STATE_DEFINED
typedef enum NostrRelayConnectionState_ {
    NOSTR_RELAY_STATE_DISCONNECTED = 0,
    NOSTR_RELAY_STATE_CONNECTING,
    NOSTR_RELAY_STATE_CONNECTED,
    NOSTR_RELAY_STATE_BACKOFF
} NostrRelayConnectionState;
#endif

/* Forward declaration */
struct NostrRelay;

/* Callback for connection state changes */
typedef void (*NostrRelayStateCallback)(struct NostrRelay *relay,
                                        NostrRelayConnectionState old_state,
                                        NostrRelayConnectionState new_state,
                                        void *user_data);

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
    int invalid_sig_count;    /* current number of nodes in list */

    /* Reconnection with exponential backoff (nostrc-4du) */
    NostrRelayConnectionState connection_state;
    int reconnect_attempt;           /* Number of failed reconnection attempts */
    uint64_t backoff_ms;             /* Current backoff delay in milliseconds */
    uint64_t next_reconnect_time_ms; /* Absolute time (CLOCK_MONOTONIC ms) for next reconnect */
    bool auto_reconnect;             /* Enable/disable auto-reconnection (default: true) */
    bool reconnect_requested;        /* Signal to message_loop to trigger reconnect */

    /* State change callback */
    NostrRelayStateCallback state_callback;
    void *state_callback_user_data;
};

typedef struct _NostrRelayWriteRequest {
    char *msg;
    GoChannel *answer;
} NostrRelayWriteRequest;

typedef void (*NostrRelayOkCallback)(bool, char *);

/* Worker thread argument struct (nostrc-o56)
 * Used to pass a pre-ref'd context to worker threads to eliminate the race
 * between thread startup and context freeing. The context is ref'd BEFORE
 * spawning the thread, so the worker owns a valid reference from the start. */
typedef struct _NostrRelayWorkerArg {
    struct NostrRelay *relay;
    GoContext *ctx;  /* Pre-ref'd context - worker MUST unref when done */
} NostrRelayWorkerArg;

#endif // NOSTR_RELAY_PRIVATE_H
