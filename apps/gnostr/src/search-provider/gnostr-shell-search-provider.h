/**
 * gnostr-shell-search-provider.h - GNOME Shell SearchProvider2 for Nostr
 *
 * Implements the org.gnome.Shell.SearchProvider2 D-Bus interface so that
 * pressing Super and typing in GNOME Shell surfaces Nostr profiles and notes
 * from the local NDB cache.
 *
 * nostrc-sl86
 */

#ifndef GNOSTR_SHELL_SEARCH_PROVIDER_H
#define GNOSTR_SHELL_SEARCH_PROVIDER_H

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * gnostr_shell_search_provider_register:
 * @connection: The session D-Bus connection (owned by GApplication)
 * @error: Return location for error, or NULL
 *
 * Register the SearchProvider2 D-Bus object at /org/gnostr/SearchProvider.
 * Call once after GApplication has acquired the bus name.
 *
 * Returns: Registration ID (>0) on success, 0 on failure.
 */
guint gnostr_shell_search_provider_register(GDBusConnection *connection,
                                            GError         **error);

/**
 * gnostr_shell_search_provider_unregister:
 * @connection: The session D-Bus connection
 * @registration_id: The ID returned by _register()
 *
 * Unregister the SearchProvider2 D-Bus object. Safe to call with id==0.
 */
void gnostr_shell_search_provider_unregister(GDBusConnection *connection,
                                             guint            registration_id);

G_END_DECLS

#endif /* GNOSTR_SHELL_SEARCH_PROVIDER_H */
