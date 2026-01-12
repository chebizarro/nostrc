#ifndef GN_NOSTR_EVENT_MODEL_H
#define GN_NOSTR_EVENT_MODEL_H

#include "gn-nostr-event-item.h"
#include <gio/gio.h>

G_BEGIN_DECLS

#define GN_TYPE_NOSTR_EVENT_MODEL (gn_nostr_event_model_get_type())
G_DECLARE_FINAL_TYPE(GnNostrEventModel, gn_nostr_event_model, GN, NOSTR_EVENT_MODEL, GObject)

typedef struct {
    gint *kinds;
    gsize n_kinds;
    char **authors;
    gsize n_authors;
    gint64 since;
    gint64 until;
    guint limit;
} GnNostrQueryParams;

GnNostrEventModel *gn_nostr_event_model_new(void);

void gn_nostr_event_model_set_query(GnNostrEventModel *self, const GnNostrQueryParams *params);
void gn_nostr_event_model_set_thread_root(GnNostrEventModel *self, const char *root_event_id);
void gn_nostr_event_model_refresh(GnNostrEventModel *self);
void gn_nostr_event_model_clear(GnNostrEventModel *self);

gboolean gn_nostr_event_model_get_is_thread_view(GnNostrEventModel *self);
const char *gn_nostr_event_model_get_root_event_id(GnNostrEventModel *self);

G_END_DECLS

#endif
