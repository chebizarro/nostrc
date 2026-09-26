/* nss-page-files.c — nostr-seal and nostr-share defaults.
 * SPDX-License-Identifier: MIT
 */
#include "nss-ui.h"
#include "nss-config.h"

#include <string.h>

typedef struct {
  NssContext          *ctx;
  gchar               *seal_path;
  gchar               *share_path;
  NssSealConf          seal;
  NssShareConf         share;
  AdwPreferencesGroup *recips;
  gboolean             loading;
} Page;

static void
page_free(gpointer data)
{
  Page *p = data;
  nss_seal_conf_clear(&p->seal);
  g_free(p->seal_path);
  g_free(p->share_path);
  nss_context_unref(p->ctx);
  g_free(p);
}

static void
save_seal(Page *p)
{
  g_autoptr(GError) err = NULL;
  if (!p->loading && !nss_seal_conf_save(p->seal_path, &p->seal, &err))
    nss_toast(p->ctx, "Not saved: %s", err->message);
}

static void
save_share(Page *p)
{
  g_autoptr(GError) err = NULL;
  if (!p->loading && !nss_share_conf_save(p->share_path, &p->share, &err))
    nss_toast(p->ctx, "Not saved: %s", err->message);
}

static void render_recips(Page *p);

static void
on_remove(GtkButton *b, gpointer data)
{
  Page *p = data;
  const gchar *who = g_object_get_data(G_OBJECT(b), "nss-key");
  g_autoptr(GStrvBuilder) sb = g_strv_builder_new();
  for (guint i = 0; p->seal.default_recipients[i]; i++)
    if (!g_str_equal(p->seal.default_recipients[i], who))
      g_strv_builder_add(sb, p->seal.default_recipients[i]);
  g_strfreev(p->seal.default_recipients);
  p->seal.default_recipients = g_strv_builder_end(sb);
  save_seal(p);
  render_recips(p);
}

static void
on_add(AdwEntryRow *row, gpointer data)
{
  Page *p = data;
  g_autofree gchar *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
  guint8 pk[32];
  g_autoptr(GError) err = NULL;
  if (!nss_parse_pubkey(text, pk, &err)) {
    nss_toast(p->ctx, "%s", err->message);
    return;
  }
  g_autofree gchar *npub = nss_pubkey_to_npub(pk);
  for (guint i = 0; p->seal.default_recipients[i]; i++) {
    guint8 other[32];
    if (nss_parse_pubkey(p->seal.default_recipients[i], other, NULL) &&
        memcmp(other, pk, 32) == 0)
      return;
  }
  guint n = g_strv_length(p->seal.default_recipients);
  p->seal.default_recipients = g_renew(gchar *, p->seal.default_recipients, n + 2);
  p->seal.default_recipients[n] = g_strdup(npub ? npub : text);
  p->seal.default_recipients[n + 1] = NULL;
  gtk_editable_set_text(GTK_EDITABLE(row), "");
  save_seal(p);
  render_recips(p);
}

static void
render_recips(Page *p)
{
  nss_group_clear_dynamic(p->recips);
  for (guint i = 0; p->seal.default_recipients[i]; i++) {
    GtkWidget *row = nss_info_row(p->seal.default_recipients[i], NULL);
    GtkWidget *rm = nss_suffix_button("user-trash-symbolic", "Remove recipient");
    g_object_set_data_full(G_OBJECT(rm), "nss-key", g_strdup(p->seal.default_recipients[i]),
                           g_free);
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove), p);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), rm);
    nss_group_add_dynamic(p->recips, row);
  }
}

static void
on_self(AdwSwitchRow *r, GParamSpec *ps, gpointer data)
{
  (void)ps;
  Page *p = data;
  p->seal.include_self = adw_switch_row_get_active(r);
  save_seal(p);
}

static void
on_work(AdwComboRow *r, GParamSpec *ps, gpointer data)
{
  (void)ps;
  Page *p = data;
  guint i = adw_combo_row_get_selected(r);
  p->seal.work_factor = i == 0 ? 0 : NSS_SEAL_WORK_MIN + (gint)i - 1;
  save_seal(p);
}

static void
on_kind(AdwComboRow *r, GParamSpec *ps, gpointer data)
{
  (void)ps;
  Page *p = data;
  p->share.text_kind = adw_combo_row_get_selected(r) == 1 ? 30023 : 1;
  save_share(p);
}

static void
on_meta(AdwSwitchRow *r, GParamSpec *ps, gpointer data)
{
  (void)ps;
  Page *p = data;
  p->share.keep_metadata = adw_switch_row_get_active(r);
  save_share(p);
}

AdwPreferencesPage *
nss_page_files_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Files");
  adw_preferences_page_set_icon_name(page, "folder-documents-symbolic");
  Page *p = g_new0(Page, 1);
  p->ctx = nss_context_ref(ctx);
  p->seal_path = nss_seal_conf_path();
  p->share_path = nss_share_conf_path();
  g_object_set_data_full(G_OBJECT(page), "nss-page", p, page_free);
  g_autoptr(GError) err = NULL;
  if (!nss_seal_conf_load(p->seal_path, &p->seal, &err)) {
    g_warning("%s", err->message);
    g_clear_error(&err);
    nss_seal_conf_clear(&p->seal);
    nss_seal_conf_init(&p->seal);
  }
  if (!nss_share_conf_load(p->share_path, &p->share, &err)) {
    g_warning("%s", err->message);
    g_clear_error(&err);
    p->share.text_kind = 1;
    p->share.keep_metadata = FALSE;
  }
  p->loading = TRUE;

  /* ── nostr-seal ── */
  p->recips = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->recips, "Encrypt files for");
  g_autofree gchar *sd = g_strdup_printf(
    "Who can open files you encrypt with “Encrypt for…” / nostr-seal when you don't "
    "pick anyone. Saved in %s.", p->seal_path);
  adw_preferences_group_set_description(p->recips, sd);
  AdwSwitchRow *self = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self), "Include yourself");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self), "So you can open what you encrypt");
  adw_switch_row_set_active(self, p->seal.include_self);
  g_signal_connect(self, "notify::active", G_CALLBACK(on_self), p);
  adw_preferences_group_add(p->recips, GTK_WIDGET(self));
  AdwEntryRow *add = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add), "Add a recipient (npub1…)");
  adw_entry_row_set_show_apply_button(add, TRUE);
  g_signal_connect(add, "apply", G_CALLBACK(on_add), p);
  g_signal_connect(add, "entry-activated", G_CALLBACK(on_add), p);
  adw_preferences_group_add(p->recips, GTK_WIDGET(add));
  render_recips(p);
  adw_preferences_page_add(page, p->recips);

  AdwPreferencesGroup *pw = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(pw, "Passphrase encryption");
  AdwComboRow *work = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(work), "Key stretching");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(work),
    "Stronger settings resist guessing longer but take more memory and time to open");
  const gchar *levels[] = { "Default (16)", "16 — 64 MiB", "17 — 128 MiB", "18 — 256 MiB",
                            "19 — 512 MiB", "20 — 1 GiB", NULL };
  g_autoptr(GtkStringList) wm = gtk_string_list_new(levels);
  adw_combo_row_set_model(work, G_LIST_MODEL(wm));
  adw_combo_row_set_selected(work, p->seal.work_factor == 0
                                     ? 0 : (guint)(p->seal.work_factor - NSS_SEAL_WORK_MIN + 1));
  g_signal_connect(work, "notify::selected", G_CALLBACK(on_work), p);
  adw_preferences_group_add(pw, GTK_WIDGET(work));
  adw_preferences_page_add(page, pw);

  /* ── nostr-share ── */
  AdwPreferencesGroup *share = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(share, "Sharing");
  g_autofree gchar *shd = g_strdup_printf("Defaults for Share to Nostr. Saved in %s.",
                                          p->share_path);
  adw_preferences_group_set_description(share, shd);
  AdwComboRow *kind = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(kind), "Share plain text as");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(kind), "You can still choose per share");
  const gchar *kinds[] = { "Note (kind 1)", "Article (kind 30023)", NULL };
  g_autoptr(GtkStringList) km = gtk_string_list_new(kinds);
  adw_combo_row_set_model(kind, G_LIST_MODEL(km));
  adw_combo_row_set_selected(kind, p->share.text_kind == 30023 ? 1 : 0);
  g_signal_connect(kind, "notify::selected", G_CALLBACK(on_kind), p);
  adw_preferences_group_add(share, GTK_WIDGET(kind));
  AdwSwitchRow *meta = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(meta),
                                "Upload media whose metadata can't be removed");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(meta),
    "Videos, audio and HEIC photos may include your location. JPEG, PNG, WebP and "
    "GIF are always cleaned.");
  adw_switch_row_set_active(meta, p->share.keep_metadata);
  g_signal_connect(meta, "notify::active", G_CALLBACK(on_meta), p);
  adw_preferences_group_add(share, GTK_WIDGET(meta));
  adw_preferences_page_add(page, share);

  p->loading = FALSE;
  return page;
}
