#include <gtk/gtk.h>
#include <gio/gio.h>
#include "policy_store.h"
#include "accounts_store.h"

#define SIGNER_NAME  "com.nostr.Signer"
#define SIGNER_PATH  "/com/nostr/Signer"

// Forward declarations for UI pages
GtkWidget *gnostr_home_page_new(void);
GtkWidget *gnostr_permissions_page_new(struct _PolicyStore *ps);
void gnostr_permissions_page_refresh(GtkWidget *page, struct _PolicyStore *ps);
GtkWidget *gnostr_settings_page_new(AccountsStore *as);
void gnostr_settings_page_refresh(GtkWidget *page, AccountsStore *as);
/* Import dialog helper from settings_page.c */
void gnostr_settings_open_import_dialog(GtkWindow *parent, AccountsStore *as, const char *initial_account);
void gnostr_settings_open_import_dialog_with_callback(GtkWindow *parent, AccountsStore *as, const char *initial_account,
                                                      void (*on_success)(const char*, gpointer), gpointer user_data);
// Approval dialog API
void gnostr_show_approval_dialog(GtkWindow *parent, const char *account_npub,
                                 const char *app_name, const char *preview,
                                 AccountsStore *as,
                                 void (*cb)(gboolean, gboolean, const char*, gpointer), gpointer user_data);

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
  struct _PolicyStore *policy;
  GtkWidget *perms_page;
  AccountsStore *accounts;
  GtkWidget *settings_page;
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
  gchar *account;
  gboolean decision;
  gboolean remember;
} ApproveCtx;

typedef struct {
  AppUI *ui;
  gchar *request_id;
  gchar *app_id;
  gchar *account;
  gboolean decision;
  gboolean remember;
} RetryCtx;

/* Forward declaration */
static void approve_call_done(GObject *source, GAsyncResult *res, gpointer user_data);

/* File-scope callback: invoked after a successful secret import to retry approval */
static void on_import_success_retry(const char *account, gpointer user_data){
  RetryCtx *rc2 = (RetryCtx*)user_data;
  if (!rc2 || !rc2->ui || !rc2->ui->bus) { if (rc2){ g_free(rc2->request_id); g_free(rc2->app_id); g_free(rc2->account); g_free(rc2);} return; }
  if (account && *account) {
    g_free(rc2->account);
    rc2->account = g_strdup(account);
  }
  /* Re-send ApproveRequest with the same decision/remember */
  ApproveCtx *n = g_new0(ApproveCtx, 1);
  n->ui = rc2->ui; n->request_id = rc2->request_id; n->app_id = rc2->app_id; n->account = rc2->account; n->decision = rc2->decision; n->remember = rc2->remember;
  rc2->request_id = NULL; rc2->app_id = NULL; rc2->account = NULL;
  g_dbus_connection_call(n->ui->bus,
                         SIGNER_NAME,
                         SIGNER_PATH,
                         "com.nostr.Signer",
                         "ApproveRequest",
                         g_variant_new("(sbb)", n->request_id, n->decision, n->remember),
                         G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         5000,
                         NULL,
                         approve_call_done,
                         n);
  g_free(rc2);
}

static void approve_call_done(GObject *source, GAsyncResult *res, gpointer user_data){
  (void)source;
  GError *err=NULL; GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
  gboolean ok = TRUE;
  if (err) {
    g_warning("ApproveRequest failed: %s", err->message);
    g_clear_error(&err);
  }
  if (ret) {
    g_variant_get(ret, "(b)", &ok);
    g_variant_unref(ret);
  }
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  if (!ok && ctx && ctx->ui && ctx->ui->win && ctx->ui->accounts) {
    /* Likely missing session secret for selected account: prompt import, then retry */
    RetryCtx *rc = g_new0(RetryCtx, 1);
    rc->ui = ctx->ui;
    rc->request_id = g_strdup(ctx->request_id);
    rc->app_id = g_strdup(ctx->app_id);
    rc->account = g_strdup(ctx->account);
    rc->decision = ctx->decision;
    rc->remember = ctx->remember;
    /* free the original ctx now */
    g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx->account); g_free(ctx);
    gnostr_settings_open_import_dialog_with_callback(rc->ui->win, rc->ui->accounts, rc->account, on_import_success_retry, rc);
    return;
  }
  if (ctx){ g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx->account); g_free(ctx); }
}

static void on_user_decision(gboolean decision, gboolean remember, gpointer user_data){
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  if (!ctx || !ctx->ui || !ctx->ui->bus) { if (ctx) { g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx);} return; }
  ctx->decision = decision;
  ctx->remember = remember;
  if (remember && ctx->ui->policy) {
    extern void policy_store_set(struct _PolicyStore*, const gchar*, const gchar*, gboolean);
    extern void policy_store_save(struct _PolicyStore*);
    policy_store_set(ctx->ui->policy, ctx->app_id, ctx->account, decision);
    policy_store_save(ctx->ui->policy);
    if (ctx->ui->perms_page) {
      gnostr_permissions_page_refresh(ctx->ui->perms_page, ctx->ui->policy);
    }
  }
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

/* Adapter to consume selected account from dialog and forward to existing handler */
static void on_user_decision_with_account(gboolean decision, gboolean remember, const char *selected_account, gpointer user_data){
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  if (selected_account && (!ctx->account || g_strcmp0(ctx->account, selected_account) != 0)) {
    g_free(ctx->account);
    ctx->account = g_strdup(selected_account);
  }
  on_user_decision(decision, remember, ctx);
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
  /* Resolve account: if missing, use active account */
  gchar *effective_account = NULL;
  if (account && account[0] != '\0') {
    effective_account = g_strdup(account);
  } else if (ui->accounts) {
    accounts_store_get_active(ui->accounts, &effective_account);
  }
  const gchar *acct = effective_account ? effective_account : account;
  /* Auto-approve/deny if remembered */
  if (ui->policy) {
    extern gboolean policy_store_get(struct _PolicyStore*, const gchar*, const gchar*, gboolean*);
    gboolean remembered = FALSE, remembered_decision = FALSE;
    remembered = policy_store_get(ui->policy, app_id, acct, &remembered_decision);
    if (remembered) {
      ApproveCtx *actx = g_new0(ApproveCtx, 1);
      actx->ui = ui;
      actx->request_id = g_strdup(request_id);
      actx->app_id = g_strdup(app_id);
      actx->account = g_strdup(acct);
      /* Directly call ApproveRequest with remembered decision (remember=false as it's already stored) */
      on_user_decision(remembered_decision, FALSE, actx);
      g_free(effective_account);
      return;
    }
  }
  ApproveCtx *ctx = g_new0(ApproveCtx, 1);
  ctx->ui = ui;
  ctx->request_id = g_strdup(request_id);
  ctx->app_id = g_strdup(app_id);
  ctx->account = g_strdup(acct);
  gnostr_show_approval_dialog(ui->win, acct, app_id, preview, ui->accounts, on_user_decision_with_account, ctx);
  g_free(effective_account);
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

  /* Initialize policy store */
  extern struct _PolicyStore *policy_store_new(void);
  extern void policy_store_load(struct _PolicyStore*);
  ui->policy = policy_store_new();
  policy_store_load(ui->policy);

  GtkWidget *perms = gnostr_permissions_page_new(ui->policy);
  /* Initialize accounts store */
  ui->accounts = accounts_store_new();
  accounts_store_load(ui->accounts);
  GtkWidget *settings = gnostr_settings_page_new(ui->accounts);

  gtk_stack_add_titled(ui->stack, home_container, "home", "Home");
  ui->perms_page = perms;
  gtk_stack_add_titled(ui->stack, ui->perms_page, "permissions", "Permissions");
  ui->settings_page = settings;
  gtk_stack_add_titled(ui->stack, ui->settings_page, "settings", "Settings");

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
