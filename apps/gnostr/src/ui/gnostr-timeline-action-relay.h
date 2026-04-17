/**
 * gnostr-timeline-action-relay.h — Centralized action handler for timeline rows
 *
 * nostrc-hqtn: Isolates the 17 GNostr main-window action relays behind a
 * named GObject boundary. The relay is created by the app factory, attached
 * to the timeline view via qdata, and passed as user_data on every
 * NoteCardRow action signal. This enables single-call disconnect via
 * g_signal_handlers_disconnect_by_data(row, relay).
 *
 * The relay itself holds no persistent state — each handler locates the
 * main window at invocation time via gtk_widget_get_ancestor(). If the
 * row is not rooted (e.g. during teardown), the handler silently no-ops.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GNOSTR_TIMELINE_ACTION_RELAY_H
#define GNOSTR_TIMELINE_ACTION_RELAY_H

#include <glib-object.h>
#include <nostr-gtk-1.0/nostr-note-card-row.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_ACTION_RELAY (gnostr_timeline_action_relay_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineActionRelay,
                     gnostr_timeline_action_relay,
                     GNOSTR, TIMELINE_ACTION_RELAY,
                     GObject)

/**
 * gnostr_timeline_action_relay_new:
 *
 * Creates a new action relay. The relay has no constructor dependencies;
 * it discovers the main window at signal-handler time via the widget tree.
 *
 * Returns: (transfer full): A new relay instance.
 */
GnostrTimelineActionRelay *gnostr_timeline_action_relay_new(void);

/**
 * gnostr_timeline_action_relay_connect_row:
 * @self: The relay
 * @row: A NoteCardRow to connect action signals on
 *
 * Connects all 17 main-window-bound NoteCardRow action signals to
 * handler functions inside the relay. The relay is passed as user_data
 * on each connection, enabling bulk disconnect in cleanup_bound_row via:
 *
 *   g_signal_handlers_disconnect_by_data(row, relay);
 *
 * Does NOT connect:
 * - "search-hashtag" (connected separately with the timeline view as user_data)
 * - "request-embed"  (connected separately in factory_bind_cb)
 */
void gnostr_timeline_action_relay_connect_row(GnostrTimelineActionRelay *self,
                                               NostrGtkNoteCardRow       *row);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_ACTION_RELAY_H */
