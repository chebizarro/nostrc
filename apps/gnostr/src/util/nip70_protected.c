/**
 * @file nip70_protected.c
 * @brief NIP-70 Protected Events implementation
 *
 * Implements detection and handling of protected events marked with the "-" tag.
 * See NIP-70: https://github.com/nostr-protocol/nips/blob/master/70.md
 */

#include "nip70_protected.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include <string.h>
#include <stdlib.h>

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

/* Callback context for checking protection tag */
typedef struct {
  gboolean found;
} CheckProtectionCtx;

/**
 * Callback to check if a tag is the "-" protection tag.
 */
static gboolean check_protection_tag_cb(gsize index, const gchar *element_json, gpointer user_data) {
  (void)index;
  CheckProtectionCtx *ctx = user_data;

  /* Get first element of tag array (tag name) */
  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (tag_name) {
    if (g_strcmp0(tag_name, NIP70_PROTECTION_TAG) == 0) {
      ctx->found = TRUE;
      free(tag_name);
      return false; /* stop iteration */
    }
    free(tag_name);
  }
  return true; /* continue iteration */
}

/**
 * Internal helper to check if a tags JSON array contains the "-" protection tag.
 * @param tags_json JSON array string of tags
 * @return TRUE if protected
 */
static gboolean check_tags_json_for_protection(const char *tags_json) {
  if (!tags_json || !*tags_json) {
    return FALSE;
  }

  if (!gnostr_json_is_array_str(tags_json)) {
    return FALSE;
  }

  CheckProtectionCtx ctx = { .found = FALSE };
  gnostr_json_array_foreach_root(tags_json, check_protection_tag_cb, &ctx);
  return ctx.found;
}

gboolean gnostr_nip70_check_event(const char *event_json) {
  if (!event_json || !*event_json) {
    return FALSE;
  }

  if (!gnostr_json_is_valid(event_json)) {
    g_debug("nip70: failed to parse event JSON");
    return FALSE;
  }

  /* Get the raw tags array as JSON string */
  char *tags_json = NULL;
  tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
  if (!tags_json) {
    return FALSE;
  }

  gboolean is_protected = check_tags_json_for_protection(tags_json);
  free(tags_json);
  return is_protected;
}

gboolean gnostr_nip70_check_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) {
    return FALSE;
  }

  if (!gnostr_json_is_valid(tags_json)) {
    g_debug("nip70: failed to parse tags JSON");
    return FALSE;
  }

  return check_tags_json_for_protection(tags_json);
}

/* Callback context for copying tags */
typedef struct {
  GNostrJsonBuilder *builder;
} CopyTagsCtx;

/**
 * Callback to copy a tag element as-is.
 */
static gboolean copy_tag_cb(gsize index, const gchar *element_json, gpointer user_data) {
  (void)index;
  CopyTagsCtx *ctx = user_data;
  gnostr_json_builder_add_raw(ctx->builder, element_json);
  return true; /* continue iteration */
}

char *gnostr_nip70_add_protection_tag(const char *tags_json) {
  /* Check if already protected */
  if (tags_json && *tags_json && gnostr_json_is_array_str(tags_json)) {
    if (check_tags_json_for_protection(tags_json)) {
      /* Already protected, return as-is */
      return gnostr_json_compact_string(tags_json, NULL);
    }
  }

  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  if (!builder) {
    return g_strdup("[]");
  }

  gnostr_json_builder_begin_array(builder);

  /* Copy existing tags if present */
  if (tags_json && *tags_json && gnostr_json_is_array_str(tags_json)) {
    CopyTagsCtx ctx = { .builder = builder };
    gnostr_json_array_foreach_root(tags_json, copy_tag_cb, &ctx);
  }

  /* Add protection tag: ["-"] */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, NIP70_PROTECTION_TAG);
  gnostr_json_builder_end_array(builder);

  gnostr_json_builder_end_array(builder);

  char *result = gnostr_json_builder_finish(builder);

  g_debug("nip70: added protection tag to event");
  return result;
}

/* Callback context for filtering tags */
typedef struct {
  GNostrJsonBuilder *builder;
  gboolean removed;
} FilterTagsCtx;

/**
 * Callback to copy tags except protection tags.
 */
static gboolean filter_protection_tag_cb(gsize index, const gchar *element_json, gpointer user_data) {
  (void)index;
  FilterTagsCtx *ctx = user_data;

  /* Get first element of tag array (tag name) */
  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (tag_name) {
    if (g_strcmp0(tag_name, NIP70_PROTECTION_TAG) == 0) {
      /* Skip protection tag */
      ctx->removed = TRUE;
      g_debug("nip70: removed protection tag from event");
      free(tag_name);
      return true; /* continue to next tag */
    }
    free(tag_name);
  }

  /* Copy this tag */
  gnostr_json_builder_add_raw(ctx->builder, element_json);
  return true; /* continue iteration */
}

char *gnostr_nip70_remove_protection_tag(const char *tags_json) {
  if (!tags_json || !*tags_json) {
    return g_strdup("[]");
  }

  if (!gnostr_json_is_valid(tags_json) || !gnostr_json_is_array_str(tags_json)) {
    g_debug("nip70: failed to parse tags JSON for remove");
    return g_strdup(tags_json);
  }

  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  if (!builder) {
    return g_strdup(tags_json);
  }

  gnostr_json_builder_begin_array(builder);

  FilterTagsCtx ctx = { .builder = builder, .removed = FALSE };
  gnostr_json_array_foreach_root(tags_json, filter_protection_tag_cb, &ctx);

  gnostr_json_builder_end_array(builder);

  char *result = gnostr_json_builder_finish(builder);

  return result;
}

char *gnostr_nip70_build_protection_tag(void) {
  /* Build ["-"] */
  return gnostr_json_build_string_array(NIP70_PROTECTION_TAG, NULL);
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
