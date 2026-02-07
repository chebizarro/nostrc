/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrRelay: GObject wrapper for Nostr relay connections (NIP-01)
 *
 * Provides a modern GObject implementation with:
 * - Properties with notify signals (url, state, connected)
 * - Full signal support (state-changed, event-received, notice, ok, eose, closed, error)
 * - Async connect with GCancellable support
 * - GError-based error handling
 */

/* Include core libnostr headers FIRST to get the canonical type definitions */
#include "nostr-relay.h"  /* NostrRelay, NostrRelayConnectionState, and API */
#include "context.h"      /* GoContext */
#include "error.h"        /* Error, free_error */
#include "nostr-event.h"  /* NostrEvent */
#include "nostr-filter.h" /* NostrFilter */

/* Now include GObject wrapper header */
#include "nostr_relay.h"
#include <glib.h>
#include <gio/gio.h>

/* Property IDs */
enum {
    PROP_0,
    PROP_URL,
    PROP_STATE,
    PROP_CONNECTED,
    N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };
static guint gnostr_relay_signals[GNOSTR_RELAY_SIGNALS_COUNT] = { 0 };

/* Legacy signal array for backward compatibility */
static guint nostr_relay_signals[NOSTR_RELAY_SIGNALS_COUNT] = { 0 };

struct _GNostrRelay {
    GObject parent_instance;
    NostrRelay *relay;           /* Core libnostr relay */
    gchar *url;                  /* Cached URL (construct-only) */
    GNostrRelayState state;       /* Current connection state (GObject enum) */
};

G_DEFINE_TYPE(GNostrRelay, gnostr_relay, G_TYPE_OBJECT)

/* Helper to convert core state (NostrRelayConnectionState) to GObject enum (GNostrRelayState)
 * Core libnostr uses: NOSTR_RELAY_STATE_{DISCONNECTED=0, CONNECTING=1, CONNECTED=2, BACKOFF=3}
 * GObject wrapper uses: GNOSTR_RELAY_STATE_{DISCONNECTED=0, CONNECTING=1, CONNECTED=2, ERROR=3}
 */
static GNostrRelayState
core_state_to_gobject(NostrRelayConnectionState core_state)
{
    switch (core_state) {
    case NOSTR_RELAY_STATE_CONNECTED:
        return GNOSTR_RELAY_STATE_CONNECTED;
    case NOSTR_RELAY_STATE_CONNECTING:
        return GNOSTR_RELAY_STATE_CONNECTING;
    case NOSTR_RELAY_STATE_DISCONNECTED:
        return GNOSTR_RELAY_STATE_DISCONNECTED;
    case NOSTR_RELAY_STATE_BACKOFF:
        return GNOSTR_RELAY_STATE_ERROR;
    default:
        return GNOSTR_RELAY_STATE_DISCONNECTED;
    }
}

/* Internal state change helper */
static void
gnostr_relay_set_state_internal(GNostrRelay *self, GNostrRelayState new_state)
{
    if (self->state == new_state)
        return;

    GNostrRelayState old_state = self->state;
    gboolean was_connected = (old_state == GNOSTR_RELAY_STATE_CONNECTED);
    gboolean is_connected = (new_state == GNOSTR_RELAY_STATE_CONNECTED);

    self->state = new_state;

    /* Emit state-changed signal */
    g_signal_emit(self, gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_STATE_CHANGED], 0,
                  old_state, new_state);

    /* Notify property changes */
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_STATE]);

    if (was_connected != is_connected) {
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_CONNECTED]);

        /* Emit legacy signals for backward compatibility */
        if (is_connected) {
            g_signal_emit(self, nostr_relay_signals[SIGNAL_CONNECTED], 0);
        } else {
            g_signal_emit(self, nostr_relay_signals[SIGNAL_DISCONNECTED], 0);
        }
    }
}

/* Data for idle callback */
typedef struct {
    GNostrRelay *self;
    GNostrRelayState new_state;
} StateChangeData;

static gboolean
set_state_on_main_thread(gpointer user_data)
{
    StateChangeData *data = user_data;
    gnostr_relay_set_state_internal(data->self, data->new_state);
    g_object_unref(data->self);
    g_free(data);
    return G_SOURCE_REMOVE;
}

/* Core relay state callback (called from worker thread) */
static void
on_core_state_changed(NostrRelay *relay G_GNUC_UNUSED,
                      NostrRelayConnectionState old_state G_GNUC_UNUSED,
                      NostrRelayConnectionState new_state,
                      void *user_data)
{
    GNostrRelay *self = GNOSTR_RELAY(user_data);
    GNostrRelayState g_new_state = core_state_to_gobject(new_state);

    /* Schedule state update on main thread */
    StateChangeData *data = g_new(StateChangeData, 1);
    data->self = g_object_ref(self);
    data->new_state = g_new_state;

    g_idle_add_full(G_PRIORITY_DEFAULT, set_state_on_main_thread, data, NULL);

    /* Store state directly for immediate access (thread-safe) */
    __atomic_store_n(&self->state, g_new_state, __ATOMIC_SEQ_CST);
}

static void
gnostr_relay_set_property(GObject      *object,
                          guint         property_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    GNostrRelay *self = GNOSTR_RELAY(object);

    switch (property_id) {
    case PROP_URL:
        /* Construct-only: set once during construction */
        g_free(self->url);
        self->url = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_relay_get_property(GObject    *object,
                          guint       property_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    GNostrRelay *self = GNOSTR_RELAY(object);

    switch (property_id) {
    case PROP_URL:
        g_value_set_string(value, self->url);
        break;
    case PROP_STATE:
        g_value_set_enum(value, self->state);
        break;
    case PROP_CONNECTED:
        g_value_set_boolean(value, self->state == GNOSTR_RELAY_STATE_CONNECTED);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_relay_constructed(GObject *object)
{
    GNostrRelay *self = GNOSTR_RELAY(object);

    G_OBJECT_CLASS(gnostr_relay_parent_class)->constructed(object);

    /* Create the core relay with the URL set during construction */
    if (self->url) {
        Error *err = NULL;
        self->relay = nostr_relay_new(NULL /* default ctx */, self->url, &err);
        if (err) {
            g_warning("nostr_relay_new: %s", err->message ? err->message : "unknown error");
            free_error(err);
        }

        /* Configure the relay */
        if (self->relay) {
            /* Skip signature verification - nostrdb handles this during ingestion */
            self->relay->assume_valid = true;

            /* Set up state callback to receive connection state changes */
            nostr_relay_set_state_callback(self->relay, on_core_state_changed, self);
        }
    }
}

static void
gnostr_relay_finalize(GObject *object)
{
    GNostrRelay *self = GNOSTR_RELAY(object);

    /* Remove callback before freeing relay */
    if (self->relay) {
        nostr_relay_set_state_callback(self->relay, NULL, NULL);
        nostr_relay_free(self->relay);
        self->relay = NULL;
    }

    g_free(self->url);
    self->url = NULL;

    G_OBJECT_CLASS(gnostr_relay_parent_class)->finalize(object);
}

static void
gnostr_relay_class_init(GNostrRelayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = gnostr_relay_set_property;
    object_class->get_property = gnostr_relay_get_property;
    object_class->constructed = gnostr_relay_constructed;
    object_class->finalize = gnostr_relay_finalize;

    /**
     * GNostrRelay:url:
     *
     * The relay URL (e.g., "wss://relay.damus.io"). Construct-only.
     */
    obj_properties[PROP_URL] =
        g_param_spec_string("url",
                            "URL",
                            "Relay URL (construct-only)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                            G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrRelay:state:
     *
     * The current connection state.
     */
    obj_properties[PROP_STATE] =
        g_param_spec_enum("state",
                          "State",
                          "Current connection state",
                          GNOSTR_TYPE_RELAY_STATE,
                          GNOSTR_RELAY_STATE_DISCONNECTED,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrRelay:connected:
     *
     * Whether the relay is currently connected. Derived from state.
     */
    obj_properties[PROP_CONNECTED] =
        g_param_spec_boolean("connected",
                             "Connected",
                             "Whether connected (read-only, derived from state)",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);

    /* New signals with full NIP-01 support */

    /**
     * GNostrRelay::state-changed:
     * @self: the relay
     * @old_state: the previous #GNostrRelayState
     * @new_state: the new #GNostrRelayState
     *
     * Emitted when the connection state changes.
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2,
                     GNOSTR_TYPE_RELAY_STATE,
                     GNOSTR_TYPE_RELAY_STATE);

    /**
     * GNostrRelay::event-received:
     * @self: the relay
     * @event_json: the event as a JSON string
     *
     * Emitted when an EVENT message is received from the relay.
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_EVENT_RECEIVED] =
        g_signal_new("event-received",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrRelay::notice:
     * @self: the relay
     * @message: the notice message
     *
     * Emitted when a NOTICE message is received from the relay.
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_NOTICE] =
        g_signal_new("notice",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrRelay::ok:
     * @self: the relay
     * @event_id: the event ID
     * @accepted: whether the event was accepted
     * @message: optional message from relay
     *
     * Emitted when an OK message is received (response to EVENT publish).
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_OK] =
        g_signal_new("ok",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 3,
                     G_TYPE_STRING,
                     G_TYPE_BOOLEAN,
                     G_TYPE_STRING);

    /**
     * GNostrRelay::eose:
     * @self: the relay
     * @subscription_id: the subscription ID
     *
     * Emitted when an EOSE (End of Stored Events) message is received.
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_EOSE] =
        g_signal_new("eose",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * GNostrRelay::closed:
     * @self: the relay
     * @subscription_id: the subscription ID
     * @message: the close reason
     *
     * Emitted when a CLOSED message is received (subscription terminated by relay).
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_CLOSED] =
        g_signal_new("closed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2,
                     G_TYPE_STRING,
                     G_TYPE_STRING);

    /**
     * GNostrRelay::error:
     * @self: the relay
     * @error: a #GError describing the error
     *
     * Emitted when an error occurs.
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_ERROR] =
        g_signal_new("error",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_ERROR);

    /* Legacy signals for backward compatibility */
    nostr_relay_signals[SIGNAL_CONNECTED] =
        g_signal_new("connected",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    nostr_relay_signals[SIGNAL_DISCONNECTED] =
        g_signal_new("disconnected",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    /* Note: SIGNAL_EVENT_RECEIVED and SIGNAL_ERROR reuse the new signal indices */
    nostr_relay_signals[SIGNAL_EVENT_RECEIVED] =
        gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_EVENT_RECEIVED];
    nostr_relay_signals[SIGNAL_ERROR] =
        gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_ERROR];
}

static void
gnostr_relay_init(GNostrRelay *self)
{
    self->relay = NULL;
    self->url = NULL;
    self->state = GNOSTR_RELAY_STATE_DISCONNECTED;
}

/* Public API */

GNostrRelay *
gnostr_relay_new(const gchar *url)
{
    return g_object_new(GNOSTR_TYPE_RELAY,
                        "url", url,
                        NULL);
}

gboolean
gnostr_relay_connect(GNostrRelay *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    g_return_val_if_fail(self->relay != NULL, FALSE);

    /* Update state to connecting */
    gnostr_relay_set_state_internal(self, GNOSTR_RELAY_STATE_CONNECTING);

    Error *err = NULL;
    if (nostr_relay_connect(self->relay, &err)) {
        gnostr_relay_set_state_internal(self, GNOSTR_RELAY_STATE_CONNECTED);
        return TRUE;
    } else {
        gnostr_relay_set_state_internal(self, GNOSTR_RELAY_STATE_ERROR);

        GError *g_err = g_error_new(NOSTR_ERROR,
                                    NOSTR_ERROR_CONNECTION_FAILED,
                                    "%s",
                                    err && err->message ? err->message : "connect failed");
        if (err) free_error(err);

        /* Emit error signal */
        g_signal_emit(self, gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_ERROR], 0, g_err);

        if (error) {
            *error = g_err;
        } else {
            g_error_free(g_err);
        }
        return FALSE;
    }
}

void
gnostr_relay_disconnect(GNostrRelay *self)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));

    if (self->relay) {
        nostr_relay_disconnect(self->relay);
    }

    gnostr_relay_set_state_internal(self, GNOSTR_RELAY_STATE_DISCONNECTED);
}

/* Async connect implementation */

typedef struct {
    GNostrRelay *self;
} ConnectAsyncData;

static void
connect_async_data_free(ConnectAsyncData *data)
{
    if (data) {
        g_object_unref(data->self);
        g_free(data);
    }
}

static void
connect_async_thread(GTask        *task,
                     gpointer      source_object,
                     gpointer      task_data G_GNUC_UNUSED,
                     GCancellable *cancellable)
{
    GNostrRelay *self = GNOSTR_RELAY(source_object);
    GError *error = NULL;

    /* Check for cancellation */
    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        gnostr_relay_set_state_internal(self, GNOSTR_RELAY_STATE_DISCONNECTED);
        g_task_return_error(task, error);
        return;
    }

    if (gnostr_relay_connect(self, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
}

void
gnostr_relay_connect_async(GNostrRelay         *self,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_relay_connect_async);

    ConnectAsyncData *data = g_new0(ConnectAsyncData, 1);
    data->self = g_object_ref(self);
    g_task_set_task_data(task, data, (GDestroyNotify)connect_async_data_free);

    g_task_run_in_thread(task, connect_async_thread);
    g_object_unref(task);
}

gboolean
gnostr_relay_connect_finish(GNostrRelay   *self,
                            GAsyncResult  *result,
                            GError       **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean
gnostr_relay_publish(GNostrRelay *self, NostrEvent *event, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);

    if (!self->relay || !event) {
        if (error) {
            g_set_error_literal(error,
                                NOSTR_ERROR,
                                NOSTR_ERROR_INVALID_EVENT,
                                "invalid arguments");
        }
        return FALSE;
    }

    if (self->state != GNOSTR_RELAY_STATE_CONNECTED) {
        if (error) {
            g_set_error_literal(error,
                                NOSTR_ERROR,
                                NOSTR_ERROR_CONNECTION_FAILED,
                                "not connected");
        }
        return FALSE;
    }

    nostr_relay_publish(self->relay, event);
    return TRUE;
}

GPtrArray *
gnostr_relay_query_sync(GNostrRelay *self, NostrFilter *filter G_GNUC_UNUSED, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), NULL);

    /* Deprecated path: not supported by modern API. Recommend async subscription. */
    GError *err = g_error_new_literal(NOSTR_ERROR,
                                      NOSTR_ERROR_INVALID_FILTER,
                                      "query_sync is deprecated; use subscriptions");
    g_signal_emit(self, gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_ERROR], 0, err);

    if (error) {
        *error = err;
    } else {
        g_error_free(err);
    }
    return NULL;
}

/* Property accessors */

const gchar *
gnostr_relay_get_url(GNostrRelay *self)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), NULL);
    return self->url;
}

GNostrRelayState
gnostr_relay_get_state(GNostrRelay *self)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), GNOSTR_RELAY_STATE_DISCONNECTED);
    return self->state;
}

gboolean
gnostr_relay_get_connected(GNostrRelay *self)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    return self->state == GNOSTR_RELAY_STATE_CONNECTED;
}

NostrRelay *
gnostr_relay_get_core_relay(GNostrRelay *self)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), NULL);
    return self->relay;
}
