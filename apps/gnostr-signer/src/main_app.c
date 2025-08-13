#include <gtk/gtk.h>
#include <adwaita.h>
#include <gio/gio.h>
#include "policy_store.h"
#include "accounts_store.h"
#include "ui/signer-window.h"

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"

// Forward declarations for UI pages
GtkWidget *gnostr_home_page_new(void);
GtkWidget *gnostr_permissions_page_new(struct _PolicyStore *ps);
void gnostr_permissions_page_refresh(GtkWidget *page, struct _PolicyStore *ps);
GtkWidget *gnostr_settings_page_new(AccountsStore *as);
void gnostr_settings_page_refresh(GtkWidget *page, AccountsStore *as);
void gnostr_settings_open_import_dialog_with_callback(GtkWindow *parent, AccountsStore *as, const char *initial_identity,
                                                      void (*on_success)(const char*, gpointer), gpointer user_data);
// Approval dialog API
void gnostr_show_approval_dialog(GtkWindow *parent, const char *identity_npub,
                                 const char *app_name, const char *preview,
                                 AccountsStore *as,
                                 void (*cb)(gboolean, gboolean, const char*, guint64, gpointer), gpointer user_data);

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
  /* Track pending approval request_ids to avoid duplicate dialogs */
  GHashTable *pending;
} AppUI;

static void set_status(AppUI *ui, const char *text, const char *css_key) {
  gtk_label_set_text(ui->status, text);
  GtkWidget *w = GTK_WIDGET(ui->status);
  gtk_widget_remove_css_class(w, "status-ok");
  gtk_widget_remove_css_class(w, "status-error");
  if (css_key) {
    if (g_strcmp0(css_key, "ok") == 0) {
      gtk_widget_add_css_class(w, "status-ok");
    } else if (g_strcmp0(css_key, "error") == 0) {
      gtk_widget_add_css_class(w, "status-error");
    }
  }
}


static void on_app_preferences(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win) return;
  signer_window_show_page((SignerWindow*)win, "settings");
}

static void on_app_about(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *parent = gtk_application_get_active_window(app);
  AdwDialog *about = adw_about_dialog_new();
  adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "GNostr Signer");
  adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), "org.gnostr.Signer");
  adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), "0.1.0");
  adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://github.com/chebizarro/nostrc");
  adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about), "https://github.com/chebizarro/nostrc/issues");
  const char *devs[] = { "GNostr Team", NULL };
  adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(about), devs);
  adw_dialog_present(about, parent ? GTK_WIDGET(parent) : NULL);
}


typedef struct {
  AppUI *ui;
  gchar *request_id;
  gchar *app_id;
  gchar *identity;
  gboolean decision;
  gboolean remember;
  guint64 ttl_seconds;
} ApproveCtx;

typedef struct {
  AppUI *ui;
  gchar *request_id;
  gchar *app_id;
  gchar *identity;
  gboolean decision;
  gboolean remember;
  guint64 ttl_seconds;
} RetryCtx;

/* Forward declaration */
static void approve_call_done(GObject *source, GAsyncResult *res, gpointer user_data);

/* File-scope callback: invoked after a successful secret import to retry approval */
static void on_import_success_retry(const char *identity, gpointer user_data){
  RetryCtx *rc2 = (RetryCtx*)user_data;
  if (!rc2 || !rc2->ui || !rc2->ui->bus) { if (rc2){ g_free(rc2->request_id); g_free(rc2->app_id); g_free(rc2->identity); g_free(rc2);} return; }
  if (identity && *identity) {
    g_free(rc2->identity);
    rc2->identity = g_strdup(identity);
  }
  /* Re-send ApproveRequest with the same decision/remember */
  ApproveCtx *n = g_new0(ApproveCtx, 1);
  n->ui = rc2->ui; n->request_id = rc2->request_id; n->app_id = rc2->app_id; n->identity = rc2->identity; n->decision = rc2->decision; n->remember = rc2->remember; n->ttl_seconds = rc2->ttl_seconds;
  rc2->request_id = NULL; rc2->app_id = NULL; rc2->identity = NULL;
  g_dbus_connection_call(n->ui->bus,
                         SIGNER_NAME,
                         SIGNER_PATH,
                         SIGNER_NAME,
                         "ApproveRequest",
                         g_variant_new("(sbbt)", n->request_id, n->decision, n->remember, n->ttl_seconds),
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
    ok = FALSE;
    g_clear_error(&err);
  }
  if (ret) {
    g_variant_get(ret, "(b)", &ok);
    g_variant_unref(ret);
  }
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  g_message("approve_call_done: request_id=%s ok=%s", ctx && ctx->request_id?ctx->request_id:"(null)", ok?"true":"false");
  /* Done with this request_id: allow future prompts for same id */
  if (ctx && ctx->ui && ctx->ui->pending && ctx->request_id) {
    g_hash_table_remove(ctx->ui->pending, ctx->request_id);
  }
  if (!ok && ctx && ctx->ui && ctx->ui->win && ctx->ui->accounts) {
    /* Likely missing session secret for selected identity: prompt import, then retry */
    RetryCtx *rc = g_new0(RetryCtx, 1);
    rc->ui = ctx->ui;
    rc->request_id = g_strdup(ctx->request_id);
    rc->app_id = g_strdup(ctx->app_id);
    rc->identity = g_strdup(ctx->identity);
    rc->decision = ctx->decision;
    rc->remember = ctx->remember;
    /* free the original ctx now */
    g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx->identity); g_free(ctx);
    gnostr_settings_open_import_dialog_with_callback(rc->ui->win, rc->ui->accounts, rc->identity, on_import_success_retry, rc);
    return;
  }
  if (ctx){ g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx->identity); g_free(ctx); }
}

static void on_user_decision(gboolean decision, gboolean remember, gpointer user_data){
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  if (!ctx || !ctx->ui || !ctx->ui->bus) { if (ctx) { g_free(ctx->request_id); g_free(ctx->app_id); g_free(ctx);} return; }
  ctx->decision = decision;
  ctx->remember = remember;
  g_message("user_decision: request_id=%s decision=%s remember=%s identity=%s ttl=%" G_GUINT64_FORMAT,
            ctx->request_id?ctx->request_id:"(null)", decision?"accept":"reject", remember?"true":"false",
            ctx->identity?ctx->identity:"(null)", ctx->ttl_seconds);
  if (remember && ctx->ui->policy && ctx->identity && ctx->identity[0] != '\0') {
    extern void policy_store_set(struct _PolicyStore*, const gchar*, const gchar*, gboolean);
    extern void policy_store_set_with_ttl(struct _PolicyStore*, const gchar*, const gchar*, gboolean, guint64);
    extern void policy_store_save(struct _PolicyStore*);
    if (ctx->ttl_seconds > 0) {
      policy_store_set_with_ttl(ctx->ui->policy, ctx->app_id, ctx->identity, decision, ctx->ttl_seconds);
    } else {
      policy_store_set(ctx->ui->policy, ctx->app_id, ctx->identity, decision);
    }
    policy_store_save(ctx->ui->policy);
    if (ctx->ui->perms_page) {
      gnostr_permissions_page_refresh(ctx->ui->perms_page, ctx->ui->policy);
    }
  }
  g_message("sending ApproveRequest: id=%s decision=%s remember=%s", ctx->request_id?ctx->request_id:"(null)", decision?"true":"false", remember?"true":"false");
  g_dbus_connection_call(ctx->ui->bus,
                         SIGNER_NAME,
                         SIGNER_PATH,
                         SIGNER_NAME,
                         "ApproveRequest",
                         g_variant_new("(sbbt)", ctx->request_id, decision, remember, ctx->ttl_seconds),
                         G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         5000,
                         NULL,
                         approve_call_done,
                         ctx);
}

/* Adapter to consume selected identity and ttl from dialog and forward to existing handler */
static void on_user_decision_with_identity(gboolean decision, gboolean remember, const char *selected_identity, guint64 ttl_seconds, gpointer user_data){
  ApproveCtx *ctx = (ApproveCtx*)user_data;
  g_message("dialog callback: decision=%s remember=%s selected_identity=%s ttl=%" G_GUINT64_FORMAT,
            decision?"accept":"reject", remember?"true":"false",
            selected_identity?selected_identity:"(null)", ttl_seconds);
  if (selected_identity && *selected_identity && (!ctx->identity || g_strcmp0(ctx->identity, selected_identity) != 0)) {
    g_free(ctx->identity);
    ctx->identity = g_strdup(selected_identity);
  }
  /* Fallback: if identity remains empty, choose the first account from store */
  if ((!ctx->identity || ctx->identity[0] == '\0') && ctx->ui && ctx->ui->accounts) {
    extern GPtrArray *accounts_store_list(struct _AccountsStore*);
    GPtrArray *items = accounts_store_list(ctx->ui->accounts);
    if (items && items->len > 0) {
      typedef struct { char *id; char *label; } AccountEntry;
      AccountEntry *e = g_ptr_array_index(items, 0);
      if (e && e->id && *e->id) {
        g_clear_pointer(&ctx->identity, g_free);
        ctx->identity = g_strdup(e->id);
        g_message("fallback identity selected from store: %s", ctx->identity);
      }
      /* free entries */
      for (guint i=0;i<items->len;i++){ AccountEntry *x = g_ptr_array_index(items,i); if (x){ g_free(x->id); g_free(x->label); g_free(x);} }
      g_ptr_array_free(items, TRUE);
    }
  }
  ctx->ttl_seconds = ttl_seconds;
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
  const gchar *app_id=NULL, *identity=NULL, *kind=NULL, *preview=NULL, *request_id=NULL;
  g_variant_get(parameters, "(sssss)", &app_id, &identity, &kind, &preview, &request_id);
  g_message("ApprovalRequested: app_id=%s identity=%s kind=%s request_id=%s", app_id?app_id:"(null)", identity?identity:"(null)", kind?kind:"(null)", request_id?request_id:"(null)");
  /* De-dup: if we already have a pending prompt for this request_id, ignore duplicates */
  if (ui && ui->pending && request_id && *request_id) {
    if (g_hash_table_contains(ui->pending, request_id)) {
      return;
    }
    /* Insert as pending now to avoid races; remove in approve_call_done */
    g_hash_table_insert(ui->pending, g_strdup(request_id), GINT_TO_POINTER(1));
  }
  /* Resolve identity: if missing, use active identity */
  gchar *effective_identity = NULL;
  if (identity && identity[0] != '\0') {
    effective_identity = g_strdup(identity);
  } else if (ui->accounts) {
    accounts_store_get_active(ui->accounts, &effective_identity);
  }
  const gchar *acct = effective_identity ? effective_identity : identity;
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
      actx->identity = g_strdup(acct);
      /* Directly call ApproveRequest with remembered decision (remember=false as it's already stored) */
      on_user_decision(remembered_decision, FALSE, actx);
      g_free(effective_identity);
      return;
    }
  }
  ApproveCtx *ctx = g_new0(ApproveCtx, 1);
  ctx->ui = ui;
  ctx->request_id = g_strdup(request_id);
  ctx->app_id = g_strdup(app_id);
  ctx->identity = g_strdup(acct);
  gnostr_show_approval_dialog(ui->win, acct, app_id, preview, ui->accounts, on_user_decision_with_identity, ctx);
  g_free(effective_identity);
}

static void name_appeared(GDBusConnection *conn, const gchar *name, const char *owner, gpointer user_data) {
  (void)conn; (void)name; (void)owner;
  AppUI *ui = user_data;
  set_status(ui, "Signer: Available", "ok");
  gtk_widget_set_sensitive(GTK_WIDGET(ui->btn), TRUE);
}

static void name_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
  (void)conn; (void)name;
  AppUI *ui = user_data;
  set_status(ui, "Signer: Unavailable", "error");
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
  /* App menu (Quit) */
  GtkWidget *menu_btn = gtk_menu_button_new();
  gtk_widget_set_halign(menu_btn, GTK_ALIGN_END);
  gtk_widget_set_valign(menu_btn, GTK_ALIGN_CENTER);
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
  GMenu *menu = g_menu_new();
  g_menu_append(menu, "Quit", "app.quit");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_box_append(GTK_BOX(box), menu_btn);
  return box;
}

static void on_activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  /* Load application stylesheet from resources */
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(prov, "/org/gnostr/signer/css/app.css");
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  g_object_unref(prov);
  /* Present the new libadwaita SignerWindow shell */
  SignerWindow *win = signer_window_new(ADW_APPLICATION(app));
  gtk_window_present(GTK_WINDOW(win));
}

static void on_app_quit(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}

int main(int argc, char **argv) {
  g_set_prgname("gnostr-signer");
  AdwApplication *app = adw_application_new("org.gnostr.Signer", G_APPLICATION_DEFAULT_FLAGS);
  /* Install app actions */
  static const GActionEntry app_entries[] = {
    { "quit", on_app_quit, NULL, NULL, NULL },
    { "preferences", on_app_preferences, NULL, NULL, NULL },
    { "about", on_app_about, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);
  const char *quit_accels[] = { "<Primary>q", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
  const char *prefs_accels[] = { "<Primary>comma", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.preferences", prefs_accels);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
