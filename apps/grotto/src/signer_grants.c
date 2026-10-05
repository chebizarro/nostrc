/* signer_grants.c - see signer_grants.h */
#include "signer_grants.h"

#include <adwaita.h>
#include <string.h>

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"

static gchar *desktop_name_for(const gchar *app_id) {
  g_autofree gchar *want = g_strconcat(app_id, ".desktop", NULL);
  GList *all = g_app_info_get_all();
  gchar *name = NULL;
  for (GList *l = all; l && !name; l = l->next)
    if (g_strcmp0(g_app_info_get_id(l->data), want) == 0)
      name = g_strdup(g_app_info_get_display_name(l->data));
  g_list_free_full(all, g_object_unref);
  return name;
}

gchar *signer_principal_display_name(const gchar *principal) {
  if (!principal || !*principal) return g_strdup("An unidentified application");
  if (strstr(principal, "://") && !g_str_has_prefix(principal, "claimed:"))
    return g_strdup(strstr(principal, "://") + 3);
  if (g_str_has_prefix(principal, "claimed:"))
    return g_strdup(principal[8] ? principal + 8 : "An unidentified application");
  const gchar *exe = strstr(principal, "exe:");
  g_autofree gchar *base = exe ? g_path_get_basename(exe + 4) : NULL;
  const gchar *id = NULL;
  gsize idlen = 0;
  if (g_str_has_prefix(principal, "flatpak:")) { id = principal + 8; idlen = strlen(id); }
  else if (g_str_has_prefix(principal, "app:") || g_str_has_prefix(principal, "snap:")) {
    id = strchr(principal, ':') + 1;
    const gchar *semi = strchr(id, ';');
    idlen = semi ? (gsize)(semi - id) : strlen(id);
  }
  if (id) {
    g_autofree gchar *app = g_strndup(id, idlen);
    g_autofree gchar *pretty = desktop_name_for(app);
    const gchar *label = pretty ? pretty : app;
    return base ? g_strdup_printf("%s (%s)", label, base) : g_strdup(label);
  }
  return base ? g_strdup(base) : g_strdup(principal);
}

/* What the request kinds mean to a person. */
static const gchar *kind_label(const gchar *kind) {
  static const struct { const gchar *kind, *label; } k[] = {
    { "event",                  "Sign events" },
    { "get_public_key",         "Read your public key" },
    { "get_relays",             "Read your relay list" },
    { "nip04_encrypt",          "Encrypt messages (NIP-04)" },
    { "nip04_decrypt",          "Decrypt messages (NIP-04)" },
    { "nip44_encrypt",          "Encrypt messages" },
    { "nip44_decrypt",          "Decrypt messages" },
    { "nip44_conversation_key", "Derive conversation keys" },
    { "zap_decrypt",            "Decrypt private zaps" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(k); i++)
    if (g_strcmp0(kind, k[i].kind) == 0) return k[i].label;
  return kind;
}

static void clear_rows(GtkListBox *list) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))))
    gtk_list_box_remove(list, child);
}

static void add_message_row(GtkListBox *list, const gchar *title, const gchar *subtitle) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle) adw_action_row_set_subtitle(row, subtitle);
  gtk_list_box_append(list, GTK_WIDGET(row));
}

static void on_revoked(GObject *src, GAsyncResult *res, gpointer user_data) {
  GtkListBox *list = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (!r) g_warning("RevokeGrant: %s", err->message);
  if (gtk_widget_get_root(GTK_WIDGET(list))) signer_grants_list_box_refresh(list);
  g_object_unref(list);
}

static void on_revoke_clicked(GtkButton *btn, gpointer user_data) {
  GtkListBox *list = user_data;
  const gchar *kind = g_object_get_data(G_OBJECT(btn), "kind");
  const gchar *principal = g_object_get_data(G_OBJECT(btn), "principal");
  const gchar *identity = g_object_get_data(G_OBJECT(btn), "identity");
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus) return;
  gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
  g_dbus_connection_call(bus, SIGNER_NAME, SIGNER_PATH, SIGNER_NAME, "RevokeGrant",
                         g_variant_new("(sss)", kind, principal, identity), G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_revoked, g_object_ref(list));
  g_object_unref(bus);
}

static GtkWidget *grant_row(GtkListBox *list, const gchar *kind, const gchar *principal,
                            const gchar *identity, gboolean allow, guint64 until) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  g_autofree gchar *who = signer_principal_display_name(principal);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), who);
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(row), principal);

  g_autofree gchar *when = NULL;
  if (until == 0) {
    when = g_strdup("");
  } else {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    when = (gint64)until > now ? g_strdup_printf(", for %" G_GINT64_FORMAT " more min", ((gint64)until - now + 59) / 60)
                               : g_strdup(", expired");
  }
  g_autofree gchar *id = g_strcmp0(identity, "*") == 0 ? g_strdup("any identity")
                       : strlen(identity) > 20 ? g_strdup_printf("%.12s…%s", identity, identity + strlen(identity) - 6)
                       : g_strdup(identity);
  g_autofree gchar *sub = g_strdup_printf("%s: %s (%s%s)", kind_label(kind),
                                          allow ? "allowed" : "denied", id, when);
  adw_action_row_set_subtitle(row, sub);

  GtkWidget *icon = gtk_image_new_from_icon_name(allow ? "emblem-ok-symbolic" : "action-unavailable-symbolic");
  adw_action_row_add_prefix(row, icon);

  GtkWidget *btn = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(btn, "flat");
  gtk_widget_set_tooltip_text(btn, "Forget this decision (ask again next time)");
  g_object_set_data_full(G_OBJECT(btn), "kind", g_strdup(kind), g_free);
  g_object_set_data_full(G_OBJECT(btn), "principal", g_strdup(principal), g_free);
  g_object_set_data_full(G_OBJECT(btn), "identity", g_strdup(identity), g_free);
  g_signal_connect(btn, "clicked", G_CALLBACK(on_revoke_clicked), list);
  adw_action_row_add_suffix(row, btn);
  return GTK_WIDGET(row);
}

static void on_listed(GObject *src, GAsyncResult *res, gpointer user_data) {
  GtkListBox *list = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  clear_rows(list);
  if (!r) {
    g_autofree gchar *remote = err ? g_dbus_error_get_remote_error(err) : NULL;
    const gchar *why = remote && g_str_has_suffix(remote, ".UnknownMethod")
                           ? "The signer service is too old to list them."
                           : err->message;
    add_message_row(list, "Cannot read remembered decisions", why);
  } else {
    GVariantIter *it = NULL;
    const gchar *kind, *principal, *identity;
    gboolean allow;
    guint64 until;
    guint n = 0;
    g_variant_get(r, "(a(sssbt))", &it);
    while (g_variant_iter_next(it, "(&s&s&sbt)", &kind, &principal, &identity, &allow, &until)) {
      gtk_list_box_append(list, grant_row(list, kind, principal, identity, allow, until));
      n++;
    }
    g_variant_iter_free(it);
    if (n == 0)
      add_message_row(list, "No remembered decisions",
                      "Choose \"Remember\" when approving a request to see it here.");
  }
  g_object_unref(list);
}

void signer_grants_list_box_refresh(GtkListBox *list) {
  g_return_if_fail(GTK_IS_LIST_BOX(list));
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus) {
    clear_rows(list);
    add_message_row(list, "Cannot read remembered decisions", "No session bus.");
    return;
  }
  g_dbus_connection_call(bus, SIGNER_NAME, SIGNER_PATH, SIGNER_NAME, "ListGrants", NULL,
                         G_VARIANT_TYPE("(a(sssbt))"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                         on_listed, g_object_ref(list));
  g_object_unref(bus);
}
