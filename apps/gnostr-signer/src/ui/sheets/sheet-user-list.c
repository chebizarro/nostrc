/* sheet-user-list.c - User list management dialog implementation */
#include "sheet-user-list.h"
#include "../app-resources.h"

struct _SheetUserList {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_publish;
  GtkButton *btn_add;
  GtkEntry *entry_pubkey;
  GtkEntry *entry_petname;
  GtkSearchEntry *search_entry;
  GtkListBox *list_users;
  GtkLabel *lbl_title;
  GtkLabel *lbl_count;

  /* State */
  UserListType type;
  UserListStore *store;
  SheetUserListSaveCb on_publish;
  gpointer on_publish_ud;
};

G_DEFINE_TYPE(SheetUserList, sheet_user_list, ADW_TYPE_DIALOG)

/* Row data */
typedef struct {
  gchar *pubkey;
  gchar *petname;
} UserRowData;

static void user_row_data_free(UserRowData *data) {
  if (!data) return;
  g_free(data->pubkey);
  g_free(data->petname);
  g_free(data);
}

static void update_count_label(SheetUserList *self) {
  guint count = user_list_store_count(self->store);
  gchar *text = g_strdup_printf("%u %s",
    count,
    self->type == USER_LIST_FOLLOWS ? "following" : "muted");
  gtk_label_set_text(self->lbl_count, text);
  g_free(text);
}

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetUserList *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
}

static void save_list_to_store(SheetUserList *self) {
  /* Rebuild store from UI list */
  user_list_store_clear(self->store);

  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_users));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (!ADW_IS_ACTION_ROW(child)) continue;

    UserRowData *data = g_object_get_data(G_OBJECT(child), "user-data");
    if (!data) continue;

    user_list_store_add(self->store, data->pubkey, NULL, data->petname);
  }

  user_list_store_save(self->store);
}

static void on_publish(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetUserList *self = user_data;

  save_list_to_store(self);

  if (self->on_publish) {
    gchar *event_json = user_list_store_build_event_json(self->store);
    self->on_publish(self->type, event_json, self->on_publish_ud);
    g_free(event_json);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void on_row_remove(GtkButton *btn, gpointer user_data) {
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
  SheetUserList *self = g_object_get_data(G_OBJECT(row), "sheet-self");
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));

  /* Get pubkey and remove from store */
  UserRowData *data = g_object_get_data(G_OBJECT(row), "user-data");
  if (data) {
    user_list_store_remove(self->store, data->pubkey);
  }

  gtk_list_box_remove(list, GTK_WIDGET(row));
  update_count_label(self);
}

static GtkWidget *create_user_row(SheetUserList *self, const gchar *pubkey,
                                  const gchar *petname) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Display truncated pubkey or petname */
  if (petname && *petname) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), petname);
    gchar *sub = g_strdup_printf("%.12s...", pubkey);
    adw_action_row_set_subtitle(row, sub);
    g_free(sub);
  } else {
    gchar *display = g_strdup_printf("%.16s...", pubkey);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), display);
    g_free(display);
  }

  /* Remove button */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("user-trash-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_remove));

  /* Store data */
  UserRowData *data = g_new0(UserRowData, 1);
  data->pubkey = g_strdup(pubkey);
  data->petname = g_strdup(petname);
  g_object_set_data_full(G_OBJECT(row), "user-data", data, (GDestroyNotify)user_row_data_free);
  g_object_set_data(G_OBJECT(row), "sheet-self", self);

  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_row_remove), row);

  return GTK_WIDGET(row);
}

static void populate_list(SheetUserList *self, const gchar *filter) {
  /* Clear existing */
  while (TRUE) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_users));
    if (!child) break;
    gtk_list_box_remove(self->list_users, child);
  }

  /* Get filtered entries */
  GPtrArray *entries = user_list_store_search(self->store, filter);
  if (!entries) return;

  for (guint i = 0; i < entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(entries, i);
    GtkWidget *row = create_user_row(self, entry->pubkey, entry->petname);
    gtk_list_box_append(self->list_users, row);
  }

  g_ptr_array_unref(entries);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
  SheetUserList *self = user_data;
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  populate_list(self, text && *text ? text : NULL);
}

static gboolean is_valid_hex_pubkey(const gchar *s) {
  if (!s) return FALSE;
  gsize len = strlen(s);
  if (len != 64) return FALSE;
  for (gsize i = 0; i < len; i++) {
    gchar c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

static void on_add_user(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetUserList *self = user_data;

  const gchar *pubkey = gtk_editable_get_text(GTK_EDITABLE(self->entry_pubkey));
  if (!pubkey || !*pubkey) return;

  /* Validate pubkey format */
  if (!is_valid_hex_pubkey(pubkey) && !g_str_has_prefix(pubkey, "npub1")) {
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Invalid public key. Enter 64-char hex or npub1...");
    gtk_alert_dialog_show(dlg, GTK_WINDOW(root));
    g_object_unref(dlg);
    return;
  }

  /* Check for duplicate */
  if (user_list_store_contains(self->store, pubkey)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_pubkey), "");
    return;
  }

  const gchar *petname = NULL;
  if (self->type == USER_LIST_FOLLOWS && self->entry_petname) {
    petname = gtk_editable_get_text(GTK_EDITABLE(self->entry_petname));
    if (petname && !*petname) petname = NULL;
  }

  /* Add to store and UI */
  user_list_store_add(self->store, pubkey, NULL, petname);

  GtkWidget *row = create_user_row(self, pubkey, petname);
  gtk_list_box_append(self->list_users, row);

  /* Clear entries */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_pubkey), "");
  if (self->entry_petname) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_petname), "");
  }

  update_count_label(self);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)entry;
  SheetUserList *self = user_data;
  on_add_user(NULL, self);
}

static void sheet_user_list_finalize(GObject *obj) {
  SheetUserList *self = SHEET_USER_LIST(obj);
  user_list_store_free(self->store);
  G_OBJECT_CLASS(sheet_user_list_parent_class)->finalize(obj);
}

static void sheet_user_list_class_init(SheetUserListClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_user_list_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-user-list.ui");
  gtk_widget_class_bind_template_child(wc, SheetUserList, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetUserList, btn_publish);
  gtk_widget_class_bind_template_child(wc, SheetUserList, btn_add);
  gtk_widget_class_bind_template_child(wc, SheetUserList, entry_pubkey);
  gtk_widget_class_bind_template_child(wc, SheetUserList, entry_petname);
  gtk_widget_class_bind_template_child(wc, SheetUserList, search_entry);
  gtk_widget_class_bind_template_child(wc, SheetUserList, list_users);
  gtk_widget_class_bind_template_child(wc, SheetUserList, lbl_title);
  gtk_widget_class_bind_template_child(wc, SheetUserList, lbl_count);
}

static void sheet_user_list_init(SheetUserList *self) {
  self->type = USER_LIST_FOLLOWS; /* Default, will be set by _new() */
  gtk_widget_init_template(GTK_WIDGET(self));

  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_publish, "clicked", G_CALLBACK(on_publish), self);
  g_signal_connect(self->btn_add, "clicked", G_CALLBACK(on_add_user), self);
  g_signal_connect(self->entry_pubkey, "activate", G_CALLBACK(on_entry_activate), self);
  g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
}

SheetUserList *sheet_user_list_new(UserListType type) {
  SheetUserList *self = g_object_new(TYPE_SHEET_USER_LIST, NULL);
  self->type = type;

  /* Load store */
  self->store = user_list_store_new(type);
  user_list_store_load(self->store);

  /* Update UI based on type */
  if (self->lbl_title) {
    gtk_label_set_text(self->lbl_title,
      type == USER_LIST_FOLLOWS ? "Manage Follows" : "Manage Mutes");
  }

  /* Hide petname entry for mutes */
  if (type == USER_LIST_MUTES && self->entry_petname) {
    gtk_widget_set_visible(GTK_WIDGET(self->entry_petname), FALSE);
  }

  update_count_label(self);
  populate_list(self, NULL);

  return self;
}

void sheet_user_list_set_on_publish(SheetUserList *self,
                                    SheetUserListSaveCb cb,
                                    gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->on_publish = cb;
  self->on_publish_ud = user_data;
}
