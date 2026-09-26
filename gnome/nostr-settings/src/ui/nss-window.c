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

/* close-request handler: save window state, let the window close. */
static gboolean
on_close(GtkWindow *w, gpointer data)
{
  GSettings *gs = data;
  if (gs == NULL)
    return FALSE;
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
  return FALSE;
}

/* ── Offscreen screenshots (lab verification) ───────────────────────────
 * NOSTR_SETTINGS_SCREENSHOT_DIR=/tmp/x nostr-settings --page relays: after
 * NOSTR_SETTINGS_SCREENSHOT_DELAY seconds (default 6, so asynchronous loads
 * settle) render the visible page to /tmp/x/nostr-settings-<page>.png and
 * quit. One page per run: under a bare Xvfb the frame clock does not
 * re-layout the window after a page switch, so each page gets a fresh
 * window. GTK renders the snapshot itself, so any GDK backend works. */
typedef struct {
  GtkWindow *win;
  gchar     *dir;
} Shot;

static gboolean
shot_now(gpointer data)
{
  Shot *s = data;
  GtkWidget *w = GTK_WIDGET(s->win);
  const gchar *name =
    adw_preferences_window_get_visible_page_name(ADW_PREFERENCES_WINDOW(s->win));
  int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
  /* The window's children on an opaque background (the window draws its
   * own CSS background, not its children). */
  g_autoptr(GtkSnapshot) snap = gtk_snapshot_new();
  const GdkRGBA bg = { 0.98f, 0.98f, 0.984f, 1.0f };
  gtk_snapshot_append_color(snap, &bg, &GRAPHENE_RECT_INIT(0, 0, width, height));
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
    if (gtk_widget_get_visible(c))
      gtk_widget_snapshot_child(w, c, snap);
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(g_steal_pointer(&snap));
  GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(w));
  g_autofree gchar *path = g_strdup_printf("%s/nostr-settings-%s.png", s->dir,
                                           name ? name : "page");
  if (node && r) {
    g_autoptr(GdkTexture) tex = gsk_renderer_render_texture(
      r, node, &GRAPHENE_RECT_INIT(0, 0, width, height));
    if (gdk_texture_save_to_png(tex, path))
      g_print("screenshot: %s\n", path);
    else
      g_printerr("screenshot failed: %s\n", path);
  } else {
    g_printerr("screenshot failed: nothing rendered\n");
  }
  GtkApplication *app = gtk_window_get_application(s->win);
  g_free(s->dir);
  g_free(s);
  if (app)
    g_application_quit(G_APPLICATION(app));
  return G_SOURCE_REMOVE;
}

static void
maybe_screenshot(GtkWindow *win)
{
  const gchar *dir = g_getenv("NOSTR_SETTINGS_SCREENSHOT_DIR");
  if (dir == NULL || *dir == '\0')
    return;
  g_mkdir_with_parents(dir, 0755);
  Shot *s = g_new0(Shot, 1);
  s->win = win;
  s->dir = g_strdup(dir);
  /* Tall enough to show whole pages (not persisted: window state is saved
   * on close-request, which a screenshot run never emits). */
  gtk_window_set_default_size(win, 820, 1500);
  const gchar *delay = g_getenv("NOSTR_SETTINGS_SCREENSHOT_DELAY");
  guint secs = delay ? (guint)g_ascii_strtoull(delay, NULL, 10) : 6;
  g_timeout_add_seconds(secs ? secs : 6, shot_now, s);
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
  maybe_screenshot(GTK_WINDOW(win));
  return GTK_WINDOW(win);
}
