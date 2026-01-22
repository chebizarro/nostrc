/* sheet-relay-config.c - Relay configuration dialog implementation */
#include "sheet-relay-config.h"
#include "../app-resources.h"
#include "../../relay_store.h"

struct _SheetRelayConfig {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_publish;
  GtkButton *btn_add;
  GtkButton *btn_test_all;
  GtkEntry *entry_url;
  GtkListBox *list_relays;

  /* State */
  RelayStore *store;
  SheetRelayConfigSaveCb on_publish;
  gpointer on_publish_ud;
};

G_DEFINE_TYPE(SheetRelayConfig, sheet_relay_config, ADW_TYPE_DIALOG)

/* Relay row widget data */
typedef struct {
  gchar *url;
  GtkCheckButton *chk_read;
  GtkCheckButton *chk_write;
  GtkImage *status_icon;
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
  /* Clear and rebuild store from list */
  RelayStore *new_store = relay_store_new();

  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_relays));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (!ADW_IS_ACTION_ROW(child)) continue;

    RelayRowData *data = g_object_get_data(G_OBJECT(child), "relay-data");
    if (!data) continue;

    gboolean read = gtk_check_button_get_active(data->chk_read);
    gboolean write = gtk_check_button_get_active(data->chk_write);

    relay_store_add(new_store, data->url, read, write);
  }

  /* Swap stores */
  relay_store_free(self->store);
  self->store = new_store;
  relay_store_save(self->store);
}

static void on_publish(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;

  save_list_to_store(self);

  if (self->on_publish) {
    gchar *event_json = relay_store_build_event_json(self->store);
    self->on_publish(event_json, self->on_publish_ud);
    g_free(event_json);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void on_row_remove(GtkButton *btn, gpointer user_data) {
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));
  gtk_list_box_remove(list, GTK_WIDGET(row));
}

static void update_status_icon(GtkImage *icon, RelayConnectionStatus status) {
  if (!icon) return;
  switch (status) {
    case RELAY_STATUS_CONNECTING:
      gtk_image_set_from_icon_name(icon, "network-transmit-receive-symbolic");
      gtk_widget_set_tooltip_text(GTK_WIDGET(icon), "Connecting...");
      break;
    case RELAY_STATUS_CONNECTED:
      gtk_image_set_from_icon_name(icon, "emblem-ok-symbolic");
      gtk_widget_set_tooltip_text(GTK_WIDGET(icon), "Connected");
      gtk_widget_add_css_class(GTK_WIDGET(icon), "success");
      break;
    case RELAY_STATUS_DISCONNECTED:
      gtk_image_set_from_icon_name(icon, "network-offline-symbolic");
      gtk_widget_set_tooltip_text(GTK_WIDGET(icon), "Disconnected");
      break;
    case RELAY_STATUS_ERROR:
      gtk_image_set_from_icon_name(icon, "dialog-error-symbolic");
      gtk_widget_set_tooltip_text(GTK_WIDGET(icon), "Connection failed");
      gtk_widget_add_css_class(GTK_WIDGET(icon), "error");
      break;
    default:
      gtk_image_set_from_icon_name(icon, "network-wired-symbolic");
      gtk_widget_set_tooltip_text(GTK_WIDGET(icon), "Unknown");
      break;
  }
}

static GtkWidget *create_relay_row(SheetRelayConfig *self, const gchar *url,
                                   gboolean read, gboolean write) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), url);

  /* Status icon */
  GtkImage *status_icon = GTK_IMAGE(gtk_image_new_from_icon_name("network-wired-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(status_icon), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(GTK_WIDGET(status_icon), "Connection status unknown");

  /* Read checkbox */
  GtkCheckButton *chk_read = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Read"));
  gtk_check_button_set_active(chk_read, read);
  gtk_widget_set_valign(GTK_WIDGET(chk_read), GTK_ALIGN_CENTER);

  /* Write checkbox */
  GtkCheckButton *chk_write = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Write"));
  gtk_check_button_set_active(chk_write, write);
  gtk_widget_set_valign(GTK_WIDGET(chk_write), GTK_ALIGN_CENTER);

  /* Remove button */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("user-trash-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");

  /* Pack into row */
  adw_action_row_add_prefix(row, GTK_WIDGET(status_icon));
  adw_action_row_add_suffix(row, GTK_WIDGET(chk_read));
  adw_action_row_add_suffix(row, GTK_WIDGET(chk_write));
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_remove));

  /* Store data */
  RelayRowData *data = g_new0(RelayRowData, 1);
  data->url = g_strdup(url);
  data->chk_read = chk_read;
  data->chk_write = chk_write;
  data->status_icon = status_icon;
  g_object_set_data_full(G_OBJECT(row), "relay-data", data, (GDestroyNotify)relay_row_data_free);

  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_row_remove), row);

  (void)self; /* Silence unused warning */
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
    GtkWidget *row = create_relay_row(self, entry->url, entry->read, entry->write);
    gtk_list_box_append(self->list_relays, row);
  }

  g_ptr_array_unref(relays);

  /* If empty, add defaults */
  if (relay_store_count(self->store) == 0) {
    GPtrArray *defaults = relay_store_get_defaults();
    for (guint i = 0; i < defaults->len; i++) {
      RelayEntry *entry = g_ptr_array_index(defaults, i);
      GtkWidget *row = create_relay_row(self, entry->url, entry->read, entry->write);
      gtk_list_box_append(self->list_relays, row);
    }
    g_ptr_array_unref(defaults);
  }
}

static void on_add_relay(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;

  const gchar *url = gtk_editable_get_text(GTK_EDITABLE(self->entry_url));
  if (!url || !*url) return;

  if (!relay_store_validate_url(url)) {
    /* Show error */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Invalid relay URL. Must start with wss:// or ws://");
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
      /* Already exists */
      gtk_editable_set_text(GTK_EDITABLE(self->entry_url), "");
      return;
    }
  }

  /* Add new row */
  GtkWidget *row = create_relay_row(self, url, TRUE, TRUE);
  gtk_list_box_append(self->list_relays, row);

  /* Clear entry */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_url), "");
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)entry;
  SheetRelayConfig *self = user_data;
  on_add_relay(NULL, self);
}

typedef struct {
  SheetRelayConfig *self;
  gchar *url;
} TestConnectionData;

static void test_connection_cb(const gchar *url, RelayConnectionStatus status, gpointer user_data) {
  TestConnectionData *data = user_data;
  if (!data || !data->self) {
    g_free(data->url);
    g_free(data);
    return;
  }

  /* Find the row and update its icon */
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(data->self->list_relays));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (!ADW_IS_ACTION_ROW(child)) continue;

    RelayRowData *row_data = g_object_get_data(G_OBJECT(child), "relay-data");
    if (row_data && g_strcmp0(row_data->url, url) == 0) {
      update_status_icon(row_data->status_icon, status);
      break;
    }
  }

  /* Only free if this is the final status (not CONNECTING) */
  if (status != RELAY_STATUS_CONNECTING) {
    g_free(data->url);
    g_free(data);
  }
}

static void on_test_all(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetRelayConfig *self = user_data;

  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_relays));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (!ADW_IS_ACTION_ROW(child)) continue;

    RelayRowData *row_data = g_object_get_data(G_OBJECT(child), "relay-data");
    if (!row_data) continue;

    TestConnectionData *data = g_new0(TestConnectionData, 1);
    data->self = self;
    data->url = g_strdup(row_data->url);

    relay_store_test_connection(row_data->url, test_connection_cb, data);
  }
}

static void sheet_relay_config_finalize(GObject *obj) {
  SheetRelayConfig *self = SHEET_RELAY_CONFIG(obj);
  relay_store_free(self->store);
  G_OBJECT_CLASS(sheet_relay_config_parent_class)->finalize(obj);
}

static void sheet_relay_config_class_init(SheetRelayConfigClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_relay_config_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-relay-config.ui");
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_publish);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_add);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, btn_test_all);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, entry_url);
  gtk_widget_class_bind_template_child(wc, SheetRelayConfig, list_relays);
}

static void sheet_relay_config_init(SheetRelayConfig *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->store = relay_store_new();
  relay_store_load(self->store);

  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_publish, "clicked", G_CALLBACK(on_publish), self);
  g_signal_connect(self->btn_add, "clicked", G_CALLBACK(on_add_relay), self);
  g_signal_connect(self->btn_test_all, "clicked", G_CALLBACK(on_test_all), self);
  g_signal_connect(self->entry_url, "activate", G_CALLBACK(on_entry_activate), self);

  populate_list(self);
}

SheetRelayConfig *sheet_relay_config_new(void) {
  return g_object_new(TYPE_SHEET_RELAY_CONFIG, NULL);
}

void sheet_relay_config_set_on_publish(SheetRelayConfig *self,
                                       SheetRelayConfigSaveCb cb,
                                       gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->on_publish = cb;
  self->on_publish_ud = user_data;
}
