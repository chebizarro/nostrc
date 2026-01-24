/**
 * GnostrStatusDialog - NIP-38 User Status Setting Dialog
 *
 * A dialog for setting or clearing user status (general/music).
 */

#include "gnostr-status-dialog.h"
#include "../util/user_status.h"
#include <glib/gi18n.h>

struct _GnostrStatusDialog {
  AdwDialog parent_instance;

  /* Status type tabs */
  GtkWidget *status_type_switcher;
  GtkWidget *status_stack;

  /* General status page */
  GtkWidget *general_entry;
  GtkWidget *general_link_entry;
  GtkWidget *general_expiration_combo;

  /* Music status page */
  GtkWidget *music_entry;
  GtkWidget *music_link_entry;
  GtkWidget *music_expiration_combo;

  /* Action buttons */
  GtkWidget *btn_save;
  GtkWidget *btn_clear;

  /* State */
  gboolean saving;
};

G_DEFINE_TYPE(GnostrStatusDialog, gnostr_status_dialog, ADW_TYPE_DIALOG)

enum {
  SIGNAL_STATUS_UPDATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Expiration options (in seconds) */
typedef struct {
  const char *label;
  gint64 seconds;
} ExpirationOption;

static const ExpirationOption EXPIRATION_OPTIONS[] = {
  { "No expiration", 0 },
  { "1 hour", 3600 },
  { "4 hours", 14400 },
  { "12 hours", 43200 },
  { "1 day", 86400 },
  { "1 week", 604800 },
};
static const gsize NUM_EXPIRATION_OPTIONS = G_N_ELEMENTS(EXPIRATION_OPTIONS);

static void show_toast(GnostrStatusDialog *self, const char *message) {
  GtkWidget *parent = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_APPLICATION_WINDOW);
  if (parent && ADW_IS_APPLICATION_WINDOW(parent)) {
    AdwToast *toast = adw_toast_new(message);
    adw_toast_set_timeout(toast, 3);
    /* Find toast overlay in the window */
    GtkWidget *overlay = gtk_widget_get_first_child(parent);
    while (overlay) {
      if (ADW_IS_TOAST_OVERLAY(overlay)) {
        adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), toast);
        return;
      }
      overlay = gtk_widget_get_next_sibling(overlay);
    }
  }
  g_message("Status: %s", message);
}

static void on_status_published(gboolean success, const gchar *error_msg, gpointer user_data) {
  GnostrStatusDialog *self = GNOSTR_STATUS_DIALOG(user_data);

  if (!GNOSTR_IS_STATUS_DIALOG(self)) return;

  self->saving = FALSE;
  gtk_widget_set_sensitive(self->btn_save, TRUE);
  gtk_widget_set_sensitive(self->btn_clear, TRUE);

  if (success) {
    show_toast(self, "Status updated");
    g_signal_emit(self, signals[SIGNAL_STATUS_UPDATED], 0);
    adw_dialog_close(ADW_DIALOG(self));
  } else {
    show_toast(self, error_msg ? error_msg : "Failed to update status");
  }
}

static gint64 get_expiration_seconds(GtkDropDown *dropdown) {
  guint selected = gtk_drop_down_get_selected(dropdown);
  if (selected < NUM_EXPIRATION_OPTIONS) {
    return EXPIRATION_OPTIONS[selected].seconds;
  }
  return 0;
}

static void on_save_clicked(GtkButton *button, GnostrStatusDialog *self) {
  (void)button;

  if (self->saving) return;
  self->saving = TRUE;

  gtk_widget_set_sensitive(self->btn_save, FALSE);
  gtk_widget_set_sensitive(self->btn_clear, FALSE);

  /* Get current page */
  const char *visible = gtk_stack_get_visible_child_name(GTK_STACK(self->status_stack));
  gboolean is_general = (g_strcmp0(visible, "general") == 0);

  GtkEditable *entry = is_general ?
    GTK_EDITABLE(self->general_entry) : GTK_EDITABLE(self->music_entry);
  GtkEditable *link_entry = is_general ?
    GTK_EDITABLE(self->general_link_entry) : GTK_EDITABLE(self->music_link_entry);
  GtkDropDown *exp_combo = is_general ?
    GTK_DROP_DOWN(self->general_expiration_combo) : GTK_DROP_DOWN(self->music_expiration_combo);

  const gchar *content = gtk_editable_get_text(entry);
  const gchar *link_url = gtk_editable_get_text(link_entry);
  gint64 exp_seconds = get_expiration_seconds(exp_combo);

  GnostrUserStatusType type = is_general ? GNOSTR_STATUS_GENERAL : GNOSTR_STATUS_MUSIC;

  g_debug("[STATUS_DIALOG] Publishing %s status: \"%s\" (link: %s, expiration: %" G_GINT64_FORMAT "s)",
          is_general ? "general" : "music",
          content ? content : "",
          link_url ? link_url : "(none)",
          exp_seconds);

  gnostr_user_status_publish_async(
    type,
    content,
    (link_url && *link_url) ? link_url : NULL,
    exp_seconds,
    on_status_published,
    self);
}

static void on_clear_clicked(GtkButton *button, GnostrStatusDialog *self) {
  (void)button;

  if (self->saving) return;
  self->saving = TRUE;

  gtk_widget_set_sensitive(self->btn_save, FALSE);
  gtk_widget_set_sensitive(self->btn_clear, FALSE);

  /* Get current page */
  const char *visible = gtk_stack_get_visible_child_name(GTK_STACK(self->status_stack));
  gboolean is_general = (g_strcmp0(visible, "general") == 0);
  GnostrUserStatusType type = is_general ? GNOSTR_STATUS_GENERAL : GNOSTR_STATUS_MUSIC;

  g_debug("[STATUS_DIALOG] Clearing %s status", is_general ? "general" : "music");

  gnostr_user_status_clear_async(type, on_status_published, self);
}

static void populate_expiration_combo(GtkDropDown *dropdown) {
  GtkStringList *model = gtk_string_list_new(NULL);
  for (gsize i = 0; i < NUM_EXPIRATION_OPTIONS; i++) {
    gtk_string_list_append(model, EXPIRATION_OPTIONS[i].label);
  }
  gtk_drop_down_set_model(dropdown, G_LIST_MODEL(model));
  gtk_drop_down_set_selected(dropdown, 0);
  g_object_unref(model);
}

static GtkWidget *create_status_page(GnostrStatusDialog *self,
                                      const char *title,
                                      GtkWidget **out_entry,
                                      GtkWidget **out_link_entry,
                                      GtkWidget **out_exp_combo) {
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(page, 12);
  gtk_widget_set_margin_bottom(page, 12);
  gtk_widget_set_margin_start(page, 12);
  gtk_widget_set_margin_end(page, 12);

  /* Status entry */
  GtkWidget *status_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(status_group), title);
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(status_group),
    "Share what you're up to with your followers");

  GtkWidget *entry_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry_row), "Status");
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(entry_row), FALSE);
  gtk_editable_set_max_width_chars(GTK_EDITABLE(entry_row), 100);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(status_group), entry_row);
  *out_entry = entry_row;

  /* Link entry */
  GtkWidget *link_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(link_row), "Link (optional)");
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(link_row), FALSE);
  gtk_editable_set_max_width_chars(GTK_EDITABLE(link_row), 200);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(status_group), link_row);
  *out_link_entry = link_row;

  /* Expiration dropdown */
  GtkWidget *exp_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(exp_row), "Expiration");

  GtkStringList *exp_model = gtk_string_list_new(NULL);
  for (gsize i = 0; i < NUM_EXPIRATION_OPTIONS; i++) {
    gtk_string_list_append(exp_model, EXPIRATION_OPTIONS[i].label);
  }
  adw_combo_row_set_model(ADW_COMBO_ROW(exp_row), G_LIST_MODEL(exp_model));
  adw_combo_row_set_selected(ADW_COMBO_ROW(exp_row), 0);
  g_object_unref(exp_model);

  adw_preferences_group_add(ADW_PREFERENCES_GROUP(status_group), exp_row);
  *out_exp_combo = exp_row;

  gtk_box_append(GTK_BOX(page), status_group);

  return page;
}

static void gnostr_status_dialog_init(GnostrStatusDialog *self) {
  /* Set dialog properties */
  adw_dialog_set_title(ADW_DIALOG(self), "Set Status");
  adw_dialog_set_content_width(ADW_DIALOG(self), 400);
  adw_dialog_set_content_height(ADW_DIALOG(self), 450);

  /* Create main content box */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* Header bar with close button */
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), TRUE);
  gtk_box_append(GTK_BOX(content), header);

  /* Status type switcher (tabs for General/Music) */
  self->status_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->status_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

  self->status_type_switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(self->status_type_switcher),
                               GTK_STACK(self->status_stack));
  gtk_widget_set_halign(self->status_type_switcher, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(self->status_type_switcher, 12);
  gtk_widget_set_margin_bottom(self->status_type_switcher, 6);

  /* General status page */
  GtkWidget *general_page = create_status_page(self, "General Status",
    &self->general_entry, &self->general_link_entry, &self->general_expiration_combo);
  gtk_stack_add_titled(GTK_STACK(self->status_stack), general_page, "general", "General");

  /* Music status page */
  GtkWidget *music_page = create_status_page(self, "Music Status",
    &self->music_entry, &self->music_link_entry, &self->music_expiration_combo);
  gtk_stack_add_titled(GTK_STACK(self->status_stack), music_page, "music", "Music");

  gtk_box_append(GTK_BOX(content), self->status_type_switcher);
  gtk_box_append(GTK_BOX(content), self->status_stack);

  /* Action buttons */
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(button_box, 12);
  gtk_widget_set_margin_bottom(button_box, 24);

  self->btn_clear = gtk_button_new_with_label("Clear Status");
  gtk_widget_add_css_class(self->btn_clear, "destructive-action");
  g_signal_connect(self->btn_clear, "clicked", G_CALLBACK(on_clear_clicked), self);
  gtk_box_append(GTK_BOX(button_box), self->btn_clear);

  self->btn_save = gtk_button_new_with_label("Save Status");
  gtk_widget_add_css_class(self->btn_save, "suggested-action");
  g_signal_connect(self->btn_save, "clicked", G_CALLBACK(on_save_clicked), self);
  gtk_box_append(GTK_BOX(button_box), self->btn_save);

  gtk_box_append(GTK_BOX(content), button_box);

  adw_dialog_set_child(ADW_DIALOG(self), content);
}

static void gnostr_status_dialog_dispose(GObject *obj) {
  GnostrStatusDialog *self = GNOSTR_STATUS_DIALOG(obj);
  (void)self;
  G_OBJECT_CLASS(gnostr_status_dialog_parent_class)->dispose(obj);
}

static void gnostr_status_dialog_class_init(GnostrStatusDialogClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_status_dialog_dispose;

  signals[SIGNAL_STATUS_UPDATED] = g_signal_new(
    "status-updated",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

GnostrStatusDialog *gnostr_status_dialog_new(void) {
  return g_object_new(GNOSTR_TYPE_STATUS_DIALOG, NULL);
}

void gnostr_status_dialog_present(GnostrStatusDialog *self, GtkWidget *parent) {
  g_return_if_fail(GNOSTR_IS_STATUS_DIALOG(self));
  adw_dialog_present(ADW_DIALOG(self), parent);
}

void gnostr_status_dialog_set_current_status(GnostrStatusDialog *self,
                                              const gchar *general_status,
                                              const gchar *music_status) {
  g_return_if_fail(GNOSTR_IS_STATUS_DIALOG(self));

  if (general_status && *general_status) {
    gtk_editable_set_text(GTK_EDITABLE(self->general_entry), general_status);
  }
  if (music_status && *music_status) {
    gtk_editable_set_text(GTK_EDITABLE(self->music_entry), music_status);
  }
}
