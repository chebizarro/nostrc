#include "nostr_relay.h"
#include <glib.h>

/* NostrRelay GObject implementation */
G_DEFINE_TYPE(NostrRelay, nostr_relay, G_TYPE_OBJECT)

static guint nostr_relay_signals[NOSTR_RELAY_SIGNALS_COUNT] = { 0 };

static void nostr_relay_finalize(GObject *object) {
    NostrRelay *self = NOSTR_RELAY(object);
    if (self->relay) {
        relay_free(self->relay);
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

NostrRelay *nostr_relay_new(const gchar *url) {
    NostrRelay *self = g_object_new(NOSTR_TYPE_RELAY, NULL);
    self->relay = relay_new(url);
    return self;
}

gboolean nostr_relay_connect(NostrRelay *self, GError **error) {
    if (relay_connect(self->relay, error)) {
        g_signal_emit(self, nostr_relay_signals[SIGNAL_CONNECTED], 0);
        return TRUE;
    } else {
        g_signal_emit(self, nostr_relay_signals[SIGNAL_ERROR], 0, *error);
        return FALSE;
    }
}

gboolean nostr_relay_publish(NostrRelay *self, NostrEvent *event, GError **error) {
    if (relay_publish(self->relay, event->event, error)) {
        return TRUE;
    } else {
        g_signal_emit(self, nostr_relay_signals[SIGNAL_ERROR], 0, *error);
        return FALSE;
    }
}

GPtrArray *nostr_relay_query_sync(NostrRelay *self, NostrFilter *filter, GError **error) {
    GPtrArray *events = relay_query_sync(self->relay, &filter->filter, error);
    if (events) {
        for (guint i = 0; i < events->len; i++) {
            NostrEvent *event = g_object_new(NOSTR_TYPE_EVENT, NULL);
            event->event = g_ptr_array_index(events, i);
            g_signal_emit(self, nostr_relay_signals[SIGNAL_EVENT_RECEIVED], 0, event);
        }
        return events;
    } else {
        g_signal_emit(self, nostr_relay_signals[SIGNAL_ERROR], 0, *error);
        return NULL;
    }
}