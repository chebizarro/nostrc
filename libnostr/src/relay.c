#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-filter.h"
#include "nostr-envelope.h"
#include "error_codes.h"
#include "json.h"
#include "nostr-kinds.h"
#include "relay-private.h"
#include "connection-private.h"
#include "subscription-private.h"
#include "nostr-subscription.h"
#include "nostr-utils.h"
#include "nostr/metrics.h"
#include "security_limits_runtime.h"
#include "nostr_log.h"
#include "go.h"
#include "channel.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int shutdown_dbg_enabled(void) {
    static int inited = 0;
    static int enabled = 0;
    if (!inited) {
        const char *e = getenv("NOSTR_DEBUG_SHUTDOWN");
        enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        inited = 1;
    }
    return enabled;
}

/* === Security: invalid signature tracking (per-pubkey sliding window + ban) === */

/* Max entries in invalid signature list to prevent unbounded memory growth.
 * When exceeded, expired entries are evicted. If still over limit, oldest entries removed. */
#define INVALIDSIG_MAX_ENTRIES 10000

typedef struct InvalidSigNode {
    char *pk;                    /* hex npub (x-only hex) */
    int count;                   /* fails in current window */
    time_t window_start;         /* epoch seconds */
    time_t banned_until;         /* epoch seconds; 0 if not banned */
    struct InvalidSigNode *next;
} InvalidSigNode;

static time_t now_epoch_s(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

/* Caller should hold r->priv->mutex */
static InvalidSigNode *invalidsig_find(InvalidSigNode *head, const char *pk) {
    for (InvalidSigNode *n = head; n; n = n->next) {
        if (n->pk && pk && strcmp(n->pk, pk) == 0) return n;
    }
    return NULL;
}

/* Evict expired/stale entries from the invalid signature list.
 * Caller should hold r->priv->mutex. */
static void invalidsig_evict(NostrRelay *r) {
    if (!r || !r->priv) return;
    time_t now = now_epoch_s();
    int64_t window_sec = nostr_limit_invalidsig_window_seconds();

    /* First pass: remove entries that are not banned and have expired windows */
    InvalidSigNode **pp = (InvalidSigNode **)&r->priv->invalid_sig_head;
    while (*pp) {
        InvalidSigNode *n = *pp;
        int expired_window = (now - n->window_start > window_sec);
        int not_banned = (n->banned_until <= now);
        if (expired_window && not_banned) {
            *pp = n->next;
            free(n->pk);
            free(n);
            r->priv->invalid_sig_count--;
        } else {
            pp = &n->next;
        }
    }

    /* If still over limit, remove oldest (tail) entries */
    while (r->priv->invalid_sig_count > INVALIDSIG_MAX_ENTRIES / 2) {
        /* Find and remove tail node - O(n) but only happens during eviction */
        InvalidSigNode **tail_pp = (InvalidSigNode **)&r->priv->invalid_sig_head;
        if (!*tail_pp) break;
        while ((*tail_pp)->next) tail_pp = &(*tail_pp)->next;
        InvalidSigNode *tail = *tail_pp;
        *tail_pp = NULL;
        free(tail->pk);
        free(tail);
        r->priv->invalid_sig_count--;
    }
}

/* Caller should hold r->priv->mutex */
static InvalidSigNode *invalidsig_get_or_add(NostrRelay *r, const char *pk) {
    InvalidSigNode *head = (InvalidSigNode *)r->priv->invalid_sig_head;
    InvalidSigNode *n = invalidsig_find(head, pk);
    if (n) return n;

    /* Evict if over limit before adding new entry */
    if (r->priv->invalid_sig_count >= INVALIDSIG_MAX_ENTRIES) {
        invalidsig_evict(r);
    }

    n = (InvalidSigNode *)calloc(1, sizeof(InvalidSigNode));
    if (!n) return NULL;
    n->pk = strdup(pk ? pk : "");
    n->window_start = now_epoch_s();
    n->next = (InvalidSigNode *)r->priv->invalid_sig_head;
    r->priv->invalid_sig_head = n;
    r->priv->invalid_sig_count++;
    return n;
}

/* Public helpers used in message loop (locking done by caller) */
int nostr_invalidsig_is_banned(NostrRelay *r, const char *pk) {
    if (!r || !r->priv || !pk) return 0;
    InvalidSigNode *n = invalidsig_find((InvalidSigNode *)r->priv->invalid_sig_head, pk);
    if (!n) return 0;
    time_t now = now_epoch_s();
    return (n->banned_until > now) ? 1 : 0;
}

void nostr_invalidsig_record_fail(NostrRelay *r, const char *pk) {
    if (!r || !r->priv || !pk) return;
    InvalidSigNode *n = invalidsig_get_or_add(r, pk);
    if (!n) return;
    time_t now = now_epoch_s();
    /* Slide window */
    if (now - n->window_start > nostr_limit_invalidsig_window_seconds()) {
        n->window_start = now;
        n->count = 0;
    }
    n->count++;
    if (n->count >= nostr_limit_invalidsig_threshold()) {
        n->banned_until = now + nostr_limit_invalidsig_ban_seconds();
        n->count = 0; /* reset after ban */
        n->window_start = now;
    }
}

// Forward declarations for workers and helpers used before their definition
static void *write_error(void *arg);
static void *write_operations(void *arg);
static void *message_loop(void *arg);
static void relay_set_state(NostrRelay *relay, NostrRelayConnectionState new_state);
static uint64_t get_monotonic_time_ms(void);
static uint64_t calculate_backoff_with_jitter(int attempt);
static bool relay_attempt_reconnect(NostrRelay *r);
static void relay_debug_emit(NostrRelay *r, const char *s) {
    if (!r || !r->priv || !r->priv->debug_raw || !s) return;
    char *copy = strdup(s);
    if (!copy) return;
    // non-blocking: if channel is full, drop
    (void)go_channel_try_send(r->priv->debug_raw, copy);
}

void nostr_relay_enable_debug_raw(NostrRelay *relay, int enable) {
    if (!relay || !relay->priv) return;
    nsync_mu_lock(&relay->priv->mutex);
    if (enable) {
        if (!relay->priv->debug_raw) {
            relay->priv->debug_raw = go_channel_create(128);
        }
    } else {
        if (relay->priv->debug_raw) {
            go_channel_close(relay->priv->debug_raw);
            go_channel_free(relay->priv->debug_raw);
            relay->priv->debug_raw = NULL;
        }
    }
    nsync_mu_unlock(&relay->priv->mutex);
}

GoChannel *nostr_relay_get_debug_raw_channel(NostrRelay *relay) {
    if (!relay || !relay->priv) return NULL;
    return relay->priv->debug_raw;
}

bool nostr_relay_is_connected(NostrRelay *relay) {
    if (!relay) return false;
    nsync_mu_lock(&relay->priv->mutex);
    bool connected = false;
    if (relay->connection && relay->connection->priv) {
        nsync_mu_lock(&relay->connection->priv->mutex);
        /* In test mode, connection is always "connected" since we bypass real network */
        if (relay->connection->priv->test_mode) {
            connected = true;
        } else {
            /* Check if wsi exists - this indicates an active connection attempt.
             * Note: This returns true even before WebSocket handshake completes,
             * which allows messages to be queued for sending. The handshake
             * completes asynchronously and messages are sent once established. */
            connected = (relay->connection->priv->wsi != NULL);
        }
        nsync_mu_unlock(&relay->connection->priv->mutex);
    }
    nsync_mu_unlock(&relay->priv->mutex);
    return connected;
}

bool nostr_relay_is_established(NostrRelay *relay) {
    if (!relay) return false;
    nsync_mu_lock(&relay->priv->mutex);
    bool established = false;
    if (relay->connection && relay->connection->priv) {
        nsync_mu_lock(&relay->connection->priv->mutex);
        if (relay->connection->priv->test_mode) {
            established = true;
        } else {
            /* Check both: wsi exists AND WebSocket handshake completed */
            established = (relay->connection->priv->wsi != NULL &&
                          relay->connection->priv->established);
        }
        nsync_mu_unlock(&relay->connection->priv->mutex);
    }
    nsync_mu_unlock(&relay->priv->mutex);
    return established;
}

/* GLib-style accessors (header: nostr-relay.h) */
const char *nostr_relay_get_url_const(const NostrRelay *relay) {
    if (!relay) return NULL;
    return relay->url;
}

GoContext *nostr_relay_get_context(const NostrRelay *relay) {
    if (!relay || !relay->priv) return NULL;
    return relay->priv->connection_context;
}

GoChannel *nostr_relay_get_write_channel(const NostrRelay *relay) {
    if (!relay || !relay->priv) return NULL;
    return relay->priv->write_queue;
}

NostrRelay *nostr_relay_new(GoContext *context, const char *url, Error **err) {
    if (url == NULL) {
        if (err) *err = new_error(1, "invalid relay URL");
        return NULL;
    }
    CancelContextResult cancellabe = go_context_with_cancel(context);

    NostrRelay *relay = (NostrRelay *)calloc(1, sizeof(NostrRelay));
    NostrRelayPrivate *priv = (NostrRelayPrivate *)calloc(1, sizeof(NostrRelayPrivate));
    if (!relay || !priv) {
        if (err) *err = new_error(1, "failed to allocate memory for NostrRelay struct");
        return NULL;
    }

    relay->url = strdup(url);
    relay->subscriptions = go_hash_map_create(16);
    relay->assume_valid = false;
    relay->refcount = 1;

    relay->priv = priv;
    nsync_mu_init(&relay->priv->mutex);
    relay->priv->connection_context = cancellabe.context;
    relay->priv->connection_context_cancel = cancellabe.cancel;
    relay->priv->ok_callbacks = go_hash_map_create(16);
    relay->priv->write_queue = go_channel_create(16);
    relay->priv->subscription_channel_close_queue = go_channel_create(16);
    relay->priv->debug_raw = NULL;
    go_wait_group_init(&relay->priv->workers);
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_new: initialized workers and queues for %s\n", relay->url);
    // request_header

    relay->priv->notice_handler = NULL;
    relay->priv->custom_handler = NULL;

    /* Initialize reconnection state (nostrc-4du) */
    relay->priv->connection_state = NOSTR_RELAY_STATE_DISCONNECTED;
    relay->priv->reconnect_attempt = 0;
    relay->priv->backoff_ms = 0;
    relay->priv->next_reconnect_time_ms = 0;
    relay->priv->auto_reconnect = true;  /* Enabled by default */
    relay->priv->reconnect_requested = false;
    relay->priv->state_callback = NULL;
    relay->priv->state_callback_user_data = NULL;

    return (NostrRelay *)relay;
}

NostrRelay *nostr_relay_ref(NostrRelay *relay) {
    if (!relay) return NULL;
    nsync_mu_lock(&relay->priv->mutex);
    relay->refcount++;
    nsync_mu_unlock(&relay->priv->mutex);
    return relay;
}

static void relay_free_impl(NostrRelay *relay) {
    if (!relay) return;
    // Signal background loops to stop
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: cancel connection context\n");
    if (relay->priv && relay->priv->connection_context_cancel) {
        relay->priv->connection_context_cancel(relay->priv->connection_context);
    }
    // Close queues to unblock workers
    if (relay->priv) {
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: closing queues\n");
        if (relay->priv->write_queue) go_channel_close(relay->priv->write_queue);
        if (relay->priv->subscription_channel_close_queue) go_channel_close(relay->priv->subscription_channel_close_queue);
        if (relay->priv->debug_raw) go_channel_close(relay->priv->debug_raw);
    }
    // Snapshot and NULL out connection BEFORE waiting for workers.
    // Workers read relay->connection under priv->mutex (write_operations line 494);
    // they'll see NULL and skip the write, avoiding UAF on the freed connection.
    // This matches the safe ordering in nostr_relay_close().
    NostrConnection *conn = NULL;
    if (relay->priv) {
        nsync_mu_lock(&relay->priv->mutex);
        conn = relay->connection;
        relay->connection = NULL;
        nsync_mu_unlock(&relay->priv->mutex);
    }
    /* nostrc-ws1: Close send/recv channels BEFORE waiting for workers.
     * Without this, write_operations can block forever in
     * go_channel_send(send_channel) if the channel is full and the LWS
     * service thread is busy (e.g., synchronous DNS).  Closing the channels
     * sets chan->closed and broadcasts cond_full/cond_empty, which unblocks
     * any worker stuck in go_channel_send/receive.
     *
     * This is safe because:
     * - relay->connection is already NULL (workers won't start new ops)
     * - go_channel_close is idempotent (double-close is a no-op)
     * - LWS callbacks check conn via opaque user data (NULL after
     *   nostr_connection_close), so they won't access closed channels */
    if (conn) {
        if (conn->recv_channel) go_channel_close(conn->recv_channel);
        if (conn->send_channel) go_channel_close(conn->send_channel);
    }
    // Wait for worker goroutines to finish — workers will now unblock
    // quickly since channels are closed and context is canceled.
    if (relay->priv) {
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: waiting for workers\n");
        go_wait_group_wait(&relay->priv->workers);
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: workers joined\n");
        go_wait_group_destroy(&relay->priv->workers);
    }
    // NOW safe to free connection — all workers have exited.
    // go_channel_free handles already-closed channels correctly.
    if (conn) {
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: closing network connection\n");
        if (conn->recv_channel) { go_channel_free(conn->recv_channel); conn->recv_channel = NULL; }
        if (conn->send_channel) { go_channel_free(conn->send_channel); conn->send_channel = NULL; }
        nostr_connection_close(conn);
    }

    // Free resources
    if (relay->priv) {
        if (relay->priv->write_queue) { go_channel_free(relay->priv->write_queue); relay->priv->write_queue = NULL; }
        if (relay->priv->subscription_channel_close_queue) { go_channel_free(relay->priv->subscription_channel_close_queue); relay->priv->subscription_channel_close_queue = NULL; }
        if (relay->priv->debug_raw) { go_channel_free(relay->priv->debug_raw); relay->priv->debug_raw = NULL; }
        if (relay->priv->ok_callbacks) { go_hash_map_destroy(relay->priv->ok_callbacks); relay->priv->ok_callbacks = NULL; }
        if (relay->priv->challenge) { free(relay->priv->challenge); relay->priv->challenge = NULL; }
        // Free invalid signature tracking linked list
        InvalidSigNode *node = (InvalidSigNode *)relay->priv->invalid_sig_head;
        while (node) {
            InvalidSigNode *next = node->next;
            free(node->pk);
            free(node);
            node = next;
        }
        relay->priv->invalid_sig_head = NULL;
        // Use reference counting to safely free connection_context.
        // The context starts with refcount=1 from go_context_with_cancel.
        // go_context_unref decrements and frees when refcount hits 0.
        if (relay->priv->connection_context) {
            go_context_unref(relay->priv->connection_context);
            relay->priv->connection_context = NULL;
        }
    }
    if (relay->subscriptions) { go_hash_map_destroy(relay->subscriptions); relay->subscriptions = NULL; }
    if (relay->url) { free(relay->url); relay->url = NULL; }
    if (relay->priv) { free(relay->priv); relay->priv = NULL; }
    free(relay);
}

void nostr_relay_unref(NostrRelay *relay) {
    if (!relay) return;
    int do_free = 0;
    nsync_mu_lock(&relay->priv->mutex);
    if (relay->refcount > 0) {
        relay->refcount--;
        if (relay->refcount == 0) do_free = 1;
    }
    nsync_mu_unlock(&relay->priv->mutex);
    if (do_free) relay_free_impl(relay);
}

void nostr_relay_free(NostrRelay *relay) {
    if (!relay) return;
    nostr_relay_unref(relay);
}

bool nostr_relay_connect(NostrRelay *relay, Error **err) {
    if (!relay) {
        if (err) *err = new_error(1, "relay must be initialized with a call to nostr_relay_new()");
        return false;
    }

    /* nostrc-kw9r: Shared relay registry may cause multiple pools to try
     * connecting the same NostrRelay — if already connected, skip. */
    if (relay->connection != NULL) {
        return true;
    }

    /* Set state to connecting (nostrc-4du) */
    relay_set_state(relay, NOSTR_RELAY_STATE_CONNECTING);

    NostrConnection *conn = nostr_connection_new(relay->url);
    if (!conn) {
        relay_set_state(relay, NOSTR_RELAY_STATE_DISCONNECTED);
        if (err) *err = new_error(1, "error opening websocket to '%s'\n", relay->url);
        return false;
    }
    relay->connection = conn;

    /* Reset reconnect state on successful connection (nostrc-4du) */
    nsync_mu_lock(&relay->priv->mutex);
    relay->priv->reconnect_attempt = 0;
    relay->priv->backoff_ms = 0;
    nsync_mu_unlock(&relay->priv->mutex);
    relay_set_state(relay, NOSTR_RELAY_STATE_CONNECTED);

    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] relay_connect: starting workers\n");

    /* nostrc-o56: Pass pre-ref'd context to workers to eliminate startup race.
     * We ref the context TWICE here (once per worker) BEFORE spawning threads.
     * This ensures each worker owns a valid reference from the moment it starts,
     * eliminating the race where the worker reads connection_context and then
     * the context gets freed before the worker can ref it. */
    GoContext *ctx = relay->priv->connection_context;
    if (!ctx) {
        relay_set_state(relay, NOSTR_RELAY_STATE_DISCONNECTED);
        if (err) *err = new_error(1, "no connection context");
        return false;
    }

    /* Pre-ref for each worker (they will unref when done) */
    go_context_ref(ctx);
    go_context_ref(ctx);

    /* Allocate worker args - workers free these when done */
    NostrRelayWorkerArg *write_arg = malloc(sizeof(NostrRelayWorkerArg));
    NostrRelayWorkerArg *loop_arg = malloc(sizeof(NostrRelayWorkerArg));
    if (!write_arg || !loop_arg) {
        if (write_arg) { go_context_unref(ctx); free(write_arg); }
        if (loop_arg) { go_context_unref(ctx); free(loop_arg); }
        relay_set_state(relay, NOSTR_RELAY_STATE_DISCONNECTED);
        if (err) *err = new_error(1, "failed to allocate worker args");
        return false;
    }

    write_arg->relay = relay;
    write_arg->ctx = ctx;
    loop_arg->relay = relay;
    loop_arg->ctx = ctx;

    go_wait_group_add(&relay->priv->workers, 2);
    go(write_operations, write_arg);
    go(message_loop, loop_arg);

    return true;
}

static void *write_error(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    go_channel_send(chan, new_error(0, "connection closed"));
    return NULL;
}

// Worker: processes relay->priv->write_queue and writes frames to the connection.
static void *write_operations(void *arg) {
    /* nostrc-o56: Receive pre-ref'd context via arg struct to eliminate race.
     * The context was ref'd BEFORE this thread was spawned, so we own a valid
     * reference from the very first instruction. No more race window! */
    NostrRelayWorkerArg *warg = (NostrRelayWorkerArg *)arg;
    if (!warg) return NULL;

    NostrRelay *r = warg->relay;
    GoContext *ctx = warg->ctx;  /* Already ref'd - we own this reference */
    free(warg);  /* Free the arg struct now that we've extracted values */

    if (!r || !r->priv) {
        if (ctx) go_context_unref(ctx);
        return NULL;
    }
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] write_operations: start\n");

    if (!ctx) {
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] write_operations: no context, exiting\n");
        go_wait_group_done(&r->priv->workers);
        return NULL;
    }
    /* Note: ctx is already ref'd by caller - no need to ref here */

    for (;;) {
        // Fast-path: if connection context is canceled, exit promptly
        if (go_context_is_canceled(ctx)) {
            break;
        }
        NostrRelayWriteRequest *req = NULL;
        GoSelectCase cases[] = {
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = r->priv->write_queue, .value = NULL, .recv_buf = (void **)&req },
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ctx->done, .value = NULL, .recv_buf = NULL },
        };
        int idx = go_select(cases, 2);
        if (idx == 1) {
            // context canceled
            break;
        }
        if (idx != 0) {
            // unexpected; continue
            continue;
        }
        if (!req) {
            // queue closed or spurious
            break;
        }

        Error *werr = NULL;
        nsync_mu_lock(&r->priv->mutex);
        NostrConnection *conn = r->connection;
        nsync_mu_unlock(&r->priv->mutex);
        if (!conn) {
            werr = new_error(1, "no connection");
        } else {
            // Metrics: time WS write and count bytes
            nostr_metric_timer t = {0};
            nostr_metric_timer_start(&t);
            nostr_connection_write_message(conn, r->priv->connection_context, req->msg, &werr);
            static nostr_metric_histogram *h_ws_write_ns;
            if (!h_ws_write_ns) h_ws_write_ns = nostr_metric_histogram_get("ws_write_ns");
            nostr_metric_timer_stop(&t, h_ws_write_ns);
            if (req->msg) {
                nostr_metric_counter_add("ws_tx_bytes", (uint64_t)strlen(req->msg));
                nostr_metric_counter_add("ws_tx_messages", 1);
            }
        }
        // The writer owns req->msg copy
        if (req->msg) free(req->msg);
        // Send result back to caller
        if (werr) {
            go_channel_send(req->answer, werr);
        } else {
            go_channel_send(req->answer, NULL);
        }
        free(req);
    }

    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] write_operations: exit\n");
    go_context_unref(ctx);  // Release context reference (nostrc-0q4)
    go_wait_group_done(&r->priv->workers);  // Use local 'r', not freed 'arg'
    return NULL;
}

/* Forward declaration for subscription re-firing */
#include "nostr-subscription.h"
#include "subscription-private.h"

/* Snapshot entry for subscription re-firing (nostrc-1zqm) */
typedef struct {
    NostrSubscription *sub;
    int counter;
} RefireEntry;

/* Collector used during snapshot phase */
typedef struct {
    RefireEntry *entries;
    size_t count;
    size_t capacity;
} RefireCollector;

/* Callback for snapshot phase: quickly collects subscription pointers.
 * Runs under per-bucket lock but does NO blocking work (nostrc-1zqm). */
static bool collect_subscription_cb(HashKey *key, void *value, void *user_data) {
    (void)key;
    RefireCollector *collector = (RefireCollector *)user_data;
    NostrSubscription *sub = (NostrSubscription *)value;
    if (!sub || !sub->filters || !sub->priv) return true;

    if (collector->count >= collector->capacity) {
        size_t new_cap = collector->capacity * 2;
        RefireEntry *new_entries = realloc(collector->entries, new_cap * sizeof(RefireEntry));
        if (!new_entries) return false;  /* stop iteration on OOM */
        collector->entries = new_entries;
        collector->capacity = new_cap;
    }
    /* Take a reference to keep the subscription alive during Phase 2 (nostrc-nr96) */
    nostr_subscription_ref(sub);
    collector->entries[collector->count].sub = sub;
    collector->entries[collector->count].counter = sub->priv->counter;
    collector->count++;
    return true;
}

/* Helper to re-fire all active subscriptions after reconnection (nostrc-4du, nostrc-1zqm).
 *
 * Uses a two-phase snapshot-then-fire approach to prevent use-after-free:
 * Phase 1: Collect subscription pointers under per-bucket locks (fast, non-blocking).
 * Phase 2: Re-fire each subscription WITHOUT holding hash map locks.
 *
 * This prevents the race where nostr_subscription_fire() blocks for up to 3 seconds
 * inside go_select_timeout while holding a bucket lock, during which another thread
 * could free the subscription through a different bucket's lock path. */
static void relay_refire_subscriptions(NostrRelay *r) {
    if (!r || !r->subscriptions) return;

    static int debug = -1;
    if (debug < 0) debug = getenv("NOSTR_DEBUG_LIFECYCLE") ? 1 : 0;

    /* Phase 1: Snapshot - collect subscription pointers under bucket locks.
     * The callback is fast (just stores pointers), so locks are held briefly. */
    RefireCollector collector = {0};
    collector.capacity = 32;
    collector.entries = calloc(collector.capacity, sizeof(RefireEntry));
    if (!collector.entries) return;

    go_hash_map_for_each_with_data(r->subscriptions, collect_subscription_cb, &collector);

    /* Phase 2: Fire - iterate snapshot WITHOUT holding any hash map locks.
     * Each entry holds a ref (taken in Phase 1) that keeps the subscription
     * alive even if another thread calls nostr_subscription_free(). (nostrc-nr96) */
    int refire_count = 0;
    for (size_t i = 0; i < collector.count; i++) {
        NostrSubscription *sub = collector.entries[i].sub;
        int counter = collector.entries[i].counter;

        /* Verify subscription is still in the hash map (it may have been
         * logically freed — removed from map — but our ref keeps memory alive) */
        NostrSubscription *current = go_hash_map_get_int(r->subscriptions, counter);
        if (current != sub) {
            nostr_subscription_unref(sub);
            continue;
        }

        /* Re-verify filters */
        if (!sub->filters) {
            nostr_subscription_unref(sub);
            continue;
        }

        Error *err = NULL;
        if (nostr_subscription_fire(sub, &err)) {
            refire_count++;
            if (debug) {
                fprintf(stderr, "[RECONNECT] Re-fired subscription sid=%s\n",
                        sub->priv && sub->priv->id ? sub->priv->id : "?");
            }
        } else {
            if (debug) {
                fprintf(stderr, "[RECONNECT] Failed to re-fire subscription sid=%s: %s\n",
                        sub->priv && sub->priv->id ? sub->priv->id : "?",
                        err ? err->message : "unknown");
            }
            if (err) free_error(err);
        }
        nostr_subscription_unref(sub);
    }

    free(collector.entries);

    if (refire_count > 0 || debug) {
        fprintf(stderr, "[RECONNECT] Re-fired %d subscription(s) for %s\n",
                refire_count, r->url ? r->url : "unknown");
    }
    nostr_metric_counter_add("relay_subscriptions_refired", (uint64_t)refire_count);
}

/* Attempt to reconnect the relay (nostrc-4du)
 * Returns true if reconnection succeeded, false otherwise.
 * Does NOT start new workers - the calling worker continues after reconnection. */
static bool relay_attempt_reconnect(NostrRelay *r) {
    if (!r || !r->priv) return false;

    relay_set_state(r, NOSTR_RELAY_STATE_CONNECTING);

    /* Close old connection if it exists */
    nsync_mu_lock(&r->priv->mutex);
    NostrConnection *old_conn = r->connection;
    r->connection = NULL;
    nsync_mu_unlock(&r->priv->mutex);

    if (old_conn) {
        nostr_connection_close(old_conn);
    }

    /* Create new connection */
    NostrConnection *new_conn = nostr_connection_new(r->url);
    if (!new_conn) {
        relay_set_state(r, NOSTR_RELAY_STATE_DISCONNECTED);
        return false;
    }

    /* Install new connection */
    nsync_mu_lock(&r->priv->mutex);
    r->connection = new_conn;
    r->priv->reconnect_attempt = 0;
    r->priv->backoff_ms = 0;
    nsync_mu_unlock(&r->priv->mutex);

    relay_set_state(r, NOSTR_RELAY_STATE_CONNECTED);

    /* Re-fire active subscriptions */
    relay_refire_subscriptions(r);

    nostr_metric_counter_add("relay_reconnect_success", 1);
    fprintf(stderr, "[RECONNECT] Successfully reconnected to %s\n", r->url ? r->url : "unknown");

    return true;
}

// Cached environment variables to avoid repeated getenv() calls
static int metrics_sample_rate = 0;
static int debug_incoming_cached = -1;
static int debug_eose_cached = -1;

static void init_cached_env(void) {
    if (metrics_sample_rate == 0) {
        const char *rate = getenv("NOSTR_METRICS_SAMPLE_RATE");
        metrics_sample_rate = rate ? atoi(rate) : 100;
        if (metrics_sample_rate <= 0) metrics_sample_rate = 100;
    }
    if (debug_incoming_cached == -1) {
        const char *dbg = getenv("NOSTR_DEBUG_INCOMING");
        debug_incoming_cached = (dbg && *dbg && strcmp(dbg, "0") != 0) ? 1 : 0;
    }
    if (debug_eose_cached == -1) {
        debug_eose_cached = getenv("NOSTR_DEBUG_EOSE") ? 1 : 0;
    }
}

// Worker: reads messages from the connection, parses envelopes, dispatches,
// and emits concise debug summaries on the optional debug_raw channel.
// Handles automatic reconnection with exponential backoff (nostrc-4du).
static void *message_loop(void *arg) {
    /* nostrc-o56: Receive pre-ref'd context via arg struct to eliminate race.
     * The context was ref'd BEFORE this thread was spawned, so we own a valid
     * reference from the very first instruction. No more race window! */
    NostrRelayWorkerArg *warg = (NostrRelayWorkerArg *)arg;
    if (!warg) return NULL;

    NostrRelay *r = warg->relay;
    GoContext *ctx = warg->ctx;  /* Already ref'd - we own this reference */
    free(warg);  /* Free the arg struct now that we've extracted values */

    if (!r || !r->priv) {
        if (ctx) go_context_unref(ctx);
        return NULL;
    }
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] message_loop: start\n");

    if (!ctx) {
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] message_loop: no context, exiting\n");
        go_wait_group_done(&r->priv->workers);
        return NULL;
    }
    /* Note: ctx is already ref'd by caller - no need to ref here */

    init_cached_env();

    /* nostrc-of8l: Match WebSocket reassembly buffer (128KB). The old 4KB buffer
     * silently dropped kind:0 profiles, kind:3 contacts, and kind:10002 relay
     * lists that exceeded 4KB, triggering spurious reconnects and channel backpressure. */
    char *buf = malloc(131072);
    char *priority_buf = malloc(131072);
    if (!buf || !priority_buf) {
        free(buf); free(priority_buf);
        if (ctx) go_context_unref(ctx);
        go_wait_group_done(&r->priv->workers);
        return NULL;
    }
    const size_t buf_size = 131072;
    Error *err = NULL;
    uint64_t msg_count = 0;
    int has_priority = 0;
    bool context_canceled = false;  /* Track if context was canceled (vs connection lost) */

    /* Outer loop for reconnection (nostrc-4du) */
    for (;;) {
        /* Inner loop: process messages while connected */
        for (;;) {
            nsync_mu_lock(&r->priv->mutex);
            NostrConnection *conn = r->connection;
            nsync_mu_unlock(&r->priv->mutex);
            if (!conn) break;

            // Validate connection's recv_channel before use to prevent use-after-free
            // during shutdown or reconnection. The channel may be freed by another thread.
            GoChannel *recv_ch = conn->recv_channel;
            if (!recv_ch || recv_ch->magic != GO_CHANNEL_MAGIC) {
                if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] message_loop: recv_channel invalid, breaking\n");
                break;
            }

            // Check for priority message first if we have one pending
            if (has_priority) {
                strcpy(buf, priority_buf);
                has_priority = 0;
            } else {
                // Validate context before use - exit if context is invalid/freed
                if (!ctx->done || ctx->done->magic != GO_CHANNEL_MAGIC) {
                    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] message_loop: context invalid, exiting\n");
                    context_canceled = true;
                    break;
                }
                // Check if context is canceled (relay is being closed)
                if (go_context_is_canceled(ctx)) {
                    context_canceled = true;
                    break;
                }
                // Read next message
                nostr_connection_read_message(conn, ctx, buf, buf_size, &err);
                if (err) {
                    free_error(err);
                    err = NULL;
                    break;
                }
                if (buf[0] == '\0') continue;
            }
        
        msg_count++;

        // Sample metrics to reduce overhead (only every Nth message)
        int record_metrics = (msg_count % metrics_sample_rate) == 0;
        size_t blen_for_metrics = 0;
        
        if (record_metrics) {
            blen_for_metrics = strlen(buf);
            nostr_metric_counter_add("ws_rx_bytes_sampled", (uint64_t)blen_for_metrics * metrics_sample_rate);
            nostr_metric_counter_add("ws_rx_messages_sampled", metrics_sample_rate);
        }

        // Use cached debug flag
        if (debug_incoming_cached) {
            size_t blen = record_metrics ? blen_for_metrics : strlen(buf);
            size_t show = blen < 512 ? blen : 512;
            fprintf(stderr, "[incoming] %.*s%s\n",
                    (int)show, buf,
                    (blen > show ? "..." : ""));
        }

        // Parse envelope (only time parsing for sampled messages)
        NostrEnvelope *envelope;
        if (record_metrics) {
            nostr_metric_timer t_parse = {0};
            nostr_metric_timer_start(&t_parse);
            envelope = nostr_envelope_parse(buf);
            static nostr_metric_histogram *h_envelope_parse_ns;
            if (!h_envelope_parse_ns) h_envelope_parse_ns = nostr_metric_histogram_get("envelope_parse_ns");
            nostr_metric_timer_stop(&t_parse, h_envelope_parse_ns);
        } else {
            envelope = nostr_envelope_parse(buf);
        }
        
        if (!envelope) {
            if (debug_incoming_cached) {
                size_t blen = strlen(buf);
                size_t show = blen < 256 ? blen : 256;
                fprintf(stderr, "[incoming][unparsed] %.*s%s\n",
                        (int)show, buf,
                        (blen > show ? "..." : ""));
            }
            if (r->priv->custom_handler) r->priv->custom_handler(buf);
            continue;
        }

        switch (envelope->type) {
        case NOSTR_ENVELOPE_NOTICE: {
            NostrNoticeEnvelope *ne = (NostrNoticeEnvelope *)envelope;
            if (r->priv->notice_handler) r->priv->notice_handler(ne->message);
            /* nostrc-95c: Log NOTICE messages at INFO level for debugging relay issues */
            fprintf(stderr, "[RELAY_NOTICE] relay=%s message=\"%s\"\n",
                    r->url ? r->url : "unknown", ne->message ? ne->message : "");
            char tmp[256]; snprintf(tmp, sizeof(tmp), "NOTICE: %s", ne->message ? ne->message : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_EOSE: {
            NostrEOSEEnvelope *env = (NostrEOSEEnvelope *)envelope;
            if (env->message) {
                int serial = nostr_sub_id_to_serial(env->message);
                if (serial < 0) {
                    /* hq-3xato: Log when subscription ID fails to parse - this would prevent EOSE dispatch */
                    if (debug_eose_cached || getenv("NOSTR_DEBUG_LIFECYCLE")) {
                        fprintf(stderr, "[EOSE_ERROR] relay=%s - failed to parse subscription ID from '%s'\n",
                                r->url ? r->url : "unknown", env->message);
                    }
                    nostr_metric_counter_add("eose_parse_error", 1);
                } else {
                    NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, serial);
                    if (subscription) {
                        if (debug_eose_cached) {
                            fprintf(stderr, "[EOSE_DISPATCH] relay=%s sid=%s serial=%d - dispatching to subscription\n",
                                    r->url ? r->url : "unknown", env->message, serial);
                        }
                        nostr_subscription_dispatch_eose(subscription);
                    } else {
                        /* This is NORMAL when a subscription is closed due to timeout before EOSE arrives.
                         * The subscription_free removes from the map, then EOSE arrives from the relay.
                         * Only log in debug mode to avoid noise during normal operation. */
                        if (debug_eose_cached || getenv("NOSTR_DEBUG_LIFECYCLE")) {
                            fprintf(stderr, "[SUB_LIFECYCLE] EOSE_LATE relay=%s sid=%s serial=%d (subscription already freed - normal for slow relays)\n",
                                    r->url ? r->url : "unknown", env->message, serial);
                        }
                        nostr_metric_counter_add("eose_late_arrival", 1);
                    }
                }
            } else {
                /* hq-3xato: Log when EOSE has no subscription ID */
                if (debug_eose_cached) {
                    fprintf(stderr, "[EOSE_ERROR] relay=%s - EOSE with NULL subscription ID\n",
                            r->url ? r->url : "unknown");
                }
            }
            char tmp[128]; snprintf(tmp, sizeof(tmp), "EOSE sid=%s", env->message ? env->message : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_AUTH: {
            // Free previous challenge if any
            if (r->priv->challenge) { free(r->priv->challenge); r->priv->challenge = NULL; }
            // Copy challenge string (don't alias envelope's string that will be freed)
            const char *ch = ((NostrAuthEnvelope *)envelope)->challenge;
            r->priv->challenge = ch ? strdup(ch) : NULL;
            char tmp[256]; snprintf(tmp, sizeof(tmp), "AUTH challenge=%s", r->priv->challenge ? r->priv->challenge : "");
            relay_debug_emit(r, tmp);

            // Invoke auth callback if registered (nostrc-7og)
            NostrRelayAuthCallback auth_cb = NULL;
            void *auth_cb_data = NULL;
            nsync_mu_lock(&r->priv->mutex);
            auth_cb = r->priv->auth_callback;
            auth_cb_data = r->priv->auth_callback_user_data;
            nsync_mu_unlock(&r->priv->mutex);
            if (auth_cb && r->priv->challenge) {
                auth_cb(r, r->priv->challenge, auth_cb_data);
            }
            break; }
        case NOSTR_ENVELOPE_EVENT: {
            NostrEventEnvelope *env = (NostrEventEnvelope *)envelope;
            // Emit summary BEFORE handing event to subscription
            if (env->event) {
                char tmp[256];
                const char *id = env->event->id ? env->event->id : "";
                const char *pk = env->event->pubkey ? env->event->pubkey : "";
                snprintf(tmp, sizeof(tmp), "EVENT kind=%d pubkey=%.8s id=%.8s", env->event->kind, pk, id);
                relay_debug_emit(r, tmp);
            }
            NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, nostr_sub_id_to_serial(env->subscription_id));
            if (subscription && env->event) {
                /* Security: drop events from banned pubkeys early */
                int banned = 0;
                if (env->event->pubkey && *env->event->pubkey) {
                    nsync_mu_lock(&r->priv->mutex);
                    banned = nostr_invalidsig_is_banned(r, env->event->pubkey);
                    nsync_mu_unlock(&r->priv->mutex);
                }
                if (banned) {
                    // drop banned pubkey events (rate-limited log and metric)
                    if (env->event->pubkey)
                        nostr_rl_log(NLOG_WARN, "relay", "drop banned pk=%.8s", env->event->pubkey);
                    nostr_metric_counter_add("event_ban_drop", 1);
                    break;
                }
                // Optionally verify signature if available
                bool verified = true;
                if (!r->assume_valid) {
                    if (record_metrics) {
                        nostr_metric_timer t_verify = {0};
                        nostr_metric_timer_start(&t_verify);
                        verified = nostr_event_check_signature(env->event);
                        static nostr_metric_histogram *h_event_verify_ns;
                        if (!h_event_verify_ns) h_event_verify_ns = nostr_metric_histogram_get("event_verify_ns");
                        nostr_metric_timer_stop(&t_verify, h_event_verify_ns);
                        nostr_metric_counter_add("event_verify_sampled", metrics_sample_rate);
                    } else {
                        verified = nostr_event_check_signature(env->event);
                    }
                }
                if (!verified) {
                    if (env->event->pubkey && *env->event->pubkey) {
                        nsync_mu_lock(&r->priv->mutex);
                        nostr_invalidsig_record_fail(r, env->event->pubkey);
                        nsync_mu_unlock(&r->priv->mutex);
                    }
                    nostr_metric_counter_add("event_invalidsig_record", 1);
                    // drop invalid event
                    if (debug_incoming_cached) {
                        char tmp[256];
                        const char *id = env->event->id ? env->event->id : "";
                        snprintf(tmp, sizeof(tmp), "DROP invalid signature id=%.8s", id);
                        relay_debug_emit(r, tmp);
                    }
                } else {
                    /* Producer-side rate limiting (nostrc-7u2):
                     * Check queue pressure before dispatching.
                     * - Critical events (DMs, zaps, mentions) always dispatched
                     * - Low priority events (reactions) dropped under extreme pressure
                     * - Throttle (sleep) when queue is filling up */
                    uint32_t util = nostr_subscription_get_queue_utilization(subscription);
                    NostrEventPriority priority = nostr_event_get_priority(env->event, NULL);

                    /* Under extreme pressure (>95%), drop low priority events */
                    if (util > 95 && priority == NOSTR_EVENT_PRIORITY_LOW) {
                        nostr_metric_counter_add("event_drop_backpressure", 1);
                        nostr_rl_log(NLOG_DEBUG, "relay", "drop low-priority event: queue %u%% full", util);
                        /* Event dropped - will be freed by envelope cleanup */
                    } else {
                        /* Apply throttle delay for non-critical events under pressure */
                        if (priority != NOSTR_EVENT_PRIORITY_CRITICAL) {
                            uint64_t delay_us = nostr_subscription_get_throttle_delay_us(subscription);
                            if (delay_us > 0) {
                                nostr_metric_counter_add("relay_throttle_applied", 1);
                                usleep((useconds_t)delay_us);
                            }
                        }

                        if (record_metrics) {
                            nostr_metric_timer t_dispatch = {0};
                            nostr_metric_timer_start(&t_dispatch);
                            nostr_subscription_dispatch_event(subscription, env->event);
                            static nostr_metric_histogram *h_event_dispatch_ns;
                            if (!h_event_dispatch_ns) h_event_dispatch_ns = nostr_metric_histogram_get("event_dispatch_ns");
                            nostr_metric_timer_stop(&t_dispatch, h_event_dispatch_ns);
                            nostr_metric_counter_add("event_dispatch_sampled", metrics_sample_rate);
                        } else {
                            nostr_subscription_dispatch_event(subscription, env->event);
                        }
                        // ownership passed to subscription; avoid double free
                        env->event = NULL;
                    }
                }
            }
            break; }
        case NOSTR_ENVELOPE_CLOSED: {
            NostrClosedEnvelope *env = (NostrClosedEnvelope *)envelope;
            NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, nostr_sub_id_to_serial(env->subscription_id));
            if (subscription) nostr_subscription_dispatch_closed(subscription, env->reason);
            /* nostrc-95c: Log CLOSED with reason to help debug subscription issues */
            fprintf(stderr, "[RELAY_CLOSED] relay=%s subscription=%s reason=\"%s\"\n",
                    r->url ? r->url : "unknown",
                    env->subscription_id ? env->subscription_id : "",
                    env->reason ? env->reason : "");
            char tmp[256]; snprintf(tmp, sizeof(tmp), "CLOSED sid=%s reason=%s",
                                   env->subscription_id ? env->subscription_id : "",
                                   env->reason ? env->reason : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_OK: {
            NostrOKEnvelope *oe = (NostrOKEnvelope *)envelope;
            /* nostrc-95c: Log OK failures at WARN level to surface publish rejections */
            if (!oe->ok) {
                fprintf(stderr, "[RELAY_OK_FAIL] relay=%s event=%s reason=\"%s\"\n",
                        r->url ? r->url : "unknown",
                        oe->event_id ? oe->event_id : "",
                        oe->reason ? oe->reason : "");
            }
            char tmp[256]; snprintf(tmp, sizeof(tmp), "OK id=%s ok=%s reason=%s",
                                   oe->event_id ? oe->event_id : "",
                                   oe->ok ? "true" : "false",
                                   oe->reason ? oe->reason : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_COUNT: {
            NostrCountEnvelope *ce = (NostrCountEnvelope *)envelope;
            NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, nostr_sub_id_to_serial(ce->subscription_id));
            if (subscription && subscription->priv->count_result) {
                int64_t *val = (int64_t *)malloc(sizeof(int64_t));
                if (val) { *val = (int64_t)ce->count; go_channel_send(subscription->priv->count_result, val); }
            }
            char tmp[64]; snprintf(tmp, sizeof(tmp), "COUNT=%d", ce->count);
            relay_debug_emit(r, tmp);
            break; }
        default:
            break;
        }

        nostr_envelope_free(envelope);
        }  /* end inner message processing loop */

        /* ================================================================
         * Reconnection logic (nostrc-4du)
         * ================================================================ */

        /* If context was canceled (relay being closed), exit without reconnecting */
        if (context_canceled) {
            relay_set_state(r, NOSTR_RELAY_STATE_DISCONNECTED);
            break;  /* exit outer loop */
        }

        /* Check if auto-reconnect is enabled */
        nsync_mu_lock(&r->priv->mutex);
        bool should_reconnect = r->priv->auto_reconnect;
        nsync_mu_unlock(&r->priv->mutex);

        if (!should_reconnect) {
            relay_set_state(r, NOSTR_RELAY_STATE_DISCONNECTED);
            break;  /* exit outer loop */
        }

        /* Check context again (may have been canceled during processing) */
        if (go_context_is_canceled(ctx)) {
            relay_set_state(r, NOSTR_RELAY_STATE_DISCONNECTED);
            break;  /* exit outer loop */
        }

        /* Connection lost, attempt reconnection with backoff */
        relay_set_state(r, NOSTR_RELAY_STATE_DISCONNECTED);

        /* Increment reconnect attempt counter */
        nsync_mu_lock(&r->priv->mutex);
        r->priv->reconnect_attempt++;
        int attempt = r->priv->reconnect_attempt;
        nsync_mu_unlock(&r->priv->mutex);

        /* Calculate backoff with jitter */
        uint64_t backoff_ms = calculate_backoff_with_jitter(attempt - 1);

        fprintf(stderr, "[RECONNECT] Connection lost to %s, attempt %d, waiting %llums\n",
                r->url ? r->url : "unknown", attempt, (unsigned long long)backoff_ms);
        nostr_metric_counter_add("relay_reconnect_attempt", 1);

        /* Set state to backoff */
        nsync_mu_lock(&r->priv->mutex);
        r->priv->backoff_ms = backoff_ms;
        r->priv->next_reconnect_time_ms = get_monotonic_time_ms() + backoff_ms;
        nsync_mu_unlock(&r->priv->mutex);
        relay_set_state(r, NOSTR_RELAY_STATE_BACKOFF);

        /* Wait for backoff period, checking for context cancellation */
        uint64_t wait_start = get_monotonic_time_ms();
        while (get_monotonic_time_ms() - wait_start < backoff_ms) {
            /* Check if context is canceled or reconnect_now was called */
            nsync_mu_lock(&r->priv->mutex);
            bool reconnect_now = r->priv->reconnect_requested;
            r->priv->reconnect_requested = false;
            nsync_mu_unlock(&r->priv->mutex);

            if (reconnect_now) break;  /* Skip remaining backoff */
            if (go_context_is_canceled(ctx)) {
                context_canceled = true;
                break;  /* Cancel reconnection */
            }
            usleep(100000);  /* Sleep 100ms between checks */
        }

        if (context_canceled) {
            relay_set_state(r, NOSTR_RELAY_STATE_DISCONNECTED);
            break;  /* exit outer loop */
        }

        /* Attempt reconnection */
        if (relay_attempt_reconnect(r)) {
            /* Success! Reset counters and continue processing messages */
            has_priority = 0;
            msg_count = 0;
            continue;  /* back to outer loop -> inner loop */
        }

        /* Reconnection failed, will retry with increased backoff */
        /* Continue outer loop to try again */
    }  /* end outer reconnection loop */

    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] message_loop: exit\n");
    free(buf);
    free(priority_buf);
    go_context_unref(ctx);  // Release context reference (nostrc-0q4)
    go_wait_group_done(&r->priv->workers);  // Use local 'r', not freed 'arg'
    return NULL;
}

int nsync_go_context_is_canceled(const void *ctx) {
    return go_context_is_canceled((GoContext *)ctx);
}

GoChannel *nostr_relay_write(NostrRelay *r, char *msg) {
    if (!r) return NULL;
    GoChannel *chan = go_channel_create(1);

    // Copy message so caller can free its own buffer safely
    char *msg_copy = strdup(msg ? msg : "");
    NostrRelayWriteRequest *req = (NostrRelayWriteRequest *)malloc(sizeof(NostrRelayWriteRequest));
    if (!req || !msg_copy) {
        if (req) free(req);
        if (msg_copy) free(msg_copy);
        go(write_error, chan);
        return chan;
    }
    req->msg = msg_copy;
    req->answer = chan;

    // Enqueue request (non-blocking); if fails and context canceled, return error
    if (go_channel_send(r->priv->write_queue, req) != 0) {
        // Fallback: if cannot enqueue, surface error
        go(write_error, chan);
        free(req->msg);
        free(req);
        return chan;
    }
    return chan;
}

void nostr_relay_publish(NostrRelay *relay, NostrEvent *event) {
    if (!relay) {
        fprintf(stderr, "[nostr_relay_publish] ERROR: relay is NULL\n");
        return;
    }
    if (!event) {
        fprintf(stderr, "[nostr_relay_publish] ERROR: event is NULL\n");
        return;
    }

    nostr_metric_timer t_ser = {0};
    nostr_metric_timer_start(&t_ser);
    char *event_json = nostr_event_serialize_compact(event);
    static nostr_metric_histogram *h_event_serialize_ns;
    if (!h_event_serialize_ns) h_event_serialize_ns = nostr_metric_histogram_get("event_serialize_ns");
    nostr_metric_timer_stop(&t_ser, h_event_serialize_ns);
    if (!event_json) {
        fprintf(stderr, "[nostr_relay_publish] ERROR: failed to serialize event\n");
        return;
    }

    nostr_metric_counter_add("events_published", 1);
    /* NIP-01 requires client publish envelope: ["EVENT", <event>] */
    size_t ej_len = strlen(event_json);
    size_t frame_len = 10 /* ["EVENT",] */ + ej_len + 1;
    char *frame = (char *)malloc(frame_len);
    if (!frame) {
        fprintf(stderr, "[nostr_relay_publish] ERROR: malloc failed\n");
        free(event_json);
        return;
    }
    /* Build the frame without trailing newline */
    /* snprintf format: ["EVENT",%s] */
    int nw = snprintf(frame, frame_len, "[\"EVENT\",%s]", event_json);
    free(event_json);
    if (nw <= 0 || (size_t)nw >= frame_len) {
        fprintf(stderr, "[nostr_relay_publish] ERROR: snprintf failed\n");
        free(frame);
        return;
    }

    /* Log the EVENT being sent for debugging (matching nostr_subscription_fire style) */
    fprintf(stderr, "[nostr_relay_publish] sending to %s: %s\n",
            relay->url ? relay->url : "(null)", frame);

    nostr_metric_counter_add("ws_tx_bytes", (uint64_t)nw);

    /* Enqueue the write and wait for confirmation */
    GoChannel *write_ch = nostr_relay_write(relay, frame);
    free(frame);

    if (!write_ch) {
        fprintf(stderr, "[nostr_relay_publish] ERROR: nostr_relay_write returned NULL channel\n");
        return;
    }

    /* Non-blocking check for immediate write failure only.
     * DO NOT block — nostr_relay_publish is called from the GTK main
     * thread (likes, DMs, chess, reports, etc.) and any blocking wait
     * stalls the entire UI.  The relay writer thread will complete the
     * WebSocket send asynchronously. */
    {
        Error *write_err = NULL;
        GoSelectCase cases[1];
        cases[0].op = GO_SELECT_RECEIVE;
        cases[0].chan = write_ch;
        cases[0].recv_buf = (void **)&write_err;

        GoSelectResult result = go_select_timeout(cases, 1, 0);

        if (result.selected_case >= 0 && write_err) {
            fprintf(stderr, "[nostr_relay_publish] write failed: %s\n",
                    write_err->message ? write_err->message : "unknown");
            free_error(write_err);
            return;
        }
    }
}

void nostr_relay_auth(NostrRelay *relay, void (*sign)(NostrEvent *, Error **), Error **err) {

    NostrEvent auth_event = {
        .id = NULL,
        .pubkey = NULL,
        .created_at = 0,
        .kind = NOSTR_KIND_CLIENT_AUTHENTICATION,
        .tags = nostr_tags_new(2,
                nostr_tag_new("challenge", relay->priv->challenge, NULL),
                nostr_tag_new("relay", relay->url, NULL)),
        .content = strdup(""),
        .sig = NULL};
    Error *sig_err = NULL;
    sign(&auth_event, &sig_err);
    if (sig_err) {
        if (err) *err = sig_err;
        return;
    }
    nostr_relay_publish(relay, &auth_event);
    nostr_tags_free(auth_event.tags);
}

bool nostr_relay_subscribe(NostrRelay *relay, GoContext *ctx, NostrFilters *filters, Error **err) {
    // Ensure the relay connection exists
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to %s", relay->url);
        return false;
    }

    // Prepare the subscription
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, filters);
    if (!subscription) {
        if (err && *err == NULL) *err = new_error(1, "failed to prepare subscription");
        return false;
    }

    // Send the subscription request (Fire the subscription)
    if (!nostr_subscription_fire(subscription, err)) {
        if (err && *err == NULL) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "couldn't subscribe to filter at relay");
        return false;
    }

    // Successfully subscribed
    return true;
}

NostrSubscription *nostr_relay_prepare_subscription(NostrRelay *relay, GoContext *ctx, NostrFilters *filters) {
    if (!relay || !filters || !ctx) {
        return NULL;
    }

    // Create subscription - nostr_subscription_new() already generates a unique ID
    // from the global g_sub_counter and sets up the context derived from relay's
    // connection context. We use that ID directly to avoid counter desync issues.
    NostrSubscription *subscription = nostr_subscription_new(relay, filters);
    if (!subscription) return NULL;
    
    // Note: nostr_subscription_new() already creates a context derived from the relay's
    // connection context and starts the lifecycle thread. We don't create a new context
    // here to avoid orphaning the lifecycle thread which is waiting on the original context.
    (void)ctx; // Mark as intentionally unused
    subscription->priv->match = nostr_filters_match; // Function for matching filters with events

    // Store subscription in relay subscriptions map using the ID set by nostr_subscription_new
    go_hash_map_insert_int(relay->subscriptions, subscription->priv->counter, subscription);

    return subscription;
}

GoChannel *nostr_relay_query_events(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, Error **err) {
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to relay");
        return NULL;
    }

    NostrFilters filters = {
        .filters = filter,
        .count = 1,
        .capacity = 1,
    };

    // Prepare the subscription
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        if (err) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "failed to prepare subscription");
        return NULL;
    }

    // Fire the subscription (send REQ)
    if (!nostr_subscription_fire(subscription, err)) {
        if (err) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "couldn't subscribe to filter at relay");
        return NULL;
    }

    // Return the channel where events will be received
    return subscription->events;
}

NostrEvent **nostr_relay_query_sync(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, int *event_count, Error **err) {
    if (!relay->connection) {
        *err = new_error(1, "not connected to relay");
        return NULL;
    }

    // Hold a reference to connection_context to prevent use-after-free if relay
    // is freed while we're in go_select. The context's refcount ensures it won't
    // be freed until we release our reference.
    GoContext *conn_ctx = relay->priv->connection_context;
    if (!conn_ctx) {
        *err = new_error(1, "relay has no connection context");
        return NULL;
    }
    go_context_ref(conn_ctx);

    // Create an array to store the events
    size_t max_events = (filter->limit > 0) ? filter->limit : 250; // Default to 250 if no limit is specified
    NostrEvent **events = (NostrEvent **)malloc(max_events * sizeof(NostrEvent *));
    if (!events) {
        *err = new_error(1, "failed to allocate memory for events");
        go_context_unref(conn_ctx);
        return NULL;
    }

    NostrFilters filters = {
        .filters = filter,
        .count = 1,
        .capacity = 1,
    };

    // Prepare the subscription
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        *err = new_error(1, "failed to prepare subscription");
        free(events);
        go_context_unref(conn_ctx);
        return NULL;
    }

    // Fire the subscription (send REQ)
    if (!nostr_subscription_fire(subscription, err)) {
        free(events);
        go_context_unref(conn_ctx);
        return NULL;
    }

    // Wait for events or until the subscription closes
    // nostrc-9o1: Use proper connection signals instead of arbitrary timeouts
    size_t received_count = 0;
    GoSelectCase cases[] = {
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = subscription->events, .value = NULL, .recv_buf = NULL },
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = subscription->end_of_stored_events, .value = NULL, .recv_buf = NULL },
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = conn_ctx->done, .value = NULL, .recv_buf = NULL },
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = subscription->closed_reason, .value = NULL, .recv_buf = NULL },
    };

    while (true) {
        // Select which event happens (receiving an event or end of stored events)
        int result = go_select(cases, 4);
        switch (result) {
        case 0: { // New event received
            if (received_count >= max_events) {
                max_events *= 2; // Expand the events array if needed
                NostrEvent **new_events = (NostrEvent **)realloc(events, max_events * sizeof(NostrEvent *));
                if (!new_events) {
                    if (err) *err = new_error(1, "failed to expand event array");
                    free(events);
                    go_context_unref(conn_ctx);
                    return NULL;
                }
                events = new_events;
            }

            NostrEvent *event = NULL;
            go_channel_receive(subscription->events, (void **)&event);
            events[received_count++] = event; // Store the event
            break;
        }
        case 1: {                             // End of stored events (EOSE)
            nostr_subscription_unsubscribe(subscription); // Unsubscribe from the relay
            *event_count = (int)received_count;    // Set the event count for the caller
            go_context_unref(conn_ctx);
            return events;                    // Return the array of events
        }
        case 2: { // Connection context is canceled (relay is closing)
            if (err) *err = new_error(1, "relay connection closed while querying events");
            free(events);
            go_context_unref(conn_ctx);
            return NULL;
        }
        case 3: { // Subscription closed by relay (CLOSED message)
            // Relay sent CLOSED - return what we have so far
            nostr_subscription_unsubscribe(subscription);
            *event_count = (int)received_count;
            go_context_unref(conn_ctx);
            return events;
        }
        default:
            break;
        }
    }
}

int64_t nostr_relay_count(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, Error **err) {
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to relay");
        return 0;
    }

    NostrFilters filters = {
        .filters = filter,
        .count = 1,
        .capacity = 1,
    };

    // Prepare the subscription (but don't fire it yet)
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        if (err) *err = new_error(1, "failed to prepare subscription");
        return 0;
    }

    subscription->priv->count_result = go_channel_create(1);

    // Fire the subscription (send REQ)
    if (!nostr_subscription_fire(subscription, err)) {
        if (err) *err = new_error(1, "failed to send subscription request");
        return -1;
    }

    // Wait for count result
    int64_t *count = NULL;
    if (go_channel_receive(subscription->priv->count_result, (void **)&count) != 0 || !count) {
        if (err) *err = new_error(1, "failed to receive count result");
        return -1;
    }
    int64_t result = *count;
    free(count);
    return result;
}

bool nostr_relay_close(NostrRelay *r, Error **err) {
    if (!r) {
        if (err) *err = new_error(ERR_RELAY_CLOSE_FAILED, "invalid relay");
        return false;
    }

    // Cancel context to wake workers and stop I/O
    nsync_mu_lock(&r->priv->mutex);
    r->priv->connection_context_cancel(r->priv->connection_context);
    // Close queues to unblock writer/select before tearing down connection
    if (r->priv->write_queue) go_channel_close(r->priv->write_queue);
    if (r->priv->subscription_channel_close_queue) go_channel_close(r->priv->subscription_channel_close_queue);
    // Snapshot connection and clear relay pointer so workers see NULL
    NostrConnection *conn = r->connection;
    r->connection = NULL;
    nsync_mu_unlock(&r->priv->mutex);

    if (!conn) {
        if (err) *err = new_error(ERR_RELAY_CLOSE_FAILED, "relay not connected");
        return false;
    }

    /* nostrc-ws1: Close send/recv channels BEFORE waiting for workers to
     * prevent deadlock.  write_operations can block in go_channel_send()
     * on a full send_channel; closing it wakes the blocked sender.
     * go_channel_close is idempotent — double-close in nostr_connection_close
     * is safe. */
    if (conn->recv_channel) go_channel_close(conn->recv_channel);
    if (conn->send_channel) go_channel_close(conn->send_channel);
    // Workers observe closed channels / NULL connection / canceled context → exit
    go_wait_group_wait(&r->priv->workers);
    // Now that workers are done, it's safe to free channels and connection.
    if (conn->recv_channel) { go_channel_free(conn->recv_channel); conn->recv_channel = NULL; }
    if (conn->send_channel) { go_channel_free(conn->send_channel); conn->send_channel = NULL; }
    nostr_connection_close(conn);
    return true;
}

void nostr_relay_disconnect(NostrRelay *relay) {
    if (!relay) return;
    Error *err = NULL;
    (void)nostr_relay_close(relay, &err);
    if (err) free_error(err);
}

/* Legacy relay_unsubscribe removed. Use nostr_subscription_unsubscribe() via NostrSubscription* or
 * provide a thin wrapper in higher layers if needed. */

/* ========================================================================
 * Auto-reconnection with exponential backoff (nostrc-4du)
 * ======================================================================== */

/* Configuration constants */
#define RECONNECT_INITIAL_DELAY_MS    1000    /* 1 second */
#define RECONNECT_MAX_DELAY_MS        300000  /* 5 minutes */
#define RECONNECT_JITTER_FACTOR       0.5     /* ±50% jitter */

static uint64_t get_monotonic_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Simple pseudo-random for jitter (doesn't need crypto quality) */
static double random_double(void) {
    static unsigned int seed = 0;
    if (seed == 0) {
        seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    }
    seed = seed * 1103515245 + 12345;
    return (double)(seed % 10000) / 10000.0;
}

/* Calculate backoff with jitter: backoff * (1 - jitter/2 + random * jitter) */
static uint64_t calculate_backoff_with_jitter(int attempt) {
    /* Exponential backoff: initial * 2^attempt */
    uint64_t backoff = RECONNECT_INITIAL_DELAY_MS;
    for (int i = 0; i < attempt && backoff < RECONNECT_MAX_DELAY_MS; i++) {
        backoff *= 2;
    }
    if (backoff > RECONNECT_MAX_DELAY_MS) {
        backoff = RECONNECT_MAX_DELAY_MS;
    }

    /* Apply jitter: backoff * (0.5 + random(0, 0.5)) = backoff * [0.5, 1.0] */
    double jitter_multiplier = (1.0 - RECONNECT_JITTER_FACTOR / 2.0) +
                               random_double() * RECONNECT_JITTER_FACTOR;
    return (uint64_t)(backoff * jitter_multiplier);
}

/* Helper to update state and invoke callback */
static void relay_set_state(NostrRelay *relay, NostrRelayConnectionState new_state) {
    if (!relay || !relay->priv) return;

    NostrRelayConnectionState old_state;
    NostrRelayStateCallback callback = NULL;
    void *user_data = NULL;

    nsync_mu_lock(&relay->priv->mutex);
    old_state = relay->priv->connection_state;
    if (old_state != new_state) {
        relay->priv->connection_state = new_state;
        callback = relay->priv->state_callback;
        user_data = relay->priv->state_callback_user_data;
    }
    nsync_mu_unlock(&relay->priv->mutex);

    /* Invoke callback outside the lock */
    if (callback && old_state != new_state) {
        callback(relay, old_state, new_state, user_data);
    }
}

const char *nostr_relay_get_connection_state_name(NostrRelayConnectionState state) {
    switch (state) {
        case NOSTR_RELAY_STATE_DISCONNECTED: return "disconnected";
        case NOSTR_RELAY_STATE_CONNECTING:   return "connecting";
        case NOSTR_RELAY_STATE_CONNECTED:    return "connected";
        case NOSTR_RELAY_STATE_BACKOFF:      return "backoff";
        default:                              return "unknown";
    }
}

void nostr_relay_set_auto_reconnect(NostrRelay *relay, bool enable) {
    if (!relay || !relay->priv) return;
    nsync_mu_lock(&relay->priv->mutex);
    relay->priv->auto_reconnect = enable;
    nsync_mu_unlock(&relay->priv->mutex);
}

bool nostr_relay_get_auto_reconnect(NostrRelay *relay) {
    if (!relay || !relay->priv) return false;
    nsync_mu_lock(&relay->priv->mutex);
    bool result = relay->priv->auto_reconnect;
    nsync_mu_unlock(&relay->priv->mutex);
    return result;
}

NostrRelayConnectionState nostr_relay_get_connection_state(NostrRelay *relay) {
    if (!relay || !relay->priv) return NOSTR_RELAY_STATE_DISCONNECTED;
    nsync_mu_lock(&relay->priv->mutex);
    NostrRelayConnectionState state = relay->priv->connection_state;
    nsync_mu_unlock(&relay->priv->mutex);
    return state;
}

void nostr_relay_set_state_callback(NostrRelay *relay,
                                    NostrRelayStateCallback callback,
                                    void *user_data) {
    if (!relay || !relay->priv) return;
    nsync_mu_lock(&relay->priv->mutex);
    relay->priv->state_callback = callback;
    relay->priv->state_callback_user_data = user_data;
    nsync_mu_unlock(&relay->priv->mutex);
}

void nostr_relay_set_auth_callback(NostrRelay *relay,
                                   NostrRelayAuthCallback callback,
                                   void *user_data) {
    if (!relay || !relay->priv) return;
    nsync_mu_lock(&relay->priv->mutex);
    relay->priv->auth_callback = callback;
    relay->priv->auth_callback_user_data = user_data;
    nsync_mu_unlock(&relay->priv->mutex);
}

int nostr_relay_get_reconnect_attempt(NostrRelay *relay) {
    if (!relay || !relay->priv) return 0;
    nsync_mu_lock(&relay->priv->mutex);
    int attempt = relay->priv->reconnect_attempt;
    nsync_mu_unlock(&relay->priv->mutex);
    return attempt;
}

uint64_t nostr_relay_get_next_reconnect_ms(NostrRelay *relay) {
    if (!relay || !relay->priv) return 0;
    nsync_mu_lock(&relay->priv->mutex);
    NostrRelayConnectionState state = relay->priv->connection_state;
    uint64_t next_time = relay->priv->next_reconnect_time_ms;
    nsync_mu_unlock(&relay->priv->mutex);

    if (state != NOSTR_RELAY_STATE_BACKOFF) return 0;

    uint64_t now = get_monotonic_time_ms();
    if (next_time <= now) return 0;
    return next_time - now;
}

void nostr_relay_reconnect_now(NostrRelay *relay) {
    if (!relay || !relay->priv) return;
    nsync_mu_lock(&relay->priv->mutex);
    NostrRelayConnectionState state = relay->priv->connection_state;
    if (state == NOSTR_RELAY_STATE_DISCONNECTED || state == NOSTR_RELAY_STATE_BACKOFF) {
        relay->priv->reconnect_requested = true;
        relay->priv->next_reconnect_time_ms = 0;  /* Clear backoff delay */
    }
    nsync_mu_unlock(&relay->priv->mutex);
}

void nostr_relay_set_custom_handler(NostrRelay *relay, bool (*handler)(const char *)) {
    if (!relay || !relay->priv) return;
    relay->priv->custom_handler = handler;
}
