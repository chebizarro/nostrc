#include "nostr_event.h"
#include <glib.h>

/* NostrEvent GObject implementation */
G_DEFINE_TYPE(NostrEvent, nostr_event, G_TYPE_OBJECT)

static void nostr_event_finalize(GObject *object) {
    NostrEvent *self = NOSTR_EVENT(object);
    if (self->event) {
        nostr_event_free(self->event);
    }
    G_OBJECT_CLASS(nostr_event_parent_class)->finalize(object);
}

static void nostr_event_class_init(NostrEventClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_event_finalize;

    g_object_class_install_property(
        object_class,
        PROP_ID,
        g_param_spec_string("id", "ID", "Event ID", NULL, G_PARAM_READWRITE));

    g_object_class_install_property(
        object_class,
        PROP_PUBKEY,
        g_param_spec_string("pubkey", "PubKey", "Event Public Key", NULL, G_PARAM_READWRITE));

    g_object_class_install_property(
        object_class,
        PROP_CREATED_AT,
        g_param_spec_int64("created-at", "Created At", "Event Creation Time", G_MININT64, G_MAXINT64, 0, G_PARAM_READWRITE));

    g_object_class_install_property(
        object_class,
        PROP_KIND,
        g_param_spec_int("kind", "Kind", "Event Kind", G_MININT, G_MAXINT, 0, G_PARAM_READWRITE));

    g_object_class_install_property(
        object_class,
        PROP_CONTENT,
        g_param_spec_string("content", "Content", "Event Content", NULL, G_PARAM_READWRITE));

    g_object_class_install_property(
        object_class,
        PROP_SIG,
        g_param_spec_string("sig", "Sig", "Event Signature", NULL, G_PARAM_READWRITE));
}

static void nostr_event_init(NostrEvent *self) {
    self->event = nostr_event_new();
}

NostrEvent *nostr_event_new() {
    return g_object_new(NOSTR_TYPE_EVENT, NULL);
}

void nostr_event_set_id(NostrEvent *self, const gchar *id) {
    g_free(self->event->id);
    self->event->id = g_strdup(id);
}

const gchar *nostr_event_get_id(NostrEvent *self) {
    return self->event->id;
}

void nostr_event_set_pubkey(NostrEvent *self, const gchar *pubkey) {
    g_free(self->event->pubkey);
    self->event->pubkey = g_strdup(pubkey);
}

const gchar *nostr_event_get_pubkey(NostrEvent *self) {
    return self->event->pubkey;
}

void nostr_event_set_created_at(NostrEvent *self, gint64 created_at) {
    self->event->created_at = created_at;
}

gint64 nostr_event_get_created_at(NostrEvent *self) {
    return self->event->created_at;
}

void nostr_event_set_kind(NostrEvent *self, gint kind) {
    self->event->kind = kind;
}

gint nostr_event_get_kind(NostrEvent *self) {
    return self->event->kind;
}

void nostr_event_set_content(NostrEvent *self, const gchar *content) {
    g_free(self->event->content);
    self->event->content = g_strdup(content);
}

const gchar *nostr_event_get_content(NostrEvent *self) {
    return self->event->content;
}

void nostr_event_set_sig(NostrEvent *self, const gchar *sig) {
    g_free(self->event->sig);
    self->event->sig = g_strdup(sig);
}

const gchar *nostr_event_get_sig(NostrEvent *self) {
    return self->event->sig;
}