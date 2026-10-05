/* nss-page-identity.c — accounts from org.nostr.Signer and the keyring.
 * SPDX-License-Identifier: MIT
 */
#include "nss-ui.h"
#include "nss-identity.h"

typedef struct {
  NssContext          *ctx;
  AdwPreferencesGroup *active;
  AdwPreferencesGroup *keyring;
  AdwPreferencesGroup *relays;
} Page;

typedef struct {
  gchar     *npub;
  gchar     *signer_error;
  gchar    **relays;
  gchar     *relays_error;
  GPtrArray *identities;
  gchar     *keyring_error;
} Loaded;

static void
loaded_free(gpointer p)
{
  Loaded *l = p;
  g_free(l->npub);
  g_free(l->signer_error);
  g_strfreev(l->relays);
  g_free(l->relays_error);
  if (l->identities)
    g_ptr_array_unref(l->identities);
  g_free(l->keyring_error);
  g_free(l);
}

static gpointer
load(gpointer data, GError **error)
{
  (void)error;
  GDBusConnection *bus = data;
  Loaded *l = g_new0(Loaded, 1);
  GError *e = NULL;
  l->npub = nss_signer_get_npub(bus, &e);
  if (l->npub == NULL) {
    l->signer_error = g_strdup(e ? e->message : "unknown error");
    g_clear_error(&e);
  } else {
    l->relays = nss_signer_get_relays(bus, &e);
    if (l->relays == NULL) {
      l->relays_error = g_strdup(e ? e->message : "unknown error");
      g_clear_error(&e);
    }
  }
  l->identities = nss_keyring_identities(l->npub, &e);
  if (e) {
    l->keyring_error = g_strdup(e->message);
    g_clear_error(&e);
  }
  return l;
}

static void
loaded(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)error;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  Loaded *l = result;
  (void)data;
  nss_group_clear_dynamic(p->active);
  nss_group_clear_dynamic(p->keyring);
  nss_group_clear_dynamic(p->relays);

  if (l->npub) {
    const gchar *label = NULL;
    for (guint i = 0; l->identities && i < l->identities->len; i++) {
      NssIdentity *id = g_ptr_array_index(l->identities, i);
      if (id->active)
        label = id->label;
    }
    nss_group_add_dynamic(p->active,
      nss_status_row(label ? label : "Active identity", l->npub, "avatar-default-symbolic"));
  } else {
    g_autofree gchar *sub = g_strdup_printf(
      "Apps sign through org.nostr.Signer, which is not available: %s", l->signer_error);
    nss_group_add_dynamic(p->active, nss_status_row("No signer", sub, "dialog-warning-symbolic"));
  }

  if (!nss_keyring_available()) {
    nss_group_add_dynamic(p->keyring,
      nss_info_row("Keyring support not built in", "Built without libsecret"));
  } else if (l->keyring_error) {
    nss_group_add_dynamic(p->keyring, nss_info_row("Keyring unavailable", l->keyring_error));
  } else if (l->identities->len == 0) {
    nss_group_add_dynamic(p->keyring,
      nss_info_row("No Nostr keys in the keyring", "Create or import one in the signer"));
  } else {
    for (guint i = 0; i < l->identities->len; i++) {
      NssIdentity *id = g_ptr_array_index(l->identities, i);
      GtkWidget *row = nss_info_row(id->label ? id->label : "Unnamed identity", id->npub);
      if (g_strcmp0(id->origin, "hardware") == 0)
        adw_action_row_add_prefix(ADW_ACTION_ROW(row),
                                  gtk_image_new_from_icon_name("auth-smartcard-symbolic"));
      if (id->active) {
        GtkWidget *tag = gtk_label_new("Active");
        gtk_widget_add_css_class(tag, "accent");
        gtk_widget_set_valign(tag, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), tag);
      }
      nss_group_add_dynamic(p->keyring, row);
    }
  }

  if (l->relays_error) {
    nss_group_add_dynamic(p->relays, nss_info_row("Could not read relays", l->relays_error));
  } else if (l->relays == NULL || l->relays[0] == NULL) {
    nss_group_add_dynamic(p->relays,
      nss_info_row("None configured", l->npub ? "Apps fall back to their own relays"
                                              : "Unlock the signer to see its relays"));
  } else {
    for (guint i = 0; l->relays[i]; i++)
      nss_group_add_dynamic(p->relays, nss_info_row(l->relays[i], NULL));
  }
}

static void
page_free(gpointer data)
{
  Page *p = data;
  nss_context_unref(p->ctx);
  g_free(p);
}

static void
refresh(Page *p, GtkWidget *owner)
{
  if (p->ctx->bus == NULL)
    return;
  nss_run(owner, load, loaded, g_object_ref(p->ctx->bus), g_object_unref, loaded_free);
}

static void
on_seahorse(GtkButton *b, gpointer data)
{
  (void)b;
  nss_launch_desktop(data, "org.gnome.seahorse.Application.desktop", NULL);
}

static void
on_signer(GtkButton *b, gpointer data)
{
  (void)b;
  nss_launch_desktop(data, "org.nostr.Grotto.desktop", "settings");
}

static void
on_refresh(GtkButton *b, gpointer data)
{
  Page *p = data;
  refresh(p, gtk_widget_get_ancestor(GTK_WIDGET(b), ADW_TYPE_PREFERENCES_PAGE));
}

static GtkWidget *
link_row(const gchar *title, const gchar *subtitle, const gchar *desktop_id,
         GCallback cb, gpointer data)
{
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (!nss_desktop_installed(desktop_id)) {
    g_autofree gchar *why = g_strdup_printf("%s — not installed", subtitle);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), why);
    gtk_widget_set_sensitive(row, FALSE);
    return row;
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  GtkWidget *go = gtk_image_new_from_icon_name("go-next-symbolic");
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), go);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  g_signal_connect(row, "activated", cb, data);
  return row;
}

AdwPreferencesPage *
nss_page_identity_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Identity");
  adw_preferences_page_set_icon_name(page, "avatar-default-symbolic");
  Page *p = g_new0(Page, 1);
  p->ctx = nss_context_ref(ctx);
  g_object_set_data_full(G_OBJECT(page), "nss-page", p, page_free);

  p->active = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->active, "Active identity");
  adw_preferences_group_set_description(p->active,
    "The identity your signer uses for apps on this computer. Your secret key "
    "never leaves the signer.");
  GtkWidget *reload = nss_suffix_button("view-refresh-symbolic", "Reload");
  g_signal_connect(reload, "clicked", G_CALLBACK(on_refresh), p);
  adw_preferences_group_set_header_suffix(p->active, reload);
  adw_preferences_page_add(page, p->active);

  p->keyring = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->keyring, "Identities in your keyring");
  adw_preferences_page_add(page, p->keyring);

  p->relays = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->relays, "Relays set in the signer");
  adw_preferences_group_set_description(p->relays,
    "Used by apps that ask the signer where to connect. Edit your public relay "
    "list on the Relays page.");
  adw_preferences_page_add(page, p->relays);

  AdwPreferencesGroup *manage = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(manage, "Manage keys");
  adw_preferences_group_add(manage,
    link_row("Open in Passwords and Keys", "See and back up the keyring items",
             "org.gnome.seahorse.Application.desktop", G_CALLBACK(on_seahorse), ctx));
  adw_preferences_group_add(manage,
    link_row("Signer settings", "Create, import or switch identities; app permissions",
             "org.nostr.Grotto.desktop", G_CALLBACK(on_signer), ctx));
  adw_preferences_page_add(page, manage);

  if (ctx->bus == NULL)
    nss_group_add_dynamic(p->active, nss_info_row("No session bus", NULL));
  else
    refresh(p, GTK_WIDGET(page));
  return page;
}
