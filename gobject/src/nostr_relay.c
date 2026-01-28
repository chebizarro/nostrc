#include "nostr_relay.h"
#include <glib.h>
/* Use modern libnostr relay API (nostr_relay_*)
 * Core header provides NostrRelay type. */
#include "nostr-relay.h" /* NostrRelay and API */
#include "context.h"     /* GoContext */
#include "error.h"       /* Error, free_error */
#include "nostr-event.h" /* NostrEvent */
#include "nostr-filter.h"/* NostrFilter */
/* NOTE: No synchronous query helper in modern API; consumers should use
 * subscriptions. */

/* GNostrRelay GObject implementation */
G_DEFINE_TYPE(GNostrRelay, gnostr_relay, G_TYPE_OBJECT)

static guint nostr_relay_signals[NOSTR_RELAY_SIGNALS_COUNT] = { 0 };

static void gnostr_relay_finalize(GObject *object) {
    GNostrRelay *self = GNOSTR_RELAY(object);
    if (self->relay) {
        nostr_relay_free(self->relay);
    }
    G_OBJECT_CLASS(gnostr_relay_parent_class)->finalize(object);
}

static void gnostr_relay_class_init(GNostrRelayClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_relay_finalize;

    nostr_relay_signals[SIGNAL_CONNECTED] = g_signal_new(
        "connected",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    nostr_relay_signals[SIGNAL_DISCONNECTED] = g_signal_new(
        "disconnected",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    nostr_relay_signals[SIGNAL_EVENT_RECEIVED] = g_signal_new(
        "event-received",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_POINTER);

    nostr_relay_signals[SIGNAL_ERROR] = g_signal_new(
        "error",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void gnostr_relay_init(GNostrRelay *self) {
    self->relay = NULL;
}

GNostrRelay *gnostr_relay_new(const gchar *url) {
    GNostrRelay *self = g_object_new(GNOSTR_TYPE_RELAY, NULL);
    Error *err = NULL;
    self->relay = nostr_relay_new(NULL /* default ctx */, url, &err);
    if (err) {
        g_warning("nostr_relay_new: %s", err->message ? err->message : "unknown error");
        free_error(err);
    }
    /* Skip signature verification - nostrdb handles this during ingestion */
    if (self->relay) {
        self->relay->assume_valid = true;
    }
    return self;
}

gboolean gnostr_relay_connect(GNostrRelay *self, GError **error) {
    Error *err = NULL;
    if (nostr_relay_connect(self->relay, &err)) {
        g_signal_emit(self, nostr_relay_signals[SIGNAL_CONNECTED], 0);
        return TRUE;
    } else {
        if (error) {
            g_set_error_literal(error,
                                g_quark_from_static_string("nostr-relay-error"),
                                1,
                                err && err->message ? err->message : "connect failed");
        }
        if (err) free_error(err);
        g_signal_emit(self, nostr_relay_signals[SIGNAL_ERROR], 0, error ? *error : NULL);
        return FALSE;
    }
}

gboolean gnostr_relay_publish(GNostrRelay *self, NostrEvent *event, GError **error) {
    if (!self || !self->relay || !event) {
        if (error) g_set_error_literal(error, g_quark_from_static_string("nostr-relay-error"), 1, "invalid arguments");
        return FALSE;
    }
    nostr_relay_publish(self->relay, event);
    return TRUE;
}

GPtrArray *gnostr_relay_query_sync(GNostrRelay *self, NostrFilter *filter, GError **error) {
    /* Deprecated path: not supported by modern API. Recommend async subscription. */
    if (error) g_set_error_literal(error, g_quark_from_static_string("nostr-relay-error"), 2, "query_sync is deprecated; use subscriptions");
    g_signal_emit(self, nostr_relay_signals[SIGNAL_ERROR], 0, error ? *error : NULL);
    return NULL;
}