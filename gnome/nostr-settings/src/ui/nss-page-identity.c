/* Signer relay summary only. Identity inventory comes from Control1, never
 * directly from the keyring in this host. SPDX-License-Identifier: MIT */
#include "nss-ui.h"
#include "nss-identity.h"

static gpointer load(gpointer data, GError **error)
{
  return nss_signer_get_relays(data, error);
}
static void loaded(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)data; AdwActionRow *row = g_object_get_data(G_OBJECT(owner), "relay-summary");
  gchar **relays = result;
  g_autofree gchar *summary = relays ? g_strjoinv(", ", relays) : NULL;
  adw_action_row_set_subtitle(row, error ? "Signer relay summary unavailable" :
    summary && *summary ? summary : "No signer relays configured");
}
static void open_relays(GtkButton *button, gpointer data)
{
  (void)button; NssContext *ctx = data;
  if (ctx->window) nss_window_show_page(ctx->window, "relays");
}
AdwPreferencesPage *nss_page_identity_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  AdwActionRow *row = ADW_ACTION_ROW(nss_info_row("Signer relays", "Reading signer relay summary…"));
  GtkWidget *button = gtk_button_new_with_label("Relays");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  g_signal_connect(button, "clicked", G_CALLBACK(open_relays), ctx);
  /* Page lifetime outlives its button and the async load. */
  g_object_set_data_full(G_OBJECT(page), "relay-context", nss_context_ref(ctx), (GDestroyNotify)nss_context_unref);
  adw_action_row_add_suffix(row, button);
  adw_preferences_group_add(group, GTK_WIDGET(row));
  adw_preferences_page_add(page, group);
  g_object_set_data(G_OBJECT(page), "relay-summary", row);
  if (ctx->bus) nss_run(GTK_WIDGET(page), load, loaded, g_object_ref(ctx->bus), g_object_unref, (GDestroyNotify)g_strfreev);
  else adw_action_row_set_subtitle(row, "Signer relay summary unavailable");
  return page;
}
