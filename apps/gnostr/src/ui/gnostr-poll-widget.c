/**
 * NIP-88: Poll Widget Implementation
 *
 * Displays poll events (kind 1068) with voting interface.
 * Supports both single choice (radio) and multiple choice (checkbox) polls.
 */

#include "gnostr-poll-widget.h"
#include <glib.h>
#include <glib/gi18n.h>

struct _GnostrPollWidget {
  GtkWidget parent_instance;

  /* Widget references */
  GtkWidget *root_box;           /* Main container */
  GtkWidget *options_box;        /* Container for poll options */
  GtkWidget *vote_button;        /* Submit vote button */
  GtkWidget *status_label;       /* Shows "X votes" or "Poll closed" */
  GtkWidget *time_label;         /* Shows time remaining or "Closed" */

  /* Poll state */
  char *poll_id;                 /* Poll event ID (hex) */
  gboolean multiple_choice;      /* TRUE for multi-select */
  gint64 closed_at;              /* Unix timestamp when poll closes */
  guint total_votes;             /* Total vote count */
  gboolean has_voted;            /* TRUE if current user voted */
  gboolean is_logged_in;         /* TRUE if user is logged in */

  /* Options */
  GPtrArray *options;            /* Array of GnostrPollOption* */
  GPtrArray *option_buttons;     /* Array of GtkCheckButton* (or radio) */
  GPtrArray *option_bars;        /* Array of GtkProgressBar* for results */
  GPtrArray *option_count_labels; /* Array of GtkLabel* for vote counts */

  /* User's votes */
  GArray *user_vote_indices;     /* Indices user voted for */

  /* Timer for updating time remaining */
  guint time_update_timer;
};

G_DEFINE_TYPE(GnostrPollWidget, gnostr_poll_widget, GTK_TYPE_WIDGET)

enum {
  SIGNAL_VOTE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

/* Forward declarations */
static void update_time_display(GnostrPollWidget *self);
static void update_results_display(GnostrPollWidget *self);
static void rebuild_options_ui(GnostrPollWidget *self);

static void gnostr_poll_widget_dispose(GObject *obj) {
  GnostrPollWidget *self = GNOSTR_POLL_WIDGET(obj);

  if (self->time_update_timer > 0) {
    g_source_remove(self->time_update_timer);
    self->time_update_timer = 0;
  }

  g_clear_pointer(&self->root_box, gtk_widget_unparent);

  G_OBJECT_CLASS(gnostr_poll_widget_parent_class)->dispose(obj);
}

static void gnostr_poll_widget_finalize(GObject *obj) {
  GnostrPollWidget *self = GNOSTR_POLL_WIDGET(obj);

  g_clear_pointer(&self->poll_id, g_free);

  if (self->options) {
    for (guint i = 0; i < self->options->len; i++) {
      GnostrPollOption *opt = g_ptr_array_index(self->options, i);
      g_free(opt->text);
      g_free(opt);
    }
    g_ptr_array_free(self->options, TRUE);
    self->options = NULL;
  }

  g_clear_pointer(&self->option_buttons, g_ptr_array_unref);
  g_clear_pointer(&self->option_bars, g_ptr_array_unref);
  g_clear_pointer(&self->option_count_labels, g_ptr_array_unref);

  if (self->user_vote_indices) {
    g_array_unref(self->user_vote_indices);
    self->user_vote_indices = NULL;
  }

  G_OBJECT_CLASS(gnostr_poll_widget_parent_class)->finalize(obj);
}

static void on_vote_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPollWidget *self = GNOSTR_POLL_WIDGET(user_data);
  (void)btn;

  if (!self->poll_id || gnostr_poll_widget_is_closed(self)) {
    return;
  }

  GArray *selected = gnostr_poll_widget_get_selected(self);
  if (!selected || selected->len == 0) {
    if (selected) g_array_unref(selected);
    return;
  }

  /* Emit vote-requested signal */
  g_signal_emit(self, signals[SIGNAL_VOTE_REQUESTED], 0,
                self->poll_id, selected);

  g_array_unref(selected);
}

static gboolean time_update_callback(gpointer user_data) {
  GnostrPollWidget *self = GNOSTR_POLL_WIDGET(user_data);
  if (!GNOSTR_IS_POLL_WIDGET(self)) {
    return G_SOURCE_REMOVE;
  }
  update_time_display(self);

  /* Check if poll just closed */
  if (gnostr_poll_widget_is_closed(self)) {
    /* Disable voting if poll just closed */
    if (self->vote_button && GTK_IS_WIDGET(self->vote_button)) {
      gtk_widget_set_sensitive(self->vote_button, FALSE);
    }
    /* Show results */
    update_results_display(self);
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

static void update_time_display(GnostrPollWidget *self) {
  if (!self->time_label || !GTK_IS_LABEL(self->time_label)) return;

  if (self->closed_at <= 0) {
    gtk_widget_set_visible(self->time_label, FALSE);
    return;
  }

  gtk_widget_set_visible(self->time_label, TRUE);

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gint64 remaining = self->closed_at - now;

  if (remaining <= 0) {
    gtk_label_set_text(GTK_LABEL(self->time_label), "Poll closed");
    gtk_widget_add_css_class(self->time_label, "poll-closed");
    return;
  }

  char *time_str;
  if (remaining < 60) {
    time_str = g_strdup_printf("%"G_GINT64_FORMAT"s remaining", remaining);
  } else if (remaining < 3600) {
    time_str = g_strdup_printf("%"G_GINT64_FORMAT"m remaining", remaining / 60);
  } else if (remaining < 86400) {
    time_str = g_strdup_printf("%"G_GINT64_FORMAT"h remaining", remaining / 3600);
  } else {
    time_str = g_strdup_printf("%"G_GINT64_FORMAT"d remaining", remaining / 86400);
  }

  gtk_label_set_text(GTK_LABEL(self->time_label), time_str);
  g_free(time_str);
}

static void update_results_display(GnostrPollWidget *self) {
  if (!self->options || self->options->len == 0) return;
  if (!self->option_bars || !self->option_count_labels) return;

  gboolean show_results = self->has_voted || gnostr_poll_widget_is_closed(self);

  for (guint i = 0; i < self->options->len && i < self->option_bars->len; i++) {
    GnostrPollOption *opt = g_ptr_array_index(self->options, i);
    GtkWidget *bar = g_ptr_array_index(self->option_bars, i);
    GtkWidget *count_label = g_ptr_array_index(self->option_count_labels, i);

    if (show_results) {
      gtk_widget_set_visible(bar, TRUE);
      gtk_widget_set_visible(count_label, TRUE);

      /* Calculate percentage */
      double fraction = 0.0;
      if (self->total_votes > 0) {
        fraction = (double)opt->vote_count / (double)self->total_votes;
      }

      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), fraction);

      /* Update count label */
      char *count_str;
      if (self->total_votes > 0) {
        int percent = (int)(fraction * 100.0 + 0.5);
        count_str = g_strdup_printf("%u (%d%%)", opt->vote_count, percent);
      } else {
        count_str = g_strdup_printf("%u", opt->vote_count);
      }
      gtk_label_set_text(GTK_LABEL(count_label), count_str);
      g_free(count_str);

      /* Highlight user's choice */
      if (self->user_vote_indices) {
        for (guint j = 0; j < self->user_vote_indices->len; j++) {
          int idx = g_array_index(self->user_vote_indices, int, j);
          if (idx == (int)i) {
            gtk_widget_add_css_class(bar, "poll-option-voted");
            break;
          }
        }
      }
    } else {
      gtk_widget_set_visible(bar, FALSE);
      gtk_widget_set_visible(count_label, FALSE);
    }
  }

  /* Update status label */
  if (self->status_label && GTK_IS_LABEL(self->status_label)) {
    char *status_str = g_strdup_printf("%u vote%s",
                                        self->total_votes,
                                        self->total_votes == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(self->status_label), status_str);
    g_free(status_str);
  }

  /* Disable vote button if already voted or closed */
  if (self->vote_button && GTK_IS_WIDGET(self->vote_button)) {
    gboolean can_vote = self->is_logged_in &&
                        !self->has_voted &&
                        !gnostr_poll_widget_is_closed(self);
    gtk_widget_set_sensitive(self->vote_button, can_vote);
    gtk_widget_set_visible(self->vote_button, !self->has_voted && !gnostr_poll_widget_is_closed(self));
  }
}

static void on_option_toggled(GtkCheckButton *btn, gpointer user_data) {
  GnostrPollWidget *self = GNOSTR_POLL_WIDGET(user_data);
  (void)btn;

  /* For single choice, other buttons are handled by the radio group */
  /* For multiple choice, nothing special needed */

  /* Update vote button sensitivity based on selection */
  GArray *selected = gnostr_poll_widget_get_selected(self);
  if (self->vote_button && GTK_IS_WIDGET(self->vote_button)) {
    gboolean has_selection = selected && selected->len > 0;
    gboolean can_vote = self->is_logged_in &&
                        !self->has_voted &&
                        !gnostr_poll_widget_is_closed(self) &&
                        has_selection;
    gtk_widget_set_sensitive(self->vote_button, can_vote);
  }
  if (selected) g_array_unref(selected);
}

static void rebuild_options_ui(GnostrPollWidget *self) {
  /* Clear existing option widgets */
  if (self->options_box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->options_box)) != NULL) {
      gtk_box_remove(GTK_BOX(self->options_box), child);
    }
  }

  if (self->option_buttons) {
    g_ptr_array_set_size(self->option_buttons, 0);
  } else {
    self->option_buttons = g_ptr_array_new();
  }

  if (self->option_bars) {
    g_ptr_array_set_size(self->option_bars, 0);
  } else {
    self->option_bars = g_ptr_array_new();
  }

  if (self->option_count_labels) {
    g_ptr_array_set_size(self->option_count_labels, 0);
  } else {
    self->option_count_labels = g_ptr_array_new();
  }

  if (!self->options || self->options->len == 0) return;

  GtkWidget *first_button = NULL;

  for (guint i = 0; i < self->options->len; i++) {
    GnostrPollOption *opt = g_ptr_array_index(self->options, i);

    /* Option container */
    GtkWidget *option_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(option_row, "poll-option-row");

    /* Top row: button + count */
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    /* Create toggle button (radio or checkbox) */
    GtkWidget *button;
    if (self->multiple_choice) {
      button = gtk_check_button_new_with_label(opt->text);
    } else {
      button = gtk_check_button_new_with_label(opt->text);
      if (first_button) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(button),
                                   GTK_CHECK_BUTTON(first_button));
      } else {
        first_button = button;
      }
    }
    gtk_widget_add_css_class(button, "poll-option-button");
    gtk_widget_set_hexpand(button, TRUE);
    g_signal_connect(button, "toggled", G_CALLBACK(on_option_toggled), self);
    g_ptr_array_add(self->option_buttons, button);

    /* Vote count label (hidden until results shown) */
    GtkWidget *count_label = gtk_label_new("0");
    gtk_widget_add_css_class(count_label, "poll-option-count");
    gtk_widget_set_visible(count_label, FALSE);
    g_ptr_array_add(self->option_count_labels, count_label);

    gtk_box_append(GTK_BOX(top_row), button);
    gtk_box_append(GTK_BOX(top_row), count_label);
    gtk_box_append(GTK_BOX(option_row), top_row);

    /* Progress bar for results (hidden until results shown) */
    GtkWidget *bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(bar, "poll-option-bar");
    gtk_widget_set_visible(bar, FALSE);
    g_ptr_array_add(self->option_bars, bar);
    gtk_box_append(GTK_BOX(option_row), bar);

    gtk_box_append(GTK_BOX(self->options_box), option_row);
  }

  /* Update display based on current state */
  update_results_display(self);
}

static void gnostr_poll_widget_class_init(GnostrPollWidgetClass *klass) {
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  gobj_class->dispose = gnostr_poll_widget_dispose;
  gobj_class->finalize = gnostr_poll_widget_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

  /**
   * GnostrPollWidget::vote-requested:
   * @self: the poll widget
   * @poll_id: the poll event ID (hex string)
   * @selected_indices: GArray of ints with selected option indices
   *
   * Emitted when user clicks the vote button.
   */
  signals[SIGNAL_VOTE_REQUESTED] =
      g_signal_new("vote-requested",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE, 2,
                   G_TYPE_STRING,
                   G_TYPE_POINTER);
}

static void gnostr_poll_widget_init(GnostrPollWidget *self) {
  self->multiple_choice = FALSE;
  self->closed_at = 0;
  self->total_votes = 0;
  self->has_voted = FALSE;
  self->is_logged_in = FALSE;
  self->options = g_ptr_array_new();
  self->user_vote_indices = NULL;

  /* Build UI */
  self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));
  gtk_widget_add_css_class(self->root_box, "poll-widget");

  /* Poll header with icon */
  GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(header_box, "poll-header");

  GtkWidget *poll_icon = gtk_image_new_from_icon_name("view-list-bullet-symbolic");
  gtk_widget_add_css_class(poll_icon, "poll-icon");
  gtk_box_append(GTK_BOX(header_box), poll_icon);

  GtkWidget *poll_label = gtk_label_new("Poll");
  gtk_widget_add_css_class(poll_label, "poll-title");
  gtk_box_append(GTK_BOX(header_box), poll_label);

  /* Time remaining label (right-aligned) */
  self->time_label = gtk_label_new("");
  gtk_widget_set_hexpand(self->time_label, TRUE);
  gtk_widget_set_halign(self->time_label, GTK_ALIGN_END);
  gtk_widget_add_css_class(self->time_label, "poll-time");
  gtk_widget_set_visible(self->time_label, FALSE);
  gtk_box_append(GTK_BOX(header_box), self->time_label);

  gtk_box_append(GTK_BOX(self->root_box), header_box);

  /* Options container */
  self->options_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(self->options_box, "poll-options");
  gtk_box_append(GTK_BOX(self->root_box), self->options_box);

  /* Footer: status + vote button */
  GtkWidget *footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(footer_box, "poll-footer");

  self->status_label = gtk_label_new("0 votes");
  gtk_widget_add_css_class(self->status_label, "poll-status");
  gtk_widget_set_hexpand(self->status_label, TRUE);
  gtk_widget_set_halign(self->status_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(footer_box), self->status_label);

  self->vote_button = gtk_button_new_with_label("Vote");
  gtk_widget_add_css_class(self->vote_button, "poll-vote-button");
  gtk_widget_add_css_class(self->vote_button, "suggested-action");
  gtk_widget_set_sensitive(self->vote_button, FALSE);
  g_signal_connect(self->vote_button, "clicked", G_CALLBACK(on_vote_clicked), self);
  gtk_box_append(GTK_BOX(footer_box), self->vote_button);

  gtk_box_append(GTK_BOX(self->root_box), footer_box);
}

GnostrPollWidget *gnostr_poll_widget_new(void) {
  return g_object_new(GNOSTR_TYPE_POLL_WIDGET, NULL);
}

void gnostr_poll_widget_set_poll_id(GnostrPollWidget *self, const char *poll_id_hex) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));
  g_free(self->poll_id);
  self->poll_id = g_strdup(poll_id_hex);
}

const char *gnostr_poll_widget_get_poll_id(GnostrPollWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_WIDGET(self), NULL);
  return self->poll_id;
}

void gnostr_poll_widget_set_options(GnostrPollWidget *self,
                                     GnostrPollOption *options,
                                     gsize count) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));

  /* Clear existing options */
  if (self->options) {
    for (guint i = 0; i < self->options->len; i++) {
      GnostrPollOption *opt = g_ptr_array_index(self->options, i);
      g_free(opt->text);
      g_free(opt);
    }
    g_ptr_array_set_size(self->options, 0);
  }

  /* Add new options */
  for (gsize i = 0; i < count; i++) {
    GnostrPollOption *opt = g_new0(GnostrPollOption, 1);
    opt->index = options[i].index;
    opt->text = g_strdup(options[i].text);
    opt->vote_count = options[i].vote_count;
    g_ptr_array_add(self->options, opt);
  }

  rebuild_options_ui(self);
}

void gnostr_poll_widget_set_multiple_choice(GnostrPollWidget *self, gboolean multiple) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));
  if (self->multiple_choice != multiple) {
    self->multiple_choice = multiple;
    rebuild_options_ui(self);
  }
}

gboolean gnostr_poll_widget_is_multiple_choice(GnostrPollWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_WIDGET(self), FALSE);
  return self->multiple_choice;
}

void gnostr_poll_widget_set_closed_at(GnostrPollWidget *self, gint64 closed_at) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));
  self->closed_at = closed_at;

  /* Cancel existing timer */
  if (self->time_update_timer > 0) {
    g_source_remove(self->time_update_timer);
    self->time_update_timer = 0;
  }

  update_time_display(self);

  /* LEGITIMATE TIMEOUT - Periodic poll countdown update (30s intervals).
   * nostrc-b0h: Audited - UI countdown timer is appropriate. */
  if (closed_at > 0 && !gnostr_poll_widget_is_closed(self)) {
    self->time_update_timer = g_timeout_add_seconds(30, time_update_callback, self);
  }

  update_results_display(self);
}

gint64 gnostr_poll_widget_get_closed_at(GnostrPollWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_WIDGET(self), 0);
  return self->closed_at;
}

gboolean gnostr_poll_widget_is_closed(GnostrPollWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_WIDGET(self), TRUE);
  if (self->closed_at <= 0) return FALSE;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  return now >= self->closed_at;
}

void gnostr_poll_widget_set_total_votes(GnostrPollWidget *self, guint total) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));
  self->total_votes = total;
  update_results_display(self);
}

guint gnostr_poll_widget_get_total_votes(GnostrPollWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_WIDGET(self), 0);
  return self->total_votes;
}

void gnostr_poll_widget_update_vote_counts(GnostrPollWidget *self,
                                            guint *vote_counts,
                                            gsize count) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));
  if (!self->options) return;

  guint total = 0;
  for (gsize i = 0; i < count && i < self->options->len; i++) {
    GnostrPollOption *opt = g_ptr_array_index(self->options, i);
    opt->vote_count = vote_counts[i];
    total += vote_counts[i];
  }

  self->total_votes = total;
  update_results_display(self);
}

void gnostr_poll_widget_set_has_voted(GnostrPollWidget *self, gboolean has_voted) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));
  self->has_voted = has_voted;
  update_results_display(self);

  /* Disable option buttons if user has voted */
  if (self->option_buttons) {
    for (guint i = 0; i < self->option_buttons->len; i++) {
      GtkWidget *btn = g_ptr_array_index(self->option_buttons, i);
      if (GTK_IS_WIDGET(btn)) {
        gtk_widget_set_sensitive(btn, !has_voted && !gnostr_poll_widget_is_closed(self));
      }
    }
  }
}

gboolean gnostr_poll_widget_has_voted(GnostrPollWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_WIDGET(self), FALSE);
  return self->has_voted;
}

void gnostr_poll_widget_set_user_votes(GnostrPollWidget *self,
                                        int *indices,
                                        gsize count) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));

  if (self->user_vote_indices) {
    g_array_unref(self->user_vote_indices);
    self->user_vote_indices = NULL;
  }

  if (indices && count > 0) {
    self->user_vote_indices = g_array_sized_new(FALSE, FALSE, sizeof(int), count);
    g_array_append_vals(self->user_vote_indices, indices, count);

    /* Check the buttons for user's votes */
    if (self->option_buttons) {
      for (gsize i = 0; i < count; i++) {
        int idx = indices[i];
        if (idx >= 0 && (guint)idx < self->option_buttons->len) {
          GtkWidget *btn = g_ptr_array_index(self->option_buttons, idx);
          if (GTK_IS_CHECK_BUTTON(btn)) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(btn), TRUE);
          }
        }
      }
    }
  }

  update_results_display(self);
}

void gnostr_poll_widget_set_logged_in(GnostrPollWidget *self, gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_POLL_WIDGET(self));
  self->is_logged_in = logged_in;
  update_results_display(self);
}

GArray *gnostr_poll_widget_get_selected(GnostrPollWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_WIDGET(self), NULL);

  GArray *selected = g_array_new(FALSE, FALSE, sizeof(int));

  if (self->option_buttons) {
    for (guint i = 0; i < self->option_buttons->len; i++) {
      GtkWidget *btn = g_ptr_array_index(self->option_buttons, i);
      if (GTK_IS_CHECK_BUTTON(btn) && gtk_check_button_get_active(GTK_CHECK_BUTTON(btn))) {
        int idx = (int)i;
        g_array_append_val(selected, idx);
      }
    }
  }

  return selected;
}
