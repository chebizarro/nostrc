/**
 * NIP-88: Poll Composer Implementation
 *
 * UI for creating poll events (kind 1068).
 */

#include "gnostr-poll-composer.h"
#include <glib.h>
#include <glib/gi18n.h>

#define MAX_POLL_OPTIONS 10
#define MIN_POLL_OPTIONS 2

struct _GnostrPollComposer {
  GtkWidget parent_instance;

  /* Widget references */
  GtkWidget *root_box;
  GtkWidget *question_entry;       /* Poll question */
  GtkWidget *options_box;          /* Container for option entries */
  GtkWidget *add_option_button;    /* Button to add more options */
  GtkWidget *multiple_choice_switch; /* Toggle for multi-select */
  GtkWidget *duration_dropdown;    /* Duration selector */
  GtkWidget *create_button;        /* Create poll button */
  GtkWidget *cancel_button;        /* Cancel button */

  /* Option entries */
  GPtrArray *option_entries;       /* Array of GtkEntry* for options */

  /* State */
  gboolean multiple_choice;
  gint64 closed_at;                /* 0 = no closing time */
};

G_DEFINE_TYPE(GnostrPollComposer, gnostr_poll_composer, GTK_TYPE_WIDGET)

enum {
  SIGNAL_POLL_CREATED,
  SIGNAL_CANCELLED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

/* Duration options (in seconds) */
typedef struct {
  const char *label;
  gint64 seconds;
} DurationOption;

static const DurationOption duration_options[] = {
  {"No time limit", 0},
  {"5 minutes", 5 * 60},
  {"15 minutes", 15 * 60},
  {"1 hour", 60 * 60},
  {"6 hours", 6 * 60 * 60},
  {"12 hours", 12 * 60 * 60},
  {"1 day", 24 * 60 * 60},
  {"3 days", 3 * 24 * 60 * 60},
  {"1 week", 7 * 24 * 60 * 60},
  {NULL, 0}
};

static void update_add_button_visibility(GnostrPollComposer *self);
static void update_create_button_sensitivity(GnostrPollComposer *self);
static void add_option_entry(GnostrPollComposer *self);

static void gnostr_poll_composer_dispose(GObject *obj) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(obj);
  g_clear_pointer(&self->root_box, gtk_widget_unparent);
  G_OBJECT_CLASS(gnostr_poll_composer_parent_class)->dispose(obj);
}

static void gnostr_poll_composer_finalize(GObject *obj) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(obj);

  g_clear_pointer(&self->option_entries, g_ptr_array_unref);

  G_OBJECT_CLASS(gnostr_poll_composer_parent_class)->finalize(obj);
}

static void on_remove_option_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);

  /* Find which option this button belongs to */
  GtkWidget *entry = g_object_get_data(G_OBJECT(btn), "option-entry");
  if (!entry || !GTK_IS_ENTRY(entry)) return;

  /* Don't allow removing if we're at minimum */
  if (self->option_entries->len <= MIN_POLL_OPTIONS) return;

  /* Find and remove the entry from our array */
  guint idx = 0;
  gboolean found = FALSE;
  for (guint i = 0; i < self->option_entries->len; i++) {
    if (g_ptr_array_index(self->option_entries, i) == entry) {
      idx = i;
      found = TRUE;
      break;
    }
  }

  if (!found) return;

  /* Remove the entry row */
  GtkWidget *row = gtk_widget_get_parent(entry);
  if (row) {
    gtk_box_remove(GTK_BOX(self->options_box), row);
  }

  g_ptr_array_remove_index(self->option_entries, idx);

  /* Update placeholder numbers */
  for (guint i = 0; i < self->option_entries->len; i++) {
    GtkWidget *e = g_ptr_array_index(self->option_entries, i);
    g_autofree char *placeholder = g_strdup_printf("Option %u", i + 1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
  }

  update_add_button_visibility(self);
  update_create_button_sensitivity(self);
}

static void on_option_changed(GtkEditable *editable, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);
  (void)editable;
  update_create_button_sensitivity(self);
}

static void add_option_entry(GnostrPollComposer *self) {
  if (self->option_entries->len >= MAX_POLL_OPTIONS) return;

  guint num = self->option_entries->len + 1;

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(row, "poll-option-entry-row");

  /* Option number label */
  g_autofree char *label_text = g_strdup_printf("%u.", num);
  GtkWidget *label = gtk_label_new(label_text);
  gtk_widget_add_css_class(label, "poll-option-number");
  gtk_widget_set_size_request(label, 24, -1);
  gtk_box_append(GTK_BOX(row), label);

  /* Option text entry */
  GtkWidget *entry = gtk_entry_new();
  g_autofree char *placeholder = g_strdup_printf("Option %u", num);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_entry_set_max_length(GTK_ENTRY(entry), 100);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_widget_add_css_class(entry, "poll-option-entry");
  g_signal_connect(entry, "changed", G_CALLBACK(on_option_changed), self);
  gtk_box_append(GTK_BOX(row), entry);

  g_ptr_array_add(self->option_entries, entry);

  /* Remove button (hidden for first MIN_POLL_OPTIONS entries) */
  GtkWidget *remove_btn = gtk_button_new_from_icon_name("list-remove-symbolic");
  gtk_widget_add_css_class(remove_btn, "flat");
  gtk_widget_add_css_class(remove_btn, "poll-option-remove");
  gtk_widget_set_tooltip_text(remove_btn, "Remove option");
  g_object_set_data(G_OBJECT(remove_btn), "option-entry", entry);
  g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_remove_option_clicked), self);
  gtk_box_append(GTK_BOX(row), remove_btn);

  /* Hide remove button if at minimum */
  gtk_widget_set_visible(remove_btn, self->option_entries->len > MIN_POLL_OPTIONS);

  gtk_box_append(GTK_BOX(self->options_box), row);

  update_add_button_visibility(self);
}

static void on_add_option_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);
  (void)btn;
  add_option_entry(self);
}

static void on_multiple_choice_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);
  (void)pspec;
  self->multiple_choice = gtk_switch_get_active(GTK_SWITCH(object));
}

static void on_duration_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);
  (void)pspec;

  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
  if (selected < G_N_ELEMENTS(duration_options) - 1) {
    gint64 duration_secs = duration_options[selected].seconds;
    if (duration_secs > 0) {
      self->closed_at = (g_get_real_time() / G_USEC_PER_SEC) + duration_secs;
    } else {
      self->closed_at = 0;
    }
  }
}

static void on_create_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);
  (void)btn;

  if (!gnostr_poll_composer_is_valid(self)) return;

  const char *question = gnostr_poll_composer_get_question(self);
  GPtrArray *options = gnostr_poll_composer_get_options(self);

  g_signal_emit(self, signals[SIGNAL_POLL_CREATED], 0,
                question, options, self->multiple_choice, self->closed_at);

  g_ptr_array_unref(options);
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);
  (void)btn;
  g_signal_emit(self, signals[SIGNAL_CANCELLED], 0);
}

static void on_question_changed(GtkEditable *editable, gpointer user_data) {
  GnostrPollComposer *self = GNOSTR_POLL_COMPOSER(user_data);
  (void)editable;
  update_create_button_sensitivity(self);
}

static void update_add_button_visibility(GnostrPollComposer *self) {
  if (self->add_option_button && GTK_IS_WIDGET(self->add_option_button)) {
    gtk_widget_set_visible(self->add_option_button,
                            self->option_entries->len < MAX_POLL_OPTIONS);
  }

  /* Update remove button visibility for all options */
  for (guint i = 0; i < self->option_entries->len; i++) {
    GtkWidget *entry = g_ptr_array_index(self->option_entries, i);
    GtkWidget *row = gtk_widget_get_parent(entry);
    if (row) {
      GtkWidget *child = gtk_widget_get_first_child(row);
      while (child) {
        if (GTK_IS_BUTTON(child)) {
          gtk_widget_set_visible(child, self->option_entries->len > MIN_POLL_OPTIONS);
        }
        child = gtk_widget_get_next_sibling(child);
      }
    }
  }
}

static void update_create_button_sensitivity(GnostrPollComposer *self) {
  if (self->create_button && GTK_IS_WIDGET(self->create_button)) {
    gtk_widget_set_sensitive(self->create_button,
                             gnostr_poll_composer_is_valid(self));
  }
}

static void gnostr_poll_composer_class_init(GnostrPollComposerClass *klass) {
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  gobj_class->dispose = gnostr_poll_composer_dispose;
  gobj_class->finalize = gnostr_poll_composer_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

  /**
   * GnostrPollComposer::poll-created:
   * @self: the poll composer
   * @question: the poll question text
   * @options: GPtrArray of option strings
   * @multiple_choice: TRUE if multiple selection allowed
   * @closed_at: Unix timestamp when poll closes, or 0
   *
   * Emitted when user creates a valid poll.
   */
  signals[SIGNAL_POLL_CREATED] =
      g_signal_new("poll-created",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE, 4,
                   G_TYPE_STRING,
                   G_TYPE_POINTER,
                   G_TYPE_BOOLEAN,
                   G_TYPE_INT64);

  /**
   * GnostrPollComposer::cancelled:
   * @self: the poll composer
   *
   * Emitted when user cancels poll creation.
   */
  signals[SIGNAL_CANCELLED] =
      g_signal_new("cancelled",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE, 0);
}

static void gnostr_poll_composer_init(GnostrPollComposer *self) {
  self->multiple_choice = FALSE;
  self->closed_at = 0;
  self->option_entries = g_ptr_array_new();

  /* Build UI */
  self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));
  gtk_widget_add_css_class(self->root_box, "poll-composer");
  gtk_widget_set_margin_start(self->root_box, 12);
  gtk_widget_set_margin_end(self->root_box, 12);
  gtk_widget_set_margin_top(self->root_box, 12);
  gtk_widget_set_margin_bottom(self->root_box, 12);

  /* Header */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *header_icon = gtk_image_new_from_icon_name("view-list-bullet-symbolic");
  gtk_widget_add_css_class(header_icon, "poll-composer-icon");
  GtkWidget *header_label = gtk_label_new("Create Poll");
  gtk_widget_add_css_class(header_label, "poll-composer-title");
  gtk_widget_add_css_class(header_label, "title-3");
  gtk_box_append(GTK_BOX(header), header_icon);
  gtk_box_append(GTK_BOX(header), header_label);
  gtk_box_append(GTK_BOX(self->root_box), header);

  /* Question entry */
  GtkWidget *question_label = gtk_label_new("Question");
  gtk_widget_add_css_class(question_label, "poll-section-label");
  gtk_widget_set_halign(question_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(self->root_box), question_label);

  self->question_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->question_entry), "What's your question?");
  gtk_entry_set_max_length(GTK_ENTRY(self->question_entry), 200);
  gtk_widget_add_css_class(self->question_entry, "poll-question-entry");
  g_signal_connect(self->question_entry, "changed", G_CALLBACK(on_question_changed), self);
  gtk_box_append(GTK_BOX(self->root_box), self->question_entry);

  /* Options section */
  GtkWidget *options_label = gtk_label_new("Options");
  gtk_widget_add_css_class(options_label, "poll-section-label");
  gtk_widget_set_halign(options_label, GTK_ALIGN_START);
  gtk_widget_set_margin_top(options_label, 8);
  gtk_box_append(GTK_BOX(self->root_box), options_label);

  self->options_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(self->options_box, "poll-options-container");
  gtk_box_append(GTK_BOX(self->root_box), self->options_box);

  /* Add initial options (minimum 2) */
  for (int i = 0; i < MIN_POLL_OPTIONS; i++) {
    add_option_entry(self);
  }

  /* Add option button */
  self->add_option_button = gtk_button_new();
  GtkWidget *add_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *add_icon = gtk_image_new_from_icon_name("list-add-symbolic");
  GtkWidget *add_label = gtk_label_new("Add Option");
  gtk_box_append(GTK_BOX(add_content), add_icon);
  gtk_box_append(GTK_BOX(add_content), add_label);
  gtk_button_set_child(GTK_BUTTON(self->add_option_button), add_content);
  gtk_widget_add_css_class(self->add_option_button, "flat");
  gtk_widget_add_css_class(self->add_option_button, "poll-add-option");
  gtk_widget_set_halign(self->add_option_button, GTK_ALIGN_START);
  g_signal_connect(self->add_option_button, "clicked", G_CALLBACK(on_add_option_clicked), self);
  gtk_box_append(GTK_BOX(self->root_box), self->add_option_button);

  /* Settings section */
  GtkWidget *settings_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(settings_box, "poll-settings");
  gtk_widget_set_margin_top(settings_box, 12);

  /* Multiple choice toggle */
  GtkWidget *multi_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *multi_label = gtk_label_new("Allow multiple selections");
  gtk_widget_set_hexpand(multi_label, TRUE);
  gtk_widget_set_halign(multi_label, GTK_ALIGN_START);
  self->multiple_choice_switch = gtk_switch_new();
  g_signal_connect(self->multiple_choice_switch, "notify::active",
                   G_CALLBACK(on_multiple_choice_changed), self);
  gtk_box_append(GTK_BOX(multi_row), multi_label);
  gtk_box_append(GTK_BOX(multi_row), self->multiple_choice_switch);
  gtk_box_append(GTK_BOX(settings_box), multi_row);

  /* Duration dropdown */
  GtkWidget *duration_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *duration_label = gtk_label_new("Poll duration");
  gtk_widget_set_hexpand(duration_label, TRUE);
  gtk_widget_set_halign(duration_label, GTK_ALIGN_START);

  /* Build duration strings array */
  GtkStringList *duration_strings = gtk_string_list_new(NULL);
  for (int i = 0; duration_options[i].label != NULL; i++) {
    gtk_string_list_append(duration_strings, duration_options[i].label);
  }

  self->duration_dropdown = gtk_drop_down_new(G_LIST_MODEL(duration_strings), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(self->duration_dropdown), 0);
  g_signal_connect(self->duration_dropdown, "notify::selected",
                   G_CALLBACK(on_duration_changed), self);
  gtk_box_append(GTK_BOX(duration_row), duration_label);
  gtk_box_append(GTK_BOX(duration_row), self->duration_dropdown);
  gtk_box_append(GTK_BOX(settings_box), duration_row);

  gtk_box_append(GTK_BOX(self->root_box), settings_box);

  /* Action buttons */
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(button_box, 12);

  self->cancel_button = gtk_button_new_with_label("Cancel");
  gtk_widget_add_css_class(self->cancel_button, "poll-cancel-button");
  g_signal_connect(self->cancel_button, "clicked", G_CALLBACK(on_cancel_clicked), self);
  gtk_box_append(GTK_BOX(button_box), self->cancel_button);

  self->create_button = gtk_button_new_with_label("Create Poll");
  gtk_widget_add_css_class(self->create_button, "poll-create-button");
  gtk_widget_add_css_class(self->create_button, "suggested-action");
  gtk_widget_set_sensitive(self->create_button, FALSE);
  g_signal_connect(self->create_button, "clicked", G_CALLBACK(on_create_clicked), self);
  gtk_box_append(GTK_BOX(button_box), self->create_button);

  gtk_box_append(GTK_BOX(self->root_box), button_box);
}

GnostrPollComposer *gnostr_poll_composer_new(void) {
  return g_object_new(GNOSTR_TYPE_POLL_COMPOSER, NULL);
}

const char *gnostr_poll_composer_get_question(GnostrPollComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_COMPOSER(self), NULL);
  if (!self->question_entry || !GTK_IS_ENTRY(self->question_entry)) return NULL;
  return gtk_editable_get_text(GTK_EDITABLE(self->question_entry));
}

void gnostr_poll_composer_set_question(GnostrPollComposer *self, const char *question) {
  g_return_if_fail(GNOSTR_IS_POLL_COMPOSER(self));
  if (!self->question_entry || !GTK_IS_ENTRY(self->question_entry)) return;
  gtk_editable_set_text(GTK_EDITABLE(self->question_entry), question ? question : "");
}

GPtrArray *gnostr_poll_composer_get_options(GnostrPollComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_COMPOSER(self), NULL);

  GPtrArray *options = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; i < self->option_entries->len; i++) {
    GtkWidget *entry = g_ptr_array_index(self->option_entries, i);
    if (!GTK_IS_ENTRY(entry)) continue;

    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text && *text) {
      g_ptr_array_add(options, g_strdup(text));
    }
  }

  return options;
}

gboolean gnostr_poll_composer_is_multiple_choice(GnostrPollComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_COMPOSER(self), FALSE);
  return self->multiple_choice;
}

void gnostr_poll_composer_set_multiple_choice(GnostrPollComposer *self, gboolean multiple) {
  g_return_if_fail(GNOSTR_IS_POLL_COMPOSER(self));
  self->multiple_choice = multiple;
  if (self->multiple_choice_switch && GTK_IS_SWITCH(self->multiple_choice_switch)) {
    gtk_switch_set_active(GTK_SWITCH(self->multiple_choice_switch), multiple);
  }
}

gint64 gnostr_poll_composer_get_closed_at(GnostrPollComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_COMPOSER(self), 0);
  return self->closed_at;
}

void gnostr_poll_composer_set_closed_at(GnostrPollComposer *self, gint64 closed_at) {
  g_return_if_fail(GNOSTR_IS_POLL_COMPOSER(self));
  self->closed_at = closed_at;
}

void gnostr_poll_composer_clear(GnostrPollComposer *self) {
  g_return_if_fail(GNOSTR_IS_POLL_COMPOSER(self));

  /* Clear question */
  if (self->question_entry && GTK_IS_ENTRY(self->question_entry)) {
    gtk_editable_set_text(GTK_EDITABLE(self->question_entry), "");
  }

  /* Clear all option entries */
  for (guint i = 0; i < self->option_entries->len; i++) {
    GtkWidget *entry = g_ptr_array_index(self->option_entries, i);
    if (GTK_IS_ENTRY(entry)) {
      gtk_editable_set_text(GTK_EDITABLE(entry), "");
    }
  }

  /* Remove extra options (keep only MIN_POLL_OPTIONS) */
  while (self->option_entries->len > MIN_POLL_OPTIONS) {
    GtkWidget *entry = g_ptr_array_index(self->option_entries, self->option_entries->len - 1);
    GtkWidget *row = gtk_widget_get_parent(entry);
    if (row) {
      gtk_box_remove(GTK_BOX(self->options_box), row);
    }
    g_ptr_array_remove_index(self->option_entries, self->option_entries->len - 1);
  }

  /* Reset settings */
  self->multiple_choice = FALSE;
  self->closed_at = 0;

  if (self->multiple_choice_switch && GTK_IS_SWITCH(self->multiple_choice_switch)) {
    gtk_switch_set_active(GTK_SWITCH(self->multiple_choice_switch), FALSE);
  }

  if (self->duration_dropdown && GTK_IS_DROP_DOWN(self->duration_dropdown)) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->duration_dropdown), 0);
  }

  update_add_button_visibility(self);
  update_create_button_sensitivity(self);
}

gboolean gnostr_poll_composer_is_valid(GnostrPollComposer *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_COMPOSER(self), FALSE);

  /* Must have a question */
  const char *question = gnostr_poll_composer_get_question(self);
  if (!question || !*question) return FALSE;

  /* Must have at least MIN_POLL_OPTIONS non-empty options */
  GPtrArray *options = gnostr_poll_composer_get_options(self);
  gboolean valid = options->len >= MIN_POLL_OPTIONS;
  g_ptr_array_unref(options);

  return valid;
}
