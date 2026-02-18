/* sheet-select-account.c - Account selection and management dialog implementation */
#include "sheet-select-account.h"
#include "sheet-import-key.h"
#include "../app-resources.h"
#include "../../accounts_store.h"
#include "../../secret_store.h"
#include "../../settings_manager.h"

/* Forward declarations */
static void on_import_pubkey_submit(GtkButton *btn, gpointer user_data);
static void populate_list(SheetSelectAccount *self);

struct _SheetSelectAccount {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_add_new;
  GtkButton *btn_import;
  GtkButton *btn_import_pubkey;
  GtkListBox *list_accounts;
  GtkLabel *lbl_empty;

  /* State */
  AccountsStore *accounts;
  SheetSelectAccountCb on_select;
  gpointer on_select_ud;
  gchar *selected_npub;
};

G_DEFINE_TYPE(SheetSelectAccount, sheet_select_account, ADW_TYPE_DIALOG)

/* Account row data */
typedef struct {
  gchar *npub;
  gchar *label;
  gboolean has_secret;
  gboolean watch_only;
} AccountRowData;

static void account_row_data_free(AccountRowData *data) {
  if (!data) return;
  g_free(data->npub);
  g_free(data->label);
  g_free(data);
}

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSelectAccount *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
}

static void on_row_remove(GtkButton *btn, gpointer user_data) {
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
  SheetSelectAccount *self = g_object_get_data(G_OBJECT(row), "sheet-self");

  AccountRowData *data = g_object_get_data(G_OBJECT(row), "account-data");
  if (!data || !self) return;

  /* Remove from stores */
  accounts_store_remove(self->accounts, data->npub, NULL);
  accounts_store_save(self->accounts, NULL);

  /* Also remove from secret store */
  secret_store_remove(data->npub);

  /* Remove row from list */
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));
  gtk_list_box_remove(list, GTK_WIDGET(row));

  /* Show empty label if no accounts */
  if (accounts_store_count(self->accounts) == 0) {
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_empty), TRUE);
  }
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  SheetSelectAccount *self = user_data;

  AccountRowData *data = g_object_get_data(G_OBJECT(row), "account-data");
  if (!data) return;

  /* Set as active */
  accounts_store_set_active(self->accounts, data->npub, NULL);
  accounts_store_save(self->accounts, NULL);

  /* Invoke callback */
  if (self->on_select) {
    self->on_select(data->npub, self->on_select_ud);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static GtkWidget *create_account_row(SheetSelectAccount *self, const gchar *npub,
                                     const gchar *label, gboolean has_secret,
                                     gboolean watch_only, gboolean is_active) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Display label or truncated npub */
  if (label && *label) {
    /* Add watch-only indicator to title if applicable */
    if (watch_only) {
      g_autofree gchar *title = g_strdup_printf("%s (Watch Only)", label);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    } else {
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
    }
    /* Show truncated npub as subtitle */
    if (npub && strlen(npub) > 16) {
      g_autofree gchar *sub = g_strdup_printf("%.12s...%.4s", npub, npub + strlen(npub) - 4);
      adw_action_row_set_subtitle(row, sub);
    } else {
      adw_action_row_set_subtitle(row, npub);
    }
  } else {
    /* Show truncated npub as title */
    if (npub && strlen(npub) > 20) {
      g_autofree gchar *display = NULL;
      if (watch_only) {
        display = g_strdup_printf("%.12s...%.4s (Watch Only)", npub, npub + strlen(npub) - 4);
      } else {
        display = g_strdup_printf("%.12s...%.4s", npub, npub + strlen(npub) - 4);
      }
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), display);
    } else {
      if (watch_only) {
        g_autofree gchar *display = g_strdup_printf("%s (Watch Only)", npub ? npub : "Unknown");
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), display);
      } else {
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), npub ? npub : "Unknown");
      }
    }
  }

  /* Active indicator */
  if (is_active) {
    GtkImage *check = GTK_IMAGE(gtk_image_new_from_icon_name("emblem-ok-symbolic"));
    gtk_widget_set_valign(GTK_WIDGET(check), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(GTK_WIDGET(check), "success");
    adw_action_row_add_prefix(row, GTK_WIDGET(check));
  }

  /* Key status indicator - different icon for watch-only accounts */
  const char *icon_name;
  const char *tooltip;
  if (watch_only) {
    icon_name = "view-reveal-symbolic";  /* Eye icon for watch-only */
    tooltip = "Watch-only account (cannot sign)";
  } else if (has_secret) {
    icon_name = "dialog-password-symbolic";
    tooltip = "Private key available";
  } else {
    icon_name = "security-low-symbolic";
    tooltip = "No private key found";
  }
  GtkImage *key_icon = GTK_IMAGE(gtk_image_new_from_icon_name(icon_name));
  gtk_widget_set_valign(GTK_WIDGET(key_icon), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(GTK_WIDGET(key_icon), tooltip);
  /* Add dim styling for watch-only accounts */
  if (watch_only) {
    gtk_widget_add_css_class(GTK_WIDGET(key_icon), "dim-label");
  }
  adw_action_row_add_suffix(row, GTK_WIDGET(key_icon));

  /* Remove button */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("user-trash-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "destructive-action");
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_remove));

  /* Make row activatable */
  adw_action_row_set_activatable_widget(row, NULL);

  /* Store data */
  AccountRowData *data = g_new0(AccountRowData, 1);
  data->npub = g_strdup(npub);
  data->label = g_strdup(label);
  data->has_secret = has_secret;
  data->watch_only = watch_only;
  g_object_set_data_full(G_OBJECT(row), "account-data", data, (GDestroyNotify)account_row_data_free);
  g_object_set_data(G_OBJECT(row), "sheet-self", self);

  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_row_remove), row);

  return GTK_WIDGET(row);
}

static void populate_list(SheetSelectAccount *self) {
  /* Clear existing rows */
  while (TRUE) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_accounts));
    if (!child) break;
    gtk_list_box_remove(self->list_accounts, child);
  }

  /* Get active account */
  gchar *active_id = NULL;
  accounts_store_get_active(self->accounts, &active_id, NULL);

  /* Load accounts */
  GPtrArray *accounts = accounts_store_list(self->accounts);
  if (!accounts || accounts->len == 0) {
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_empty), TRUE);
    g_free(active_id);
    if (accounts) g_ptr_array_unref(accounts);
    return;
  }

  gtk_widget_set_visible(GTK_WIDGET(self->lbl_empty), FALSE);

  for (guint i = 0; i < accounts->len; i++) {
    AccountEntry *entry = g_ptr_array_index(accounts, i);
    gboolean is_active = (active_id && g_strcmp0(entry->id, active_id) == 0);

    GtkWidget *row = create_account_row(self, entry->id, entry->label,
                                        entry->has_secret, entry->watch_only,
                                        is_active);
    gtk_list_box_append(self->list_accounts, row);
  }

  g_ptr_array_unref(accounts);
  g_free(active_id);
}

static void on_add_new(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSelectAccount *self = user_data;

  /* Generate new key */
  gchar *npub = NULL;
  if (accounts_store_generate_key(self->accounts, NULL, &npub, NULL)) {
    accounts_store_save(self->accounts, NULL);
    populate_list(self);

    /* Select the new account */
    if (self->on_select) {
      self->on_select(npub, self->on_select_ud);
    }
  }
  g_free(npub);
}

/* Callback when import succeeds */
static void on_import_success(const char *npub, const char *label, gpointer user_data) {
  SheetSelectAccount *self = (SheetSelectAccount*)user_data;
  if (!self) return;

  /* Add to accounts store if not already present */
  if (npub && *npub) {
    if (!accounts_store_exists(self->accounts, npub)) {
      accounts_store_add(self->accounts, npub, label, NULL);
    } else if (label && *label) {
      accounts_store_set_label(self->accounts, npub, label, NULL);
    }
    accounts_store_set_active(self->accounts, npub, NULL);
    accounts_store_save(self->accounts, NULL);

    /* Refresh the list */
    populate_list(self);

    /* Notify callback */
    if (self->on_select) {
      self->on_select(npub, self->on_select_ud);
    }
  }
}

static void on_import(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSelectAccount *self = user_data;
  if (!self) return;

  /* Open import key dialog */
  SheetImportKey *import_dlg = sheet_import_key_new();
  sheet_import_key_set_on_success(import_dlg, on_import_success, self);

  GtkWidget *parent = GTK_WIDGET(self);
  GtkRoot *root = gtk_widget_get_root(parent);
  adw_dialog_present(ADW_DIALOG(import_dlg), root ? GTK_WIDGET(root) : parent);
}

/* Dialog response callback for watch-only import */
static void on_pubkey_dialog_response(GtkAlertDialog *dialog, GAsyncResult *result, gpointer user_data) {
  SheetSelectAccount *self = (SheetSelectAccount*)user_data;
  if (!self) return;

  GError *err = NULL;
  int response = gtk_alert_dialog_choose_finish(dialog, result, &err);
  if (err) {
    g_clear_error(&err);
    return;
  }

  /* response 0 = Cancel, 1 = Import */
  if (response != 1) return;

  /* Get the pubkey from clipboard since we can't get it from the dialog
   * For a better UX, we should create a proper sheet dialog.
   * For now, prompt user to paste and use a text entry dialog approach */
}

/* Callback when import pubkey entry dialog finishes */
static void on_pubkey_entry_response(GObject *source, GAsyncResult *result, gpointer user_data) {
  SheetSelectAccount *self = (SheetSelectAccount*)user_data;
  GtkEditable *entry = GTK_EDITABLE(g_object_get_data(source, "entry"));
  GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);

  GError *err = NULL;
  int response = gtk_alert_dialog_choose_finish(dialog, result, &err);
  if (err) {
    g_clear_error(&err);
    return;
  }

  if (response != 1) return;  /* Not "Import" */

  const char *pubkey = entry ? gtk_editable_get_text(entry) : NULL;
  if (!pubkey || !*pubkey) return;

  /* Try to import the public key as watch-only */
  gchar *npub = NULL;
  if (accounts_store_import_pubkey(self->accounts, pubkey, NULL, &npub, NULL)) {
    accounts_store_save(self->accounts, NULL);
    populate_list(self);

    /* Notify callback */
    if (self->on_select) {
      self->on_select(npub, self->on_select_ud);
    }

    /* Show success message */
    GtkAlertDialog *success = gtk_alert_dialog_new("Watch-only account added!\n\nPublic key: %s\n\nThis account can only view events, not sign them.",
                                                    npub && strlen(npub) > 20 ?
                                                    g_strdup_printf("%.12s...%.8s", npub, npub + strlen(npub) - 8) : npub);
    gtk_alert_dialog_show(success, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(success);
  } else {
    /* Show error */
    GtkAlertDialog *error = gtk_alert_dialog_new("Failed to import public key.\n\nMake sure the key is a valid npub (npub1...) or 64-character hex public key.");
    gtk_alert_dialog_show(error, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(error);
  }
  g_free(npub);
}

static void on_import_pubkey(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetSelectAccount *self = user_data;
  if (!self) return;

  /* Create a simple entry dialog for the public key
   * Using AdwMessageDialog would be better but GtkAlertDialog with extra content works */
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, "Import Watch-Only Account");

  /* Create content box */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);

  /* Description label */
  GtkWidget *desc = gtk_label_new("Enter a Nostr public key to add as a watch-only account.\nThis account will not have signing capability.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_widget_add_css_class(desc, "dim-label");
  gtk_box_append(GTK_BOX(content), desc);

  /* Entry for pubkey */
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "npub1... or 64-character hex");
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(content), entry);

  /* Optional label entry */
  GtkWidget *label_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(label_entry), "Label (optional)");
  gtk_widget_set_hexpand(label_entry, TRUE);
  gtk_box_append(GTK_BOX(content), label_entry);

  /* Button box */
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(button_box, 8);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
  GtkWidget *import_btn = gtk_button_new_with_label("Import");
  gtk_widget_add_css_class(import_btn, "suggested-action");

  gtk_box_append(GTK_BOX(button_box), cancel_btn);
  gtk_box_append(GTK_BOX(button_box), import_btn);
  gtk_box_append(GTK_BOX(content), button_box);

  /* Wrap in toolbar view with header */
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);

  adw_dialog_set_child(dialog, toolbar);

  /* Store references for callbacks */
  g_object_set_data(G_OBJECT(dialog), "entry", entry);
  g_object_set_data(G_OBJECT(dialog), "label_entry", label_entry);
  g_object_set_data(G_OBJECT(dialog), "self", self);

  /* Connect button signals */
  g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(adw_dialog_close), dialog);
  g_signal_connect(import_btn, "clicked", G_CALLBACK(on_import_pubkey_submit), dialog);

  /* Present dialog */
  GtkWidget *parent = GTK_WIDGET(self);
  GtkRoot *root = gtk_widget_get_root(parent);
  adw_dialog_present(dialog, root ? GTK_WIDGET(root) : parent);
}

/* Handler for the import button in the pubkey dialog */
static void on_import_pubkey_submit(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AdwDialog *dialog = ADW_DIALOG(user_data);
  SheetSelectAccount *self = g_object_get_data(G_OBJECT(dialog), "self");
  GtkEntry *entry = g_object_get_data(G_OBJECT(dialog), "entry");
  GtkEntry *label_entry = g_object_get_data(G_OBJECT(dialog), "label_entry");

  if (!self || !entry) {
    adw_dialog_close(dialog);
    return;
  }

  const char *pubkey = gtk_editable_get_text(GTK_EDITABLE(entry));
  const char *label = gtk_editable_get_text(GTK_EDITABLE(label_entry));

  if (!pubkey || !*pubkey) {
    /* Show error inline - for now just close */
    adw_dialog_close(dialog);
    return;
  }

  /* Try to import the public key as watch-only */
  gchar *npub = NULL;
  if (accounts_store_import_pubkey(self->accounts, pubkey, (label && *label) ? label : NULL, &npub, NULL)) {
    accounts_store_save(self->accounts, NULL);
    populate_list(self);

    adw_dialog_close(dialog);

    /* Notify callback */
    if (self->on_select) {
      self->on_select(npub, self->on_select_ud);
    }

    /* Show success message */
    g_autofree gchar *display_npub = (npub && strlen(npub) > 20) ?
        g_strdup_printf("%.12s...%.8s", npub, npub + strlen(npub) - 8) : g_strdup(npub);
    GtkAlertDialog *success = gtk_alert_dialog_new("Watch-only account added!\n\nPublic key: %s\n\nThis account can view events but cannot sign them.",
                                                    display_npub);
    gtk_alert_dialog_show(success, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(success);
  } else {
    adw_dialog_close(dialog);

    /* Show error */
    GtkAlertDialog *error = gtk_alert_dialog_new("Failed to import public key.\n\nMake sure the key is a valid npub (npub1...) or 64-character hex public key.");
    gtk_alert_dialog_show(error, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(error);
  }
  g_free(npub);
}

static void sheet_select_account_finalize(GObject *obj) {
  SheetSelectAccount *self = SHEET_SELECT_ACCOUNT(obj);
  accounts_store_free(self->accounts);
  g_free(self->selected_npub);
  G_OBJECT_CLASS(sheet_select_account_parent_class)->finalize(obj);
}

static void sheet_select_account_class_init(SheetSelectAccountClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_select_account_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-select-account.ui");
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, btn_add_new);
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, btn_import);
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, btn_import_pubkey);
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, list_accounts);
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, lbl_empty);
}

static void sheet_select_account_init(SheetSelectAccount *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->accounts = accounts_store_new();
  accounts_store_load(self->accounts, NULL);
  accounts_store_sync_with_secrets(self->accounts);

  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_add_new, "clicked", G_CALLBACK(on_add_new), self);
  if (self->btn_import) {
    g_signal_connect(self->btn_import, "clicked", G_CALLBACK(on_import), self);
  }
  if (self->btn_import_pubkey) {
    g_signal_connect(self->btn_import_pubkey, "clicked", G_CALLBACK(on_import_pubkey), self);
  }
  g_signal_connect(self->list_accounts, "row-activated", G_CALLBACK(on_row_activated), self);

  populate_list(self);
}

SheetSelectAccount *sheet_select_account_new(void) {
  return g_object_new(TYPE_SHEET_SELECT_ACCOUNT, NULL);
}

void sheet_select_account_set_on_select(SheetSelectAccount *self,
                                         SheetSelectAccountCb cb,
                                         gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->on_select = cb;
  self->on_select_ud = user_data;
}

void sheet_select_account_refresh(SheetSelectAccount *self) {
  g_return_if_fail(self != NULL);
  accounts_store_sync_with_secrets(self->accounts);
  populate_list(self);
}
