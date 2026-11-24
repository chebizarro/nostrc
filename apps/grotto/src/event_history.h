/* Daemon-owned activity metadata storage.
 * SPDX-License-Identifier: MIT
 */
#ifndef APPS_GNOSTR_SIGNER_EVENT_HISTORY_H
#define APPS_GNOSTR_SIGNER_EVENT_HISTORY_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GN_TYPE_EVENT_HISTORY (gn_event_history_get_type())

G_DECLARE_FINAL_TYPE(GnEventHistory, gn_event_history, GN, EVENT_HISTORY, GObject)

/* Daemon-owned activity metadata. Legacy GUI history is
 * not authoritative; its event_history.json is neither imported nor returned.
 * All calls run on the daemon's owning thread. activity.json is schema v1.
 * No previews, reasons, ciphertext, URI or crypto result enters this API. */
GnEventHistory *gn_event_history_new_activity(const gchar *config_home);
guint64 gn_event_history_record_activity(GnEventHistory *self, GVariant *terminal_snapshot);
GVariant *gn_event_history_list_activity(GnEventHistory *self, guint64 before_sequence, guint limit);
gboolean gn_event_history_clear_activity(GnEventHistory *self, GError **error);
gboolean gn_event_history_activity_healthy(GnEventHistory *self);
guint gn_event_history_activity_count(GnEventHistory *self);
guint64 gn_event_history_activity_sequence(GnEventHistory *self);
guint gn_event_history_signed_today(GnEventHistory *self);
void gn_event_history_set_retention_days(GnEventHistory *self, guint days);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_EVENT_HISTORY_H */
