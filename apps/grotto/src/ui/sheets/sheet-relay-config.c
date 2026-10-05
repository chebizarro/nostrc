/* sheet-relay-config.c - Simplified relay configuration dialog
 *
 * Provides a minimal UI for managing Nostr relays with simple
 * enable/disable checkboxes.
 */
#include "sheet-relay-config.h"
#include "../app-resources.h"
#include "../../relay_store.h"
#include "../../accounts_store.h"

struct _SheetRelayConfig {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_save;
  GtkButton *btn_add_relay;
  GtkButton *btn_add;
  GtkButton *btn_cancel_add;
  GtkEntry *entry_url;
  GtkListBox *list_relays;
  GtkBox *box_add_entry;

  /* State */
  RelayStore *store;
  gchar *identity;
  SheetRelayConfigSaveCb on_publish;
  gpointer on_publish_ud;
};

G_DEFINE_TYPE(SheetRelayConfig, sheet_relay_config, ADW_TYPE_DIALOG)

/* Relay row data - simplified to just URL and enabled state */
typedef struct {
  gchar *url;
  GtkCheckButton *chk_enabled;
} RelayRowData;

static void relay_row_data_free(RelayRowData *data) {
  if (!data) return;
  g_free(data->url);
  g_free(data);
}

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
}

static void save_list_to_store(SheetRelayConfig *self) {
  RelayStore *new_store = relay_store_new();

  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_relays));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    RelayRowData *data = g_object_get_data(G_OBJECT(child), "relay-data");
    if (!data) continue;

    gboolean enabled = gtk_check_button_get_active(data->chk_enabled);
    /* Enabled relays have both read and write permissions */
    relay_store_add(new_store, data->url, enabled, enabled);
  }

  relay_store_free(self->store);
  self->store = new_store;
  relay_store_save(self->store);
}

static void on_save(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;

  save_list_to_store(self);

  if (self->on_publish) {
    gchar *event_json = relay_store_build_event_json(self->store);
    self->on_publish(event_json, self->identity, self->on_publish_ud);
    g_free(event_json);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void on_row_remove(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GtkWidget *row = GTK_WIDGET(user_data);
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(row));
  gtk_list_box_remove(list, row);
}

/* Create a simple relay row with checkbox and URL */
static GtkWidget *create_relay_row(SheetRelayConfig *self, const gchar *url, gboolean enabled) {
  (void)self;

  GtkBox *row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
  gtk_widget_set_margin_top(GTK_WIDGET(row), 8);
  gtk_widget_set_margin_bottom(GTK_WIDGET(row), 8);
  gtk_widget_set_margin_start(GTK_WIDGET(row), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(row), 12);

  /* Enable checkbox */
  GtkCheckButton *chk_enabled = GTK_CHECK_BUTTON(gtk_check_button_new());
  gtk_check_button_set_active(chk_enabled, enabled);
  gtk_widget_set_valign(GTK_WIDGET(chk_enabled), GTK_ALIGN_CENTER);
  gtk_box_append(row, GTK_WIDGET(chk_enabled));

  /* URL label */
  GtkLabel *lbl_url = GTK_LABEL(gtk_label_new(url));
  gtk_label_set_xalign(lbl_url, 0);
  gtk_label_set_ellipsize(lbl_url, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(GTK_WIDGET(lbl_url), TRUE);
  gtk_box_append(row, GTK_WIDGET(lbl_url));

  /* Remove button (hidden until hover) */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("edit-delete-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "circular");
  gtk_widget_set_opacity(GTK_WIDGET(btn_remove), 0.5);
  gtk_box_append(row, GTK_WIDGET(btn_remove));

  /* Store data */
  RelayRowData *data = g_new0(RelayRowData, 1);
  data->url = g_strdup(url);
  data->chk_enabled = chk_enabled;
  g_object_set_data_full(G_OBJECT(row), "relay-data", data, (GDestroyNotify)relay_row_data_free);

  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_row_remove), row);

  return GTK_WIDGET(row);
}

static void populate_list(SheetRelayConfig *self) {
  /* Clear existing */
  while (TRUE) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_relays));
    if (!child) break;
    gtk_list_box_remove(self->list_relays, child);
  }

  /* Load from store */
  GPtrArray *relays = relay_store_list(self->store);
  if (!relays) return;

  for (guint i = 0; i < relays->len; i++) {
    RelayEntry *entry = g_ptr_array_index(relays, i);
    /* Consider relay enabled if it has either read or write permission */
    gboolean enabled = entry->read || entry->write;
    GtkWidget *row = create_relay_row(self, entry->url, enabled);
    gtk_list_box_append(self->list_relays, row);
  }

  g_ptr_array_unref(relays);

  /* If empty, add defaults */
  if (relay_store_count(self->store) == 0) {
    GPtrArray *defaults = relay_store_get_defaults();
    for (guint i = 0; i < defaults->len; i++) {
      RelayEntry *entry = g_ptr_array_index(defaults, i);
      gboolean enabled = entry->read || entry->write;
      GtkWidget *row = create_relay_row(self, entry->url, enabled);
      gtk_list_box_append(self->list_relays, row);
    }
    g_ptr_array_unref(defaults);
  }
}

static void on_show_add_entry(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;

  /* Hide "Add Relay" button, show entry */
  gtk_widget_set_visible(GTK_WIDGET(self->btn_add_relay), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->box_add_entry), TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(self->entry_url));
}

static void on_hide_add_entry(SheetRelayConfig *self) {
  /* Hide entry, show "Add Relay" button */
  gtk_widget_set_visible(GTK_WIDGET(self->box_add_entry), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->btn_add_relay), TRUE);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_url), "");
}

static void on_cancel_add(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;
  on_hide_add_entry(self);
}

static void on_add_relay(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;

  const gchar *url = gtk_editable_get_text(GTK_EDITABLE(self->entry_url));
  if (!url || !*url) return;

  if (!relay_store_validate_url(url)) {
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Invalid URL. Must start with wss:// or ws://");
    gtk_alert_dialog_show(dlg, GTK_WINDOW(root));
    g_object_unref(dlg);
    return;
  }

  /* Check for duplicate */
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_relays));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    RelayRowData *data = g_object_get_data(G_OBJECT(child), "relay-data");
    if (data && g_strcmp0(data->url, url) == 0) {
      on_hide_add_entry(self);
      return;
    }
  }

  /* Add new row (enabled by default) */
  GtkWidget *row = create_relay_row(self, url, TRUE);
  gtk_list_box_append(self->list_relays, row);

  on_hide_add_entry(self);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)entry;
  SheetRelayConfig *self = user_data;
  on_add_relay(NULL, self);
}

static void sheet_relay_config_finalize(GObject *obj) {
  SheetRelayConfig *self = SHEET_RELAY_CONFIG(obj);
  relay_store_free(self->store);
  g_free(self->identity);
  G_OBJECT_CLASS(sheet_relay_config_parent_class)->finalize(obj);
}

static void sheet_relay_config_class_init(SheetRelayConfigClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_relay_config_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-relay-config.ui");
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_save);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_add_relay);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_add);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_cancel_add);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, entry_url);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, list_relays);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, box_add_entry);
}

static void sheet_relay_config_init(SheetRelayConfig *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->store = NULL;
  self->identity = NULL;

  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_save, "clicked", G_CALLBACK(on_save), self);
  g_signal_connect(self->btn_add_relay, "clicked", G_CALLBACK(on_show_add_entry), self);
  g_signal_connect(self->btn_add, "clicked", G_CALLBACK(on_add_relay), self);
  g_signal_connect(self->btn_cancel_add, "clicked", G_CALLBACK(on_cancel_add), self);
  g_signal_connect(self->entry_url, "activate", G_CALLBACK(on_entry_activate), self);
}

static void setup_store_and_populate(SheetRelayConfig *self, const gchar *identity) {
  if (self->store) {
    relay_store_free(self->store);
    self->store = NULL;
  }
  g_free(self->identity);
  self->identity = identity ? g_strdup(identity) : NULL;

  self->store = relay_store_new_for_identity(identity);
  relay_store_load(self->store);

  if (identity && relay_store_count(self->store) == 0) {
    RelayStore *global = relay_store_new();
    relay_store_load(global);
    if (relay_store_count(global) > 0) {
      relay_store_copy_from(self->store, global);
    } else {
      relay_store_reset_to_defaults(self->store);
    }
    relay_store_free(global);
  }

  populate_list(self);
}

SheetRelayConfig *sheet_relay_config_new(void) {
  SheetRelayConfig *self = g_object_new(TYPE_SHEET_RELAY_CONFIG, NULL);
  setup_store_and_populate(self, NULL);
  return self;
}

SheetRelayConfig *sheet_relay_config_new_for_identity(const gchar *identity) {
  SheetRelayConfig *self = g_object_new(TYPE_SHEET_RELAY_CONFIG, NULL);
  setup_store_and_populate(self, identity);
  return self;
}

void sheet_relay_config_set_on_publish(SheetRelayConfig *self,
                                       SheetRelayConfigSaveCb cb,
                                       gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->on_publish = cb;
  self->on_publish_ud = user_data;
}

const gchar *sheet_relay_config_get_identity(SheetRelayConfig *self) {
  g_return_val_if_fail(self != NULL, NULL);
  return self->identity;
}
