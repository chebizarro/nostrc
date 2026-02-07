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

/* GObject wrapper header */
#include "nostr_subscription.h"
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

struct _GNostrSubscription {
    GObject parent_instance;

    NostrSubscription *subscription;      /* core subscription pointer */
    GNostrRelay *relay;                   /* owning relay (ref held) */
    GNostrSubscriptionState state;        /* lifecycle state */
    guint event_count;                    /* events received (atomic) */

    /* Monitor thread */
    GThread *monitor_thread;
    gboolean monitor_running;             /* atomic flag */
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
    gchar *event_json;
} EventSignalData;

typedef struct {
    GNostrSubscription *self;
} EoseSignalData;

typedef struct {
    GNostrSubscription *self;
    gchar *reason;
} ClosedSignalData;

static gboolean
emit_event_on_main(gpointer data)
{
    EventSignalData *sdata = data;
    GNostrSubscription *self = sdata->self;

    __atomic_add_fetch(&self->event_count, 1, __ATOMIC_SEQ_CST);
    g_signal_emit(self, sub_signals[GNOSTR_SUBSCRIPTION_SIGNAL_EVENT], 0,
                  sdata->event_json);

    g_free(sdata->event_json);
    g_object_unref(self);
    g_free(sdata);
    return G_SOURCE_REMOVE;
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

        /* Drain events channel */
        void *msg = NULL;
        while (ch_events && go_channel_try_receive(ch_events, &msg) == 0) {
            any_activity = TRUE;
            if (msg) {
                NostrEvent *ev = (NostrEvent *)msg;
                char *json = nostr_event_serialize(ev);
                nostr_event_free(ev);
                if (json) {
                    EventSignalData *sdata = g_new(EventSignalData, 1);
                    sdata->self = g_object_ref(self);
                    sdata->event_json = json;
                    g_idle_add_full(G_PRIORITY_DEFAULT, emit_event_on_main, sdata, NULL);
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
        nostr_subscription_free(self->subscription);
        self->subscription = NULL;
    }

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

    /* Prepare the core subscription (allocates channels, generates ID) */
    GoContext *bg = go_context_background();
    self->subscription = nostr_relay_prepare_subscription(core_relay, bg, filters);

    if (!self->subscription) {
        g_warning("Failed to prepare subscription on relay %s",
                  gnostr_relay_get_url(relay));
        g_clear_object(&self->relay);
        g_object_unref(self);
        return NULL;
    }

    g_debug("Created subscription %s on %s",
            nostr_subscription_get_id_const(self->subscription),
            gnostr_relay_get_url(relay));

    return self;
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
