/* page-history.c - UI page for viewing transaction/event history
 *
 * SPDX-License-Identifier: MIT
 */
#include "page-history.h"
#include "../event_history.h"
#include "app-resources.h"

#include <time.h>

/* Page size for pagination */
#define PAGE_SIZE 50

struct _GnPageHistory {
  AdwPreferencesPage parent_instance;

  /* Template widgets */
  GtkListBox *history_list;
  GtkLabel *lbl_entry_count;
  GtkButton *btn_clear_history;
  GtkButton *btn_export;
  GtkButton *btn_refresh;
  GtkButton *btn_prev_page;
  GtkButton *btn_next_page;
  GtkLabel *lbl_page_info;
  GtkStack *empty_stack;
  AdwComboRow *combo_kind_filter;
  AdwComboRow *combo_client_filter;
  GtkButton *btn_clear_filters;

  /* Filter state */
  gint filter_kind;          /* -1 = all */
  gchar *filter_client;      /* NULL = all */
  gint64 filter_start_time;  /* 0 = no start bound */
  gint64 filter_end_time;    /* 0 = no end bound */

  /* Pagination state */
  guint current_page;
  guint total_pages;
  guint total_entries;

  /* Kind filter model */
  GtkStringList *kind_model;
  GArray *kind_values;  /* Actual kind values corresponding to model indices */

  /* Client filter model */
  GtkStringList *client_model;
  GPtrArray *client_values;  /* Client pubkeys corresponding to model indices */
};

G_DEFINE_FINAL_TYPE(GnPageHistory, gn_page_history, ADW_TYPE_PREFERENCES_PAGE)

/* Forward declarations */
static void populate_history_list(GnPageHistory *self);
static void update_filter_models(GnPageHistory *self);
static GtkWidget *create_history_row(GnEventHistoryEntry *entry);
static void update_page_info(GnPageHistory *self);
static void update_entry_count(GnPageHistory *self);

/* Callback for clear history confirmation dialog */
static void on_clear_history_response(AdwAlertDialog *dlg, const gchar *response, gpointer ud) {
  (void)dlg;
  GnPageHistory *page = GN_PAGE_HISTORY(ud);
  if (g_strcmp0(response, "clear") == 0) {
    GnEventHistory *history = gn_event_history_get_default();
    gn_event_history_clear(history);
    page->current_page = 0;
    populate_history_list(page);
  }
}

/* Callback for file export save dialog */
static void on_export_file_save_cb(GObject *source, GAsyncResult *result, gpointer ud) {
  GnPageHistory *page = GN_PAGE_HISTORY(ud);
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  GError *error = NULL;

  GFile *file = gtk_file_dialog_save_finish(dlg, result, &error);
  if (file) {
    gchar *path = g_file_get_path(file);

    /* Determine format from extension */
    const gchar *format = "json";
    if (g_str_has_suffix(path, ".csv"))
      format = "csv";

    GnEventHistory *history = gn_event_history_get_default();
    GError *export_error = NULL;

    if (!gn_event_history_export_to_file(history, path, format, NULL, &export_error)) {
      g_warning("page-history: export failed: %s", export_error->message);
      g_clear_error(&export_error);
    } else {
      g_debug("page-history: exported to %s", path);
    }

    g_free(path);
    g_object_unref(file);
  }

  if (error) {
    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
      g_warning("page-history: file dialog error: %s", error->message);
    g_clear_error(&error);
  }

  (void)page;
}

/* Nostr event kind names */
static const gchar *
get_kind_name(gint kind)
{
  switch (kind) {
    case 0: return "Metadata (0)";
    case 1: return "Short Text Note (1)";
    case 2: return "Recommend Relay (2)";
    case 3: return "Contacts (3)";
    case 4: return "Encrypted DM (4)";
    case 5: return "Event Deletion (5)";
    case 6: return "Repost (6)";
    case 7: return "Reaction (7)";
    case 8: return "Badge Award (8)";
    case 16: return "Generic Repost (16)";
    case 40: return "Channel Creation (40)";
    case 41: return "Channel Metadata (41)";
    case 42: return "Channel Message (42)";
    case 43: return "Channel Hide (43)";
    case 44: return "Channel Mute (44)";
    case 1063: return "File Metadata (1063)";
    case 1311: return "Live Chat (1311)";
    case 1984: return "Report (1984)";
    case 1985: return "Label (1985)";
    case 9734: return "Zap Request (9734)";
    case 9735: return "Zap Receipt (9735)";
    case 10000: return "Mute List (10000)";
    case 10001: return "Pin List (10001)";
    case 10002: return "Relay List (10002)";
    case 13194: return "Wallet Info (13194)";
    case 22242: return "Client Auth (22242)";
    case 23194: return "Wallet Request (23194)";
    case 23195: return "Wallet Response (23195)";
    case 24133: return "NIP-46 Request (24133)";
    case 30000: return "Profile Badges (30000)";
    case 30001: return "Bookmark List (30001)";
    case 30008: return "Badge Definition (30008)";
    case 30009: return "Badge Definition (30009)";
    case 30023: return "Long-form Content (30023)";
    case 30078: return "App-specific Data (30078)";
    case 30311: return "Live Event (30311)";
    default:
      if (kind >= 10000 && kind < 20000)
        return "Replaceable Event";
      if (kind >= 20000 && kind < 30000)
        return "Ephemeral Event";
      if (kind >= 30000 && kind < 40000)
        return "Parameterized Replaceable";
      return "Unknown";
  }
}

static const gchar *
get_result_icon(GnEventHistoryResult result)
{
  switch (result) {
    case GN_EVENT_HISTORY_SUCCESS: return "emblem-ok-symbolic";
    case GN_EVENT_HISTORY_DENIED: return "action-unavailable-symbolic";
    case GN_EVENT_HISTORY_ERROR: return "dialog-error-symbolic";
    case GN_EVENT_HISTORY_TIMEOUT: return "appointment-soon-symbolic";
    default: return "dialog-question-symbolic";
  }
}

static const gchar *
get_result_css_class(GnEventHistoryResult result)
{
  switch (result) {
    case GN_EVENT_HISTORY_SUCCESS: return "success";
    case GN_EVENT_HISTORY_DENIED: return "warning";
    case GN_EVENT_HISTORY_ERROR: return "error";
    case GN_EVENT_HISTORY_TIMEOUT: return "warning";
    default: return "dim-label";
  }
}

static const gchar *
get_result_text(GnEventHistoryResult result)
{
  switch (result) {
    case GN_EVENT_HISTORY_SUCCESS: return "Success";
    case GN_EVENT_HISTORY_DENIED: return "Denied";
    case GN_EVENT_HISTORY_ERROR: return "Error";
    case GN_EVENT_HISTORY_TIMEOUT: return "Timeout";
    default: return "Unknown";
  }
}

/* Button click handlers */
static void
on_clear_history_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);

  /* Show confirmation dialog */
  GtkWidget *window = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_WINDOW);

  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(
    adw_alert_dialog_new("Clear History?",
                          "This will permanently delete all event history entries. This cannot be undone."));

  adw_alert_dialog_add_responses(dialog,
    "cancel", "Cancel",
    "clear", "Clear History",
    NULL);

  adw_alert_dialog_set_response_appearance(dialog, "clear", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_close_response(dialog, "cancel");

  g_signal_connect(dialog, "response", G_CALLBACK(on_clear_history_response), self);

  adw_dialog_present(ADW_DIALOG(dialog), window ? GTK_WIDGET(window) : GTK_WIDGET(self));
}

static void
on_export_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);

  GtkWidget *window = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_WINDOW);

  /* Create file chooser for save */
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Export History");

  /* Default filename with timestamp */
  gchar *default_name = g_strdup_printf("gnostr_history_%ld.json", (long)time(NULL));
  gtk_file_dialog_set_initial_name(dialog, default_name);
  g_free(default_name);

  /* Set filters */
  GtkFileFilter *json_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(json_filter, "JSON files");
  gtk_file_filter_add_pattern(json_filter, "*.json");

  GtkFileFilter *csv_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(csv_filter, "CSV files");
  gtk_file_filter_add_pattern(csv_filter, "*.csv");

  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, json_filter);
  g_list_store_append(filters, csv_filter);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));

  g_object_unref(json_filter);
  g_object_unref(csv_filter);
  g_object_unref(filters);

  gtk_file_dialog_save(dialog, window ? GTK_WINDOW(window) : NULL, NULL,
    on_export_file_save_cb, self);

  g_object_unref(dialog);
}

static void
on_refresh_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);
  gn_page_history_refresh(self);
}

static void
on_prev_page_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);

  if (self->current_page > 0) {
    self->current_page--;
    populate_history_list(self);
  }
}

static void
on_next_page_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);

  if (self->current_page < self->total_pages - 1) {
    self->current_page++;
    populate_history_list(self);
  }
}

static void
on_kind_filter_changed(AdwComboRow *combo, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);

  guint selected = adw_combo_row_get_selected(combo);

  if (selected == 0 || selected == GTK_INVALID_LIST_POSITION) {
    self->filter_kind = -1;  /* All */
  } else if (self->kind_values && selected - 1 < self->kind_values->len) {
    self->filter_kind = g_array_index(self->kind_values, gint, selected - 1);
  }

  self->current_page = 0;
  populate_history_list(self);
}

static void
on_client_filter_changed(AdwComboRow *combo, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);

  guint selected = adw_combo_row_get_selected(combo);

  g_free(self->filter_client);
  self->filter_client = NULL;

  if (selected > 0 && selected != GTK_INVALID_LIST_POSITION) {
    if (self->client_values && selected - 1 < self->client_values->len) {
      self->filter_client = g_strdup(g_ptr_array_index(self->client_values, selected - 1));
    }
  }

  self->current_page = 0;
  populate_history_list(self);
}

static void
on_clear_filters_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnPageHistory *self = GN_PAGE_HISTORY(user_data);
  gn_page_history_clear_filters(self);
}

/* Copy event ID to clipboard */
static void
on_copy_event_id_clicked(GtkButton *btn, gpointer user_data)
{
  GnEventHistoryEntry *entry = GN_EVENT_HISTORY_ENTRY(user_data);
  (void)btn;

  const gchar *event_id = gn_event_history_entry_get_event_id(entry);
  if (!event_id) return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
  gdk_clipboard_set_text(clipboard, event_id);

  g_debug("page-history: copied event ID to clipboard: %s", event_id);
}

/* Create a row widget for a history entry */
static GtkWidget *
create_history_row(GnEventHistoryEntry *entry)
{
  gint kind = gn_event_history_entry_get_event_kind(entry);
  const gchar *client_app = gn_event_history_entry_get_client_app(entry);
  const gchar *client_pubkey = gn_event_history_entry_get_client_pubkey(entry);
  const gchar *method = gn_event_history_entry_get_method(entry);
  GnEventHistoryResult result = gn_event_history_entry_get_result(entry);
  const gchar *content_preview = gn_event_history_entry_get_content_preview(entry);

  /* Create row */
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Title: event kind and method */
  const gchar *kind_name = get_kind_name(kind);
  gchar *title = g_strdup_printf("%s - %s", kind_name, method ? method : "sign_event");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_free(title);

  /* Subtitle: client app/pubkey, timestamp, preview */
  gchar *formatted_time = gn_event_history_entry_format_timestamp(entry);
  gchar *truncated_id = gn_event_history_entry_get_truncated_event_id(entry);

  gchar *client_display;
  if (client_app && *client_app) {
    client_display = g_strdup(client_app);
  } else if (client_pubkey && strlen(client_pubkey) > 12) {
    client_display = g_strdup_printf("%.8s...%.4s", client_pubkey, client_pubkey + strlen(client_pubkey) - 4);
  } else {
    client_display = g_strdup(client_pubkey ? client_pubkey : "Unknown");
  }

  GString *subtitle = g_string_new("");
  g_string_append_printf(subtitle, "%s | %s | %s",
                          client_display, formatted_time, get_result_text(result));

  if (truncated_id && *truncated_id) {
    g_string_append_printf(subtitle, " | ID: %s", truncated_id);
  }

  if (content_preview && *content_preview) {
    /* Truncate preview for display */
    gchar *preview = g_strndup(content_preview, 60);
    if (strlen(content_preview) > 60) {
      gchar *tmp = g_strdup_printf("%s...", preview);
      g_free(preview);
      preview = tmp;
    }
    g_string_append_printf(subtitle, "\n%s", preview);
    g_free(preview);
  }

  adw_action_row_set_subtitle(row, subtitle->str);
  adw_action_row_set_subtitle_lines(row, 2);

  g_string_free(subtitle, TRUE);
  g_free(client_display);
  g_free(formatted_time);
  g_free(truncated_id);

  /* Add result status icon */
  GtkWidget *status_icon = gtk_image_new_from_icon_name(get_result_icon(result));
  gtk_widget_add_css_class(status_icon, get_result_css_class(result));
  adw_action_row_add_prefix(row, status_icon);

  /* Add copy button for event ID */
  const gchar *event_id = gn_event_history_entry_get_event_id(entry);
  if (event_id && *event_id) {
    GtkWidget *copy_btn = gtk_button_new_from_icon_name("edit-copy-symbolic");
    gtk_widget_set_valign(copy_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(copy_btn, "flat");
    gtk_widget_add_css_class(copy_btn, "circular");
    gtk_widget_set_tooltip_text(copy_btn, "Copy full event ID to clipboard");

    g_signal_connect(copy_btn, "clicked",
                     G_CALLBACK(on_copy_event_id_clicked),
                     entry);

    adw_action_row_add_suffix(row, copy_btn);
  }

  return GTK_WIDGET(row);
}

/* Populate the history list */
static void
populate_history_list(GnPageHistory *self)
{
  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->history_list))) != NULL) {
    gtk_list_box_remove(self->history_list, child);
  }

  /* Get filtered entries */
  GnEventHistory *history = gn_event_history_get_default();

  GPtrArray *entries = gn_event_history_filter(history,
    self->filter_kind,
    self->filter_client,
    self->filter_start_time,
    self->filter_end_time,
    self->current_page * PAGE_SIZE,
    PAGE_SIZE);

  /* Calculate total for pagination */
  GPtrArray *all_entries = gn_event_history_filter(history,
    self->filter_kind,
    self->filter_client,
    self->filter_start_time,
    self->filter_end_time,
    0, 0);

  self->total_entries = all_entries->len;
  self->total_pages = (self->total_entries + PAGE_SIZE - 1) / PAGE_SIZE;
  if (self->total_pages == 0) self->total_pages = 1;

  g_ptr_array_unref(all_entries);

  if (!entries || entries->len == 0) {
    gtk_stack_set_visible_child_name(self->empty_stack, "empty");
    if (entries)
      g_ptr_array_unref(entries);
    update_page_info(self);
    update_entry_count(self);
    return;
  }

  gtk_stack_set_visible_child_name(self->empty_stack, "list");

  /* Add rows for each entry */
  for (guint i = 0; i < entries->len; i++) {
    GnEventHistoryEntry *entry = g_ptr_array_index(entries, i);
    GtkWidget *row = create_history_row(entry);
    gtk_list_box_append(self->history_list, row);
  }

  g_ptr_array_unref(entries);

  update_page_info(self);
  update_entry_count(self);
}

/* Update pagination info */
static void
update_page_info(GnPageHistory *self)
{
  gchar *text = g_strdup_printf("Page %u of %u", self->current_page + 1, self->total_pages);
  gtk_label_set_text(self->lbl_page_info, text);
  g_free(text);

  /* Enable/disable pagination buttons */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_prev_page), self->current_page > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next_page),
                            self->current_page < self->total_pages - 1);
}

/* Update entry count label */
static void
update_entry_count(GnPageHistory *self)
{
  GnEventHistory *history = gn_event_history_get_default();
  guint total = gn_event_history_get_entry_count(history);

  gchar *text;
  if (self->filter_kind >= 0 || self->filter_client) {
    text = g_strdup_printf("%u matching / %u total entries", self->total_entries, total);
  } else {
    text = g_strdup_printf("%u entries", total);
  }

  gtk_label_set_text(self->lbl_entry_count, text);
  g_free(text);

  /* Enable/disable action buttons */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_clear_history), total > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_export), total > 0);
}

/* Update filter dropdown models */
static void
update_filter_models(GnPageHistory *self)
{
  GnEventHistory *history = gn_event_history_get_default();

  /* Update kind filter model */
  if (self->kind_model) {
    /* Clear and rebuild */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->kind_model));
    for (guint i = 0; i < n; i++)
      gtk_string_list_remove(self->kind_model, 0);
  }

  if (self->kind_values) {
    g_array_unref(self->kind_values);
  }
  self->kind_values = g_array_new(FALSE, FALSE, sizeof(gint));

  gtk_string_list_append(self->kind_model, "All Kinds");

  gint *kinds = gn_event_history_get_unique_kinds(history);
  if (kinds) {
    for (gint *k = kinds; *k >= 0; k++) {
      const gchar *name = get_kind_name(*k);
      gtk_string_list_append(self->kind_model, name);
      g_array_append_val(self->kind_values, *k);
    }
    g_free(kinds);
  }

  /* Update client filter model */
  if (self->client_model) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->client_model));
    for (guint i = 0; i < n; i++)
      gtk_string_list_remove(self->client_model, 0);
  }

  if (self->client_values) {
    g_ptr_array_unref(self->client_values);
  }
  self->client_values = g_ptr_array_new_with_free_func(g_free);

  gtk_string_list_append(self->client_model, "All Clients");

  gchar **clients = gn_event_history_get_unique_clients(history);
  if (clients) {
    for (gchar **c = clients; *c; c++) {
      /* Truncate for display */
      gchar *display;
      if (strlen(*c) > 16) {
        display = g_strdup_printf("%.8s...%.4s", *c, *c + strlen(*c) - 4);
      } else {
        display = g_strdup(*c);
      }
      gtk_string_list_append(self->client_model, display);
      g_ptr_array_add(self->client_values, g_strdup(*c));
      g_free(display);
    }
    g_strfreev(clients);
  }
}

static void
gn_page_history_dispose(GObject *object)
{
  GnPageHistory *self = GN_PAGE_HISTORY(object);

  g_free(self->filter_client);
  self->filter_client = NULL;

  if (self->kind_values) {
    g_array_unref(self->kind_values);
    self->kind_values = NULL;
  }

  if (self->client_values) {
    g_ptr_array_unref(self->client_values);
    self->client_values = NULL;
  }

  G_OBJECT_CLASS(gn_page_history_parent_class)->dispose(object);
}

static void
gn_page_history_class_init(GnPageHistoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gn_page_history_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
                                              APP_RESOURCE_PATH "/ui/page-history.ui");

  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, history_list);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, lbl_entry_count);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, btn_clear_history);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, btn_export);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, btn_refresh);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, btn_prev_page);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, btn_next_page);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, lbl_page_info);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, empty_stack);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, combo_kind_filter);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, combo_client_filter);
  gtk_widget_class_bind_template_child(widget_class, GnPageHistory, btn_clear_filters);
}

static void
gn_page_history_init(GnPageHistory *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize state */
  self->filter_kind = -1;
  self->filter_client = NULL;
  self->filter_start_time = 0;
  self->filter_end_time = 0;
  self->current_page = 0;
  self->total_pages = 1;
  self->total_entries = 0;

  /* Create filter models */
  self->kind_model = gtk_string_list_new(NULL);
  self->client_model = gtk_string_list_new(NULL);
  self->kind_values = NULL;
  self->client_values = NULL;

  adw_combo_row_set_model(self->combo_kind_filter, G_LIST_MODEL(self->kind_model));
  adw_combo_row_set_model(self->combo_client_filter, G_LIST_MODEL(self->client_model));

  /* Connect button signals */
  g_signal_connect(self->btn_clear_history, "clicked",
                   G_CALLBACK(on_clear_history_clicked), self);
  g_signal_connect(self->btn_export, "clicked",
                   G_CALLBACK(on_export_clicked), self);
  g_signal_connect(self->btn_refresh, "clicked",
                   G_CALLBACK(on_refresh_clicked), self);
  g_signal_connect(self->btn_prev_page, "clicked",
                   G_CALLBACK(on_prev_page_clicked), self);
  g_signal_connect(self->btn_next_page, "clicked",
                   G_CALLBACK(on_next_page_clicked), self);
  g_signal_connect(self->btn_clear_filters, "clicked",
                   G_CALLBACK(on_clear_filters_clicked), self);

  /* Connect filter signals */
  g_signal_connect(self->combo_kind_filter, "notify::selected",
                   G_CALLBACK(on_kind_filter_changed), self);
  g_signal_connect(self->combo_client_filter, "notify::selected",
                   G_CALLBACK(on_client_filter_changed), self);

  /* Initial population */
  update_filter_models(self);
  populate_history_list(self);
}

GnPageHistory *
gn_page_history_new(void)
{
  return g_object_new(GN_TYPE_PAGE_HISTORY, NULL);
}

void
gn_page_history_refresh(GnPageHistory *self)
{
  g_return_if_fail(GN_IS_PAGE_HISTORY(self));

  /* Reload history from disk */
  GnEventHistory *history = gn_event_history_get_default();
  gn_event_history_load(history);

  /* Update UI */
  update_filter_models(self);
  populate_history_list(self);
}

void
gn_page_history_clear_filters(GnPageHistory *self)
{
  g_return_if_fail(GN_IS_PAGE_HISTORY(self));

  self->filter_kind = -1;
  g_free(self->filter_client);
  self->filter_client = NULL;
  self->filter_start_time = 0;
  self->filter_end_time = 0;
  self->current_page = 0;

  /* Reset combo boxes */
  adw_combo_row_set_selected(self->combo_kind_filter, 0);
  adw_combo_row_set_selected(self->combo_client_filter, 0);

  populate_history_list(self);
}
