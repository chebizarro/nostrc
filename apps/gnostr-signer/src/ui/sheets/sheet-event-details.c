/* sheet-event-details.c - Event Details View dialog implementation
 *
 * Displays full information about a signed Nostr event with copy buttons
 * for each field and expandable content/tags sections.
 */
#include "sheet-event-details.h"
#include "../app-resources.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

struct _SheetEventDetails {
  AdwDialog parent_instance;

  /* Template children - Header */
  GtkButton *btn_close;

  /* Template children - Event Info */
  GtkLabel *lbl_kind;
  GtkLabel *lbl_kind_name;
  GtkLabel *lbl_datetime;

  /* Template children - Copyable fields */
  AdwActionRow *row_pubkey;
  AdwActionRow *row_event_id;
  AdwActionRow *row_signature;
  GtkButton *btn_copy_pubkey;
  GtkButton *btn_copy_event_id;
  GtkButton *btn_copy_signature;

  /* Template children - Expandable sections */
  GtkExpander *expander_content;
  GtkLabel *lbl_content;
  GtkExpander *expander_tags;
  GtkListBox *list_tags;

  /* State - full values for clipboard */
  gchar *pubkey_full;
  gchar *event_id_full;
  gchar *signature_full;
  gchar *content_full;
};

G_DEFINE_TYPE(SheetEventDetails, sheet_event_details, ADW_TYPE_DIALOG)

/* Get human-readable name for event kind */
static const gchar *get_kind_name(gint kind) {
  switch (kind) {
    case 0: return "Metadata";
    case 1: return "Short Text Note";
    case 2: return "Recommend Relay";
    case 3: return "Contacts";
    case 4: return "Encrypted Direct Message";
    case 5: return "Event Deletion";
    case 6: return "Repost";
    case 7: return "Reaction";
    case 8: return "Badge Award";
    case 40: return "Channel Creation";
    case 41: return "Channel Metadata";
    case 42: return "Channel Message";
    case 43: return "Channel Hide Message";
    case 44: return "Channel Mute User";
    case 1063: return "File Metadata";
    case 1984: return "Report";
    case 9734: return "Zap Request";
    case 9735: return "Zap";
    case 10000: return "Mute List";
    case 10001: return "Pin List";
    case 10002: return "Relay List Metadata";
    case 13194: return "Wallet Info";
    case 22242: return "Client Authentication";
    case 23194: return "Wallet Request";
    case 23195: return "Wallet Response";
    case 24133: return "Nostr Connect";
    case 27235: return "HTTP Auth";
    case 30000: return "Categorized People List";
    case 30001: return "Categorized Bookmark List";
    case 30008: return "Profile Badges";
    case 30009: return "Badge Definition";
    case 30023: return "Long-form Content";
    case 30078: return "Application-specific Data";
    default:
      if (kind >= 10000 && kind < 20000) return "Replaceable Event";
      if (kind >= 20000 && kind < 30000) return "Ephemeral Event";
      if (kind >= 30000 && kind < 40000) return "Parameterized Replaceable Event";
      return "Unknown";
  }
}

/* Truncate a hex string for display (first 8...last 6 chars) */
static gchar *truncate_hex(const gchar *hex, gsize show_start, gsize show_end) {
  if (!hex) return g_strdup("(none)");
  gsize len = strlen(hex);
  if (len <= show_start + show_end + 3) {
    return g_strdup(hex);
  }
  return g_strdup_printf("%.*s...%s", (int)show_start, hex, hex + len - show_end);
}

/* Copy text to clipboard */
static void copy_to_clipboard(GtkWidget *widget, const gchar *text) {
  if (!text || !widget) return;

  GdkDisplay *display = gtk_widget_get_display(widget);
  if (!display) return;

  GdkClipboard *clipboard = gdk_display_get_clipboard(display);
  if (clipboard) {
    gdk_clipboard_set_text(clipboard, text);
  }
}

/* Button handlers */
static void on_close(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetEventDetails *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
}

static void on_copy_pubkey(GtkButton *btn, gpointer user_data) {
  SheetEventDetails *self = user_data;
  copy_to_clipboard(GTK_WIDGET(btn), self->pubkey_full);
}

static void on_copy_event_id(GtkButton *btn, gpointer user_data) {
  SheetEventDetails *self = user_data;
  copy_to_clipboard(GTK_WIDGET(btn), self->event_id_full);
}

static void on_copy_signature(GtkButton *btn, gpointer user_data) {
  SheetEventDetails *self = user_data;
  copy_to_clipboard(GTK_WIDGET(btn), self->signature_full);
}

/* Format Unix timestamp to human-readable datetime */
static gchar *format_datetime(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup("(unknown)");

  time_t t = (time_t)timestamp;
  struct tm *tm_info = localtime(&t);
  if (!tm_info) return g_strdup("(invalid)");

  gchar buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
  return g_strdup(buf);
}

/* Create a row for a tag in the tags list */
static GtkWidget *create_tag_row(JsonArray *tag_array) {
  if (!tag_array) return NULL;

  guint len = json_array_get_length(tag_array);
  if (len == 0) return NULL;

  GString *tag_str = g_string_new("[");

  for (guint i = 0; i < len; i++) {
    JsonNode *elem = json_array_get_element(tag_array, i);
    if (json_node_get_value_type(elem) == G_TYPE_STRING) {
      const gchar *val = json_node_get_string(elem);
      if (i > 0) g_string_append(tag_str, ", ");
      g_string_append_printf(tag_str, "\"%s\"", val ? val : "");
    }
  }
  g_string_append(tag_str, "]");

  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), tag_str->str);
  gtk_widget_add_css_class(row, "monospace");

  gchar *result = g_string_free(tag_str, FALSE);
  g_free(result);

  return row;
}

/* Parse and display tags from JSON array string */
static void display_tags(SheetEventDetails *self, const gchar *tags_json) {
  if (!self->list_tags) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_tags))) != NULL) {
    gtk_list_box_remove(self->list_tags, child);
  }

  if (!tags_json || !*tags_json) {
    GtkWidget *empty_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(empty_row), "(no tags)");
    gtk_list_box_append(self->list_tags, empty_row);
    return;
  }

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    GtkWidget *error_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(error_row), "(invalid JSON)");
    gtk_list_box_append(self->list_tags, error_row);
    g_clear_error(&error);
    g_object_unref(parser);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
    GtkWidget *error_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(error_row), "(not an array)");
    gtk_list_box_append(self->list_tags, error_row);
    g_object_unref(parser);
    return;
  }

  JsonArray *tags = json_node_get_array(root);
  guint tag_count = json_array_get_length(tags);

  if (tag_count == 0) {
    GtkWidget *empty_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(empty_row), "(no tags)");
    gtk_list_box_append(self->list_tags, empty_row);
  } else {
    for (guint i = 0; i < tag_count; i++) {
      JsonNode *tag_node = json_array_get_element(tags, i);
      if (tag_node && JSON_NODE_HOLDS_ARRAY(tag_node)) {
        JsonArray *tag_array = json_node_get_array(tag_node);
        GtkWidget *row = create_tag_row(tag_array);
        if (row) {
          gtk_list_box_append(self->list_tags, row);
        }
      }
    }
  }

  g_object_unref(parser);
}

static void sheet_event_details_finalize(GObject *obj) {
  SheetEventDetails *self = SHEET_EVENT_DETAILS(obj);

  g_free(self->pubkey_full);
  g_free(self->event_id_full);
  g_free(self->signature_full);
  g_free(self->content_full);

  G_OBJECT_CLASS(sheet_event_details_parent_class)->finalize(obj);
}

static void sheet_event_details_class_init(SheetEventDetailsClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_event_details_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-event-details.ui");

  /* Header */
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, btn_close);

  /* Event info labels */
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, lbl_kind);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, lbl_kind_name);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, lbl_datetime);

  /* Copyable field rows */
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, row_pubkey);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, row_event_id);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, row_signature);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, btn_copy_pubkey);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, btn_copy_event_id);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, btn_copy_signature);

  /* Expandable sections */
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, expander_content);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, lbl_content);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, expander_tags);
  gtk_widget_class_bind_template_child(wc, SheetEventDetails, list_tags);
}

static void sheet_event_details_init(SheetEventDetails *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect button handlers */
  if (self->btn_close) {
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close), self);
  }
  if (self->btn_copy_pubkey) {
    g_signal_connect(self->btn_copy_pubkey, "clicked", G_CALLBACK(on_copy_pubkey), self);
  }
  if (self->btn_copy_event_id) {
    g_signal_connect(self->btn_copy_event_id, "clicked", G_CALLBACK(on_copy_event_id), self);
  }
  if (self->btn_copy_signature) {
    g_signal_connect(self->btn_copy_signature, "clicked", G_CALLBACK(on_copy_signature), self);
  }
}

SheetEventDetails *sheet_event_details_new(void) {
  return g_object_new(TYPE_SHEET_EVENT_DETAILS, NULL);
}

void sheet_event_details_set_event(SheetEventDetails *self,
                                   gint kind,
                                   gint64 created_at,
                                   const gchar *pubkey,
                                   const gchar *event_id,
                                   const gchar *signature,
                                   const gchar *content,
                                   const gchar *tags_json) {
  g_return_if_fail(self != NULL);

  /* Store full values for clipboard */
  g_free(self->pubkey_full);
  g_free(self->event_id_full);
  g_free(self->signature_full);
  g_free(self->content_full);

  self->pubkey_full = g_strdup(pubkey);
  self->event_id_full = g_strdup(event_id);
  self->signature_full = g_strdup(signature);
  self->content_full = g_strdup(content);

  /* Update kind display */
  if (self->lbl_kind) {
    gchar *kind_str = g_strdup_printf("%d", kind);
    gtk_label_set_text(self->lbl_kind, kind_str);
    g_free(kind_str);
  }
  if (self->lbl_kind_name) {
    gtk_label_set_text(self->lbl_kind_name, get_kind_name(kind));
  }

  /* Update datetime */
  if (self->lbl_datetime) {
    gchar *dt = format_datetime(created_at);
    gtk_label_set_text(self->lbl_datetime, dt);
    g_free(dt);
  }

  /* Update copyable field rows with truncated display */
  if (self->row_pubkey) {
    gchar *truncated = truncate_hex(pubkey, 12, 8);
    adw_action_row_set_subtitle(self->row_pubkey, truncated);
    g_free(truncated);
  }
  if (self->row_event_id) {
    gchar *truncated = truncate_hex(event_id, 12, 8);
    adw_action_row_set_subtitle(self->row_event_id, truncated);
    g_free(truncated);
  }
  if (self->row_signature) {
    gchar *truncated = truncate_hex(signature, 12, 8);
    adw_action_row_set_subtitle(self->row_signature, truncated);
    g_free(truncated);
  }

  /* Update content */
  if (self->lbl_content) {
    gtk_label_set_text(self->lbl_content, content ? content : "(empty)");
  }

  /* Update tags */
  display_tags(self, tags_json);
}

void sheet_event_details_set_event_json(SheetEventDetails *self,
                                        const gchar *event_json) {
  g_return_if_fail(self != NULL);

  if (!event_json || !*event_json) {
    sheet_event_details_set_event(self, 0, 0, NULL, NULL, NULL, "(no event)", "[]");
    return;
  }

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("Failed to parse event JSON: %s", error->message);
    g_clear_error(&error);
    g_object_unref(parser);
    sheet_event_details_set_event(self, 0, 0, NULL, NULL, NULL, "(parse error)", "[]");
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    sheet_event_details_set_event(self, 0, 0, NULL, NULL, NULL, "(not an object)", "[]");
    return;
  }

  JsonObject *obj = json_node_get_object(root);

  gint kind = 0;
  gint64 created_at = 0;
  const gchar *pubkey = NULL;
  const gchar *event_id = NULL;
  const gchar *sig = NULL;
  const gchar *content = NULL;
  gchar *tags_json = NULL;

  if (json_object_has_member(obj, "kind")) {
    kind = (gint)json_object_get_int_member(obj, "kind");
  }
  if (json_object_has_member(obj, "created_at")) {
    created_at = json_object_get_int_member(obj, "created_at");
  }
  if (json_object_has_member(obj, "pubkey")) {
    pubkey = json_object_get_string_member(obj, "pubkey");
  }
  if (json_object_has_member(obj, "id")) {
    event_id = json_object_get_string_member(obj, "id");
  }
  if (json_object_has_member(obj, "sig")) {
    sig = json_object_get_string_member(obj, "sig");
  }
  if (json_object_has_member(obj, "content")) {
    content = json_object_get_string_member(obj, "content");
  }
  if (json_object_has_member(obj, "tags")) {
    JsonNode *tags_node = json_object_get_member(obj, "tags");
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, tags_node);
    tags_json = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
  }

  sheet_event_details_set_event(self, kind, created_at, pubkey, event_id, sig, content, tags_json);

  g_free(tags_json);
  g_object_unref(parser);
}
