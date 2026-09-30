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

#ifdef GNOSTR_TESTING
#include "nostr_relay_test_hooks.h"
#endif

/* Property IDs */
enum {
    PROP_0,
    PROP_URL,
    PROP_STATE,
    PROP_CONNECTED,
    N_PROPERTIES
};

/* nostrc-oz77: bound on waiting for a relay's WebSocket handshake in
 * gnostr_relay_connect(); matches gnostr's OK bound for publishes. */
#define GNOSTR_RELAY_HANDSHAKE_TIMEOUT_MS 15000u

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };
static guint gnostr_relay_signals[GNOSTR_RELAY_SIGNALS_COUNT] = { 0 };

/* Legacy signal array for backward compatibility */
static guint nostr_relay_signals[NOSTR_RELAY_SIGNALS_COUNT] = { 0 };

/* nostrc-kw9r: Shared relay registry — deduplicates WebSocket connections.
 * Multiple GNostrPool instances that connect to the same relay URL share a
 * single GNostrRelay (and thus a single NostrRelay / NostrConnection).
 *
 * The registry does not keep relays alive: each entry is a weak reference
 * (nostrc-flp7). It used to hold the bare pointer, and a lookup on another
 * thread in the window between the last unref and finalize's removal
 * g_object_ref()ed a relay being finalized and returned it. g_weak_ref_get()
 * returns NULL for such a relay; the lookup then registers a new one, and
 * `owner` (compared, never dereferenced) keeps the dying relay's finalize
 * from removing its successor's entry. */
G_LOCK_DEFINE_STATIC(relay_registry);
static GHashTable *g_relay_registry = NULL; /* URL → RelayRegistryEntry* (owned) */

typedef struct {
    GWeakRef relay;
    gconstpointer owner;
} RelayRegistryEntry;

static void
relay_registry_entry_free(gpointer p)
{
    RelayRegistryEntry *entry = p;
    g_weak_ref_clear(&entry->relay);
    g_free(entry);
}

#ifdef GNOSTR_TESTING
/* Test seams, see nostr_relay_test_hooks.h. */
G_LOCK_DEFINE_STATIC(relay_test_hook);
static GNostrRelayTestHook relay_test_hook;
static gpointer relay_test_hook_data;
static gint relay_test_live_callback_data;

void
gnostr_relay_test_set_hook(GNostrRelayTestHook hook, gpointer hook_data)
{
    G_LOCK(relay_test_hook);
    relay_test_hook = hook;
    relay_test_hook_data = hook_data;
    G_UNLOCK(relay_test_hook);
}

gint
gnostr_relay_test_live_callback_data(void)
{
    return g_atomic_int_get(&relay_test_live_callback_data);
}

static void
relay_test_point(GNostrRelayTestPoint point)
{
    G_LOCK(relay_test_hook);
    GNostrRelayTestHook hook = relay_test_hook;
    gpointer hook_data = relay_test_hook_data;
    G_UNLOCK(relay_test_hook);
    if (hook)
        hook(point, hook_data);
}
#define RELAY_TEST_POINT(point) relay_test_point(GNOSTR_RELAY_TEST_POINT_##point)
#else
#define RELAY_TEST_POINT(point) ((void)0)
#endif

/* Ref-counted weak-ref container passed as user_data to worker-thread
 * callbacks (state-changed, auth-challenge, ok).  Because the callback can
 * fire AFTER the GNostrRelay has been finalized, we cannot pass `self`
 * directly — g_object_ref() on freed memory is UB.  Instead we pass
 * this tiny struct whose lifetime is managed by g_atomic_ref_count.
 *
 * nostrc-flp7: each core callback registration owns a reference, released by
 * libnostr (the _full setters' destroy notify) only once the callback is
 * removed and no worker is still inside it; each queued idle owns another. */
typedef struct {
    GWeakRef weak_relay;
    gatomicrefcount ref_count;
} RelayCallbackData;

static RelayCallbackData *
relay_callback_data_new(GNostrRelay *self)
{
    RelayCallbackData *d = g_new(RelayCallbackData, 1);
    g_weak_ref_init(&d->weak_relay, self);
    g_atomic_ref_count_init(&d->ref_count);
#ifdef GNOSTR_TESTING
    g_atomic_int_inc(&relay_test_live_callback_data);
#endif
    return d;
}

static RelayCallbackData *
relay_callback_data_ref(RelayCallbackData *d)
{
    g_atomic_ref_count_inc(&d->ref_count);
    return d;
}

static void
relay_callback_data_unref(RelayCallbackData *d)
{
    if (g_atomic_ref_count_dec(&d->ref_count)) {
        g_weak_ref_clear(&d->weak_relay);
        g_free(d);
#ifdef GNOSTR_TESTING
        g_atomic_int_add(&relay_test_live_callback_data, -1);
#endif
    }
}

/* NostrRelayDestroyNotify for a core callback registration's reference. */
static void
relay_callback_data_release(void *d)
{
    relay_callback_data_unref(d);
}

/* NIP-42 sign handler (nostrc-7og). Refcounted (nostrc-flp7):
 * gnostr_relay_authenticate() can run on a worker thread (a publish from
 * nostr_relay_publish_async()) and holds a reference while it signs, so a
 * gnostr_relay_set_auth_handler() on another thread cannot destroy the user
 * data that call is still using. The relay's pointer is read and replaced
 * only under the relay_auth_handler lock. */
typedef struct {
    gatomicrefcount ref_count;
    GNostrRelayAuthSignFunc func;
    gpointer user_data;
    GDestroyNotify destroy;
} RelayAuthHandler;

G_LOCK_DEFINE_STATIC(relay_auth_handler);

static void
relay_auth_handler_unref(RelayAuthHandler *handler)
{
    if (handler && g_atomic_ref_count_dec(&handler->ref_count)) {
        if (handler->destroy && handler->user_data)
            handler->destroy(handler->user_data);
        g_free(handler);
    }
}

struct _GNostrRelay {
    GObject parent_instance;
    NostrRelay *relay;           /* Core libnostr relay */
    gchar *url;                  /* Cached URL (construct-only) */
    /* nostrc-qp24.4.5: two views of the connection state. `state` is the
     * latest core state, stored atomically by the worker thread so readers
     * (get_state, publish, connect fast paths) see it immediately.
     * `emitted_state` is the last state announced by state-changed and is
     * only touched where signals are emitted; comparing against it (never
     * against `state`) is what lets every queued transition be emitted. */
    GNostrRelayState state;
    GNostrRelayState emitted_state;
    guint64 state_serial;        /* atomic: serial of the last queued core transition */
    guint64 state_barrier;       /* atomic: transitions queued at or before this
                                  * serial predate an explicit disconnect */
#ifdef ENABLE_NIP11
    RelayInformationDocument *nip11_info;  /* Cached NIP-11 info (owned) */
    GCancellable *nip11_cancellable;       /* Cancel in-flight NIP-11 fetch */
#endif
    /* NIP-42 authentication (nostrc-7og) */
    RelayAuthHandler *auth_handler; /* NULL when unset; see relay_auth_handler lock */
    gboolean authenticated;      /* TRUE after successful AUTH response */
};

/* Returns a reference on the current sign handler, or NULL. */
static RelayAuthHandler *
relay_dup_auth_handler(GNostrRelay *self)
{
    G_LOCK(relay_auth_handler);
    RelayAuthHandler *handler = self->auth_handler;
    if (handler)
        g_atomic_ref_count_inc(&handler->ref_count);
    G_UNLOCK(relay_auth_handler);
    return handler;
}

static gboolean
relay_has_auth_handler(GNostrRelay *self)
{
    G_LOCK(relay_auth_handler);
    gboolean has = self->auth_handler != NULL;
    G_UNLOCK(relay_auth_handler);
    return has;
}

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

/* nostrc-8mb8.1: Human-readable state name for structured logging */
static const char *
gnostr_relay_state_nick(GNostrRelayState state)
{
    switch (state) {
    case GNOSTR_RELAY_STATE_DISCONNECTED: return "disconnected";
    case GNOSTR_RELAY_STATE_CONNECTING:   return "connecting";
    case GNOSTR_RELAY_STATE_CONNECTED:    return "connected";
    case GNOSTR_RELAY_STATE_ERROR:        return "error";
    default:                              return "unknown";
    }
}

/* Announces a transition from the last emitted state. Runs where signals
 * may be emitted (the default main context for core-driven transitions). */
static void
gnostr_relay_emit_state(GNostrRelay *self, GNostrRelayState new_state)
{
    if (self->emitted_state == new_state)
        return;

    GNostrRelayState old_state = self->emitted_state;
    gboolean was_connected = (old_state == GNOSTR_RELAY_STATE_CONNECTED);
    gboolean is_connected = (new_state == GNOSTR_RELAY_STATE_CONNECTED);

    self->emitted_state = new_state;

    /* nostrc-8mb8.1: Structured relay state logging */
    g_debug("[RELAY] state=%s→%s url=%s",
            gnostr_relay_state_nick(old_state),
            gnostr_relay_state_nick(new_state),
            self->url ? self->url : "(null)");

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

/* One queued core transition. Idles of equal priority on one context are
 * dispatched in the order they were attached, so transitions are announced
 * in the order the core reported them. */
typedef struct {
    RelayCallbackData *cb_data;
    GNostrRelayState new_state;
    guint64 serial;
} StateChangeData;

static gboolean
set_state_on_main_thread(gpointer user_data)
{
    StateChangeData *data = user_data;
    GNostrRelay *self = g_weak_ref_get(&data->cb_data->weak_relay);
    if (self) {
        /* A transition queued before an explicit disconnect is stale: the
         * disconnect already announced DISCONNECTED and must not be followed
         * by, say, an old CONNECTED. */
        if (data->serial > __atomic_load_n(&self->state_barrier, __ATOMIC_SEQ_CST))
            gnostr_relay_emit_state(self, data->new_state);
        g_object_unref(self);
    }
    relay_callback_data_unref(data->cb_data);
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
    RELAY_TEST_POINT(CORE_STATE);
    RelayCallbackData *cb_data = user_data; /* kept alive by libnostr for this call */
    GNostrRelay *self = g_weak_ref_get(&cb_data->weak_relay);
    if (!self)
        return; /* relay already finalized */

    GNostrRelayState g_new_state = core_state_to_gobject(new_state);

    /* Store state directly for immediate access (thread-safe). The emission
     * compares against emitted_state, so this store no longer hides the
     * transition from the queued idle (nostrc-qp24.4.5). */
    __atomic_store_n(&self->state, g_new_state, __ATOMIC_SEQ_CST);

    /* Signals are emitted on the main context, never from this thread. */
    StateChangeData *data = g_new(StateChangeData, 1);
    data->cb_data = relay_callback_data_ref(cb_data);
    data->new_state = g_new_state;
    data->serial = __atomic_add_fetch(&self->state_serial, 1, __ATOMIC_SEQ_CST);
    g_object_unref(self);

    g_idle_add_full(G_PRIORITY_DEFAULT, set_state_on_main_thread, data, NULL);
}

/* ---- NIP-42 AUTH challenge callback (worker thread → main thread) ---- */

typedef struct {
    RelayCallbackData *cb_data;
    gchar *challenge;
} AuthChallengeData;

typedef struct {
    RelayCallbackData *cb_data;
    gchar *event_id;
    gboolean accepted;
    gchar *message;
} OkResponseData;

static gboolean
auth_challenge_on_main_thread(gpointer user_data)
{
    AuthChallengeData *data = user_data;
    GNostrRelay *self = g_weak_ref_get(&data->cb_data->weak_relay);
    if (self) {
        /* Emit auth-challenge signal */
        g_signal_emit(self, gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_AUTH_CHALLENGE], 0,
                      data->challenge);

        /* Auto-authenticate if handler is configured */
        if (relay_has_auth_handler(self)) {
            g_autoptr(GError) error = NULL;
            if (!gnostr_relay_authenticate(self, &error)) {
                g_warning("NIP-42 auto-auth failed for %s: %s",
                          self->url, error ? error->message : "unknown");
            }
        }
        g_object_unref(self);
    }

    relay_callback_data_unref(data->cb_data);
    g_free(data->challenge);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static gboolean
ok_response_on_main_thread(gpointer user_data)
{
    OkResponseData *data = user_data;
    GNostrRelay *self = g_weak_ref_get(&data->cb_data->weak_relay);
    if (self) {
        /* nostrc-8mb8.1: Structured publish OK/reject logging */
        if (data->accepted) {
            g_debug("[PUBLISH] OK relay=%s event=%.16s",
                    self->url ? self->url : "?",
                    data->event_id ? data->event_id : "?");
        } else {
            g_debug("[PUBLISH] REJECTED relay=%s event=%.16s reason=%s",
                    self->url ? self->url : "?",
                    data->event_id ? data->event_id : "?",
                    data->message ? data->message : "(none)");
        }
        g_signal_emit(self, gnostr_relay_signals[GNOSTR_RELAY_SIGNAL_OK], 0,
                      data->event_id, data->accepted, data->message);
        g_object_unref(self);
    }

    relay_callback_data_unref(data->cb_data);
    g_free(data->event_id);
    g_free(data->message);
    g_free(data);
    return G_SOURCE_REMOVE;
}

/* Core relay auth callback (called from worker thread) */
static void
on_core_auth_challenge(NostrRelay *relay G_GNUC_UNUSED,
                       const char *challenge,
                       void *user_data)
{
    RELAY_TEST_POINT(CORE_AUTH);
    RelayCallbackData *cb_data = user_data; /* kept alive by libnostr for this call */
    GNostrRelay *self = g_weak_ref_get(&cb_data->weak_relay);
    if (!self)
        return; /* relay already finalized */
    g_object_unref(self); /* just checking liveness; idle callback will re-acquire */

    AuthChallengeData *data = g_new(AuthChallengeData, 1);
    data->cb_data = relay_callback_data_ref(cb_data);
    data->challenge = g_strdup(challenge);

    g_idle_add_full(G_PRIORITY_DEFAULT, auth_challenge_on_main_thread, data, NULL);
}

/* Core relay OK callback (called from worker thread) */
static void
on_core_ok_response(const char *event_id, bool ok, const char *reason, void *user_data)
{
    RELAY_TEST_POINT(CORE_OK);
    RelayCallbackData *cb_data = user_data; /* kept alive by libnostr for this call */
    GNostrRelay *self = g_weak_ref_get(&cb_data->weak_relay);
    if (!self)
        return;
    g_object_unref(self);

    OkResponseData *data = g_new0(OkResponseData, 1);
    data->cb_data = relay_callback_data_ref(cb_data);
    data->event_id = g_strdup(event_id);
    data->accepted = ok ? TRUE : FALSE;
    data->message = g_strdup(reason);

    g_idle_add_full(G_PRIORITY_DEFAULT, ok_response_on_main_thread, data, NULL);
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

            /* Weak-ref wrapper for the worker-thread callbacks. Each
             * registration below owns a reference that libnostr releases
             * only after the callback is removed and no worker is inside it
             * any more (nostrc-flp7), so the relay keeps none of its own. */
            RelayCallbackData *cb_data = relay_callback_data_new(self);

            /* Connection state changes */
            nostr_relay_set_state_callback_full(self->relay, on_core_state_changed,
                                                relay_callback_data_ref(cb_data),
                                                relay_callback_data_release);

            /* NIP-42 challenges (nostrc-7og) */
            nostr_relay_set_auth_callback_full(self->relay, on_core_auth_challenge,
                                               relay_callback_data_ref(cb_data),
                                               relay_callback_data_release);

            /* OK publish responses, bridged onto the GObject signal surface */
            nostr_relay_set_ok_callback_full(self->relay, on_core_ok_response,
                                             relay_callback_data_ref(cb_data),
                                             relay_callback_data_release);

            relay_callback_data_unref(cb_data);
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

    RELAY_TEST_POINT(FINALIZE);

    /* nostrc-kw9r: Remove from registry when finalized, unless a lookup has
     * already replaced this dying relay's entry with a new relay. */
    if (self->url) {
        G_LOCK(relay_registry);
        if (g_relay_registry) {
            RelayRegistryEntry *entry = g_hash_table_lookup(g_relay_registry, self->url);
            if (entry && entry->owner == self) {
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

    if (self->relay) {
        /* nostrc-flp7: detach the core callbacks first. This does not wait
         * for a worker that is inside one of them right now: libnostr holds
         * that registration's reference on the callback data until the call
         * returns, so the worker reads live memory, and its g_weak_ref_get()
         * returns NULL from here on. The callback data used to be freed here,
         * before the callbacks were detached, and a worker already past
         * libnostr's lock then read it after the free (message_loop ->
         * relay_set_state -> on_core_state_changed -> g_weak_ref_get). Pending
         * idles hold references of their own and see NULL too. */
        nostr_relay_set_state_callback(self->relay, NULL, NULL);
        nostr_relay_set_auth_callback(self->relay, NULL, NULL);
        nostr_relay_set_ok_callback(self->relay, NULL, NULL);

        /* nostrc-ws3: Dispatch nostr_relay_free to a background thread.
         * relay_free_impl blocks in go_wait_group_wait() waiting for worker
         * goroutines to exit.  If finalize runs on the GTK main thread
         * (e.g., sync_relays → remove_relay → last unref → finalize),
         * this blocks the main loop and freezes the app.
         *
         * relay_free_impl cancels the connection context and closes the
         * connection's channels itself (nostrc-ws1), under the relay mutex.
         * This function used to close them first by reading
         * relay->connection without that mutex, racing message_loop's
         * reconnect, which swaps the connection and hands the old one to the
         * LWS thread to free (nostrc-flp7).
         *
         * NOTE: Do NOT call nostr_relay_close() here - it blocks waiting for
         * workers, which would freeze the main thread. */
        NostrRelay *relay = self->relay;
        self->relay = NULL;

        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(task, relay, NULL);
        g_task_run_in_thread(task, relay_free_thread_func);
        g_object_unref(task);
    }

    /* Drop the sign handler (a worker still signing holds its own ref). */
    g_clear_pointer(&self->auth_handler, relay_auth_handler_unref);

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
     * The relay URL (e.g., "wss://nos.lol"). Construct-only.
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
     * Emitted when the connection state changes. Core-driven transitions
     * are emitted on the global default main context, never on a relay
     * worker thread, one emission per transition in the order the core
     * reported them; @old_state is always the previous emission's
     * @new_state (initially %GNOSTR_RELAY_STATE_DISCONNECTED). The
     * #GNostrRelay:state property may already be ahead of @new_state when
     * further transitions are still queued. gnostr_relay_disconnect()
     * emits synchronously and supersedes transitions queued before it.
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
    self->emitted_state = GNOSTR_RELAY_STATE_DISCONNECTED;
    self->state_serial = 0;
    self->state_barrier = 0;
#ifdef ENABLE_NIP11
    self->nip11_info = NULL;
    self->nip11_cancellable = NULL;
#endif
    self->auth_handler = NULL;
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

    /* Check registry for a live relay. NULL from g_weak_ref_get() means the
     * registered relay is being finalized; replace it (nostrc-flp7). */
    if (g_relay_registry) {
        RelayRegistryEntry *entry = g_hash_table_lookup(g_relay_registry, url);
        GNostrRelay *existing = entry ? g_weak_ref_get(&entry->relay) : NULL;
        if (existing) {
            G_UNLOCK(relay_registry);
            return existing;
        }
    }

    /* Create new relay while holding lock to prevent duplicate creation */
    GNostrRelay *relay = g_object_new(GNOSTR_TYPE_RELAY, "url", url, NULL);

    if (!g_relay_registry) {
        g_relay_registry = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, relay_registry_entry_free);
    }
    RelayRegistryEntry *entry = g_new0(RelayRegistryEntry, 1);
    g_weak_ref_init(&entry->relay, relay);
    entry->owner = relay;
    g_hash_table_replace(g_relay_registry, g_strdup(url), entry);
    G_UNLOCK(relay_registry);

    return relay;
}

gboolean
gnostr_relay_connect(GNostrRelay *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_RELAY(self), FALSE);
    g_return_val_if_fail(self->relay != NULL, FALSE);

    /* nostrc-kw9r: Shared relay may already be connected by another pool.
     * Use atomic load since this may be called from worker threads.
     * nostrc-oz77: CONNECTED is set when the dial starts; only an
     * established handshake counts. */
    if (__atomic_load_n(&self->state, __ATOMIC_SEQ_CST) == GNOSTR_RELAY_STATE_CONNECTED &&
        nostr_relay_is_established(self->relay)) {
        return TRUE;
    }

    /* nostrc-blk2: Do NOT call gnostr_relay_emit_state() here.
     * This function is called from worker threads (via connect_async_thread).
     * gnostr_relay_emit_state() emits GObject signals (g_signal_emit,
     * g_object_notify_by_pspec) which are NOT thread-safe — they freeze the
     * app when signal handlers try to update GTK widgets from a worker thread.
     *
     * The core relay's state callback (on_core_state_changed, line 153)
     * already dispatches state changes to the main thread via g_idle_add
     * AND stores state atomically for immediate thread-safe reads. */

    /* nostrc-oz77: nostr_relay_connect() only starts the dial; succeed on
     * the WebSocket handshake, so a refused or dead relay is reported here
     * rather than by a publish that never gets its OK. The core relay's
     * reconnect loop keeps running for the other users of a shared relay. */
    Error *err = NULL;
    if (nostr_relay_connect(self->relay, &err) &&
        nostr_relay_wait_established(self->relay, GNOSTR_RELAY_HANDSHAKE_TIMEOUT_MS, &err)) {
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

    /* Everything the core queued up to now is superseded by this explicit
     * DISCONNECTED; later transitions (a new connect) are still announced. */
    __atomic_store_n(&self->state, GNOSTR_RELAY_STATE_DISCONNECTED, __ATOMIC_SEQ_CST);
    __atomic_store_n(&self->state_barrier,
                     __atomic_load_n(&self->state_serial, __ATOMIC_SEQ_CST),
                     __ATOMIC_SEQ_CST);
    gnostr_relay_emit_state(self, GNOSTR_RELAY_STATE_DISCONNECTED);
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
    g_autoptr(GError) error = NULL;

    /* Check for cancellation */
    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        /* nostrc-blk2: Do NOT call gnostr_relay_emit_state from worker
         * thread — it emits GObject signals that freeze GTK. State hasn't
         * changed since we haven't called connect yet. */
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }

    if (gnostr_relay_connect(self, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, g_steal_pointer(&error));
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
    if (self->state == GNOSTR_RELAY_STATE_CONNECTED && self->relay &&
        nostr_relay_is_established(self->relay)) {
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
            if (relay_has_auth_handler(self)) {
                g_autoptr(GError) auth_err = NULL;
                if (!gnostr_relay_authenticate(self, &auth_err)) {
                    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                                "relay %s requires auth and auto-auth failed: %s",
                                self->url, auth_err ? auth_err->message : "unknown");
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

    g_autoptr(GError) g_err = NULL;
    adapter->func(event, &g_err, adapter->user_data);
    if (g_err) {
        if (err) *err = new_error(1, "%s", g_err->message);
        adapter->g_error = g_steal_pointer(&g_err);
    }
}

void
gnostr_relay_set_auth_handler(GNostrRelay *self,
                               GNostrRelayAuthSignFunc sign_func,
                               gpointer user_data,
                               GDestroyNotify destroy)
{
    g_return_if_fail(GNOSTR_IS_RELAY(self));

    RelayAuthHandler *handler = NULL;
    if (sign_func) {
        handler = g_new0(RelayAuthHandler, 1);
        g_atomic_ref_count_init(&handler->ref_count);
        handler->func = sign_func;
        handler->user_data = user_data;
        handler->destroy = destroy;
    } else if (destroy && user_data) {
        destroy(user_data); /* nothing will call a cleared handler */
    }

    G_LOCK(relay_auth_handler);
    RelayAuthHandler *old = self->auth_handler;
    self->auth_handler = handler;
    G_UNLOCK(relay_auth_handler);

    /* The old user data is destroyed now, or when a gnostr_relay_authenticate()
     * still signing with it returns (nostrc-flp7). */
    relay_auth_handler_unref(old);
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

    /* Held until signing is done, so a concurrent
     * gnostr_relay_set_auth_handler() cannot free its user data (nostrc-flp7). */
    RelayAuthHandler *handler = relay_dup_auth_handler(self);
    if (!handler) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_AUTH_REQUIRED,
                            "no auth handler configured; call gnostr_relay_set_auth_handler() first");
        return FALSE;
    }

    if (self->state != GNOSTR_RELAY_STATE_CONNECTED) {
        relay_auth_handler_unref(handler);
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "not connected");
        return FALSE;
    }

    /* Set up thread-local bridge adapter */
    AuthSignAdapter adapter = {
        .func = handler->func,
        .user_data = handler->user_data,
        .g_error = NULL
    };
    _auth_sign_adapter = &adapter;

    Error *core_err = NULL;
    nostr_relay_auth(self->relay, auth_sign_bridge, &core_err);

    _auth_sign_adapter = NULL;
    relay_auth_handler_unref(handler);

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
