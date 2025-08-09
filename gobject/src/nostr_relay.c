#include "nostr_relay.h"
#include <glib.h>
/* Use modern libnostr relay API (nostr_relay_*) without including
 * libnostr/include/nostr-relay.h to avoid the NostrRelay typedef
 * collision with this GObject type. Declare the needed functions here
 * using Relay* which is typedef-compatible with the core API. */
#include "context.h"   /* GoContext */
#include "error.h"     /* Error, free_error */
#include "event.h"     /* NostrEvent */
#include "filter.h"    /* Filter / Filters */

extern Relay *nostr_relay_new(GoContext *context, const char *url, Error **err);
extern void   nostr_relay_free(Relay *relay);
extern bool   nostr_relay_connect(Relay *relay, Error **err);
extern void   nostr_relay_publish(Relay *relay, NostrEvent *event);
/* NOTE: No synchronous query helper in modern API; consumers should use
 * subscriptions. */

/* NostrRelay GObject implementation */
G_DEFINE_TYPE(NostrRelay, nostr_relay, G_TYPE_OBJECT)

static guint nostr_relay_signals[NOSTR_RELAY_SIGNALS_COUNT] = { 0 };

static void nostr_relay_finalize(GObject *object) {
    NostrRelay *self = NOSTR_RELAY(object);
    if (self->relay) {
        nostr_relay_free(self->relay);
    }
    G_OBJECT_CLASS(nostr_relay_parent_class)->finalize(object);
}

static void nostr_relay_class_init(NostrRelayClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_relay_finalize;

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
        G_TYPE_NONE, 1, NOSTR_TYPE_EVENT);

    nostr_relay_signals[SIGNAL_ERROR] = g_signal_new(
        "error",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_FIRST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void nostr_relay_init(NostrRelay *self) {
    self->relay = NULL;
}

NostrRelay *gnostr_relay_new(const gchar *url) {
    NostrRelay *self = g_object_new(NOSTR_TYPE_RELAY, NULL);
    Error *err = NULL;
    self->relay = nostr_relay_new(NULL /* default ctx */, url, &err);
    if (err) {
        g_warning("nostr_relay_new: %s", err->message ? err->message : "unknown error");
        free_error(err);
    }
    return self;
}

gboolean gnostr_relay_connect(NostrRelay *self, GError **error) {
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

gboolean gnostr_relay_publish(NostrRelay *self, NostrEvent *event, GError **error) {
    if (!self || !self->relay || !event) {
        if (error) g_set_error_literal(error, g_quark_from_static_string("nostr-relay-error"), 1, "invalid arguments");
        return FALSE;
    }
    nostr_relay_publish(self->relay, event->event);
    return TRUE;
}

GPtrArray *gnostr_relay_query_sync(NostrRelay *self, NostrFilter *filter, GError **error) {
    /* Deprecated path: not supported by modern API. Recommend async subscription. */
    if (error) g_set_error_literal(error, g_quark_from_static_string("nostr-relay-error"), 2, "query_sync is deprecated; use subscriptions");
    g_signal_emit(self, nostr_relay_signals[SIGNAL_ERROR], 0, error ? *error : NULL);
    return NULL;
}