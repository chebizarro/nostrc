/*
 * gnostr-main-window-signer-banner.c — read-only banner when GNostr has no
 * signer to use (nostrc-e5nz).
 *
 * GNostr holds no private keys. When the session signs through
 * org.nostr.Signer (NIP-55L) and that service is not on the bus, GNostr can
 * read but not sign: this banner says so and, when the signer is installed
 * (D-Bus activatable), offers to start it. It never falls back to a local
 * key. NIP-46 sessions do not depend on org.nostr.Signer and see no banner
 * for its absence. Keys an older GNostr stored itself are also flagged
 * while the signer that would import them is not running.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gnostr-main-window-private.h"
#include "../ipc/gnostr-signer-availability.h"
#include "../ipc/gnostr-signer-service.h"
#include "../ipc/signer_ipc.h"

#include <glib/gi18n.h>

/* How does the current session depend on org.nostr.Signer? A saved account
 * without NIP-46 credentials signed in through it; startup does not restore
 * such a session (only NIP-46 ones), so it is signed out until sign-in. */
static GnostrSignerNeed
session_signer_need(void)
{
  GnostrSignerMethod method = gnostr_signer_service_get_method(gnostr_signer_service_get_default());
  if (method == GNOSTR_SIGNER_METHOD_NIP46)
    return GNOSTR_SIGNER_NEED_NONE;
  if (method == GNOSTR_SIGNER_METHOD_NIP55L)
    return GNOSTR_SIGNER_NEED_ACTIVE;
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  g_autofree char *npub = g_settings_get_string(settings, "current-npub");
  g_autofree char *nip46 = g_settings_get_string(settings, "nip46-client-secret");
  return (npub && *npub && !(nip46 && *nip46)) ? GNOSTR_SIGNER_NEED_SIGNED_OUT
                                              : GNOSTR_SIGNER_NEED_NONE;
}

static void
on_status_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data); /* ref held by the query */
  GnostrSignerStatus st = { 0 };
  g_autoptr(GError) error = NULL;
  gboolean ok = gnostr_signer_status_query_finish(res, &st, &error);

  if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && self->signer_banner) {
    g_autofree char *text = ok ? gnostr_signer_status_banner_text(&st, session_signer_need())
                               : NULL;
    if (text) {
      adw_banner_set_title(self->signer_banner, text);
      adw_banner_set_button_label(self->signer_banner,
                                  gnostr_signer_status_can_start(&st) && !self->signer_starting
                                    ? _("Start GNostr Signer") : NULL);
    }
    adw_banner_set_revealed(self->signer_banner, text != NULL);
  }
  g_object_unref(self);
}

void
gnostr_main_window_signer_banner_refresh_internal(GnostrMainWindow *self)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  if (!self->signer_banner)
    return;
  if (self->signer_status_cancellable)
    g_cancellable_cancel(self->signer_status_cancellable);
  g_clear_object(&self->signer_status_cancellable);
  self->signer_status_cancellable = g_cancellable_new();
  gnostr_signer_status_query_async(self->signer_status_cancellable, on_status_ready,
                                   g_object_ref(self));
}

static void
on_signer_started(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data); /* ref held by the call */
  g_autoptr(GError) error = NULL;
  gboolean ok = gnostr_signer_start_finish(res, &error);
  self->signer_starting = FALSE;
  if (!ok && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && self->signer_banner) {
    g_autofree char *msg = g_strdup_printf(_("Could not start GNostr Signer: %s"),
                                           error ? error->message : _("unknown error"));
    gnostr_main_window_show_toast(GTK_WIDGET(self), msg);
  }
  gnostr_signer_proxy_reset();
  if (self->signer_banner)
    gnostr_main_window_signer_banner_refresh_internal(self);
  g_object_unref(self);
}

static void
on_banner_button_clicked(AdwBanner *banner, gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (self->signer_starting)
    return;
  self->signer_starting = TRUE;
  adw_banner_set_button_label(banner, NULL);
  adw_banner_set_title(banner, _("Starting GNostr Signer…"));
  /* Own cancellable: a refresh (e.g. the name-appeared callback that the
   * activation itself triggers) must not cancel the start. Only dispose does. */
  if (!self->signer_start_cancellable)
    self->signer_start_cancellable = g_cancellable_new();
  gnostr_signer_start_async(self->signer_start_cancellable, on_signer_started,
                            g_object_ref(self));
}

static void
on_signer_name_appeared(GDBusConnection *c, const gchar *name, const gchar *owner,
                        gpointer user_data)
{
  (void)c; (void)name; (void)owner;
  /* The shared proxy may have cached "not available"; start fresh. */
  gnostr_signer_proxy_reset();
  gnostr_main_window_signer_banner_refresh_internal(GNOSTR_MAIN_WINDOW(user_data));
}

static void
on_signer_name_vanished(GDBusConnection *c, const gchar *name, gpointer user_data)
{
  (void)c; (void)name;
  gnostr_main_window_signer_banner_refresh_internal(GNOSTR_MAIN_WINDOW(user_data));
}

void
gnostr_main_window_signer_banner_start_internal(GnostrMainWindow *self)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  if (!self->signer_banner || self->signer_watch_id)
    return;
  g_signal_connect(self->signer_banner, "button-clicked",
                   G_CALLBACK(on_banner_button_clicked), self);
  /* The first appeared/vanished callback performs the initial refresh.
   * Watching never auto-starts the signer. */
  self->signer_watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION, GNOSTR_SIGNER_BUS_NAME,
                                           G_BUS_NAME_WATCHER_FLAGS_NONE,
                                           on_signer_name_appeared,
                                           on_signer_name_vanished,
                                           self, NULL);
}

void
gnostr_main_window_signer_banner_dispose_internal(GnostrMainWindow *self)
{
  if (self->signer_watch_id) {
    g_bus_unwatch_name(self->signer_watch_id);
    self->signer_watch_id = 0;
  }
  if (self->signer_status_cancellable)
    g_cancellable_cancel(self->signer_status_cancellable);
  g_clear_object(&self->signer_status_cancellable);
  if (self->signer_start_cancellable)
    g_cancellable_cancel(self->signer_start_cancellable);
  g_clear_object(&self->signer_start_cancellable);
}
