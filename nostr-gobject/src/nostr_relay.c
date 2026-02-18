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

/* NIP-11 relay information document */
#ifdef ENABLE_NIP11
#include "nip11.h"
#endif

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

/* nostrc-kw9r: Shared relay registry — deduplicates WebSocket connections.
 * Multiple GNostrPool instances that connect to the same relay URL share a
 * single GNostrRelay (and thus a single NostrRelay / NostrConnection).
 * The registry stores STRONG references to keep relays alive across pool
 * removals, ensuring websocket connections are reused for subsequent subscriptions. */
G_LOCK_DEFINE_STATIC(relay_registry);
static GHashTable *g_relay_registry = NULL; /* URL → GNostrRelay* (strong ref via destroy notify) */

struct _GNostrRelay {
    GObject parent_instance;
    NostrRelay *relay;           /* Core libnostr relay */
    gchar *url;                  /* Cached URL (construct-only) */
    GNostrRelayState state;       /* Current connection state (GObject enum) */
#ifdef ENABLE_NIP11
    RelayInformationDocument *nip11_info;  /* Cached NIP-11 info (owned) */
    GCancellable *nip11_cancellable;       /* Cancel in-flight NIP-11 fetch */
#endif
    /* NIP-42 authentication (nostrc-7og) */
    GNostrRelayAuthSignFunc auth_sign_func;
    gpointer auth_sign_data;
    GDestroyNotify auth_sign_destroy;
    gboolean authenticated;      /* TRUE after successful AUTH response */
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

    /* Reset auth state on disconnect (nostrc-7og) */
    if (new_state == GNOSTR_RELAY_STATE_DISCONNECTED ||
        new_state == GNOSTR_RELAY_STATE_ERROR) {
        self->authenticated = FALSE;
    }

#ifdef ENABLE_NIP11
    /* Auto-fetch NIP-11 info when we become connected */
    if (new_state == GNOSTR_RELAY_STATE_CONNECTED && self->nip11_info == NULL) {
        gnostr_relay_fetch_nip11_async(self);
    }
#endif

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

/* ---- NIP-42 AUTH challenge callback (worker thread → main thread) ---- */

typedef struct {
    GNostrRelay *self;
    gchar *challenge;
} AuthChallengeData;

static gboolean
auth_challenge_on_main_thread(gpointer user_data)
{
    AuthChallengeData *data = user_data;
    GNostrRelay *self = data->self;

    /* Emit auth-challenge signal */
    g_signal_emit(self, gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_AUTH_CHALLENGE], 0,
                  data->challenge);

    /* Auto-authenticate if handler is configured */
    if (self->auth_sign_func) {
        GError *error = NULL;
        if (!gnostr_relay_authenticate(self, &error)) {
            g_warning("NIP-42 auto-auth failed for %s: %s",
                      self->url, error ? error->message : "unknown");
            if (error) g_error_free(error);
        }
    }

    g_object_unref(data->self);
    g_free(data->challenge);
    g_free(data);
    return G_SOURCE_REMOVE;
}

/* Core relay auth callback (called from worker thread) */
static void
on_core_auth_challenge(NostrRelay *relay G_GNUC_UNUSED,
                       const char *challenge,
                       void *user_data)
{
    GNostrRelay *self = GNOSTR_RELAY(user_data);

    AuthChallengeData *data = g_new(AuthChallengeData, 1);
    data->self = g_object_ref(self);
    data->challenge = g_strdup(challenge);

    g_idle_add_full(G_PRIORITY_DEFAULT, auth_challenge_on_main_thread, data, NULL);
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

            /* Set up auth callback for NIP-42 challenges (nostrc-7og) */
            nostr_relay_set_auth_callback(self->relay, on_core_auth_challenge, self);
        }
    }
}

/* nostrc-ws3: Background thread func for nostr_relay_free.
 * relay_free_impl calls go_wait_group_wait which blocks until
 * worker goroutines exit.  Must not run on the main thread. */
static void
relay_free_thread_func(GTask *task, gpointer source G_GNUC_UNUSED,
                       gpointer task_data, GCancellable *cancel G_GNUC_UNUSED)
{
    NostrRelay *relay = (NostrRelay *)task_data;
    nostr_relay_free(relay);
    g_task_return_boolean(task, TRUE);
}

static void
gnostr_relay_finalize(GObject *object)
{
    GNostrRelay *self = GNOSTR_RELAY(object);

    /* nostrc-kw9r: Remove from registry when finalized. With weak references,
     * this happens when all pools release the relay. Clean removal prevents
     * stale pointers in the registry. */
    if (self->url) {
        G_LOCK(relay_registry);
        if (g_relay_registry) {
            GNostrRelay *registered = g_hash_table_lookup(g_relay_registry, self->url);
            if (registered == self) {
                g_hash_table_remove(g_relay_registry, self->url);
            }
        }
        G_UNLOCK(relay_registry);
    }

#ifdef ENABLE_NIP11
    /* Cancel any in-flight NIP-11 fetch */
    if (self->nip11_cancellable) {
        g_cancellable_cancel(self->nip11_cancellable);
        g_clear_object(&self->nip11_cancellable);
    }

    /* Free cached NIP-11 info */
    if (self->nip11_info) {
        nostr_nip11_free_info(self->nip11_info);
        self->nip11_info = NULL;
    }
#endif

    /* Remove callbacks before freeing relay */
    if (self->relay) {
        nostr_relay_set_state_callback(self->relay, NULL, NULL);
        nostr_relay_set_auth_callback(self->relay, NULL, NULL);

        /* nostrc-ws3: Dispatch nostr_relay_free to a background thread.
         * relay_free_impl blocks in go_wait_group_wait() waiting for worker
         * goroutines to exit.  If finalize runs on the GTK main thread
         * (e.g., sync_relays → remove_relay → last unref → finalize),
         * this blocks the main loop and freezes the app. */
        NostrRelay *relay = self->relay;
        self->relay = NULL;

        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(task, relay, NULL);
        g_task_run_in_thread(task, relay_free_thread_func);
        g_object_unref(task);
    }

    /* Free auth handler user data */
    if (self->auth_sign_destroy && self->auth_sign_data) {
        self->auth_sign_destroy(self->auth_sign_data);
    }
    self->auth_sign_func = NULL;
    self->auth_sign_data = NULL;
    self->auth_sign_destroy = NULL;

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

#ifdef ENABLE_NIP11
    /**
     * GNostrRelay::nip11-info-fetched:
     * @self: the relay
     *
     * Emitted when NIP-11 relay information has been fetched and cached.
     * Use gnostr_relay_get_nip11_info() to access the information.
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_NIP11_INFO] =
        g_signal_new("nip11-info-fetched",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);
#endif

    /**
     * GNostrRelay::auth-challenge:
     * @self: the relay
     * @challenge: the NIP-42 challenge string from the relay
     *
     * Emitted when a NIP-42 AUTH challenge is received from the relay.
     * If an auth handler has been set via gnostr_relay_set_auth_handler(),
     * automatic authentication will be attempted after this signal is emitted.
     */
    gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_AUTH_CHALLENGE] =
        g_signal_new("auth-challenge",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

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
#ifdef ENABLE_NIP11
    self->nip11_info = NULL;
    self->nip11_cancellable = NULL;
#endif
    self->auth_sign_func = NULL;
    self->auth_sign_data = NULL;
    self->auth_sign_destroy = NULL;
    self->authenticated = FALSE;
}

/* Public API */

GNostrRelay *
gnostr_relay_new(const gchar *url)
{
    g_return_val_if_fail(url != NULL, NULL);

    /* harden-5: Hold lock across both lookup and creation to prevent TOCTOU race.
     * constructed() only allocates the core relay struct (no I/O), so this is safe. */
    G_LOCK(relay_registry);

    /* Check registry for existing relay */
    if (g_relay_registry) {
        GNostrRelay *existing = g_hash_table_lookup(g_relay_registry, url);
        if (existing) {
            g_object_ref(existing);
            G_UNLOCK(relay_registry);
            return existing;
        }
    }

    /* Create new relay while holding lock to prevent duplicate creation */
    GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", url, NULL);

    if (!g_relay_registry) {
        g_relay_registry = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    }
    g_hash_table_insert(g_relay_registry, g_strdup(url), relay);
    G_UNLOCK(relay_registry);

    return relay;
}

gboolean
gnostr_relay_connect(GNostrRelay *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    g_return_val_if_fail(self->relay != NULL, FALSE);

    /* nostrc-kw9r: Shared relay may already be connected by another pool.
     * Use atomic load since this may be called from worker threads. */
    if (__atomic_load_n(&self->state, __ATOMIC_SEQ_CST) == GNOSTR_RELAY_STATE_CONNECTED) {
        return TRUE;
    }

    /* nostrc-blk2: Do NOT call gnostr_relay_set_state_internal() here.
     * This function is called from worker threads (via connect_async_thread).
     * gnostr_relay_set_state_internal() emits GObject signals (g_signal_emit,
     * g_object_notify_by_pspec) which are NOT thread-safe — they freeze the
     * app when signal handlers try to update GTK widgets from a worker thread.
     *
     * The core relay's state callback (on_core_state_changed, line 153)
     * already dispatches state changes to the main thread via g_idle_add
     * AND stores state atomically for immediate thread-safe reads. */

    Error *err = NULL;
    if (nostr_relay_connect(self->relay, &err)) {
        return TRUE;
    } else {
        GError *g_err = g_error_new(NOSTR_ERROR,
                                    NOSTR_ERROR_CONNECTION_FAILED,
                                    "%s",
                                    err && err->message ? err->message : "connect failed");
        if (err) free_error(err);

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
        /* nostrc-blk2: Do NOT call gnostr_relay_set_state_internal from worker
         * thread — it emits GObject signals that freeze GTK. State hasn't
         * changed since we haven't called connect yet. */
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

    /* nostrc-kw9r: Shared relay may already be connected — complete immediately
     * instead of spawning a redundant worker thread. */
    if (self->state == GNOSTR_RELAY_STATE_CONNECTED) {
        g_task_return_boolean(task, TRUE);
        g_object_unref(task);
        return;
    }

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

#ifdef ENABLE_NIP11
    /* nostrc-23: Enforce NIP-11 relay limitations before publishing */
    if (self->nip11_info && self->nip11_info->limitation) {
        RelayLimitationDocument *lim = self->nip11_info->limitation;

        if (lim->auth_required && !self->authenticated) {
            /* Try auto-auth if handler is configured (nostrc-7og) */
            if (self->auth_sign_func) {
                GError *auth_err = NULL;
                if (!gnostr_relay_authenticate(self, &auth_err)) {
                    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                                "relay %s requires auth and auto-auth failed: %s",
                                self->url, auth_err ? auth_err->message : "unknown");
                    if (auth_err) g_error_free(auth_err);
                    return FALSE;
                }
                /* Auth succeeded, fall through to publish */
            } else {
                g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                            "relay %s requires NIP-42 authentication", self->url);
                return FALSE;
            }
        }

        if (lim->payment_required) {
            g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PAYMENT_REQUIRED,
                        "relay %s requires payment", self->url);
            return FALSE;
        }

        if (lim->max_message_length > 0) {
            char *json = nostr_event_serialize_compact(event);
            if (json) {
                size_t len = strlen(json);
                free(json);
                if ((int)len > lim->max_message_length) {
                    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_MESSAGE_TOO_LARGE,
                                "event size %zu exceeds relay %s limit of %d bytes",
                                len, self->url, lim->max_message_length);
                    return FALSE;
                }
            }
        }
    }
#endif

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

/* ---- NIP-42 Authentication API (nostrc-7og) ---- */

/**
 * Sign callback adapter: bridges GNostrRelayAuthSignFunc to core nostr_relay_auth's
 * void (*sign)(NostrEvent *, Error **) signature.
 */
typedef struct {
    GNostrRelayAuthSignFunc func;
    gpointer user_data;
    GError *g_error;      /* Captures GError from sign callback */
} AuthSignAdapter;

/* Thread-local adapter for bridging sign callback */
static __thread AuthSignAdapter *_auth_sign_adapter;

static void
auth_sign_bridge(NostrEvent *event, Error **err)
{
    AuthSignAdapter *adapter = _auth_sign_adapter;
    if (!adapter || !adapter->func) {
        if (err) *err = new_error(1, "no auth sign function configured");
        return;
    }

    GError *g_err = NULL;
    adapter->func(event, &g_err, adapter->user_data);
    if (g_err) {
        if (err) *err = new_error(1, "%s", g_err->message);
        adapter->g_error = g_err;
    }
}

void
gnostr_relay_set_auth_handler(GNostrRelay *self,
                               GNostrRelayAuthSignFunc sign_func,
                               gpointer user_data,
                               GDestroyNotify destroy)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));

    /* Free old handler data */
    if (self->auth_sign_destroy && self->auth_sign_data) {
        self->auth_sign_destroy(self->auth_sign_data);
    }

    self->auth_sign_func = sign_func;
    self->auth_sign_data = user_data;
    self->auth_sign_destroy = destroy;
    self->authenticated = FALSE;
}

gboolean
gnostr_relay_authenticate(GNostrRelay *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);

    if (!self->relay) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "no core relay");
        return FALSE;
    }

    if (!self->auth_sign_func) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                            "no auth handler configured; call gnostr_relay_set_auth_handler() first");
        return FALSE;
    }

    if (self->state != GNOSTR_RELAY_STATE_CONNECTED) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "not connected");
        return FALSE;
    }

    /* Set up thread-local bridge adapter */
    AuthSignAdapter adapter = {
        .func = self->auth_sign_func,
        .user_data = self->auth_sign_data,
        .g_error = NULL
    };
    _auth_sign_adapter = &adapter;

    Error *core_err = NULL;
    nostr_relay_auth(self->relay, auth_sign_bridge, &core_err);

    _auth_sign_adapter = NULL;

    if (adapter.g_error) {
        if (error) {
            *error = adapter.g_error;
        } else {
            g_error_free(adapter.g_error);
        }
        if (core_err) free_error(core_err);
        return FALSE;
    }

    if (core_err) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                    "%s", core_err->message ? core_err->message : "auth failed");
        free_error(core_err);
        return FALSE;
    }

    self->authenticated = TRUE;
    return TRUE;
}

gboolean
gnostr_relay_get_authenticated(GNostrRelay *self)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    return self->authenticated;
}

#ifdef ENABLE_NIP11
/* ---- NIP-11 Relay Information ---- */

/* Data for delivering NIP-11 result on main thread */
typedef struct {
    GNostrRelay *self;
    RelayInformationDocument *info;
} Nip11ResultData;

static gboolean
nip11_deliver_on_main_thread(gpointer user_data)
{
    Nip11ResultData *data = user_data;
    GNostrRelay *self = data->self;

    /* Store info if we don't already have one (first fetch wins) */
    if (self->nip11_info == NULL && data->info != NULL) {
        self->nip11_info = data->info;
        data->info = NULL;  /* ownership transferred */
        g_signal_emit(self, gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_NIP11_INFO], 0);
    }

    /* Clean up */
    if (data->info)
        nostr_nip11_free_info(data->info);
    g_object_unref(data->self);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void
nip11_fetch_thread(GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data G_GNUC_UNUSED,
                   GCancellable *cancellable)
{
    GNostrRelay *self = GNOSTR_RELAY(source_object);

    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    /* Convert wss:// URL to https:// for NIP-11 HTTP fetch */
    g_autofree gchar *http_url = NULL;
    if (g_str_has_prefix(self->url, "wss://")) {
        http_url = g_strconcat("https://", self->url + 6, NULL);
    } else if (g_str_has_prefix(self->url, "ws://")) {
        http_url = g_strconcat("http://", self->url + 5, NULL);
    } else {
        http_url = g_strdup(self->url);
    }

    RelayInformationDocument *info = nostr_nip11_fetch_info(http_url);

    if (g_cancellable_is_cancelled(cancellable)) {
        if (info) nostr_nip11_free_info(info);
        g_task_return_boolean(task, FALSE);
        return;
    }

    if (info) {
        /* Deliver result on main thread */
        Nip11ResultData *data = g_new0(Nip11ResultData, 1);
        data->self = g_object_ref(self);
        data->info = info;
        g_idle_add(nip11_deliver_on_main_thread, data);
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_boolean(task, FALSE);
        g_debug("NIP-11 fetch failed for %s", self->url);
    }
}

void
gnostr_relay_fetch_nip11_async(GNostrRelay *self)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));
    g_return_if_fail(self->url != NULL);

    /* Cancel any previous in-flight fetch */
    if (self->nip11_cancellable) {
        g_cancellable_cancel(self->nip11_cancellable);
        g_clear_object(&self->nip11_cancellable);
    }

    self->nip11_cancellable = g_cancellable_new();

    GTask *task = g_task_new(self, self->nip11_cancellable, NULL, NULL);
    g_task_set_source_tag(task, gnostr_relay_fetch_nip11_async);
    g_task_run_in_thread(task, nip11_fetch_thread);
    g_object_unref(task);
}

const GNostrRelayNip11Info *
gnostr_relay_get_nip11_info(GNostrRelay *self)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), NULL);
    return (const GNostrRelayNip11Info *)self->nip11_info;
}

gboolean
gnostr_relay_supports_nip(GNostrRelay *self, gint nip)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);

    if (!self->nip11_info || !self->nip11_info->supported_nips)
        return FALSE;

    for (int i = 0; i < self->nip11_info->supported_nips_count; i++) {
        if (self->nip11_info->supported_nips[i] == nip)
            return TRUE;
    }
    return FALSE;
}
#endif /* ENABLE_NIP11 */
