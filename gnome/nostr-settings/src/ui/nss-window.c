/* nss-window.c — the AdwPreferencesWindow and its pages.
 * SPDX-License-Identifier: MIT
 */
#include "nss-ui.h"

typedef struct {
  const gchar *name;
  AdwPreferencesPage *(*make)(NssContext *ctx);
} PageDef;

static const PageDef PAGES[] = {
  { "identity",      nss_page_identity_new },
  { "relays",        nss_page_relays_new },
  { "notifications", nss_page_notifications_new },
  { "wallet",        nss_page_wallet_new },
  { "media",         nss_page_media_new },
  { "files",         nss_page_files_new },
};

static gboolean
known_page(const gchar *name)
{
  for (gsize i = 0; name && i < G_N_ELEMENTS(PAGES); i++)
    if (g_str_equal(PAGES[i].name, name))
      return TRUE;
  return FALSE;
}

static GSettings *
window_settings(void)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default();
  g_autoptr(GSettingsSchema) schema =
    src ? g_settings_schema_source_lookup(src, "org.nostr.Settings", TRUE) : NULL;
  return schema ? g_settings_new("org.nostr.Settings") : NULL;
}

static void
on_close(GtkWindow *w, gpointer data)
{
  GSettings *gs = data;
  if (gs == NULL)
    return;
  if (!gtk_window_is_maximized(w)) {
    int width = 0, height = 0;
    gtk_window_get_default_size(w, &width, &height);
    g_settings_set_int(gs, "window-width", width);
    g_settings_set_int(gs, "window-height", height);
  }
  g_settings_set_boolean(gs, "window-maximized", gtk_window_is_maximized(w));
  AdwPreferencesPage *vis = adw_preferences_window_get_visible_page(ADW_PREFERENCES_WINDOW(w));
  if (vis)
    g_settings_set_string(gs, "last-page", adw_preferences_page_get_name(vis));
}

/* ── Offscreen screenshots (lab verification) ───────────────────────────
 * NOSTR_SETTINGS_SCREENSHOT_DIR=/tmp/x: once mapped, show each page for a
 * few seconds (async loads settle), render the window to
 * DIR/nostr-settings-<page>.png and quit. Works on any GDK backend
 * (xvfb, broadway) because GTK renders the snapshot itself. */
typedef struct {
  GtkWindow *win;
  gchar     *dir;
  guint      idx;
} Shots;

static void
shot_page(Shots *s)
{
  GtkWidget *w = GTK_WIDGET(s->win);
  g_autoptr(GdkPaintable) p = gtk_widget_paintable_new(w);
  int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
  g_autoptr(GtkSnapshot) snap = gtk_snapshot_new();
  gdk_paintable_snapshot(p, GDK_SNAPSHOT(snap), width, height);
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(g_steal_pointer(&snap));
  GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(w));
  if (node == NULL || r == NULL)
    return;
  g_autoptr(GdkTexture) tex = gsk_renderer_render_texture(
    r, node, &GRAPHENE_RECT_INIT(0, 0, width, height));
  g_autofree gchar *path = g_strdup_printf("%s/nostr-settings-%s.png", s->dir,
                                           PAGES[s->idx].name);
  if (gdk_texture_save_to_png(tex, path))
    g_print("screenshot: %s\n", path);
  else
    g_printerr("screenshot failed: %s\n", path);
}

static gboolean
shot_tick(gpointer data)
{
  Shots *s = data;
  shot_page(s);
  if (++s->idx >= G_N_ELEMENTS(PAGES)) {
    GtkApplication *app = gtk_window_get_application(s->win);
    g_free(s->dir);
    g_free(s);
    if (app)
      g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
  }
  adw_preferences_window_set_visible_page_name(ADW_PREFERENCES_WINDOW(s->win),
                                               PAGES[s->idx].name);
  return G_SOURCE_CONTINUE;
}

static void
maybe_screenshots(GtkWindow *win)
{
  const gchar *dir = g_getenv("NOSTR_SETTINGS_SCREENSHOT_DIR");
  if (dir == NULL || *dir == '\0')
    return;
  g_mkdir_with_parents(dir, 0755);
  Shots *s = g_new0(Shots, 1);
  s->win = win;
  s->dir = g_strdup(dir);
  const gchar *delay = g_getenv("NOSTR_SETTINGS_SCREENSHOT_DELAY");
  guint secs = delay ? (guint)g_ascii_strtoull(delay, NULL, 10) : 6;
  adw_preferences_window_set_visible_page_name(ADW_PREFERENCES_WINDOW(win), PAGES[0].name);
  g_timeout_add_seconds(secs ? secs : 6, shot_tick, s);
}

GtkWindow *
nss_window_new(GtkApplication *app, const gchar *page)
{
  AdwPreferencesWindow *win = ADW_PREFERENCES_WINDOW(adw_preferences_window_new());
  gtk_window_set_application(GTK_WINDOW(win), app);
  gtk_window_set_title(GTK_WINDOW(win), "Nostr Settings");
  adw_preferences_window_set_search_enabled(win, TRUE);

  NssContext *ctx = nss_context_new(win);
  g_object_set_data_full(G_OBJECT(win), "nss-context", ctx,
                         (GDestroyNotify)nss_context_unref);

  for (gsize i = 0; i < G_N_ELEMENTS(PAGES); i++) {
    AdwPreferencesPage *p = PAGES[i].make(ctx);
    adw_preferences_page_set_name(p, PAGES[i].name);
    adw_preferences_window_add(win, p);
  }

  GSettings *gs = window_settings();
  if (gs != NULL) {
    g_object_set_data_full(G_OBJECT(win), "nss-gsettings", gs, g_object_unref);
    gtk_window_set_default_size(GTK_WINDOW(win), g_settings_get_int(gs, "window-width"),
                                g_settings_get_int(gs, "window-height"));
    if (g_settings_get_boolean(gs, "window-maximized"))
      gtk_window_maximize(GTK_WINDOW(win));
    g_autofree gchar *last = g_settings_get_string(gs, "last-page");
    if (page == NULL && known_page(last))
      adw_preferences_window_set_visible_page_name(win, last);
  } else {
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 640);
  }
  if (known_page(page))
    adw_preferences_window_set_visible_page_name(win, page);
  else if (page != NULL)
    g_printerr("nostr-settings: unknown page “%s”\n", page);
  g_signal_connect(win, "close-request", G_CALLBACK(on_close), gs);
  maybe_screenshots(GTK_WINDOW(win));
  return GTK_WINDOW(win);
}
