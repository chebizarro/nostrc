/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrSubscription: GObject wrapper for Nostr subscription lifecycle (nostrc-wjlt)
 *
 * Provides a signal-driven interface for Nostr subscriptions:
 * - Properties with notify signals (id, active, state)
 * - Event/EOSE/Closed signals emitted on main thread
 * - Monitor thread drains core GoChannels
 * - Proper GObject reference counting and cleanup
 */

#define G_LOG_DOMAIN "gnostr-subscription"

/* Core libnostr headers FIRST to get canonical type definitions */
#include "nostr-subscription.h"   /* NostrSubscription, core API */
#include "nostr-relay.h"          /* NostrRelay core */
#include "nostr-event.h"          /* NostrEvent, nostr_event_free */
#include "nostr-filter.h"         /* NostrFilters */
#include "json.h"                 /* nostr_event_serialize */
#include "context.h"              /* GoContext */
#include "channel.h"              /* GoChannel, go_channel_try_receive */
#include "error.h"                /* Error, free_error */

/* GObject wrapper headers */
#include "nostr_subscription.h"
#include "nostr_relay.h"                /* gnostr_relay_get_nip11_info */
#include "nostr_subscription_registry.h" /* relay subscription count */

/* NIP-11 relay information document */
#ifdef ENABLE_NIP11
#include "nip11.h"
#endif

#include <glib.h>

/* Property IDs */
enum {
    PROP_0,
    PROP_ID,
    PROP_ACTIVE,
    PROP_STATE,
    N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };
static guint sub_signals[GNOSTR_SUBSCRIPTION_SIGNALS_COUNT] = { 0 };

/* nostrc-mzab / nostrc-kw9r: Max events to emit per main loop iteration.
 * Prevents startup floods from blocking the UI — between batches the
 * main loop processes GTK redraws and input events.
 *
 * History: 5 (original, too slow → throttle cascade → recv_channel overflow)
 *        → 50 (too aggressive — starves GTK rendering + nostrdb dispatch,
 *              threads broke because main loop had no time between batches)
 *        → 20 (balanced: 4× original, drains fast enough to avoid overflow
 *              while leaving ~70% of each frame for rendering and dispatch) */
#define MAX_EVENTS_PER_TICK 20

struct _GNostrSubscription {
    GObject parent_instance;

    NostrSubscription *subscription;      /* core subscription pointer */
    GNostrRelay *relay;                   /* owning relay (ref held) */
    NostrFilters *owned_filters;          /* filters ownership (nostrc-aaf0) */
    GNostrSubscriptionState state;        /* lifecycle state */
    guint event_count;                    /* events received (atomic) */

    /* Monitor thread */
    GThread *monitor_thread;
    gboolean monitor_running;             /* atomic flag */

    /* Batched event delivery to main thread (nostrc-mzab).
     * The monitor thread appends serialized event JSONs to event_queue
     * and a single coalescing idle source drains them in chunks. */
    GMutex event_queue_mutex;
    GPtrArray *event_queue;               /* pending event JSON strings (char*) */
    gboolean event_idle_scheduled;        /* TRUE while drain idle is pending */
};

G_DEFINE_TYPE(GNostrSubscription, gnostr_subscription, G_TYPE_OBJECT)

/* --- Internal state management --- */

static void
gnostr_subscription_set_state_internal(GNostrSubscription *self,
                                       GNostrSubscriptionState new_state)
{
    if (self->state == new_state)
        return;

    GNostrSubscriptionState old_state = self->state;
    gboolean was_active = (old_state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
                           old_state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);
    gboolean is_active = (new_state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
                          new_state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);

    self->state = new_state;

    /* Emit state-changed signal */
    g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_STATE_CHANGED], 0,
                  old_state, new_state);

    /* Notify property changes */
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_STATE]);

    if (was_active != is_active)
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_ACTIVE]);
}

/* --- Monitor thread signal emission data --- */

typedef struct {
    GNostrSubscription *self;
} EoseSignalData;

typedef struct {
    GNostrSubscription *self;
    gchar *reason;
} ClosedSignalData;

/* nostrc-mzab: Batched event drain — processes up to MAX_EVENTS_PER_TICK
 * events per main loop iteration, then yields so GTK can render and
 * process input. Re-invoked via G_SOURCE_CONTINUE until queue is empty.
 *
 * Uses G_PRIORITY_DEFAULT_IDLE (200) so UI painting (priority 120) and
 * input events (priority 0) always take precedence over event ingestion. */
static gboolean
drain_event_queue_on_main(gpointer data)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(data);

    /* Steal up to MAX_EVENTS_PER_TICK events from the queue */
    g_mutex_lock(&self->event_queue_mutex);
    guint avail = self->event_queue->len;
    guint n = MIN(avail, MAX_EVENTS_PER_TICK);

    gchar **batch = NULL;
    if (n > 0) {
        batch = g_new(gchar *, n);
        for (guint i = 0; i < n; i++)
            batch[i] = g_ptr_array_index(self->event_queue, i);
        g_ptr_array_remove_range(self->event_queue, 0, n);
    }

    gboolean more = (self->event_queue->len > 0);
    if (!more)
        self->event_idle_scheduled = FALSE;
    g_mutex_unlock(&self->event_queue_mutex);

    /* Emit signals outside lock */
    for (guint i = 0; i < n; i++) {
        __atomic_add_fetch(&self->event_count, 1, __ATOMIC_SEQ_CST);
        g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EVENT], 0,
                      batch[i]);
        g_free(batch[i]);
    }
    g_free(batch);

    if (more)
        return G_SOURCE_CONTINUE;

    /* No more events — remove the idle source (destroy notify unrefs self) */
    return G_SOURCE_REMOVE;
}

static void
drain_event_queue_destroy(gpointer data)
{
    g_object_unref(GNOSTR_SUBSCRIPTION(data));
}

static gboolean
emit_eose_on_main(gpointer data)
{
    EoseSignalData *sdata = data;
    GNostrSubscription *self = sdata->self;

    gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);
    g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EOSE], 0);

    g_object_unref(self);
    g_free(sdata);
    return G_SOURCE_REMOVE;
}

static gboolean
emit_closed_on_main(gpointer data)
{
    ClosedSignalData *sdata = data;
    GNostrSubscription *self = sdata->self;

    gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_CLOSED);
    g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_CLOSED], 0,
                  sdata->reason);

    g_free(sdata->reason);
    g_object_unref(self);
    g_free(sdata);
    return G_SOURCE_REMOVE;
}

/* --- Monitor thread --- */

static gpointer
subscription_monitor_thread(gpointer data)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(data);
    NostrSubscription *sub = self->subscription;

    GoChannel *ch_events = nostr_subscription_get_events_channel(sub);
    GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);
    GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);

    while (__atomic_load_n(&self->monitor_running, __ATOMIC_SEQ_CST)) {
        gboolean any_activity = FALSE;

        /* Drain events channel into batched queue (nostrc-mzab) */
        void *msg = NULL;
        while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
            any_activity = TRUE;
            if (msg) {
                NostrEvent *ev = (NostrEvent *)msg;
                char *json = nostr_event_serialize(ev);
                nostr_event_free(ev);
                if (json) {
                    g_mutex_lock(&self->event_queue_mutex);
                    g_ptr_array_add(self->event_queue, json);
                    if (!self->event_idle_scheduled) {
                        self->event_idle_scheduled = TRUE;
                        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                        drain_event_queue_on_main,
                                        g_object_ref(self),
                                        drain_event_queue_destroy);
                    }
                    g_mutex_unlock(&self->event_queue_mutex);
                }
            }
            msg = NULL;
        }

        /* Check EOSE channel */
        if (ch_eose && go_channel_try_receive(ch_eose, NULL) == 0) {
            any_activity = TRUE;
            EoseSignalData *sdata = g_new(EoseSignalData, 1);
            sdata->self = g_object_ref(self);
            g_idle_add_full(G_PRIORITY_DEFAULT, emit_eose_on_main, sdata, NULL);
        }

        /* Check CLOSED channel */
        void *reason = NULL;
        if (ch_closed && go_channel_try_receive(ch_closed, &reason) == 0) {
            any_activity = TRUE;
            ClosedSignalData *sdata = g_new(ClosedSignalData, 1);
            sdata->self = g_object_ref(self);
            sdata->reason = reason ? g_strdup((const char *)reason) : NULL;
            g_idle_add_full(G_PRIORITY_DEFAULT, emit_closed_on_main, sdata, NULL);
            /* Subscription was closed by relay - stop monitoring */
            break;
        }

        if (!any_activity)
            g_usleep(1000); /* 1ms backoff */
    }

    g_object_unref(self); /* Release monitor thread's ref */
    return NULL;
}

static void
stop_monitor(GNostrSubscription *self)
{
    __atomic_store_n(&self->monitor_running, FALSE, __ATOMIC_SEQ_CST);
    if (self->monitor_thread) {
        g_thread_join(self->monitor_thread);
        self->monitor_thread = NULL;
    }
}

/* --- GObject boilerplate --- */

static void
gnostr_subscription_get_property(GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(object);

    switch (property_id) {
    case PROP_ID:
        if (self->subscription)
            g_value_set_string(value, nostr_subscription_get_id_const(self->subscription));
        else
            g_value_set_string(value, NULL);
        break;
    case PROP_ACTIVE:
        g_value_set_boolean(value,
                            self->state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
                            self->state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED);
        break;
    case PROP_STATE:
        g_value_set_enum(value, self->state);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_subscription_finalize(GObject *object)
{
    GNostrSubscription *self = GNOSTR_SUBSCRIPTION(object);

    /* Stop monitor thread first */
    stop_monitor(self);

    /* Close core subscription */
    if (self->subscription) {
        nostr_subscription_close(self->subscription, NULL);
        /* nostrc-ws3: Cancel subscription context + wait for lifecycle
         * worker BEFORE free.  Without this, subscription_destroy blocks
         * forever in go_wait_group_wait because the lifecycle thread is
         * stuck in go_channel_receive(done) waiting for context
         * cancellation that nostr_subscription_close does NOT do.
         * Same fix as nostrc-ws2 in query_thread_func. */
        nostr_subscription_wait(self->subscription);
        nostr_subscription_free(self->subscription);
        self->subscription = NULL;
    }

    /* Free owned filters AFTER core subscription is freed (nostrc-aaf0).
     * The core subscription borrows the filters pointer, so we must keep
     * them alive until after nostr_subscription_free(). */
    if (self->owned_filters) {
        nostr_filters_free(self->owned_filters);
        self->owned_filters = NULL;
    }

    /* nostrc-mzab: Discard any queued events not yet delivered.
     * The idle source (if pending) holds a ref, so finalize only runs
     * after the source is removed — the queue should already be empty.
     * This is a safety net for edge cases. */
    g_mutex_lock(&self->event_queue_mutex);
    if (self->event_queue) {
        for (guint i = 0; i < self->event_queue->len; i++)
            g_free(g_ptr_array_index(self->event_queue, i));
        g_ptr_array_free(self->event_queue, TRUE);
        self->event_queue = NULL;
    }
    g_mutex_unlock(&self->event_queue_mutex);
    g_mutex_clear(&self->event_queue_mutex);

    /* Release relay reference */
    g_clear_object(&self->relay);

    G_OBJECT_CLASS(gnostr_subscription_parent_class)->finalize(object);
}

static void
gnostr_subscription_class_init(GNostrSubscriptionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gnostr_subscription_get_property;
    object_class->finalize = gnostr_subscription_finalize;

    /**
     * GNostrSubscription:id:
     *
     * The subscription ID assigned by the core library. Read-only.
     */
    obj_properties[PROP_ID] =
        g_param_spec_string("id",
                            "ID",
                            "Subscription ID",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrSubscription:active:
     *
     * Whether the subscription is currently active (live). Read-only.
     * Derived from state: TRUE when ACTIVE or EOSE_RECEIVED.
     */
    obj_properties[PROP_ACTIVE] =
        g_param_spec_boolean("active",
                             "Active",
                             "Whether the subscription is active",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                             G_PARAM_STATIC_STRINGS);

    /**
     * GNostrSubscription:state:
     *
     * The current lifecycle state of the subscription. Read-only.
     */
    obj_properties[PROP_STATE] =
        g_param_spec_enum("state",
                          "State",
                          "Current lifecycle state",
                          GNOSTR_TYPE_SUBSCRIPTION_STATE,
                          GNOSTR_SUBSCRIPTION_STATE_PENDING,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);

    /* Signals */

    /**
     * GNostrSubscription::event:
     * @self: the subscription
     * @event_json: the event as a JSON string
     *
     * Emitted when an event is received from the relay.
     * The event is serialized as JSON for safe cross-thread delivery.
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EVENT] =
        g_signal_new("event",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrSubscription::eose:
     * @self: the subscription
     *
     * Emitted when End of Stored Events is received from the relay.
     * After EOSE, only new real-time events will arrive.
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EOSE] =
        g_signal_new("eose",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    /**
     * GNostrSubscription::closed:
     * @self: the subscription
     * @reason: (nullable): the close reason from relay, or %NULL
     *
     * Emitted when the subscription is closed, either by the client
     * or by the relay (via CLOSED message).
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_CLOSED] =
        g_signal_new("closed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrSubscription::state-changed:
     * @self: the subscription
     * @old_state: the previous #GNostrSubscriptionState
     * @new_state: the new #GNostrSubscriptionState
     *
     * Emitted when the subscription lifecycle state changes.
     */
    sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2,
                     GNOSTR_TYPE_SUBSCRIPTION_STATE,
                     GNOSTR_TYPE_SUBSCRIPTION_STATE);
}

static void
gnostr_subscription_init(GNostrSubscription *self)
{
    self->subscription = NULL;
    self->relay = NULL;
    self->state = GNOSTR_SUBSCRIPTION_STATE_PENDING;
    self->event_count = 0;
    self->monitor_thread = NULL;
    self->monitor_running = FALSE;

    /* nostrc-mzab: Batched event queue */
    g_mutex_init(&self->event_queue_mutex);
    self->event_queue = g_ptr_array_new();
    self->event_idle_scheduled = FALSE;
}

/* --- Public API --- */

GNostrSubscription *
gnostr_subscription_new(GNostrRelay *relay, NostrFilters *filters)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(relay), NULL);
    g_return_val_if_fail(filters != NULL, NULL);

    NostrRelay *core_relay = gnostr_relay_get_core_relay(relay);
    g_return_val_if_fail(core_relay != NULL, NULL);

    GNostrSubscription *self = g_object_new(GNOSTR_TYPE_SUBSCRIPTION, NULL);

    self->relay = g_object_ref(relay);

    /* Take ownership of filters — the core subscription borrows the pointer,
     * so we must keep them alive for the subscription's lifetime (nostrc-aaf0). */
    self->owned_filters = filters;

    /* Prepare the core subscription (allocates channels, generates ID) */
    GoContext *bg = go_context_background();
    self->subscription = nostr_relay_prepare_subscription(core_relay, bg, filters);

    if (!self->subscription) {
        g_warning("Failed to prepare subscription on relay %s",
                  gnostr_relay_get_url(relay));
        self->owned_filters = NULL; /* don't free caller's filters on failure */
        g_clear_object(&self->relay);
        g_object_unref(self);
        return NULL;
    }

    g_debug("Created subscription %s on %s",
            nostr_subscription_get_id_const(self->subscription),
            gnostr_relay_get_url(relay));

    return self;
}

/**
 * gnostr_subscription_detach_filters:
 *
 * Detaches filter ownership so they won't be freed in finalize.
 * Used when the subscription fails to fire and the caller retains
 * filter ownership. (nostrc-aaf0, internal API)
 */
void
gnostr_subscription_detach_filters(GNostrSubscription *self)
{
    if (self) self->owned_filters = NULL;
}

gboolean
gnostr_subscription_fire(GNostrSubscription *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), FALSE);

    if (self->state != GNOSTR_SUBSCRIPTION_STATE_PENDING) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                            "subscription is not in PENDING state");
        return FALSE;
    }

    if (!self->subscription) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "no core subscription");
        return FALSE;
    }

#ifdef ENABLE_NIP11
    /* nostrc-23: Enforce NIP-11 relay limitations before subscribing */
    if (self->relay) {
        const GNostrRelayNip11Info *nip11 = gnostr_relay_get_nip11_info(self->relay);
        if (nip11 && nip11->limitation) {
            RelayLimitationDocument *lim = nip11->limitation;

            if (lim->auth_required) {
                g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                            "relay %s requires NIP-42 authentication",
                            gnostr_relay_get_url(self->relay));
                return FALSE;
            }
            if (lim->payment_required) {
                g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PAYMENT_REQUIRED,
                            "relay %s requires payment",
                            gnostr_relay_get_url(self->relay));
                return FALSE;
            }
            if (lim->max_subscriptions > 0) {
                const gchar *url = gnostr_relay_get_url(self->relay);
                NostrSubscriptionRegistry *reg =
                    nostr_subscription_registry_get_default();
                guint current = nostr_subscription_registry_get_relay_subscription_count(reg, url);
                if ((int)current >= lim->max_subscriptions) {
                    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_SUBSCRIPTION_LIMIT,
                                "relay %s max_subscriptions (%d) reached",
                                url, lim->max_subscriptions);
                    return FALSE;
                }
            }
        }
    }
#endif

    /* Send REQ to relay */
    Error *core_err = NULL;
    if (!nostr_subscription_fire(self->subscription, &core_err)) {
        gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_ERROR);

        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                    "failed to fire subscription: %s",
                    core_err && core_err->message ? core_err->message : "unknown");
        if (core_err) free_error(core_err);
        return FALSE;
    }

    /* Transition to ACTIVE */
    gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_ACTIVE);

    /* Start monitor thread to drain channels and emit signals */
    __atomic_store_n(&self->monitor_running, TRUE, __ATOMIC_SEQ_CST);
    self->monitor_thread = g_thread_new("gnostr-sub-monitor",
                                         subscription_monitor_thread,
                                         g_object_ref(self));

    g_debug("Fired subscription %s",
            nostr_subscription_get_id_const(self->subscription));

    return TRUE;
}

void
gnostr_subscription_close(GNostrSubscription *self)
{
    g_return_if_fail(GNOSTR_IS_SUBSCRIPTION(self));

    if (self->state == GNOSTR_SUBSCRIPTION_STATE_CLOSED)
        return;

    /* Stop monitor thread */
    stop_monitor(self);

    /* Close core subscription */
    if (self->subscription) {
        nostr_subscription_close(self->subscription, NULL);
    }

    gnostr_subscription_set_state_internal(self, GNOSTR_SUBSCRIPTION_STATE_CLOSED);
    g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_CLOSED], 0, NULL);

    g_debug("Closed subscription %s",
            self->subscription ? nostr_subscription_get_id_const(self->subscription) : "(null)");
}

/* --- Property Accessors --- */

const gchar *
gnostr_subscription_get_id(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    if (!self->subscription) return NULL;
    return nostr_subscription_get_id_const(self->subscription);
}

gboolean
gnostr_subscription_get_active(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), FALSE);
    return self->state == GNOSTR_SUBSCRIPTION_STATE_ACTIVE ||
           self->state == GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED;
}

GNostrSubscriptionState
gnostr_subscription_get_state(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), GNOSTR_SUBSCRIPTION_STATE_ERROR);
    return self->state;
}

GNostrRelay *
gnostr_subscription_get_relay(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    return self->relay;
}

guint
gnostr_subscription_get_event_count(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), 0);
    return __atomic_load_n(&self->event_count, __ATOMIC_SEQ_CST);
}

NostrSubscription *
gnostr_subscription_get_core_subscription(GNostrSubscription *self)
{
    g_return_val_if_fail(GNOSTR_IS_SUBSCRIPTION(self), NULL);
    return self->subscription;
}

/* Alias used by subscription_registry — delegates to close */
void
gnostr_subscription_unsubscribe(GNostrSubscription *self)
{
    gnostr_subscription_close(self);
}

/* Stub for subscription_registry — config is registry-internal */
gconstpointer
gnostr_subscription_get_config(GNostrSubscription *self G_GNUC_UNUSED)
{
    return NULL;
}
