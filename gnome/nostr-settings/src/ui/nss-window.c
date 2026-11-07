/* nss-window.c — sidebar host for service pages and shared Grotto panels.
 * SPDX-License-Identifier: MIT
 */
#include "nss-ui.h"

typedef struct {
  const gchar *name, *title, *icon;
  AdwPreferencesPage *(*make)(NssContext *ctx);
} PageDef;

static const PageDef PAGES[] = {
  { "identity", "Identity", "avatar-default-symbolic", NULL },
  { "apps", "Connected Apps", "application-x-executable-symbolic", NULL },
  { "remote-signing", "Remote Signing", "network-server-symbolic", NULL },
  { "activity", "Activity", "document-open-recent-symbolic", NULL },
  { "relays", "Relays", "network-transmit-receive-symbolic", nss_page_relays_new },
  { "notifications", "Notifications", "preferences-system-notifications-symbolic", nss_page_notifications_new },
  { "wallet", "Wallet", "wallet-symbolic", nss_page_wallet_new },
  { "media", "Media Servers", "folder-pictures-symbolic", nss_page_media_new },
  { "files", "Files", "folder-symbolic", nss_page_files_new },
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
  g_settings_set_string(gs, "last-page", nss_window_get_page(w));
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
  const gchar *name = nss_window_get_page(s->win);
  int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
  /* The window's children on an opaque background (the window draws its
   * own CSS background, not its children). */
  g_autoptr(GtkSnapshot) snap = gtk_snapshot_new();
  const GdkRGBA bg = { 0.98f, 0.98f, 0.984f, 1.0f };
  gtk_snapshot_append_color(snap, &bg, &GRAPHENE_RECT_INIT(0, 0, width, height));
  /* Lay the children out ourselves: under a bare Xvfb the frame clock
   * stops delivering frames after startup, so updates made since (e.g.
   * the Relays page's stats poll) would otherwise leave them unallocated. */
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
    if (gtk_widget_get_visible(c)) {
      int min = 0, nat = 0;
      gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, width, &min, &nat, NULL, NULL);
      gtk_widget_allocate(c, width, MAX(height, min), -1, NULL);
      gtk_widget_snapshot_child(w, c, snap);
    }
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
  /* Tall enough to show whole pages. A size request, because GTK clamps a
   * default size to its guess of the work area (768 px under Xvfb with no
   * window manager). Not persisted: window state is saved on close-request,
   * which a screenshot run never emits. */
  gtk_widget_set_size_request(GTK_WIDGET(win), 820, 1500);
  const gchar *delay = g_getenv("NOSTR_SETTINGS_SCREENSHOT_DELAY");
  guint secs = delay ? (guint)g_ascii_strtoull(delay, NULL, 10) : 6;
  g_timeout_add_seconds(secs ? secs : 6, shot_now, s);
}

const gchar *nss_window_get_page(GtkWindow *window)
{
  return gtk_stack_get_visible_child_name(g_object_get_data(G_OBJECT(window), "nss-stack"));
}
GrottoPanel *nss_window_get_panel(GtkWindow *window, const gchar *page)
{
  g_autofree gchar *key = g_strconcat("nss-panel-", page, NULL);
  return g_object_get_data(G_OBJECT(window), key);
}
gboolean nss_window_show_page(GtkWindow *window, const gchar *page)
{
  if (!known_page(page)) return FALSE;
  GtkStack *stack = g_object_get_data(G_OBJECT(window), "nss-stack");
  NssContext *ctx = g_object_get_data(G_OBJECT(window), "nss-context");
  for (gsize i = 0; i < G_N_ELEMENTS(PAGES); i++) if (!strcmp(page, PAGES[i].name)) {
    GtkWidget *box = gtk_stack_get_child_by_name(stack, page);
    if (PAGES[i].make && !gtk_widget_get_first_child(box)) {
      GtkWidget *prefs = GTK_WIDGET(PAGES[i].make(ctx));
      adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(prefs), page);
      gtk_box_append(GTK_BOX(box), prefs);
      gtk_widget_set_vexpand(prefs, TRUE);
    }
    GtkListBox *sidebar = g_object_get_data(G_OBJECT(window), "nss-sidebar");
    gtk_list_box_select_row(sidebar, gtk_list_box_get_row_at_index(sidebar, (int)i));
    gtk_stack_set_visible_child_name(stack, page);
    AdwOverlaySplitView *split = g_object_get_data(G_OBJECT(window), "nss-split");
    if (adw_overlay_split_view_get_collapsed(split)) adw_overlay_split_view_set_show_sidebar(split, FALSE);
    return TRUE;
  }
  return FALSE;
}
static void selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  (void)box; if (row) nss_window_show_page(data, PAGES[gtk_list_box_row_get_index(row)].name);
}
static void open_current(GtkButton *button, gpointer data)
{
  (void)button; GtkWindow *window = data;
  nss_grotto_open(g_object_get_data(G_OBJECT(window), "nss-context"), nss_window_get_page(window), "");
}
static void manager_state(GrottoPanelsService *service, gpointer data)
{
  AdwBanner *banner = data;
  gboolean available = grotto_panels_service_is_available(service);
  gboolean manager = available && grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL);
  adw_banner_set_title(banner, manager ?
    "Security changes, backup and approvals open in Grotto." :
    "Signer data unavailable. Open Grotto to check the connection, permissions or key storage.");
}
GtkWindow *
nss_window_new(GtkApplication *app, const gchar *page)
{
  AdwApplicationWindow *win = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
  gtk_window_set_application(GTK_WINDOW(win), app);
  gtk_window_set_title(GTK_WINDOW(win), "Nostr Settings");
  gtk_widget_set_size_request(GTK_WIDGET(win), 360, 480);
  AdwToastOverlay *toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_application_window_set_content(win, GTK_WIDGET(toasts));
  AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
  adw_toast_overlay_set_child(toasts, GTK_WIDGET(split));
  adw_overlay_split_view_set_min_sidebar_width(split, 210);
  adw_overlay_split_view_set_max_sidebar_width(split, 240);
  AdwBreakpoint *bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 700sp"));
  GValue collapsed = G_VALUE_INIT;
  g_value_init(&collapsed, G_TYPE_BOOLEAN); g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(bp, G_OBJECT(split), "collapsed", &collapsed);
  g_value_unset(&collapsed);
  adw_application_window_add_breakpoint(win, bp);
  NssContext *ctx = nss_context_new(GTK_WINDOW(win), toasts);
  g_object_set_data_full(G_OBJECT(win), "nss-context", ctx, (GDestroyNotify)nss_context_unref);
  GtkStack *stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_hhomogeneous(stack, FALSE); gtk_stack_set_vhomogeneous(stack, FALSE);
  GtkListBox *sidebar = GTK_LIST_BOX(gtk_list_box_new());
  gtk_widget_add_css_class(GTK_WIDGET(sidebar), "navigation-sidebar");
  g_object_set_data(G_OBJECT(win), "nss-stack", stack);
  g_object_set_data(G_OBJECT(win), "nss-sidebar", sidebar);
  g_object_set_data(G_OBJECT(win), "nss-split", split);
  AdwToolbarView *side = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *side_header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_header_bar_set_title_widget(side_header, gtk_label_new("Nostr Settings"));
  adw_toolbar_view_add_top_bar(side, GTK_WIDGET(side_header));
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(sidebar));
  adw_toolbar_view_set_content(side, scroll);
  adw_overlay_split_view_set_sidebar(split, GTK_WIDGET(side));
  AdwToolbarView *content = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  GtkWidget *toggle = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(toggle), "sidebar-show-symbolic");
  gtk_widget_set_tooltip_text(toggle, "Show settings pages");
  g_object_bind_property(split, "show-sidebar", toggle, "active", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_object_bind_property(split, "collapsed", toggle, "visible", G_BINDING_SYNC_CREATE);
  adw_header_bar_pack_start(header, toggle);
  GtkWidget *open = gtk_button_new_with_label("Open in Grotto");
  g_signal_connect(open, "clicked", G_CALLBACK(open_current), win);
  adw_header_bar_pack_end(header, open);
  adw_toolbar_view_add_top_bar(content, GTK_WIDGET(header));
  AdwBanner *banner = ADW_BANNER(adw_banner_new("Signer management unavailable. Start Grotto to manage identities."));
  adw_banner_set_revealed(banner, TRUE);
  adw_toolbar_view_add_top_bar(content, GTK_WIDGET(banner));
  if (ctx->grotto) {
    g_signal_connect_object(ctx->grotto, "availability-changed", G_CALLBACK(manager_state), banner, 0);
    g_signal_connect_object(ctx->grotto, "capabilities-changed", G_CALLBACK(manager_state), banner, 0);
    manager_state(ctx->grotto, banner);
  }
  adw_toolbar_view_set_content(content, GTK_WIDGET(stack));
  adw_overlay_split_view_set_content(split, GTK_WIDGET(content));
  for (gsize i = 0; i < G_N_ELEMENTS(PAGES); i++) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_titled(stack, box, PAGES[i].name, PAGES[i].title);
    if (!PAGES[i].make) {
      GtkWidget *panel = i == 0 ? grotto_identity_panel_new(ctx->grotto, NULL) :
        i == 1 ? grotto_apps_panel_new(ctx->grotto) :
        i == 2 ? grotto_remote_panel_new(ctx->grotto) : grotto_activity_panel_new(ctx->grotto);
      nss_grotto_attach_host(GROTTO_PANEL(panel), ctx);
      gtk_widget_set_vexpand(panel, TRUE); gtk_box_append(GTK_BOX(box), panel);
      g_autofree gchar *key = g_strconcat("nss-panel-", PAGES[i].name, NULL);
      g_object_set_data(G_OBJECT(win), key, panel);
      if (i == 0) gtk_box_append(GTK_BOX(box), GTK_WIDGET(nss_page_identity_new(ctx)));
    }
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), PAGES[i].title);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(PAGES[i].icon));
    gtk_list_box_append(sidebar, row);
  }
  g_signal_connect(sidebar, "row-selected", G_CALLBACK(selected), win);

  GSettings *gs = window_settings();
  if (gs != NULL) {
    g_object_set_data_full(G_OBJECT(win), "nss-gsettings", gs, g_object_unref);
    gtk_window_set_default_size(GTK_WINDOW(win), g_settings_get_int(gs, "window-width"),
                                g_settings_get_int(gs, "window-height"));
    if (g_settings_get_boolean(gs, "window-maximized"))
      gtk_window_maximize(GTK_WINDOW(win));
    g_autofree gchar *last = g_settings_get_string(gs, "last-page");
    if (page == NULL && known_page(last))
      nss_window_show_page(GTK_WINDOW(win), last);
  } else {
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 640);
  }
  if (known_page(page))
    nss_window_show_page(GTK_WINDOW(win), page);
  else if (page != NULL)
    g_printerr("nostr-settings: unknown page “%s”\n", page);
  if (!gtk_list_box_get_selected_row(sidebar)) nss_window_show_page(GTK_WINDOW(win), "identity");
  g_signal_connect(win, "close-request", G_CALLBACK(on_close), gs);
  maybe_screenshot(GTK_WINDOW(win));
  return GTK_WINDOW(win);
}
