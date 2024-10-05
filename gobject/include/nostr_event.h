#ifndef NOSTR_EVENT_H
#define NOSTR_EVENT_H

#include <glib-object.h>
#include "nostr.h"

/* Define NostrEvent GObject */
#define NOSTR_TYPE_EVENT (nostr_event_get_type())
G_DECLARE_FINAL_TYPE(NostrEvent, nostr_event, NOSTR, EVENT, GObject)

struct _NostrEvent {
    GObject parent_instance;
    NostrEvent *event;
};

NostrEvent *nostr_event_new();
void nostr_event_set_id(NostrEvent *self, const gchar *id);
const gchar *nostr_event_get_id(NostrEvent *self);
void nostr_event_set_pubkey(NostrEvent *self, const gchar *pubkey);
const gchar *nostr_event_get_pubkey(NostrEvent *self);
void nostr_event_set_created_at(NostrEvent *self, gint64 created_at);
gint64 nostr_event_get_created_at(NostrEvent *self);
void nostr_event_set_kind(NostrEvent *self, gint kind);
gint nostr_event_get_kind(NostrEvent *self);
void nostr_event_set_content(NostrEvent *self, const gchar *content);
const gchar *nostr_event_get_content(NostrEvent *self);
void nostr_event_set_sig(NostrEvent *self, const gchar *sig);
const gchar *nostr_event_get_sig(NostrEvent *self);

#endif // NOSTR_EVENT_H