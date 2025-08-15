#include <gtk/gtk.h>
#include "../accounts_store.h"
#include <gio/gio.h>
#include <nostr/nip55l/signer_ops.h>
#include <unistd.h>
#include <pwd.h>

typedef struct {
  AccountsStore *as;
  GtkWidget *page;
  GtkWidget *list;
  GtkWidget *add_id;
  GtkWidget *add_label;
  GtkCheckButton *group_head; /* head of radio-like group */
  /* Linked user UI */
  GtkWidget *linked_box;
  GtkWidget *linked_label;
  GtkWidget *link_btn;
  GtkWidget *clear_link_btn;
} SettingsUI;

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"

typedef struct {
  SettingsUI *ui;
  GtkWidget *entry_secret;
  GtkDropDown *ident_dd;
  GtkWidget *dialog;
  void (*on_success)(const char *identity, gpointer user_data);
  gpointer cb_user_data;
} ImportCtx;

typedef struct {
  SettingsUI *ui;
  GtkDropDown *ident_dd;
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
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Remove identity?\n%s", id);
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
  /* Update linked user section when active identity changes */
  extern void update_linked_user_ui(SettingsUI*);
  update_linked_user_ui(ui);
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
      gtk_check_button_set_active(GTK_CHECK_BUTTON(check), active && g_strcmp0(active, e->id) == 0);
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
  /* Ensure linked user section reflects latest state */
  extern void update_linked_user_ui(SettingsUI*);
  update_linked_user_ui(ui);
}

static void on_add_clicked(GtkButton *btn, gpointer user_data) {
  SettingsUI *ui = user_data;
  if (!ui || !ui->as) return;
  const gchar *id = gtk_editable_get_text(GTK_EDITABLE(ui->add_id));
  const gchar *label = gtk_editable_get_text(GTK_EDITABLE(ui->add_label));
  if (!id || !*id) {
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Identity id is required");
    gtk_alert_dialog_show(dlg, parent);
    g_object_unref(dlg);
    return;
  }
  if (!accounts_store_add(ui->as, id, label)) {
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Identity already exists: %s", id);
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

  /* Key material notice + actions */
  GtkWidget *secrets_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *secrets_note = gtk_label_new("Private keys are held in memory for this session only. Only public npubs are shown.");
  gtk_label_set_wrap(GTK_LABEL(secrets_note), TRUE);
  gtk_widget_set_hexpand(secrets_note, TRUE);
  GtkWidget *btn_import = gtk_button_new_with_label("Import Key");
  GtkWidget *btn_clear  = gtk_button_new_with_label("Clear Key");
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
  gtk_entry_set_placeholder_text(GTK_ENTRY(ui->add_id), "Identity selector (key_id or npub1...)");
  GtkWidget *add_btn = gtk_button_new_with_label("Add Identity");
  g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), ui);
  gtk_box_append(GTK_BOX(form), ui->add_id);
  gtk_box_append(GTK_BOX(form), ui->add_label);
  gtk_box_append(GTK_BOX(form), add_btn);
  gtk_box_append(GTK_BOX(box), form);

  /* Linked user section */
  ui->linked_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *linked_title = gtk_label_new("Linked user:");
  ui->linked_label = gtk_label_new("(none)");
  ui->link_btn = gtk_button_new_with_label("Link current user");
  ui->clear_link_btn = gtk_button_new_with_label("Clear link");
  gtk_box_append(GTK_BOX(ui->linked_box), linked_title);
  gtk_box_append(GTK_BOX(ui->linked_box), ui->linked_label);
  gtk_widget_set_hexpand(ui->linked_label, TRUE);
  gtk_widget_set_halign(ui->linked_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(ui->linked_box), ui->link_btn);
  gtk_box_append(GTK_BOX(ui->linked_box), ui->clear_link_btn);
  gtk_box_append(GTK_BOX(box), ui->linked_box);

  /* Handlers */
  extern void on_link_current_user_clicked(GtkButton*, gpointer);
  extern void on_clear_link_clicked(GtkButton*, gpointer);
  g_signal_connect(ui->link_btn, "clicked", G_CALLBACK(on_link_current_user_clicked), ui);
  g_signal_connect(ui->clear_link_btn, "clicked", G_CALLBACK(on_clear_link_clicked), ui);

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

/* ---- Key Import/Clear helpers (C callbacks) ---- */

/* ---- Linked user helpers ---- */
static gchar *get_active_identity(SettingsUI *ui){
  if (!ui || !ui->as) return NULL;
  gchar *active=NULL; accounts_store_get_active(ui->as, &active); return active;
}

void update_linked_user_ui(SettingsUI *ui){
  if (!ui) return;
  g_autofree gchar *active = get_active_identity(ui);
  gboolean has_active = (active && *active);
  gtk_widget_set_sensitive(ui->link_btn, has_active);
  gtk_widget_set_sensitive(ui->clear_link_btn, has_active);
  if (!has_active){
    gtk_label_set_text(GTK_LABEL(ui->linked_label), "(none)");
    return;
  }
  int has_owner=0; uid_t uid=0; char *uname=NULL;
  int rc = nostr_nip55l_get_owner(active, &has_owner, &uid, &uname);
  if (rc==0 && has_owner){
    gchar buf[256]; g_snprintf(buf, sizeof buf, "%s (UID %u)", uname?uname:"?", (unsigned)uid);
    gtk_label_set_text(GTK_LABEL(ui->linked_label), buf);
  } else {
    gtk_label_set_text(GTK_LABEL(ui->linked_label), "(none)");
  }
  if (uname) free(uname);
}

void on_link_current_user_clicked(GtkButton *btn, gpointer user_data){
  (void)btn; SettingsUI *ui = user_data; if (!ui) return;
  g_autofree gchar *active = get_active_identity(ui); if (!active || !*active) return;
  uid_t uid = getuid();
  struct passwd *pw = getpwuid(uid);
  const char *uname = (pw && pw->pw_name) ? pw->pw_name : NULL;
  int rc = nostr_nip55l_set_owner(active, uid, uname);
  if (rc != 0){
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Failed to link user (rc=%d)", rc);
    gtk_alert_dialog_show(dlg, parent); g_object_unref(dlg);
  }
  update_linked_user_ui(ui);
}

void on_clear_link_clicked(GtkButton *btn, gpointer user_data){
  (void)btn; SettingsUI *ui = user_data; if (!ui) return;
  g_autofree gchar *active = get_active_identity(ui); if (!active || !*active) return;
  int rc = nostr_nip55l_clear_owner(active);
  if (rc != 0){
    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Failed to clear link (rc=%d)", rc);
    gtk_alert_dialog_show(dlg, parent); g_object_unref(dlg);
  }
  update_linked_user_ui(ui);
}

static void import_call_done(GObject *src, GAsyncResult *res, gpointer user_data){
  ImportCtx *ctx = (ImportCtx*)user_data;
  GError *e=NULL; GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
  GtkWindow *parent = NULL;
  if (ctx && ctx->dialog) parent = GTK_WINDOW(gtk_widget_get_root(ctx->dialog));
  if (!parent && ctx && ctx->ui && ctx->ui->page) parent = GTK_WINDOW(gtk_widget_get_root(ctx->ui->page));
  if (e){
    const gchar *msg = e->message ? e->message : "unknown error";
    GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed: %s\nEnsure daemon was started with NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1", msg);
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad); g_clear_error(&e);
  } else if (ret){
    gboolean ok=FALSE; const char *npub=NULL; g_variant_get(ret, "(bs)", &ok, &npub); g_variant_unref(ret);
    if (ok && ctx->on_success) {
      const char *chosen = NULL;
      if (npub && *npub) chosen = npub; /* prefer returned npub */
      else {
        gchar *ident = dropdown_get_selected_string(ctx->ident_dd);
        chosen = ident ? ident : "";
        /* We leak no memory by freeing after callback */
        if (ident) g_free(ident);
      }
      ctx->on_success(chosen, ctx->cb_user_data);
    } else if (ok && ctx->ui && ctx->ui->as) {
      /* Default settings dialog path: update accounts store and refresh UI */
      const char *id = (npub && *npub) ? npub : NULL;
      if (!id) {
        gchar *ident = dropdown_get_selected_string(ctx->ident_dd);
        id = ident ? ident : NULL;
        if (ident) g_free(ident);
      }
      if (id && *id) {
        AccountsStore *as = ctx->ui->as;
        if (accounts_store_add(as, id, NULL)) {
          accounts_store_set_active(as, id);
          accounts_store_save(as);
        }
        /* Even if it already existed, ensure active and refresh */
        accounts_store_set_active(as, id);
        accounts_store_save(as);
        extern void gnostr_settings_page_refresh(GtkWidget*, AccountsStore*);
        if (ctx->ui->page) gnostr_settings_page_refresh(ctx->ui->page, as);
      }
    }
    GtkAlertDialog *ad = gtk_alert_dialog_new(ok?"Key stored securely":"Import failed");
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
  g_autofree gchar *identity = dropdown_get_selected_string(ctx->ident_dd);
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ctx->dialog));
  if (!secret || !*secret) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Private key is required");
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
  g_dbus_connection_call(bus, SIGNER_NAME, SIGNER_PATH, "org.nostr.Signer", "StoreKey",
                         g_variant_new("(ss)", secret, identity ? identity : ""), G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, import_call_done, ctx);
  g_object_unref(bus);
}

void on_import_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  SettingsUI *ui = (SettingsUI*)user_data;
  if (!ui || !ui->as) return;
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *lbl = gtk_label_new("Paste hex private key (64 hex) or nsec. It will be stored securely.");
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nsec1... or 64-hex...");

  GtkWidget *acct_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *acct_lbl = gtk_label_new("Identity:");
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
  GtkDropDown *ident_dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
  if (to_sel != GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(ident_dd, to_sel);
  else if (gtk_drop_down_get_selected(ident_dd) == GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(ident_dd, 0);
  gtk_box_append(GTK_BOX(acct_row), acct_lbl);
  gtk_box_append(GTK_BOX(acct_row), GTK_WIDGET(ident_dd));

  gtk_box_append(GTK_BOX(content), lbl);
  gtk_box_append(GTK_BOX(content), entry);
  gtk_box_append(GTK_BOX(content), acct_row);

  GtkWidget *win = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  GtkWidget *hb = gtk_header_bar_new();
  GtkWidget *title = gtk_label_new("Import Key");
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
  ctx->ui = ui; ctx->entry_secret = entry; ctx->ident_dd = ident_dd; ctx->dialog = win; ctx->on_success=NULL; ctx->cb_user_data=NULL;
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_import_cancel_clicked), ctx);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_import_ok_clicked), ctx);
}

/* Exported helper: open Import Key dialog programmatically */
/* Extended helper with success callback */
void gnostr_settings_open_import_dialog_with_callback(GtkWindow *parent, AccountsStore *as, const char *initial_identity,
                                                      void (*on_success)(const char*, gpointer), gpointer user_data){
  if (!parent || !as) return;
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *lbl = gtk_label_new("Paste hex private key (64 hex) or nsec. It will be stored securely.");
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nsec1... or 64-hex...");

  GtkWidget *acct_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *acct_lbl = gtk_label_new("Identity:");
  GtkStringList *sl = gtk_string_list_new(NULL);
  guint to_sel = GTK_INVALID_LIST_POSITION;
  gchar *active = NULL; accounts_store_get_active(as, &active);
  GPtrArray *items = accounts_store_list(as);
  if (items){
    for (guint i=0;i<items->len;i++){
      AccountEntry *e = g_ptr_array_index(items, i);
      gtk_string_list_append(sl, e->id);
      if ((initial_identity && g_strcmp0(initial_identity, e->id)==0) || (!initial_identity && active && g_strcmp0(active, e->id)==0)) to_sel = i;
      g_free(e->id); g_free(e->label); g_free(e);
    }
    g_ptr_array_free(items, TRUE);
  }
  g_free(active);
  GtkDropDown *ident_dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
  if (to_sel != GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(ident_dd, to_sel);
  else if (gtk_drop_down_get_selected(ident_dd) == GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(ident_dd, 0);
  gtk_box_append(GTK_BOX(acct_row), acct_lbl);
  gtk_box_append(GTK_BOX(acct_row), GTK_WIDGET(ident_dd));

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
  ctx->entry_secret = entry; ctx->ident_dd = ident_dd; ctx->dialog = win;
  ctx->on_success = on_success; ctx->cb_user_data = user_data;
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_import_cancel_clicked), ctx);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_import_ok_clicked), ctx);
}



static void clear_call_done(GObject *src, GAsyncResult *res, gpointer user_data){
  ClearCtx *ctx = (ClearCtx*)user_data;
  GError *e=NULL; GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ctx->ui->page));
  if (e){
    GtkAlertDialog *ad = gtk_alert_dialog_new("Clear failed: %s\nEnsure daemon was started with NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1", e->message);
    gtk_alert_dialog_show(ad, parent);
    g_object_unref(ad); g_clear_error(&e);
  } else if (ret){
    gboolean ok=FALSE; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret);
    GtkAlertDialog *ad = gtk_alert_dialog_new(ok?"Key cleared":"Clear failed");
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
  g_autofree gchar *identity = dropdown_get_selected_string(ctx->ident_dd);
  if (!identity || !*identity){ if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog)); g_free(ctx); return; }
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to get session bus: %s", err?err->message:"unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(ctx->ui->page)));
    g_object_unref(ad); if (err) g_clear_error(&err);
    if (ctx->dialog) gtk_window_destroy(GTK_WINDOW(ctx->dialog));
    g_free(ctx);
    return;
  }
  g_dbus_connection_call(bus, SIGNER_NAME, SIGNER_PATH, "org.nostr.Signer", "ClearKey",
                         g_variant_new("(s)", identity), G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, clear_call_done, ctx);
  g_object_unref(bus);
}

void on_clear_clicked(GtkButton *btn, gpointer user_data){
  (void)btn;
  SettingsUI *ui = (SettingsUI*)user_data;
  if (!ui || !ui->as) return;
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(ui->page));

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lbl = gtk_label_new("Identity:");
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
  g_free(active);
  GtkDropDown *ident_dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
  if (to_sel != GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(ident_dd, to_sel);
  else if (gtk_drop_down_get_selected(ident_dd) == GTK_INVALID_LIST_POSITION) gtk_drop_down_set_selected(ident_dd, 0);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(ident_dd));

  GtkWidget *win = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  GtkWidget *hb = gtk_header_bar_new();
  GtkWidget *title = gtk_label_new("Clear Key");
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
  ctx->ui = ui; ctx->ident_dd = ident_dd; ctx->dialog = win;
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_clear_cancel_clicked), ctx);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_clear_ok_clicked), ctx);
}
