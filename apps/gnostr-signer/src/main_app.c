#include <gtk/gtk.h>
#include <gio/gio.h>

#define SIGNER_NAME  "com.nostr.Signer"
#define SIGNER_PATH  "/com/nostr/Signer"

// Forward declarations for UI pages
GtkWidget *gnostr_home_page_new(void);
GtkWidget *gnostr_permissions_page_new(void);
GtkWidget *gnostr_settings_page_new(void);
// Approval dialog API
void gnostr_show_approval_dialog(GtkWindow *parent, const char *account_npub,
                                 const char *app_name, const char *preview,
                                 void (*cb)(gboolean, gboolean, gpointer), gpointer user_data);

typedef struct {
  GtkLabel *status;
  GtkButton *btn;
  GtkStack *stack;
  GtkButton *nav_home;
  GtkButton *nav_perms;
  GtkButton *nav_settings;
  guint watch_id;
  GDBusConnection *bus;
  GtkWindow *win;
} AppUI;

static void set_status(AppUI *ui, const char *text, const char *css) {
  gtk_label_set_text(ui->status, text);
  if (css) {
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_string(prov, css);
    GdkDisplay *display = gdk_display_get_default();
    if (display)
      gtk_style_context_add_provider_for_display(display,
        GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
  }
}

typedef struct {
  AppUI *ui;
  gchar *request_id;
  gchar *app_id;
} ApproveCtx;

static void approve_call_done(GObject *source, GAsyncResult *res, gpointer user_data){
  (void)source;
  GError *err=NULL; GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
  if (err) { g_warning("ApproveRequest failed: %s", err->message); g_clear_error(&err); }
  if (ret) g_variant_unref(ret);
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  if (ctx){ g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx); }
}

static void on_user_decision(gboolean decision, gboolean remember, gpointer user_data){
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  if (!ctx || !ctx->ui || !ctx->ui->bus) { if (ctx) { g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx);} return; }
  (void)remember; /* TODO: plumb to settings */
  g_dbus_connection_call(ctx->ui->bus,
                         SIGNER_NAME,
                         SIGNER_PATH,
                         "com.nostr.Signer",
                         "ApproveRequest",
                         g_variant_new("(sbb)", ctx->request_id, decision, remember),
                         G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         5000,
                         NULL,
                         approve_call_done,
                         ctx);
}

static void on_approval_requested(GDBusConnection *connection,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters,
                                  gpointer user_data){
  (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
  AppUI *ui = (AppUI*)user_data;
  const gchar *app_id=NULL, *account=NULL, *kind=NULL, *preview=NULL, *request_id=NULL;
  g_variant_get(parameters, "(sssss)", &app_id, &account, &kind, &preview, &request_id);
  ApproveCtx *ctx = g_new0(ApproveCtx, 1);
  ctx->ui = ui;
  ctx->request_id = g_strdup(request_id);
  ctx->app_id = g_strdup(app_id);
  gnostr_show_approval_dialog(ui->win, account, app_id, preview, on_user_decision, ctx);
}

static void name_appeared(GDBusConnection *conn, const gchar *name, const gchar *owner, gpointer user_data) {
  (void)conn; (void)name; (void)owner;
  AppUI *ui = user_data;
  set_status(ui, "Signer: Available", "label { color: #2e7d32; font-weight: bold; }");
  gtk_widget_set_sensitive(GTK_WIDGET(ui->btn), TRUE);
}

static void name_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
  (void)conn; (void)name;
  AppUI *ui = user_data;
  set_status(ui, "Signer: Unavailable", "label { color: #c62828; font-weight: bold; }");
  gtk_widget_set_sensitive(GTK_WIDGET(ui->btn), FALSE);
}

static void on_introspect_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  GtkWindow *win = GTK_WINDOW(user_data);
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
  if (err) {
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Introspect failed: %s", err->message);
    gtk_alert_dialog_show(dlg, win);
    g_error_free(err);
    g_object_unref(dlg);
    return;
  }
  // We don't display the XML; showing success is enough for smoke test.
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Signer Introspect OK");
  gtk_alert_dialog_show(dlg, win);
  g_object_unref(dlg);
  if (ret) g_variant_unref(ret);
}

static void on_btn_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GtkWindow *win = GTK_WINDOW(user_data);
  g_dbus_connection_call(
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL),
      SIGNER_NAME,
      SIGNER_PATH,
      "org.freedesktop.DBus.Introspectable",
      "Introspect",
      NULL,
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      3000,
      NULL,
      on_introspect_done,
      win);
}

static void switch_page(AppUI *ui, const char *name) {
  gtk_stack_set_visible_child_name(ui->stack, name);
}

static void on_nav_home(GtkButton *b, gpointer user_data) { (void)b; switch_page(user_data, "home"); }
static void on_nav_perms(GtkButton *b, gpointer user_data) { (void)b; switch_page(user_data, "permissions"); }
static void on_nav_settings(GtkButton *b, gpointer user_data) { (void)b; switch_page(user_data, "settings"); }

static GtkWidget *build_home_header(AppUI *ui, GtkWindow *win) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  ui->status = GTK_LABEL(gtk_label_new("Signer: Unknown"));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(ui->status));
  ui->btn = GTK_BUTTON(gtk_button_new_with_label("DBus Introspect"));
  gtk_widget_set_sensitive(GTK_WIDGET(ui->btn), FALSE);
  g_signal_connect(ui->btn, "clicked", G_CALLBACK(on_btn_clicked), win);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(ui->btn));
  return box;
}

static void on_activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  GtkWidget *win = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(win), "GNostr Signer");
  gtk_window_set_default_size(GTK_WINDOW(win), 720, 480);

  AppUI *ui = g_new0(AppUI, 1);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), root);

  // Content stack
  ui->stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(ui->stack, GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_stack_set_vhomogeneous(ui->stack, FALSE);

  // Build Home page container: header + body
  GtkWidget *home_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(home_container, 16);
  gtk_widget_set_margin_bottom(home_container, 16);
  gtk_widget_set_margin_start(home_container, 16);
  gtk_widget_set_margin_end(home_container, 16);
  GtkWidget *home_header = build_home_header(ui, GTK_WINDOW(win));
  gtk_box_append(GTK_BOX(home_container), home_header);
  gtk_box_append(GTK_BOX(home_container), gnostr_home_page_new());

  GtkWidget *perms = gnostr_permissions_page_new();
  GtkWidget *settings = gnostr_settings_page_new();

  gtk_stack_add_titled(ui->stack, home_container, "home", "Home");
  gtk_stack_add_titled(ui->stack, perms, "permissions", "Permissions");
  gtk_stack_add_titled(ui->stack, settings, "settings", "Settings");

  gtk_box_append(GTK_BOX(root), GTK_WIDGET(ui->stack));

  // Bottom navigation
  GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(nav, "toolbar");
  gtk_widget_set_margin_top(nav, 4);
  gtk_widget_set_margin_bottom(nav, 4);
  ui->nav_home = GTK_BUTTON(gtk_button_new_with_label("Home"));
  ui->nav_perms = GTK_BUTTON(gtk_button_new_with_label("Permissions"));
  ui->nav_settings = GTK_BUTTON(gtk_button_new_with_label("Settings"));
  g_signal_connect(ui->nav_home, "clicked", G_CALLBACK(on_nav_home), ui);
  g_signal_connect(ui->nav_perms, "clicked", G_CALLBACK(on_nav_perms), ui);
  g_signal_connect(ui->nav_settings, "clicked", G_CALLBACK(on_nav_settings), ui);
  gtk_box_append(GTK_BOX(nav), GTK_WIDGET(ui->nav_home));
  gtk_box_append(GTK_BOX(nav), GTK_WIDGET(ui->nav_perms));
  gtk_box_append(GTK_BOX(nav), GTK_WIDGET(ui->nav_settings));
  gtk_box_set_homogeneous(GTK_BOX(nav), TRUE);
  gtk_box_append(GTK_BOX(root), nav);

  // DBus name watch
  ui->watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION, SIGNER_NAME,
                                  G_BUS_NAME_WATCHER_FLAGS_NONE,
                                  name_appeared, name_vanished, ui, NULL);
  ui->win = GTK_WINDOW(win);
  ui->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (ui->bus){
    g_dbus_connection_signal_subscribe(ui->bus,
                                       SIGNER_NAME,
                                       "com.nostr.Signer",
                                       "ApprovalRequested",
                                       SIGNER_PATH,
                                       NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_approval_requested,
                                       ui,
                                       NULL);
  }

  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  g_set_prgname("gnostr-signer");
  GtkApplication *app = gtk_application_new("org.gnostr.Signer", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
