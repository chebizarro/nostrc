#include <gtk/gtk.h>
#include <adwaita.h>
#include <gio/gio.h>
#include "policy_store.h"
#include "accounts_store.h"
#include "settings_manager.h"
#include "startup-timing.h"
#include "secure-mem.h"
#include "i18n.h"
#include "ui/signer-window.h"
#include "ui/onboarding-assistant.h"

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

/* Forward declarations */
static void apply_theme_preference(SettingsTheme theme);
static void on_theme_setting_changed(const char *key, gpointer user_data);
static void update_high_contrast_css(gboolean enable);
static void on_system_high_contrast_changed(GObject *obj, GParamSpec *pspec, gpointer user_data);
static gboolean should_use_high_contrast(void);

/* Global application instance for callbacks that need it */
static GtkApplication *global_app = NULL;

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

/* Action handler for new profile (Ctrl+N) */
static void on_app_new_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win) return;
  signer_window_show_new_profile((SignerWindow*)win);
}

/* Action handler for import profile (Ctrl+I) */
static void on_app_import_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win) return;
  signer_window_show_import_profile((SignerWindow*)win);
}

/* Action handler for export/backup (Ctrl+E) */
static void on_app_export(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win) return;
  signer_window_show_backup((SignerWindow*)win);
}

/* Action handler for lock session (Ctrl+L) */
static void on_app_lock(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win) return;
  signer_window_lock_session((SignerWindow*)win);
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

/* Global high-contrast CSS provider (loaded once, added/removed as needed) */
static GtkCssProvider *high_contrast_provider = NULL;

/* Apply high-contrast styling to all windows */
static void apply_high_contrast_to_windows(GtkApplication *app, gboolean enable, SettingsHighContrastVariant variant) {
  GList *windows = gtk_application_get_windows(app);
  for (GList *l = windows; l != NULL; l = l->next) {
    GtkWidget *win = GTK_WIDGET(l->data);
    /* Remove existing high-contrast classes */
    gtk_widget_remove_css_class(win, "high-contrast");
    gtk_widget_remove_css_class(win, "inverted");
    gtk_widget_remove_css_class(win, "yellow-on-black");

    if (enable) {
      /* Add base high-contrast class */
      gtk_widget_add_css_class(win, "high-contrast");
      /* Add variant-specific class */
      switch (variant) {
        case SETTINGS_HC_INVERTED:
          gtk_widget_add_css_class(win, "inverted");
          break;
        case SETTINGS_HC_YELLOW_ON_BLACK:
          gtk_widget_add_css_class(win, "yellow-on-black");
          break;
        case SETTINGS_HC_DEFAULT:
        default:
          /* Default uses just .high-contrast, no additional class */
          break;
      }
    }
  }
}

/* Load or unload high-contrast CSS provider */
static void update_high_contrast_css(gboolean enable) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display) return;

  if (enable) {
    if (!high_contrast_provider) {
      high_contrast_provider = gtk_css_provider_new();
      gtk_css_provider_load_from_resource(high_contrast_provider, "/org/gnostr/signer/css/high-contrast.css");
    }
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(high_contrast_provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_debug("High-contrast CSS loaded");
  } else {
    if (high_contrast_provider) {
      gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(high_contrast_provider));
      g_debug("High-contrast CSS unloaded");
    }
  }
}

/* Determine if high contrast should be used based on:
 * 1. Force high contrast setting (user preference)
 * 2. Theme explicitly set to high-contrast
 * 3. System high contrast mode (GNOME/GTK accessibility setting)
 */
static gboolean should_use_high_contrast(void) {
  SettingsManager *sm = settings_manager_get_default();

  /* Check if user has forced high contrast */
  if (settings_manager_get_force_high_contrast(sm)) {
    g_debug("High contrast enabled via force-high-contrast setting");
    return TRUE;
  }

  /* Check if theme is explicitly set to high-contrast */
  SettingsTheme theme = settings_manager_get_theme(sm);
  if (theme == SETTINGS_THEME_HIGH_CONTRAST) {
    g_debug("High contrast enabled via theme=high-contrast");
    return TRUE;
  }

  /* Check system high contrast preference via AdwStyleManager */
  AdwStyleManager *style_manager = adw_style_manager_get_default();
  if (adw_style_manager_get_high_contrast(style_manager)) {
    g_debug("High contrast enabled via system accessibility setting");
    return TRUE;
  }

  return FALSE;
}

/* Callback for system high contrast changes (GNOME accessibility settings) */
static void on_system_high_contrast_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  (void)user_data;

  AdwStyleManager *style_manager = ADW_STYLE_MANAGER(obj);
  gboolean system_hc = adw_style_manager_get_high_contrast(style_manager);

  g_debug("System high contrast changed: %s", system_hc ? "enabled" : "disabled");

  /* Re-evaluate whether to use high contrast */
  gboolean use_hc = should_use_high_contrast();
  update_high_contrast_css(use_hc);

  /* Apply/remove high-contrast classes on windows */
  if (global_app) {
    SettingsManager *sm = settings_manager_get_default();
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    apply_high_contrast_to_windows(global_app, use_hc, hc_variant);
  }
}

/* Callback when onboarding finishes - present main window */
static void on_onboarding_finished(gboolean completed, gpointer user_data) {
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_debug("Onboarding finished: completed=%s", completed ? "true" : "false");

  /* Present the main window now */
  SignerWindow *win = signer_window_new(ADW_APPLICATION(app));

  /* Apply high-contrast class if needed (considers system, force, and theme settings) */
  gboolean use_high_contrast = should_use_high_contrast();
  if (use_high_contrast) {
    SettingsManager *sm = settings_manager_get_default();
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    gtk_widget_add_css_class(GTK_WIDGET(win), "high-contrast");
    switch (hc_variant) {
      case SETTINGS_HC_INVERTED:
        gtk_widget_add_css_class(GTK_WIDGET(win), "inverted");
        break;
      case SETTINGS_HC_YELLOW_ON_BLACK:
        gtk_widget_add_css_class(GTK_WIDGET(win), "yellow-on-black");
        break;
      case SETTINGS_HC_DEFAULT:
      default:
        break;
    }
  }

  gtk_window_present(GTK_WINDOW(win));
}

/* Callback when async secrets sync completes */
static void on_secrets_sync_complete(AccountsStore *as, gpointer user_data) {
  (void)user_data;
  STARTUP_TIME_END(STARTUP_PHASE_SECRETS);

  g_debug("Accounts sync with secrets completed, %u accounts loaded",
          as ? accounts_store_count(as) : 0);

  /* Generate and log startup report */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_READY);
  STARTUP_TIME_END(STARTUP_PHASE_READY);
  startup_timing_report();
}

/* Global deferred state for D-Bus connection */
static GDBusConnection *deferred_dbus_conn = NULL;
static guint deferred_dbus_signal_subscription = 0;

/* Callback when async D-Bus connection completes */
static void on_dbus_connected(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  (void)user_data;

  GError *err = NULL;
  deferred_dbus_conn = g_bus_get_finish(res, &err);

  if (err) {
    g_warning("Deferred D-Bus connection failed: %s", err->message);
    g_clear_error(&err);
    STARTUP_TIME_END(STARTUP_PHASE_DBUS);
    return;
  }

  g_debug("D-Bus connection established in deferred init");
  STARTUP_TIME_END(STARTUP_PHASE_DBUS);
}

/* Deferred initialization callback - runs after window is presented */
static gboolean deferred_init_cb(gpointer user_data) {
  (void)user_data;

  gint64 deferred_start = startup_timing_measure_start();

  STARTUP_TIME_BEGIN(STARTUP_PHASE_ACCOUNTS);

  /* Create account store (fast - just INI file) */
  AccountsStore *as = accounts_store_new();
  accounts_store_load(as);

  STARTUP_TIME_END(STARTUP_PHASE_ACCOUNTS);

  /* Async sync with secrets (slow - D-Bus to libsecret/Keychain) */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_SECRETS);
  accounts_store_sync_with_secrets_async(as, on_secrets_sync_complete, NULL);

  /* Start async D-Bus connection (for approval signal subscription) */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_DBUS);
  g_bus_get(G_BUS_TYPE_SESSION, NULL, on_dbus_connected, NULL);

  startup_timing_measure_end(deferred_start, "deferred-init-scheduled", 50);

  return G_SOURCE_REMOVE;  /* Don't repeat */
}

static void on_activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  STARTUP_TIME_MARK("activate-start");

  /* Initialize settings manager and apply theme preference after GTK is initialized */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_SETTINGS);
  SettingsManager *sm = settings_manager_get_default();
  SettingsTheme initial_theme = settings_manager_get_theme(sm);
  STARTUP_TIME_END(STARTUP_PHASE_SETTINGS);

  STARTUP_TIME_BEGIN(STARTUP_PHASE_THEME);
  apply_theme_preference(initial_theme);
  STARTUP_TIME_END(STARTUP_PHASE_THEME);

  /* Listen for theme setting changes */
  settings_manager_connect_changed(sm, "theme", on_theme_setting_changed, NULL);
  settings_manager_connect_changed(sm, "high-contrast-variant", on_theme_setting_changed, NULL);
  settings_manager_connect_changed(sm, "force-high-contrast", on_theme_setting_changed, NULL);

  /* Listen for system high contrast changes (GNOME accessibility settings) */
  AdwStyleManager *style_manager = adw_style_manager_get_default();
  g_signal_connect(style_manager, "notify::high-contrast",
                   G_CALLBACK(on_system_high_contrast_changed), NULL);

  /* Load application stylesheet from resources */
  gint64 css_start = startup_timing_measure_start();
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(prov, "/org/gnostr/signer/css/app.css");
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  g_object_unref(prov);
  startup_timing_measure_end(css_start, "css-load", 30);

  /* Load high-contrast CSS if needed (considers system, force, and theme settings) */
  gboolean use_high_contrast = should_use_high_contrast();
  if (use_high_contrast) {
    gint64 hc_start = startup_timing_measure_start();
    update_high_contrast_css(use_high_contrast);
    startup_timing_measure_end(hc_start, "high-contrast-css-load", 20);
  }

  /* Check if onboarding should be shown (first run) - this is fast INI file check */
  STARTUP_TIME_MARK("onboarding-check-start");
  if (onboarding_assistant_check_should_show()) {
    g_debug("First run detected, showing onboarding wizard");

    /* Ensure OnboardingAssistant type is registered */
    g_type_ensure(TYPE_ONBOARDING_ASSISTANT);

    OnboardingAssistant *onboarding = onboarding_assistant_new();
    g_debug("Created onboarding assistant: %p", onboarding);
    onboarding_assistant_set_on_finished(onboarding, on_onboarding_finished, app);
    gtk_application_add_window(app, GTK_WINDOW(onboarding));
    gtk_window_present(GTK_WINDOW(onboarding));

    /* Schedule deferred init */
    g_idle_add(deferred_init_cb, NULL);
    return;
  }

  /* Not first run - present main window directly */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_WINDOW);
  SignerWindow *win = signer_window_new(ADW_APPLICATION(app));
  g_debug("Created main window: %p", win);

  /* Apply high-contrast class if needed */
  if (use_high_contrast) {
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    gtk_widget_add_css_class(GTK_WIDGET(win), "high-contrast");
    switch (hc_variant) {
      case SETTINGS_HC_INVERTED:
        gtk_widget_add_css_class(GTK_WIDGET(win), "inverted");
        break;
      case SETTINGS_HC_YELLOW_ON_BLACK:
        gtk_widget_add_css_class(GTK_WIDGET(win), "yellow-on-black");
        break;
      case SETTINGS_HC_DEFAULT:
      default:
        break;
    }
  }

  gtk_window_present(GTK_WINDOW(win));
  STARTUP_TIME_END(STARTUP_PHASE_WINDOW);

  STARTUP_TIME_MARK("window-presented");

  /* Schedule deferred initialization for non-critical subsystems */
  g_idle_add(deferred_init_cb, NULL);
}

static void on_app_quit(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}

/* Apply theme preference to AdwStyleManager and handle high-contrast */
static void apply_theme_preference(SettingsTheme theme) {
  AdwStyleManager *style_manager = adw_style_manager_get_default();
  AdwColorScheme color_scheme;

  /* Determine if high contrast should be used (considers system, force, and theme settings) */
  gboolean use_high_contrast = should_use_high_contrast();

  switch (theme) {
    case SETTINGS_THEME_LIGHT:
      color_scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
      break;
    case SETTINGS_THEME_DARK:
      color_scheme = ADW_COLOR_SCHEME_FORCE_DARK;
      break;
    case SETTINGS_THEME_HIGH_CONTRAST:
      /* High contrast uses force-light as base for Black-on-White variant,
       * but the CSS overrides will take effect */
      color_scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
      break;
    case SETTINGS_THEME_SYSTEM:
    default:
      color_scheme = ADW_COLOR_SCHEME_DEFAULT;
      break;
  }

  adw_style_manager_set_color_scheme(style_manager, color_scheme);

  /* Load/unload high-contrast CSS */
  update_high_contrast_css(use_high_contrast);

  /* Apply/remove high-contrast classes on windows */
  if (global_app) {
    SettingsManager *sm = settings_manager_get_default();
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    apply_high_contrast_to_windows(global_app, use_high_contrast, hc_variant);
  }

  g_debug("Theme applied: %d -> color_scheme=%d, high_contrast=%s", theme, color_scheme,
          use_high_contrast ? "true" : "false");
}

/* Callback for theme setting changes */
static void on_theme_setting_changed(const gchar *key, gpointer user_data) {
  (void)user_data;
  if (g_strcmp0(key, "theme") != 0 &&
      g_strcmp0(key, "high-contrast-variant") != 0 &&
      g_strcmp0(key, "force-high-contrast") != 0) return;

  SettingsManager *sm = settings_manager_get_default();
  SettingsTheme theme = settings_manager_get_theme(sm);
  apply_theme_preference(theme);
}

/* Callback when re-running onboarding finishes - nothing special needed */
static void on_rerun_onboarding_finished(gboolean completed, gpointer user_data) {
  (void)user_data;
  g_debug("Re-run onboarding finished: completed=%s", completed ? "true" : "false");
  /* Main window already exists, just let onboarding close */
}

/* Action handler to re-run onboarding (from settings menu) */
static void on_app_show_onboarding(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);

  g_debug("Re-running onboarding wizard from settings");

  /* Reset onboarding state so it can be run again */
  onboarding_assistant_reset();

  /* Ensure OnboardingAssistant type is registered */
  g_type_ensure(TYPE_ONBOARDING_ASSISTANT);

  OnboardingAssistant *onboarding = onboarding_assistant_new();
  onboarding_assistant_set_on_finished(onboarding, on_rerun_onboarding_finished, app);

  /* Set as transient to active window */
  GtkWindow *active_win = gtk_application_get_active_window(app);
  if (active_win) {
    gtk_window_set_transient_for(GTK_WINDOW(onboarding), active_win);
  }

  gtk_application_add_window(app, GTK_WINDOW(onboarding));
  gtk_window_present(GTK_WINDOW(onboarding));
}

/* Action handler for showing keyboard shortcuts help overlay (Ctrl+?) */
static void on_app_show_shortcuts(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win) return;

  /* Load shortcuts window from resource */
  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/signer/ui/shortcuts-window.ui");
  GtkShortcutsWindow *shortcuts = GTK_SHORTCUTS_WINDOW(gtk_builder_get_object(builder, "shortcuts_window"));

  if (shortcuts) {
    gtk_window_set_transient_for(GTK_WINDOW(shortcuts), win);
    gtk_window_present(GTK_WINDOW(shortcuts));
  }

  g_object_unref(builder);
}

int main(int argc, char **argv) {
  /* Initialize startup timing first thing */
  startup_timing_init();
  STARTUP_TIME_BEGIN(STARTUP_PHASE_INIT);

  /* Initialize secure memory subsystem for handling sensitive data
   * (private keys, passwords, session tokens, etc.) */
  gnostr_secure_mem_init();

  /* Initialize internationalization before GTK */
  gn_i18n_init();

  g_set_prgname("gnostr-signer");
  AdwApplication *app = adw_application_new("org.gnostr.Signer", G_APPLICATION_DEFAULT_FLAGS);

  STARTUP_TIME_END(STARTUP_PHASE_INIT);

  /* Store global app reference for theme change callbacks */
  global_app = GTK_APPLICATION(app);

  /* Settings will be initialized in on_activate after GTK is ready */

  /* Install app actions */
  static const GActionEntry app_entries[] = {
    { "quit", on_app_quit, NULL, NULL, NULL },
    { "preferences", on_app_preferences, NULL, NULL, NULL },
    { "about", on_app_about, NULL, NULL, NULL },
    { "new-profile", on_app_new_profile, NULL, NULL, NULL },
    { "import-profile", on_app_import_profile, NULL, NULL, NULL },
    { "export", on_app_export, NULL, NULL, NULL },
    { "lock", on_app_lock, NULL, NULL, NULL },
    { "show-onboarding", on_app_show_onboarding, NULL, NULL, NULL },
    { "show-shortcuts", on_app_show_shortcuts, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);

  /* Register keyboard accelerators */
  const char *quit_accels[] = { "<Primary>q", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);

  const char *prefs_accels[] = { "<Primary>comma", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.preferences", prefs_accels);

  const char *new_profile_accels[] = { "<Primary>n", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.new-profile", new_profile_accels);

  const char *import_accels[] = { "<Primary>i", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.import-profile", import_accels);

  const char *export_accels[] = { "<Primary>e", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.export", export_accels);

  const char *lock_accels[] = { "<Primary>l", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.lock", lock_accels);

  const char *about_accels[] = { "F1", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.about", about_accels);

  const char *shortcuts_accels[] = { "<Primary>question", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.show-shortcuts", shortcuts_accels);

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  /* Shutdown secure memory subsystem - securely zeros and frees all remaining allocations */
  gnostr_secure_mem_shutdown();

  return status;
}
