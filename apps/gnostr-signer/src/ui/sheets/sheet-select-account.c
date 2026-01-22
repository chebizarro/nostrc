/* sheet-select-account.c - Account selection and management dialog implementation */
#include "sheet-select-account.h"
#include "sheet-import-key.h"
#include "../app-resources.h"
#include "../../accounts_store.h"
#include "../../secret_store.h"
#include "../../settings_manager.h"

struct _SheetSelectAccount {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_add_new;
  GtkButton *btn_import;
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
  accounts_store_remove(self->accounts, data->npub);
  accounts_store_save(self->accounts);

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
  accounts_store_set_active(self->accounts, data->npub);
  accounts_store_save(self->accounts);

  /* Invoke callback */
  if (self->on_select) {
    self->on_select(data->npub, self->on_select_ud);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static GtkWidget *create_account_row(SheetSelectAccount *self, const gchar *npub,
                                     const gchar *label, gboolean has_secret,
                                     gboolean is_active) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Display label or truncated npub */
  if (label && *label) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
    /* Show truncated npub as subtitle */
    if (npub && strlen(npub) > 16) {
      gchar *sub = g_strdup_printf("%.12s...%.4s", npub, npub + strlen(npub) - 4);
      adw_action_row_set_subtitle(row, sub);
      g_free(sub);
    } else {
      adw_action_row_set_subtitle(row, npub);
    }
  } else {
    /* Show truncated npub as title */
    if (npub && strlen(npub) > 20) {
      gchar *display = g_strdup_printf("%.12s...%.4s", npub, npub + strlen(npub) - 4);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), display);
      g_free(display);
    } else {
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), npub ? npub : "Unknown");
    }
  }

  /* Active indicator */
  if (is_active) {
    GtkImage *check = GTK_IMAGE(gtk_image_new_from_icon_name("emblem-ok-symbolic"));
    gtk_widget_set_valign(GTK_WIDGET(check), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(GTK_WIDGET(check), "success");
    adw_action_row_add_prefix(row, GTK_WIDGET(check));
  }

  /* Key status indicator */
  GtkImage *key_icon = GTK_IMAGE(gtk_image_new_from_icon_name(
    has_secret ? "dialog-password-symbolic" : "security-low-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(key_icon), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(GTK_WIDGET(key_icon),
    has_secret ? "Private key available" : "Watch-only (no private key)");
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
  accounts_store_get_active(self->accounts, &active_id);

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
                                        entry->has_secret, is_active);
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
  if (accounts_store_generate_key(self->accounts, NULL, &npub)) {
    accounts_store_save(self->accounts);
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
      accounts_store_add(self->accounts, npub, label);
    } else if (label && *label) {
      accounts_store_set_label(self->accounts, npub, label);
    }
    accounts_store_set_active(self->accounts, npub);
    accounts_store_save(self->accounts);

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
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, list_accounts);
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, lbl_empty);
}

static void sheet_select_account_init(SheetSelectAccount *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->accounts = accounts_store_new();
  accounts_store_load(self->accounts);
  accounts_store_sync_with_secrets(self->accounts);

  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_add_new, "clicked", G_CALLBACK(on_add_new), self);
  if (self->btn_import) {
    g_signal_connect(self->btn_import, "clicked", G_CALLBACK(on_import), self);
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
