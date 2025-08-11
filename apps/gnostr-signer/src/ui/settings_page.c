#include <gtk/gtk.h>
#include "../accounts_store.h"
#include <gio/gio.h>

typedef struct {
  AccountsStore *as;
  GtkWidget *page;
  GtkWidget *list;
  GtkWidget *add_id;
  GtkWidget *add_label;
  GtkCheckButton *group_head; /* head of radio-like group */
} SettingsUI;

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"

typedef struct {
  SettingsUI *ui;
  GtkWidget *entry_secret;
  GtkDropDown *acct_dd;
  GtkWidget *dialog;
  void (*on_success)(const char *account, gpointer user_data);
  gpointer cb_user_data;
} ImportCtx;

typedef struct {
  SettingsUI *ui;
  GtkDropDown *acct_dd;
  GtkWidget *dialog;
} ClearCtx;

/* Helper to extract selected string from GtkDropDown backed by GtkStringList */
static gchar *dropdown_get_selected_string(GtkDropDown *dd){
  if (!dd) return NULL;
  guint idx = gtk_drop_down_get_selected(dd);
  if (idx == GTK_INVALID_LIST_POSITION) return NULL;
  GListModel *model = gtk_drop_down_get_model(dd);
  if (!model) return NULL;
  GObject *item = g_list_model_get_item(model, idx);
  if (!item) return NULL;
  const char *s = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
  gchar *dup = g_strdup(s);
  g_object_unref(item);
  return dup;
}

static void clear_list(GtkWidget *list) {
  GtkWidget *child = gtk_widget_get_first_child(list);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(list), child);
    child = next;
  }
}

typedef struct { SettingsUI *ui; gchar *id; } RemoveCtx;

static void on_remove_confirm(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  RemoveCtx *rc = user_data;
  GError *err = NULL;
  int resp = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &err);
  if (err) { g_warning("Remove confirm failed: %s", err->message); g_clear_error(&err); }
  if (resp == 0 && rc && rc->ui && rc->ui->as && rc->id) {
    accounts_store_remove(rc->ui->as, rc->id);
    accounts_store_save(rc->ui->as);
    extern void gnostr_settings_page_refresh(GtkWidget*, AccountsStore*);
    if (rc->ui->page) gnostr_settings_page_refresh(rc->ui->page, rc->ui->as);
  }
  if (rc) { g_free(rc->id); g_free(rc); }
}

static void on_remove_clicked(GtkButton *btn, gpointer user_data) {
  SettingsUI *ui = user_data;
  const gchar *id = g_object_get_data(G_OBJECT(btn), "id");
  if (!ui || !ui->as || !id) return;
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Remove account?\n%s", id);
  gtk_alert_dialog_set_buttons(dlg, (const char * const[]){"Remove", "Cancel", NULL});
  RemoveCtx *rc = g_new0(RemoveCtx, 1);
  rc->ui = ui;
  rc->id = g_strdup(id);
  gtk_alert_dialog_choose(dlg, parent, NULL, on_remove_confirm, rc);
}

static void on_radio_toggled(GtkToggleButton *btn, gpointer user_data) {
  if (!gtk_toggle_button_get_active(btn)) return;
  SettingsUI *ui = user_data;
  const gchar *id = g_object_get_data(G_OBJECT(btn), "id");
  if (!ui || !ui->as || !id) return;
  accounts_store_set_active(ui->as, id);
  accounts_store_save(ui->as);
}

void gnostr_settings_page_refresh(GtkWidget *page, AccountsStore *as) {
  SettingsUI *ui = g_object_get_data(G_OBJECT(page), "settings_ui");
  if (!ui) return;
  ui->as = as;
  clear_list(ui->list);
  gchar *active = NULL;
  accounts_store_get_active(as, &active);
  GPtrArray *items = accounts_store_list(as);
  ui->group_head = NULL;
  if (items) {
    for (guint i = 0; i < items->len; i++) {
      AccountEntry *e = g_ptr_array_index(items, i);
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *check = gtk_check_button_new();
      if (ui->group_head) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(check), ui->group_head);
      } else {
        ui->group_head = GTK_CHECK_BUTTON(check);
      }
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), active && g_strcmp0(active, e->id) == 0);
      g_object_set_data_full(G_OBJECT(check), "id", g_strdup(e->id), g_free);
      g_signal_connect(check, "toggled", G_CALLBACK(on_radio_toggled), ui);
      gtk_widget_set_margin_end(check, 8);

      gchar *label_text = NULL;
      if (e->label && *e->label)
        label_text = g_strdup_printf("%s — %s", e->label, e->id);
      else
        label_text = g_strdup(e->id);
      GtkWidget *lbl = gtk_label_new(label_text);
      gtk_widget_set_hexpand(lbl, TRUE);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);

      GtkWidget *btn = gtk_button_new_with_label("Remove");
      g_object_set_data_full(G_OBJECT(btn), "id", g_strdup(e->id), g_free);
      g_signal_connect(btn, "clicked", G_CALLBACK(on_remove_clicked), ui);

      gtk_box_append(GTK_BOX(row), check);
      gtk_box_append(GTK_BOX(row), lbl);
      gtk_box_append(GTK_BOX(row), btn);
      gtk_list_box_append(GTK_LIST_BOX(ui->list), row);
      g_free(label_text);
      g_free(e->id); g_free(e->label); g_free(e);
    }
    g_ptr_array_free(items, TRUE);
  }
  g_free(active);
}

static void on_add_clicked(GtkButton *btn, gpointer user_data) {
  SettingsUI *ui = user_data;
  if (!ui || !ui->as) return;
  const gchar *id = gtk_editable_get_text(GTK_EDITABLE(ui->add_id));
  const gchar *label = gtk_editable_get_text(GTK_EDITABLE(ui->add_label));
  if (!id || !*id) {
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Account id is required");
    gtk_alert_dialog_show(dlg, parent);
    g_object_unref(dlg);
    return;
  }
  if (!accounts_store_add(ui->as, id, label)) {
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Account already exists: %s", id);
    gtk_alert_dialog_show(dlg, parent);
    g_object_unref(dlg);
    return;
  }
  accounts_store_save(ui->as);
  extern void gnostr_settings_page_refresh(GtkWidget*, AccountsStore*);
  if (ui->page) gnostr_settings_page_refresh(ui->page, ui->as);
  gtk_editable_set_text(GTK_EDITABLE(ui->add_id), "");
  gtk_editable_set_text(GTK_EDITABLE(ui->add_label), "");
}

GtkWidget *gnostr_settings_page_new(AccountsStore *as) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  SettingsUI *ui = g_new0(SettingsUI, 1);
  g_object_set_data_full(G_OBJECT(box), "settings_ui", ui, g_free);
  ui->page = box;

  GtkWidget *title = gtk_label_new("Settings");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);

  /* Secrets notice + actions */
  GtkWidget *secrets_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *secrets_note = gtk_label_new("Secrets are kept in memory for this session only. Only public npubs are shown.");
  gtk_label_set_wrap(GTK_LABEL(secrets_note), TRUE);
  gtk_widget_set_hexpand(secrets_note, TRUE);
  GtkWidget *btn_import = gtk_button_new_with_label("Import Secret");
  GtkWidget *btn_clear  = gtk_button_new_with_label("Clear Secret");
  gtk_box_append(GTK_BOX(secrets_row), secrets_note);
  gtk_box_append(GTK_BOX(secrets_row), btn_import);
  gtk_box_append(GTK_BOX(secrets_row), btn_clear);
  gtk_box_append(GTK_BOX(box), secrets_row);

  /* Add form */
  GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  ui->add_label = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(ui->add_label), "");
  gtk_entry_set_placeholder_text(GTK_ENTRY(ui->add_label), "Label (optional)");
  ui->add_id = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(ui->add_id), "Account id (npub, label, ...) ");
  GtkWidget *add_btn = gtk_button_new_with_label("Add");
  g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), ui);
  gtk_box_append(GTK_BOX(form), ui->add_label);
  gtk_box_append(GTK_BOX(form), ui->add_id);
  gtk_box_append(GTK_BOX(form), add_btn);
  gtk_box_append(GTK_BOX(box), form);

  /* Accounts list */
  GtkWidget *list = gtk_list_box_new();
  ui->list = list;
  gtk_box_append(GTK_BOX(box), list);

  ui->as = as;
  gnostr_settings_page_refresh(box, as);

  /* Wire secret actions */
  extern void on_import_clicked(GtkButton*, gpointer);
  extern void on_clear_clicked(GtkButton*, gpointer);
  g_signal_connect(btn_import, "clicked", G_CALLBACK(on_import_clicked), ui);
  g_signal_connect(btn_clear,  "clicked", G_CALLBACK(on_clear_clicked), ui);

  return box;
}

GtkWidget *gnostr_settings_page_new(AccountsStore *as);

/* ---- Secret Import/Clear helpers (C callbacks) ---- */

static void import_call_done(GObject *src, GAsyncResult *res, gpointer user_data){
  ImportCtx *ctx = (ImportCtx*)user_data;
  GError *e=NULL; GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
  GtkWindow *parent = NULL;
  if (ctx && ctx->dialog) parent = GTK_WINDOW(gtk_widget_get_root(ctx->dialog));
  if (!parent && ctx && ctx->ui && ctx->ui->page) parent = GTK_WINDOW(gtk_widget_get_root(ctx->ui->page));
  if (e){
    const gchar *msg = e->message ? e->message : "unknown error";
    GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed: %s\nEnsure daemon was started with NOSTR_SIGNER_ALLOW_SECRET_MUTATIONS=1", msg);
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad); g_clear_error(&e);
  } else if (ret){
    gboolean ok=FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret);
    if (ok && ctx->on_success) {
      gchar *acct = dropdown_get_selected_string(ctx->acct_dd);
      ctx->on_success(acct ? acct : "", ctx->cb_user_data);
      g_free(acct);
    }
    GtkAlertDialog *ad = gtk_alert_dialog_new(ok?"Secret imported (session only)":"Import failed");
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad);
  }
  if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
  g_free(ctx);
}

static void on_import_cancel_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  ImportCtx *ctx = (ImportCtx*)user_data;
  if (!ctx) return;
  if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
  g_free(ctx);
}

static void on_import_ok_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  ImportCtx *ctx = (ImportCtx*)user_data;
  if (!ctx) return;
  const gchar *secret = gtk_editable_get_text(GTK_EDITABLE(ctx->entry_secret));
  g_autofree gchar *account = dropdown_get_selected_string(ctx->acct_dd);
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ctx->dialog));
  if (!secret || !*secret || !account || !*account) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Secret and account are required");
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad);
    if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    g_free(ctx);
    return;
  }
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to get session bus: %s", err?err->message:"unknown");
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad); if (err) g_clear_error(&err);
    if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    g_free(ctx);
    return;
  }
  g_dbus_connection_call(bus, SIGNER_NAME, SIGNER_PATH, "org.nostr.Signer", "StoreSecret",
                         g_variant_new("(ss)", secret, account), G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, import_call_done, ctx);
  g_object_unref(bus);
}

void on_import_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  SettingsUI *ui = (SettingsUI*)user_data;
  if (!ui || !ui->as) return;
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *lbl = gtk_label_new("Paste hex private key (64 hex) or nsec:\nIt will be kept in memory for this session only.");
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nsec1... or 64-hex...");

  GtkWidget *acct_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *acct_lbl = gtk_label_new("Account:");
  GtkStringList *sl = gtk_string_list_new(NULL);
  guint to_sel = GTK_INVALID_LIST_POSITION;
  gchar *active = NULL; accounts_store_get_active(ui->as, &active);
  GPtrArray *items = accounts_store_list(ui->as);
  if (items){
    for (guint i=0;i<items->len;i++){
      AccountEntry *e = g_ptr_array_index(items, i);
      gtk_string_list_append(sl, e->id);
      if (active && g_strcmp0(active, e->id)==0) to_sel = i;
      g_free(e->id); g_free(e->label); g_free(e);
    }
    g_ptr_array_free(items, TRUE);
  }
  GtkDropDown *acct_dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
  if (to_sel != GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(acct_dd, to_sel);
  else if (gtk_drop_down_get_selected(acct_dd) == GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(acct_dd, 0);
  gtk_box_append(GTK_BOX(acct_row), acct_lbl);
  gtk_box_append(GTK_BOX(acct_row), GTK_WIDGET(acct_dd));

  gtk_box_append(GTK_BOX(content), lbl);
  gtk_box_append(GTK_BOX(content), entry);
  gtk_box_append(GTK_BOX(content), acct_row);

  GtkWidget *win = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  GtkWidget *hb = gtk_header_bar_new();
  GtkWidget *title = gtk_label_new("Import Secret");
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), title);
  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *btn_ok = gtk_button_new_with_label("Import");
  gtk_widget_add_css_class(btn_ok, "suggested-action");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), btn_cancel);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_ok);
  gtk_window_set_titlebar(GTK_WINDOW(win), hb);
  gtk_window_set_child(GTK_WINDOW(win), content);
  gtk_window_present(GTK_WINDOW(win));

  ImportCtx *ctx = g_new0(ImportCtx, 1);
  ctx->ui = ui; ctx->entry_secret = entry; ctx->acct_dd = acct_dd; ctx->dialog = win; ctx->on_success=NULL; ctx->cb_user_data=NULL;
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_import_cancel_clicked), ctx);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_import_ok_clicked), ctx);
}

/* Exported helper: open Import Secret dialog programmatically */
/* Extended helper with success callback */
void gnostr_settings_open_import_dialog_with_callback(GtkWindow *parent, AccountsStore *as, const char *initial_account,
                                                      void (*on_success)(const char*, gpointer), gpointer user_data){
  if (!parent || !as) return;
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *lbl = gtk_label_new("Paste hex private key (64 hex) or nsec:\nIt will be kept in memory for this session only.");
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nsec1... or 64-hex...");

  GtkWidget *acct_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *acct_lbl = gtk_label_new("Account:");
  GtkStringList *sl = gtk_string_list_new(NULL);
  guint to_sel = GTK_INVALID_LIST_POSITION;
  gchar *active = NULL; accounts_store_get_active(as, &active);
  GPtrArray *items = accounts_store_list(as);
  if (items){
    for (guint i=0;i<items->len;i++){
      AccountEntry *e = g_ptr_array_index(items, i);
      gtk_string_list_append(sl, e->id);
      if ((initial_account && g_strcmp0(initial_account, e->id)==0) || (!initial_account && active && g_strcmp0(active, e->id)==0)) to_sel = i;
      g_free(e->id); g_free(e->label); g_free(e);
    }
    g_ptr_array_free(items, TRUE);
  }
  GtkDropDown *acct_dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
  if (to_sel != GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(acct_dd, to_sel);
  else if (gtk_drop_down_get_selected(acct_dd) == GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(acct_dd, 0);
  gtk_box_append(GTK_BOX(acct_row), acct_lbl);
  gtk_box_append(GTK_BOX(acct_row), GTK_WIDGET(acct_dd));

  gtk_box_append(GTK_BOX(content), lbl);
  gtk_box_append(GTK_BOX(content), entry);
  gtk_box_append(GTK_BOX(content), acct_row);

  GtkWidget *win = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  GtkWidget *hb = gtk_header_bar_new();
  GtkWidget *title = gtk_label_new("Import Secret");
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), title);
  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *btn_ok = gtk_button_new_with_label("Import");
  gtk_widget_add_css_class(btn_ok, "suggested-action");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), btn_cancel);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_ok);
  gtk_window_set_titlebar(GTK_WINDOW(win), hb);
  gtk_window_set_child(GTK_WINDOW(win), content);
  gtk_window_present(GTK_WINDOW(win));

  ImportCtx *ctx = g_new0(ImportCtx, 1);
  ctx->ui = NULL; /* not needed for this path */
  ctx->entry_secret = entry; ctx->acct_dd = acct_dd; ctx->dialog = win;
  ctx->on_success = on_success; ctx->cb_user_data = user_data;
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_import_cancel_clicked), ctx);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_import_ok_clicked), ctx);
}

/* Backwards-compatible wrapper */
void gnostr_settings_open_import_dialog(GtkWindow *parent, AccountsStore *as, const char *initial_account){
  gnostr_settings_open_import_dialog_with_callback(parent, as, initial_account, NULL, NULL);
}

static void clear_call_done(GObject *src, GAsyncResult *res, gpointer user_data){
  ClearCtx *ctx = (ClearCtx*)user_data;
  GError *e=NULL; GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ctx->ui->page));
  if (e){
    GtkAlertDialog *ad = gtk_alert_dialog_new("Clear failed: %s\nEnsure daemon was started with NOSTR_SIGNER_ALLOW_SECRET_MUTATIONS=1", e->message);
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad); g_clear_error(&e);
  } else if (ret){
    gboolean ok=FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret);
    GtkAlertDialog *ad = gtk_alert_dialog_new(ok?"Secret cleared":"Clear failed");
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad);
  }
  if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
  g_free(ctx);
}

static void on_clear_cancel_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  ClearCtx *ctx = (ClearCtx*)user_data;
  if (!ctx) return;
  if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
  g_free(ctx);
}

static void on_clear_ok_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  ClearCtx *ctx = (ClearCtx*)user_data;
  if (!ctx) return;
  g_autofree gchar *account = dropdown_get_selected_string(ctx->acct_dd);
  if (!account || !*account){ if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog)); g_free(ctx); return; }
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to get session bus: %s", err?err->message:"unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(ctx->ui->page)));
    g_object_unref(ad); if (err) g_clear_error(&err);
    if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    g_free(ctx);
    return;
  }
  g_dbus_connection_call(bus, SIGNER_NAME, SIGNER_PATH, "org.nostr.Signer", "ClearSecret",
                         g_variant_new("(s)", account), G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, clear_call_done, ctx);
  g_object_unref(bus);
}

void on_clear_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  SettingsUI *ui = (SettingsUI*)user_data;
  if (!ui || !ui->as) return;
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lbl = gtk_label_new("Account:");
  GtkStringList *sl = gtk_string_list_new(NULL);
  guint to_sel = GTK_INVALID_LIST_POSITION;
  gchar *active = NULL; accounts_store_get_active(ui->as, &active);
  GPtrArray *items = accounts_store_list(ui->as);
  if (items){
    for (guint i=0;i<items->len;i++){
      AccountEntry *e = g_ptr_array_index(items, i);
      gtk_string_list_append(sl, e->id);
      if (active && g_strcmp0(active, e->id)==0) to_sel = i;
      g_free(e->id); g_free(e->label); g_free(e);
    }
    g_ptr_array_free(items, TRUE);
  }
  GtkDropDown *acct_dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
  if (to_sel != GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(acct_dd, to_sel);
  else if (gtk_drop_down_get_selected(acct_dd) == GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(acct_dd, 0);
  gtk_box_append(GTK_BOX(content), lbl);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(acct_dd));

  GtkWidget *win = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  GtkWidget *hb = gtk_header_bar_new();
  GtkWidget *title = gtk_label_new("Clear Secret");
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), title);
  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *btn_ok = gtk_button_new_with_label("Clear");
  gtk_widget_add_css_class(btn_ok, "destructive-action");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), btn_cancel);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_ok);
  gtk_window_set_titlebar(GTK_WINDOW(win), hb);
  gtk_window_set_child(GTK_WINDOW(win), content);
  gtk_window_present(GTK_WINDOW(win));

  ClearCtx *ctx = g_new0(ClearCtx, 1);
  ctx->ui = ui; ctx->acct_dd = acct_dd; ctx->dialog = win;
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_clear_cancel_clicked), ctx);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_clear_ok_clicked), ctx);
}
