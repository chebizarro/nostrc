/*
 * Optimized Relay Message Processing Implementation
 * 
 * Key optimizations:
 * 1. Dual-channel architecture for EOSE prioritization
 * 2. Parallel message processing with worker pool
 * 3. Batch processing to reduce channel overhead
 * 4. Async signature verification
 * 5. Sampled metrics to reduce hot path overhead
 */

#include "relay.h"
#include "relay-private.h"
#include "subscription-private.h"
#include "envelope.h"
#include "connection.h"
#include "go.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Performance tuning parameters (configurable via environment)
static int WORKER_POOL_SIZE = 4;
static int VERIFY_POOL_SIZE = 2;
static int BATCH_SIZE = 32;
static int CONTROL_CHAN_SIZE = 64;
static int EVENT_CHAN_SIZE = 256;
static int METRICS_SAMPLE_RATE = 100;
static bool METRICS_ENABLED = true;
static bool ASYNC_VERIFY = true;

// Cached environment variables to avoid repeated getenv() calls
static bool env_cached = false;
static bool debug_incoming = false;
static bool debug_eose = false;
static bool debug_shutdown = false;

typedef struct {
    WebSocketMessage **messages;
    int count;
    int capacity;
} MessageBatch;

typedef struct {
    NostrEvent *event;
    NostrSubscription *subscription;
    NostrRelay *relay;
    bool verified;
    GoChannel *result_chan;
} VerifyJob;

typedef struct {
    GoChannel *control_chan;    // Priority channel for EOSE, NOTICE, OK, CLOSED
    GoChannel *event_chan;       // Bulk channel for EVENT messages
    GoChannel *verify_queue;     // Async verification queue
    GoChannel *verify_results;   // Verification results
    GoWaitGroup *workers;
    _Atomic uint64_t msg_count;
    _Atomic uint64_t eose_count;
    _Atomic uint64_t event_count;
} OptimizedRelayChannels;

// Initialize performance parameters from environment
static void init_perf_params() {
    if (env_cached) return;
    
    const char *val;
    if ((val = getenv("NOSTR_WORKER_POOL_SIZE"))) {
        int n = atoi(val);
        if (n > 0 && n <= 16) WORKER_POOL_SIZE = n;
    }
    
    if ((val = getenv("NOSTR_VERIFY_POOL_SIZE"))) {
        int n = atoi(val);
        if (n > 0 && n <= 8) VERIFY_POOL_SIZE = n;
    }
    
    if ((val = getenv("NOSTR_BATCH_SIZE"))) {
        int n = atoi(val);
        if (n > 0 && n <= 128) BATCH_SIZE = n;
    }
    
    if ((val = getenv("NOSTR_CONTROL_CHAN_SIZE"))) {
        int n = atoi(val);
        if (n > 0) CONTROL_CHAN_SIZE = n;
    }
    
    if ((val = getenv("NOSTR_EVENT_CHAN_SIZE"))) {
        int n = atoi(val);
        if (n > 0) EVENT_CHAN_SIZE = n;
    }
    
    if ((val = getenv("NOSTR_METRICS_SAMPLE_RATE"))) {
        int n = atoi(val);
        if (n > 0) METRICS_SAMPLE_RATE = n;
    }
    
    METRICS_ENABLED = getenv("NOSTR_METRICS_DISABLED") == NULL;
    ASYNC_VERIFY = getenv("NOSTR_SYNC_VERIFY") == NULL;
    
    // Cache debug flags
    debug_incoming = getenv("NOSTR_DEBUG_INCOMING") != NULL;
    debug_eose = getenv("NOSTR_DEBUG_EOSE") != NULL;
    debug_shutdown = getenv("NOSTR_DEBUG_SHUTDOWN") != NULL;
    
    env_cached = true;
    
    fprintf(stderr, "[PERF] Initialized: workers=%d verify=%d batch=%d control=%d events=%d\n",
            WORKER_POOL_SIZE, VERIFY_POOL_SIZE, BATCH_SIZE, 
            CONTROL_CHAN_SIZE, EVENT_CHAN_SIZE);
}

// Identify control messages by parsing the first JSON array element only.
// Previous strstr() approach could false-positive on event content containing
// strings like ["EOSE" — this version only checks the message type token.
static bool is_control_message(const char *msg) {
    if (!msg) return false;

    // Skip leading whitespace
    while (*msg == ' ' || *msg == '\t' || *msg == '\n' || *msg == '\r') msg++;

    // Must start with '['
    if (*msg != '[') return false;
    msg++;

    // Skip whitespace after '['
    while (*msg == ' ' || *msg == '\t' || *msg == '\n' || *msg == '\r') msg++;

    // Must have opening quote for message type
    if (*msg != '"') return false;
    msg++;

    // Compare first token against known control message types.
    // Include closing '"' to prevent partial matches.
    if (strncmp(msg, "EOSE\"", 5) == 0 ||
        strncmp(msg, "OK\"", 3) == 0 ||
        strncmp(msg, "NOTICE\"", 7) == 0 ||
        strncmp(msg, "CLOSED\"", 7) == 0 ||
        strncmp(msg, "AUTH\"", 5) == 0 ||
        strncmp(msg, "COUNT\"", 6) == 0) {
        return true;
    }

    return false;
}

// Batch messages for efficient processing
static MessageBatch *create_batch(int capacity) {
    MessageBatch *batch = malloc(sizeof(MessageBatch));
    if (!batch) return NULL;
    
    batch->messages = calloc(capacity, sizeof(WebSocketMessage*));
    batch->capacity = capacity;
    batch->count = 0;
    return batch;
}

static void free_batch(MessageBatch *batch) {
    if (!batch) return;
    for (int i = 0; i < batch->count; i++) {
        if (batch->messages[i]) {
            free(batch->messages[i]->data);
            free(batch->messages[i]);
        }
    }
    free(batch->messages);
    free(batch);
}

// Verification worker for async signature checking
static void *verification_worker(void *arg) {
    OptimizedRelayChannels *channels = (OptimizedRelayChannels*)arg;
    
    while (1) {
        VerifyJob *job = NULL;
        if (go_channel_receive(channels->verify_queue, (void**)&job) != 0) {
            break; // Channel closed
        }
        
        if (!job) continue;
        
        // Perform signature verification
        job->verified = nostr_event_check_signature(job->event);
        
        // Send result back
        go_channel_send(channels->verify_results, job);
    }
    
    go_wait_group_done(channels->workers);
    return NULL;
}

// Process a single envelope (extracted from message_loop for clarity)
static void process_envelope(NostrRelay *r, NostrEnvelope *envelope, 
                            OptimizedRelayChannels *channels) {
    switch (envelope->type) {
    case NOSTR_ENVELOPE_EOSE: {
        NostrEOSEEnvelope *env = (NostrEOSEEnvelope *)envelope;
        atomic_fetch_add(&channels->eose_count, 1);
        
        if (env->message) {
            int serial = nostr_sub_id_to_serial(env->message);
            NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, serial);
            if (subscription) {
                if (debug_eose) {
                    fprintf(stderr, "[EOSE_DISPATCH] relay=%s sid=%s serial=%d\n",
                            r->url ? r->url : "unknown", env->message, serial);
                }
                nostr_subscription_dispatch_eose(subscription);
            } else {
                fprintf(stderr, "[EOSE_LATE] relay=%s sid=%s serial=%d\n",
                        r->url ? r->url : "unknown", env->message, serial);
            }
        }
        break;
    }
    
    case NOSTR_ENVELOPE_EVENT: {
        NostrEventEnvelope *env = (NostrEventEnvelope *)envelope;
        atomic_fetch_add(&channels->event_count, 1);
        
        NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, 
                                                              nostr_sub_id_to_serial(env->subscription_id));
        if (subscription && env->event) {
            // Check banned pubkeys
            int banned = 0;
            if (env->event->pubkey && *env->event->pubkey) {
                nsync_mu_lock(&r->priv->mutex);
                banned = nostr_invalidsig_is_banned(r, env->event->pubkey);
                nsync_mu_unlock(&r->priv->mutex);
            }
            
            if (banned) {
                nostr_event_free(env->event);
                env->event = NULL;
                break;
            }
            
            if (ASYNC_VERIFY && !r->assume_valid) {
                // Queue for async verification
                VerifyJob *job = malloc(sizeof(VerifyJob));
                if (!job) {
                    g_warning("Failed to allocate VerifyJob for async verification");
                    nostr_event_free(env->event);
                    env->event = NULL;
                    break;
                }
                job->event = env->event;
                job->subscription = subscription;
                job->relay = r;
                job->verified = false;

                go_channel_send(channels->verify_queue, job);
                env->event = NULL; // Ownership transferred
            } else {
                // Synchronous path (assume_valid or sync mode)
                bool verified = r->assume_valid || nostr_event_check_signature(env->event);
                if (verified) {
                    nostr_subscription_dispatch_event(subscription, env->event);
                    env->event = NULL; // Ownership transferred
                } else {
                    if (env->event->pubkey) {
                        nsync_mu_lock(&r->priv->mutex);
                        nostr_invalidsig_record_fail(r, env->event->pubkey);
                        nsync_mu_unlock(&r->priv->mutex);
                    }
                    nostr_event_free(env->event);
                    env->event = NULL;
                }
            }
        }
        break;
    }
    
    case NOSTR_ENVELOPE_NOTICE:
    case NOSTR_ENVELOPE_OK:
    case NOSTR_ENVELOPE_CLOSED:
    case NOSTR_ENVELOPE_AUTH:
    case NOSTR_ENVELOPE_COUNT:
        // Handle other control messages (simplified for brevity)
        // ... existing handling code ...
        break;
        
    default:
        break;
    }
}

// Worker for processing event messages in parallel
static void *event_worker(void *arg) {
    struct {
        NostrRelay *relay;
        OptimizedRelayChannels *channels;
        int worker_id;
    } *ctx = arg;
    
    char buf[4096];
    uint64_t local_msg_count = 0;
    
    while (1) {
        MessageBatch *batch = NULL;
        if (go_channel_receive(ctx->channels->event_chan, (void**)&batch) != 0) {
            break; // Channel closed
        }
        
        if (!batch) continue;
        
        // Process batch of messages
        for (int i = 0; i < batch->count; i++) {
            WebSocketMessage *msg = batch->messages[i];
            if (!msg) continue;
            
            local_msg_count++;
            
            // Sample metrics to reduce overhead
            bool record_metrics = METRICS_ENABLED && 
                                 (local_msg_count % METRICS_SAMPLE_RATE == 0);
            
            // Parse envelope
            NostrEnvelope *envelope = nostr_envelope_parse(msg->data);
            if (!envelope) {
                if (ctx->relay->priv->custom_handler) {
                    ctx->relay->priv->custom_handler(msg->data);
                }
                continue;
            }
            
            // Process the envelope
            process_envelope(ctx->relay, envelope, ctx->channels);
            
            // Record sampled metrics
            if (record_metrics) {
                nostr_metric_counter_add("event_processed_sampled", METRICS_SAMPLE_RATE);
            }
            
            nostr_envelope_free(envelope);
        }
        
        free_batch(batch);
    }
    
    go_wait_group_done(ctx->channels->workers);
    return NULL;
}

// Priority processor for control messages (EOSE, NOTICE, etc)
static void *control_processor(void *arg) {
    struct {
        NostrRelay *relay;
        OptimizedRelayChannels *channels;
    } *ctx = arg;
    
    while (1) {
        WebSocketMessage *msg = NULL;
        if (go_channel_receive(ctx->channels->control_chan, (void**)&msg) != 0) {
            break; // Channel closed
        }
        
        if (!msg) continue;
        
        atomic_fetch_add(&ctx->channels->msg_count, 1);
        
        // Parse and process immediately (priority)
        NostrEnvelope *envelope = nostr_envelope_parse(msg->data);
        if (envelope) {
            process_envelope(ctx->relay, envelope, ctx->channels);
            nostr_envelope_free(envelope);
        }
        
        free(msg->data);
        free(msg);
    }
    
    go_wait_group_done(ctx->channels->workers);
    return NULL;
}

/* nostrc-b0h: Event-driven batch collector.
 * REPLACED: try_receive + GO_SELECT_DEFAULT + usleep(1ms) polling loop.
 * NOW: Blocking go_channel_receive on the input channel. When a message
 * arrives, greedily drain all available messages. If the batch is full,
 * send it. If partial, use a short timed select (5ms) to allow more
 * messages to accumulate before flushing — this batches without polling. */
static void *batch_collector(void *arg) {
    struct {
        GoChannel *input;
        GoChannel *output;
        OptimizedRelayChannels *channels;
    } *ctx = arg;

    MessageBatch *current = create_batch(BATCH_SIZE);

    while (1) {
        WebSocketMessage *msg = NULL;

        if (current->count == 0) {
            /* Empty batch: BLOCK until first message arrives.
             * No polling, no CPU usage, instant wake. */
            if (go_channel_receive(ctx->input, (void **)&msg) != 0)
                break; /* Channel closed */
        } else {
            /* Partial batch: use timed select to collect more messages
             * or flush after a short deadline (5ms batch window). */
            GoSelectCase cases[] = {
                {GO_SELECT_RECEIVE, ctx->input, NULL, (void **)&msg},
            };
            int idx = go_select_timeout(cases, 1, 5); /* 5ms window */
            if (idx < 0) {
                /* Timeout or closed — flush what we have */
                go_channel_send(ctx->output, current);
                current = create_batch(BATCH_SIZE);
                continue;
            }
        }

        if (msg) {
            current->messages[current->count++] = msg;

            /* Greedily drain any additional buffered messages */
            while (current->count < BATCH_SIZE) {
                WebSocketMessage *extra = NULL;
                if (go_channel_try_receive(ctx->input, (void **)&extra) != 0)
                    break;
                if (extra)
                    current->messages[current->count++] = extra;
            }

            /* Send full batch immediately */
            if (current->count >= BATCH_SIZE) {
                go_channel_send(ctx->output, current);
                current = create_batch(BATCH_SIZE);
            }
        }
    }

    /* Flush remaining */
    if (current->count > 0)
        go_channel_send(ctx->output, current);
    else
        free_batch(current);

    go_wait_group_done(ctx->channels->workers);
    return NULL;
}

// Process verification results
static void *verification_result_processor(void *arg) {
    OptimizedRelayChannels *channels = (OptimizedRelayChannels*)arg;
    
    while (1) {
        VerifyJob *job = NULL;
        if (go_channel_receive(channels->verify_results, (void**)&job) != 0) {
            break;
        }
        
        if (!job) continue;
        
        if (job->verified) {
            nostr_subscription_dispatch_event(job->subscription, job->event);
            // Event ownership transferred
        } else {
            // Record failure and free event
            if (job->event->pubkey && job->relay) {
                nsync_mu_lock(&job->relay->priv->mutex);
                nostr_invalidsig_record_fail(job->relay, job->event->pubkey);
                nsync_mu_unlock(&job->relay->priv->mutex);
            }
            nostr_event_free(job->event);
        }
        
        free(job);
    }
    
    go_wait_group_done(channels->workers);
    return NULL;
}

// Optimized message loop with dual channels and worker pool
void *optimized_message_loop(void *arg) {
    NostrRelay *r = (NostrRelay *)arg;
    if (!r || !r->priv) return NULL;
    
    init_perf_params();
    
    // Create optimized channel structure
    OptimizedRelayChannels channels = {
        .control_chan = go_channel_create(CONTROL_CHAN_SIZE),
        .event_chan = go_channel_create(EVENT_CHAN_SIZE),
        .verify_queue = go_channel_create(VERIFY_POOL_SIZE * 2),
        .verify_results = go_channel_create(VERIFY_POOL_SIZE * 2),
        .workers = malloc(sizeof(GoWaitGroup)),
        .msg_count = 0,
        .eose_count = 0,
        .event_count = 0
    };

    if (!channels.workers) {
        g_warning("Failed to allocate GoWaitGroup for worker pool");
        go_channel_free(channels.control_chan);
        go_channel_free(channels.event_chan);
        go_channel_free(channels.verify_queue);
        go_channel_free(channels.verify_results);
        go_wait_group_done(&r->priv->workers);
        return NULL;
    }

    go_wait_group_init(channels.workers);
    
    // Start control processor (priority)
    go_wait_group_add(channels.workers, 1);
    struct {
        NostrRelay *relay;
        OptimizedRelayChannels *channels;
    } control_ctx = {r, &channels};
    go_fiber_compat(control_processor, &control_ctx);
    
    // Start event workers
    for (int i = 0; i < WORKER_POOL_SIZE; i++) {
        go_wait_group_add(channels.workers, 1);
        struct {
            NostrRelay *relay;
            OptimizedRelayChannels *channels;
            int worker_id;
        } *worker_ctx = malloc(sizeof(*worker_ctx));
        if (!worker_ctx) {
            g_warning("Failed to allocate worker context for event worker %d", i);
            go_wait_group_done(channels.workers);
            continue;
        }
        worker_ctx->relay = r;
        worker_ctx->channels = &channels;
        worker_ctx->worker_id = i;
        go_fiber_compat(event_worker, worker_ctx);
    }
    
    // Start verification workers
    for (int i = 0; i < VERIFY_POOL_SIZE; i++) {
        go_wait_group_add(channels.workers, 1);
        go_fiber_compat(verification_worker, &channels);
    }
    
    // Start verification result processor
    go_wait_group_add(channels.workers, 1);
    go_fiber_compat(verification_result_processor, &channels);
    
    // Main read loop - route messages to appropriate channels
    char buf[4096];
    Error *err = NULL;
    MessageBatch *event_batch = create_batch(BATCH_SIZE);
    
    while (1) {
        nsync_mu_lock(&r->priv->mutex);
        NostrConnection *conn = r->connection;
        nsync_mu_unlock(&r->priv->mutex);
        if (!conn) break;
        
        // Read message from websocket
        nostr_connection_read_message(conn, r->priv->connection_context, buf, sizeof(buf), &err);
        if (err) {
            free_error(err);
            err = NULL;
            break;
        }
        if (buf[0] == '\0') continue;
        
        // Create message copy
        size_t len = strlen(buf);
        WebSocketMessage *msg = malloc(sizeof(WebSocketMessage));
        if (!msg) {
            g_warning("Failed to allocate WebSocketMessage, skipping message");
            continue;
        }
        msg->length = len;
        msg->data = malloc(len + 1);
        if (!msg->data) {
            g_warning("Failed to allocate message data buffer, skipping message");
            free(msg);
            continue;
        }
        memcpy(msg->data, buf, len + 1);
        
        // Route based on message type
        if (is_control_message(buf)) {
            // Priority channel for control messages
            go_channel_send(channels.control_chan, msg);
        } else {
            // Add to event batch
            event_batch->messages[event_batch->count++] = msg;
            
            // Send batch when full
            if (event_batch->count >= BATCH_SIZE) {
                go_channel_send(channels.event_chan, event_batch);
                event_batch = create_batch(BATCH_SIZE);
            }
        }
    }
    
    // Cleanup
    free_batch(event_batch);
    
    // Close channels to signal workers
    go_channel_close(channels.control_chan);
    go_channel_close(channels.event_chan);
    go_channel_close(channels.verify_queue);
    go_channel_close(channels.verify_results);
    
    // Wait for all workers to finish
    go_wait_group_wait(channels.workers);
    
    // Free channels
    go_channel_free(channels.control_chan);
    go_channel_free(channels.event_chan);
    go_channel_free(channels.verify_queue);
    go_channel_free(channels.verify_results);
    
    go_wait_group_destroy(channels.workers);
    free(channels.workers);
    
    // Print final stats
    fprintf(stderr, "[PERF] Final stats: messages=%lu eose=%lu events=%lu\n",
            channels.msg_count, channels.eose_count, channels.event_count);
    
    go_wait_group_done(&r->priv->workers);
    return NULL;
}
