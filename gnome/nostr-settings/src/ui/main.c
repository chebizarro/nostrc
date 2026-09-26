/* main.c — org.nostr.Settings.
 * SPDX-License-Identifier: MIT
 *
 *   nostr-settings [--page identity|relays|notifications|wallet|media|files]
 */
#include "nss-ui.h"

static gchar *opt_page;

static void
on_activate(GtkApplication *app, gpointer data)
{
  (void)data;
  GtkWindow *w = gtk_application_get_active_window(app);
  if (w == NULL)
    w = nss_window_new(app, opt_page);
  else if (opt_page)
    adw_preferences_window_set_visible_page_name(ADW_PREFERENCES_WINDOW(w), opt_page);
  gtk_window_present(w);
}

static gint
on_options(GApplication *app, GVariantDict *opts, gpointer data)
{
  (void)app; (void)data;
  const gchar *page = NULL;
  if (g_variant_dict_lookup(opts, "page", "&s", &page)) {
    g_free(opt_page);
    opt_page = g_strdup(page);
  }
  return -1;
}

int
main(int argc, char **argv)
{
  g_autoptr(AdwApplication) app =
    adw_application_new("org.nostr.Settings", G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option(G_APPLICATION(app), "page", 'p', G_OPTION_FLAG_NONE,
                                G_OPTION_ARG_STRING,
                                "Open this page (identity, relays, notifications, "
                                "wallet, media, files)", "PAGE");
  g_signal_connect(app, "handle-local-options", G_CALLBACK(on_options), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int rc = g_application_run(G_APPLICATION(app), argc, argv);
  g_free(opt_page);
  return rc;
}
