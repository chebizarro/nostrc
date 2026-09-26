/* nss-page-media.c — the user's Blossom server list (BUD-03, kind 10063).
 * SPDX-License-Identifier: MIT
 */
#include "nss-ui.h"
#include "nss-lists.h"
#include "nss-net.h"

typedef struct {
  NssContext          *ctx;
  GtkWidget           *page;
  AdwPreferencesGroup *group;
  GtkWidget           *status;
  GtkWidget           *publish;
  GPtrArray           *servers;       /* gchar* */
  GPtrArray           *relay_list;    /* NssRelayEntry* or NULL */
  gchar              **signer_relays;
  gchar               *pubkey_hex;
} Page;

static void
page_free(gpointer data)
{
  Page *p = data;
  g_ptr_array_unref(p->servers);
  if (p->relay_list)
    g_ptr_array_unref(p->relay_list);
  g_strfreev(p->signer_relays);
  g_free(p->pubkey_hex);
  nss_context_unref(p->ctx);
  g_free(p);
}

static void render(Page *p);

static void
set_dirty(Page *p)
{
  gtk_widget_set_sensitive(p->publish, p->pubkey_hex != NULL && p->servers->len > 0);
}

static void
on_remove(GtkButton *b, gpointer data)
{
  Page *p = data;
  guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "nss-index"));
  if (i < p->servers->len)
    g_ptr_array_remove_index(p->servers, i);
  set_dirty(p);
  render(p);
}

static void
on_up(GtkButton *b, gpointer data)
{
  Page *p = data;
  guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "nss-index"));
  if (i == 0 || i >= p->servers->len)
    return;
  gpointer t = p->servers->pdata[i - 1];
  p->servers->pdata[i - 1] = p->servers->pdata[i];
  p->servers->pdata[i] = t;
  set_dirty(p);
  render(p);
}

static void
render(Page *p)
{
  nss_group_clear_dynamic(p->group);
  for (guint i = 0; i < p->servers->len; i++) {
    const gchar *url = g_ptr_array_index(p->servers, i);
    GtkWidget *row = nss_info_row(url, i == 0 ? "Preferred — uploads go here first" : NULL);
    if (i > 0) {
      GtkWidget *up = nss_suffix_button("go-up-symbolic", "Prefer this server");
      g_object_set_data(G_OBJECT(up), "nss-index", GUINT_TO_POINTER(i));
      g_signal_connect(up, "clicked", G_CALLBACK(on_up), p);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), up);
    }
    GtkWidget *rm = nss_suffix_button("user-trash-symbolic", "Remove server");
    g_object_set_data(G_OBJECT(rm), "nss-index", GUINT_TO_POINTER(i));
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove), p);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), rm);
    nss_group_add_dynamic(p->group, row);
  }
  if (p->servers->len == 0)
    nss_group_add_dynamic(p->group, nss_info_row("No media servers",
      "Add a Blossom server (https://…) where your pictures and files are stored"));
}

static void
on_add(AdwEntryRow *row, gpointer data)
{
  Page *p = data;
  g_autoptr(GError) err = NULL;
  gchar *url = nss_blossom_url_normalize(gtk_editable_get_text(GTK_EDITABLE(row)), &err);
  if (url == NULL) {
    nss_toast(p->ctx, "%s", err->message);
    return;
  }
  for (guint i = 0; i < p->servers->len; i++)
    if (g_str_equal(g_ptr_array_index(p->servers, i), url)) {
      g_free(url);
      return;
    }
  g_ptr_array_add(p->servers, url);
  gtk_editable_set_text(GTK_EDITABLE(row), "");
  set_dirty(p);
  render(p);
}

static gpointer
load(gpointer data, GError **error)
{
  return nss_user_lists_load(data, TRUE, error);
}

static void
loaded(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)data;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  NssUserLists *l = result;
  if (l == NULL) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "Cannot load your server list");
    nss_row_set_subtitle_plain(p->status, error ? error->message : "Signer unavailable");
    render(p);
    return;
  }
  p->pubkey_hex = g_strdup(l->pubkey_hex);
  p->signer_relays = g_strdupv(l->signer_relays);
  if (l->relay_list)
    p->relay_list = g_ptr_array_ref(l->relay_list);
  g_ptr_array_set_size(p->servers, 0);
  for (guint i = 0; l->blossom && l->blossom[i]; i++)
    g_ptr_array_add(p->servers, g_strdup(l->blossom[i]));
  if (l->blossom) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "Published server list");
    g_autofree gchar *s = g_strdup_printf("Loaded from %s", l->blossom_source);
    nss_row_set_subtitle_plain(p->status, s);
  } else {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "No server list published yet");
    nss_row_set_subtitle_plain(p->status, "Apps fall back to their own servers");
  }
  set_dirty(p);
  gtk_widget_set_sensitive(p->publish, FALSE);
  render(p);
}

static void
published(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)data;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  NssPublishReport *r = result;
  gtk_widget_set_sensitive(p->publish, TRUE);
  if (r == NULL) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "Not published");
    nss_row_set_subtitle_plain(p->status, error ? error->message : "failed");
    return;
  }
  gboolean ok = r->n_required > 0 && r->n_required_ok == r->n_required;
  g_autofree gchar *title = ok
    ? g_strdup_printf("Published — all %u relays confirmed", r->n_required)
    : g_strdup_printf("Partly published — %u of %u relays confirmed", r->n_required_ok,
                      r->n_required);
  g_autofree gchar *text = nss_publish_report_text(r);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), title);
  nss_row_set_subtitle_plain(p->status, text);
  gtk_widget_set_sensitive(p->publish, !ok);
}

static void
on_publish(GtkButton *b, gpointer data)
{
  (void)b;
  Page *p = data;
  g_ptr_array_add(p->servers, NULL);
  g_autoptr(GError) err = NULL;
  g_autofree gchar *json = nss_blossom_list_build((const gchar *const *)p->servers->pdata,
                                                  p->pubkey_hex,
                                                  g_get_real_time() / G_USEC_PER_SEC, &err);
  g_ptr_array_remove_index(p->servers, p->servers->len - 1);
  if (json == NULL) {
    nss_toast(p->ctx, "%s", err->message);
    return;
  }
  NssPublishJob *j = g_new0(NssPublishJob, 1);
  j->bus = g_object_ref(p->ctx->bus);
  j->unsigned_json = g_steal_pointer(&json);
  j->targets = nss_blossom_publish_targets(p->relay_list, (const gchar *const *)p->signer_relays,
                                           &j->required);
  if (j->targets == NULL || j->targets[0] == NULL) {
    nss_publish_job_free(j);
    nss_toast(p->ctx, "No relays to publish to — publish a relay list on the Relays page");
    return;
  }
  gtk_widget_set_sensitive(p->publish, FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "Publishing…");
  nss_row_set_subtitle_plain(p->status, "Approve the request in your signer");
  nss_run(p->page, nss_publish_job_run, published, j, nss_publish_job_free,
          nss_publish_report_free);
}

AdwPreferencesPage *
nss_page_media_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Media servers");
  adw_preferences_page_set_icon_name(page, "folder-pictures-symbolic");
  Page *p = g_new0(Page, 1);
  p->ctx = nss_context_ref(ctx);
  p->page = GTK_WIDGET(page);
  p->servers = g_ptr_array_new_with_free_func(g_free);
  g_object_set_data_full(G_OBJECT(page), "nss-page", p, page_free);

  p->group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->group, "Media servers");
  adw_preferences_group_set_description(p->group,
    "Blossom servers that hold your pictures, videos and files (BUD-03, kind "
    "10063). Share to Nostr and other apps upload to the first one and fall back "
    "to the rest.");
  AdwEntryRow *add = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add), "Add server (https://…)");
  adw_entry_row_set_show_apply_button(add, TRUE);
  g_signal_connect(add, "apply", G_CALLBACK(on_add), p);
  g_signal_connect(add, "entry-activated", G_CALLBACK(on_add), p);
  adw_preferences_group_add(p->group, GTK_WIDGET(add));
  p->status = nss_info_row("Loading your server list…", NULL);
  p->publish = gtk_button_new_with_label("Publish");
  gtk_widget_add_css_class(p->publish, "suggested-action");
  gtk_widget_set_valign(p->publish, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(p->publish, FALSE);
  g_signal_connect(p->publish, "clicked", G_CALLBACK(on_publish), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->status), p->publish);
  adw_preferences_group_add(p->group, p->status);
  adw_preferences_page_add(page, p->group);
  render(p);

  if (ctx->bus)
    nss_run(GTK_WIDGET(page), load, loaded, g_object_ref(ctx->bus), g_object_unref,
            (GDestroyNotify)nss_user_lists_free);
  return page;
}
