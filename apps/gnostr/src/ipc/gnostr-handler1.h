/*
 * gnostr-handler1 — org.nostr.Handler1 for GNostr (nostrc-prqu.3).
 *
 * nostr-dispatcher hands an already-fetched event to a running handler by
 * calling org.nostr.Handler1.OpenEvent on the bus name derived from the
 * handler's desktop id (gnome/dbus/org.nostr.Handler1.xml). GNostr's
 * desktop id is org.gnostr.gnostr.desktop while its GApplication id is
 * org.gnostr.Client (kept: nostr-notify, the wallet agent's per-app budget
 * and existing settings/keyring entries all key on it). So the primary
 * instance also owns org.gnostr.gnostr, as an alias used only for this
 * interface, and exports Handler1 at /org/gnostr/gnostr.
 *
 * GTK-free so it can be tested on a private bus.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GNOSTR_HANDLER1_H
#define GNOSTR_HANDLER1_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_HANDLER1_BUS_NAME    "org.gnostr.gnostr"          /* desktop id */
#define GNOSTR_HANDLER1_OBJECT_PATH "/org/gnostr/gnostr"
#define GNOSTR_HANDLER1_INTERFACE   "org.nostr.Handler1"

/* Called on the main context for each OpenEvent. Return FALSE with @error
 * set to reject the event (the dispatcher then launches the URI instead). */
typedef gboolean (*GnostrHandler1OpenEventFunc)(guint kind,
                                                const char *event_json,
                                                const char *const *relays,
                                                gpointer user_data,
                                                GError **error);

typedef struct _GnostrHandler1 GnostrHandler1;

/* Export the interface on @connection, then own GNOSTR_HANDLER1_BUS_NAME. */
GnostrHandler1 *gnostr_handler1_export(GDBusConnection *connection,
                                       GnostrHandler1OpenEventFunc open_event,
                                       gpointer user_data,
                                       GError **error);

/* Release the name and unexport. NULL-safe. */
void gnostr_handler1_unexport(GnostrHandler1 *handler);

G_END_DECLS

#endif /* GNOSTR_HANDLER1_H */
