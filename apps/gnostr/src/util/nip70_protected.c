/**
 * @file nip70_protected.c
 * @brief NIP-70 Protected Events implementation
 *
 * Implements detection and handling of protected events marked with the "-" tag.
 * See NIP-70: https://github.com/nostr-protocol/nips/blob/master/70.md
 */

#include "nip70_protected.h"
#include <jansson.h>
#include <string.h>

/* CSS class for protected badge styling */
#define NIP70_BADGE_CSS_CLASS "nip70-protected-badge"

/* Default tooltip for protected events */
#define NIP70_DEFAULT_TOOLTIP "Protected Event - This note is marked for limited distribution and should not be rebroadcast"

/* Protection tag marker */
#define NIP70_PROTECTION_TAG "-"

const char *gnostr_nip70_status_to_string(GnostrProtectedStatus status) {
  switch (status) {
    case GNOSTR_PROTECTED_STATUS_UNKNOWN:
      return "unknown";
    case GNOSTR_PROTECTED_STATUS_UNPROTECTED:
      return "unprotected";
    case GNOSTR_PROTECTED_STATUS_PROTECTED:
      return "protected";
    default:
      return "unknown";
  }
}

GnostrProtectedResult *gnostr_nip70_result_new(void) {
  GnostrProtectedResult *result = g_new0(GnostrProtectedResult, 1);
  result->status = GNOSTR_PROTECTED_STATUS_UNKNOWN;
  return result;
}

void gnostr_nip70_result_free(GnostrProtectedResult *result) {
  if (!result) return;
  g_free(result->event_id);
  g_free(result->relay_hint);
  g_free(result);
}

/**
 * Internal helper to check if a tags array contains the "-" protection tag.
 * @param tags JSON array of tags
 * @return TRUE if protected
 */
static gboolean check_tags_for_protection(json_t *tags) {
  if (!tags || !json_is_array(tags)) {
    return FALSE;
  }

  size_t tag_count = json_array_size(tags);
  for (size_t i = 0; i < tag_count; i++) {
    json_t *tag = json_array_get(tags, i);
    if (!tag || !json_is_array(tag) || json_array_size(tag) < 1) {
      continue;
    }

    json_t *tag_name = json_array_get(tag, 0);
    if (tag_name && json_is_string(tag_name)) {
      const char *name = json_string_value(tag_name);
      if (name && g_strcmp0(name, NIP70_PROTECTION_TAG) == 0) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

gboolean gnostr_nip70_check_event(const char *event_json) {
  if (!event_json || !*event_json) {
    return FALSE;
  }

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_debug("nip70: failed to parse event JSON: %s", error.text);
    return FALSE;
  }

  json_t *tags = json_object_get(root, "tags");
  gboolean is_protected = check_tags_for_protection(tags);

  json_decref(root);
  return is_protected;
}

gboolean gnostr_nip70_check_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) {
    return FALSE;
  }

  json_error_t error;
  json_t *tags = json_loads(tags_json, 0, &error);
  if (!tags) {
    g_debug("nip70: failed to parse tags JSON: %s", error.text);
    return FALSE;
  }

  gboolean is_protected = check_tags_for_protection(tags);

  json_decref(tags);
  return is_protected;
}

char *gnostr_nip70_add_protection_tag(const char *tags_json) {
  json_t *tags = NULL;

  if (tags_json && *tags_json) {
    json_error_t error;
    tags = json_loads(tags_json, 0, &error);
    if (!tags) {
      g_debug("nip70: failed to parse tags JSON for add: %s", error.text);
      /* Create new empty array */
      tags = json_array();
    }
  } else {
    tags = json_array();
  }

  if (!json_is_array(tags)) {
    json_decref(tags);
    tags = json_array();
  }

  /* Check if protection tag already exists */
  if (check_tags_for_protection(tags)) {
    /* Already protected, return as-is */
    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);
    return result;
  }

  /* Add protection tag */
  json_t *protection_tag = json_array();
  json_array_append_new(protection_tag, json_string(NIP70_PROTECTION_TAG));
  json_array_append_new(tags, protection_tag);

  char *result = json_dumps(tags, JSON_COMPACT);
  json_decref(tags);

  g_debug("nip70: added protection tag to event");
  return result;
}

char *gnostr_nip70_remove_protection_tag(const char *tags_json) {
  if (!tags_json || !*tags_json) {
    return g_strdup("[]");
  }

  json_error_t error;
  json_t *tags = json_loads(tags_json, 0, &error);
  if (!tags) {
    g_debug("nip70: failed to parse tags JSON for remove: %s", error.text);
    return g_strdup(tags_json);
  }

  if (!json_is_array(tags)) {
    json_decref(tags);
    return g_strdup(tags_json);
  }

  /* Create new array without protection tag */
  json_t *new_tags = json_array();
  size_t tag_count = json_array_size(tags);

  for (size_t i = 0; i < tag_count; i++) {
    json_t *tag = json_array_get(tags, i);
    if (!tag || !json_is_array(tag) || json_array_size(tag) < 1) {
      json_array_append(new_tags, tag);
      continue;
    }

    json_t *tag_name = json_array_get(tag, 0);
    if (tag_name && json_is_string(tag_name)) {
      const char *name = json_string_value(tag_name);
      if (name && g_strcmp0(name, NIP70_PROTECTION_TAG) == 0) {
        /* Skip protection tag */
        g_debug("nip70: removed protection tag from event");
        continue;
      }
    }

    json_array_append(new_tags, tag);
  }

  char *result = json_dumps(new_tags, JSON_COMPACT);
  json_decref(tags);
  json_decref(new_tags);

  return result;
}

char *gnostr_nip70_build_protection_tag(void) {
  json_t *tag = json_array();
  json_array_append_new(tag, json_string(NIP70_PROTECTION_TAG));

  char *result = json_dumps(tag, JSON_COMPACT);
  json_decref(tag);

  return result;
}

gboolean gnostr_nip70_can_rebroadcast(const char *event_json) {
  /* Protected events should NOT be rebroadcast */
  return !gnostr_nip70_check_event(event_json);
}

gboolean gnostr_nip70_should_warn_repost(const char *event_json) {
  /* Warn if event is protected */
  return gnostr_nip70_check_event(event_json);
}

/* --- UI Widget Helpers --- */

GtkWidget *gnostr_nip70_create_protected_badge(void) {
  return gnostr_nip70_create_protected_badge_with_tooltip(NULL);
}

GtkWidget *gnostr_nip70_create_protected_badge_with_tooltip(const char *tooltip) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(box, NIP70_BADGE_CSS_CLASS);

  /* Lock icon */
  GtkWidget *icon = gtk_image_new_from_icon_name("channel-secure-symbolic");
  gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_NORMAL);
  gtk_box_append(GTK_BOX(box), icon);

  /* "Protected" label */
  GtkWidget *label = gtk_label_new("Protected");
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_box_append(GTK_BOX(box), label);

  /* Set tooltip */
  const char *tip = tooltip ? tooltip : NIP70_DEFAULT_TOOLTIP;
  gtk_widget_set_tooltip_text(box, tip);

  return box;
}

/* Warning dialog response callback context */
typedef struct {
  GnostrNip70WarningCallback callback;
  gpointer user_data;
} WarningDialogContext;

static void on_warning_dialog_response(GObject *source,
                                        GAsyncResult *result,
                                        gpointer user_data) {
  WarningDialogContext *ctx = (WarningDialogContext *)user_data;
  GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);

  GError *error = NULL;
  int response = gtk_alert_dialog_choose_finish(dialog, result, &error);

  gboolean proceed = FALSE;
  if (error) {
    g_debug("nip70: warning dialog error: %s", error->message);
    g_error_free(error);
  } else {
    /* Button 0 is "Proceed Anyway", Button 1 is "Cancel" */
    proceed = (response == 0);
  }

  if (ctx->callback) {
    ctx->callback(proceed, ctx->user_data);
  }

  g_free(ctx);
}

void gnostr_nip70_show_repost_warning_dialog(GtkWindow *parent,
                                              const char *event_id_hex,
                                              GnostrNip70WarningCallback callback,
                                              gpointer user_data) {
  GtkAlertDialog *dialog = gtk_alert_dialog_new("Repost Protected Event?");

  char *short_id = NULL;
  if (event_id_hex && strlen(event_id_hex) >= 8) {
    short_id = g_strndup(event_id_hex, 8);
  }

  char *detail = g_strdup_printf(
    "This note%s%s is marked as protected (NIP-70).\n\n"
    "Protected events are meant for limited distribution and should not "
    "normally be rebroadcast to other relays.\n\n"
    "Are you sure you want to repost this note?",
    short_id ? " (" : "",
    short_id ? short_id : ""
  );

  gtk_alert_dialog_set_detail(dialog, detail);
  g_free(detail);
  g_free(short_id);

  const char *buttons[] = {"Proceed Anyway", "Cancel", NULL};
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 1);
  gtk_alert_dialog_set_default_button(dialog, 1);

  WarningDialogContext *ctx = g_new0(WarningDialogContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;

  gtk_alert_dialog_choose(dialog, parent, NULL, on_warning_dialog_response, ctx);
  g_object_unref(dialog);
}

/* --- Composer Integration --- */

GtkWidget *gnostr_nip70_create_protection_toggle(void) {
  GtkWidget *button = gtk_toggle_button_new();

  /* Create content box */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  /* Lock icon */
  GtkWidget *icon = gtk_image_new_from_icon_name("channel-secure-symbolic");
  gtk_box_append(GTK_BOX(box), icon);

  /* Label */
  GtkWidget *label = gtk_label_new("Protected");
  gtk_box_append(GTK_BOX(box), label);

  gtk_button_set_child(GTK_BUTTON(button), box);

  /* Styling */
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "nip70-protection-toggle");

  /* Tooltip explaining the feature */
  gtk_widget_set_tooltip_text(button,
    "Mark this note as protected (NIP-70).\n"
    "Protected notes are meant only for the specific relay they're published to "
    "and should not be rebroadcast by relays.");

  return button;
}

gboolean gnostr_nip70_get_toggle_state(GtkWidget *toggle) {
  g_return_val_if_fail(GTK_IS_TOGGLE_BUTTON(toggle), FALSE);
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
}

void gnostr_nip70_set_toggle_state(GtkWidget *toggle, gboolean protected) {
  g_return_if_fail(GTK_IS_TOGGLE_BUTTON(toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), protected);
}
