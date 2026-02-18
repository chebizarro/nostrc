/*
 * gnostr-poll-card.c - NIP-88 Poll Card Widget Implementation
 *
 * Displays kind 1018 poll events with voting interface.
 * Supports both single choice (radio) and multiple choice (checkbox) polls.
 */

#include "gnostr-poll-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip88_polls.h"
#include "../util/utils.h"
#include <glib/gi18n.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

struct _GnostrPollCard {
  GtkWidget parent_instance;

  /* Main widgets */
  GtkWidget *root;                 /* Main container */
  GtkWidget *header_box;           /* Poll icon + title + time */
  GtkWidget *poll_icon;            /* Poll indicator icon */
  GtkWidget *poll_type_label;      /* "Poll" or "Multiple Choice Poll" */
  GtkWidget *time_label;           /* Time remaining or "Closed" */
  GtkWidget *question_label;       /* Poll question text */
  GtkWidget *options_box;          /* Container for poll options */
  GtkWidget *footer_box;           /* Vote count + vote button */
  GtkWidget *vote_count_label;     /* "X votes" */
  GtkWidget *vote_button;          /* Submit vote button */
  GtkWidget *refresh_button;       /* Refresh results button */

  /* Author widgets */
  GtkWidget *author_box;           /* Author info row */
  GtkWidget *author_avatar;        /* Author's avatar */
  GtkWidget *author_avatar_initials; /* Fallback initials */
  GtkWidget *author_name_label;    /* Author's name */
  GtkWidget *created_at_label;     /* Creation time */

  /* Poll state */
  gchar *poll_id;                  /* Poll event ID (hex) */
  gchar *author_pubkey;            /* Author's pubkey (hex) */
  gboolean multiple_choice;        /* TRUE for multi-select */
  gint64 end_time;                 /* Unix timestamp when poll closes */
  gint64 created_at;               /* Poll creation timestamp */
  guint total_votes;               /* Total number of voters */
  gboolean has_voted;              /* TRUE if current user voted */
  gboolean is_logged_in;           /* TRUE if user is logged in */

  /* Options data */
  GPtrArray *options;              /* Array of GnostrPollCardOption* */
  GPtrArray *option_buttons;       /* Array of GtkCheckButton* */
  GPtrArray *option_bars;          /* Array of GtkProgressBar* for results */
  GPtrArray *option_count_labels;  /* Array of GtkLabel* for vote counts */
  GPtrArray *option_percent_labels; /* Array of GtkLabel* for percentages */

  /* User's votes */
  GArray *user_vote_indices;       /* Indices user voted for */

  /* Timer for updating time remaining */
  guint time_update_timer;

#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
#endif
};

G_DEFINE_TYPE(GnostrPollCard, gnostr_poll_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_VOTE_CLICKED,
  SIGNAL_RESULTS_REQUESTED,
  SIGNAL_OPEN_PROFILE,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_time_display(GnostrPollCard *self);
static void update_results_display(GnostrPollCard *self);
static void rebuild_options_ui(GnostrPollCard *self);
static gchar *format_time_remaining(gint64 seconds);
static gchar *format_timestamp(gint64 created_at);

static void gnostr_poll_card_dispose(GObject *obj) {
  GnostrPollCard *self = GNOSTR_POLL_CARD(obj);

  if (self->time_update_timer > 0) {
    g_source_remove(self->time_update_timer);
    self->time_update_timer = 0;
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
  }
  /* Shared session is managed globally - do not clear here */
#endif

  /* Clear child widgets */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_widget_unparent(child);
    child = next;
  }

  G_OBJECT_CLASS(gnostr_poll_card_parent_class)->dispose(obj);
}

static void gnostr_poll_card_finalize(GObject *obj) {
  GnostrPollCard *self = GNOSTR_POLL_CARD(obj);

  g_clear_pointer(&self->poll_id, g_free);
  g_clear_pointer(&self->author_pubkey, g_free);

  if (self->options) {
    for (guint i = 0; i < self->options->len; i++) {
      GnostrPollCardOption *opt = g_ptr_array_index(self->options, i);
      g_free(opt->text);
      g_free(opt);
    }
  }
  g_clear_pointer(&self->options, g_ptr_array_unref);

  g_clear_pointer(&self->option_buttons, g_ptr_array_unref);
  g_clear_pointer(&self->option_bars, g_ptr_array_unref);
  g_clear_pointer(&self->option_count_labels, g_ptr_array_unref);
  g_clear_pointer(&self->option_percent_labels, g_ptr_array_unref);

  g_clear_pointer(&self->user_vote_indices, g_array_unref);

  G_OBJECT_CLASS(gnostr_poll_card_parent_class)->finalize(obj);
}

static gchar *format_time_remaining(gint64 seconds) {
  if (seconds <= 0) {
    return g_strdup(_("Closed"));
  }

  if (seconds < 60) {
    return g_strdup_printf(g_dngettext(NULL, "%"G_GINT64_FORMAT" second left",
                                        "%"G_GINT64_FORMAT" seconds left", (int)seconds), seconds);
  } else if (seconds < 3600) {
    gint64 minutes = seconds / 60;
    return g_strdup_printf(g_dngettext(NULL, "%"G_GINT64_FORMAT" minute left",
                                        "%"G_GINT64_FORMAT" minutes left", (int)minutes), minutes);
  } else if (seconds < 86400) {
    gint64 hours = seconds / 3600;
    return g_strdup_printf(g_dngettext(NULL, "%"G_GINT64_FORMAT" hour left",
                                        "%"G_GINT64_FORMAT" hours left", (int)hours), hours);
  } else {
    gint64 days = seconds / 86400;
    return g_strdup_printf(g_dngettext(NULL, "%"G_GINT64_FORMAT" day left",
                                        "%"G_GINT64_FORMAT" days left", (int)days), days);
  }
}

static gchar *format_timestamp(gint64 created_at) {
  if (created_at <= 0) return g_strdup("");

  GDateTime *dt = g_date_time_new_from_unix_local(created_at);
  if (!dt) return g_strdup("");

  GDateTime *now = g_date_time_new_now_local();
  GTimeSpan diff = g_date_time_difference(now, dt);
  g_date_time_unref(now);

  gchar *result;
  gint64 seconds = diff / G_TIME_SPAN_SECOND;

  if (seconds < 60) {
    result = g_strdup(_("just now"));
  } else if (seconds < 3600) {
    gint minutes = (gint)(seconds / 60);
    result = g_strdup_printf(g_dngettext(NULL, "%dm ago", "%dm ago", minutes), minutes);
  } else if (seconds < 86400) {
    gint hours = (gint)(seconds / 3600);
    result = g_strdup_printf(g_dngettext(NULL, "%dh ago", "%dh ago", hours), hours);
  } else if (seconds < 604800) {
    gint days = (gint)(seconds / 86400);
    result = g_strdup_printf(g_dngettext(NULL, "%dd ago", "%dd ago", days), days);
  } else {
    result = g_date_time_format(dt, "%b %d, %Y");
  }

  g_date_time_unref(dt);
  return result;
}

static void update_time_display(GnostrPollCard *self) {
  if (!GTK_IS_LABEL(self->time_label)) return;

  if (self->end_time <= 0) {
    gtk_widget_set_visible(self->time_label, FALSE);
    return;
  }

  gtk_widget_set_visible(self->time_label, TRUE);

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gint64 remaining = self->end_time - now;

  gchar *time_str = format_time_remaining(remaining);
  gtk_label_set_text(GTK_LABEL(self->time_label), time_str);
  g_free(time_str);

  if (remaining <= 0) {
    gtk_widget_add_css_class(self->time_label, "poll-closed");
    gtk_widget_remove_css_class(self->time_label, "poll-ending-soon");
  } else if (remaining < 3600) {
    /* Less than 1 hour - add warning style */
    gtk_widget_add_css_class(self->time_label, "poll-ending-soon");
    gtk_widget_remove_css_class(self->time_label, "poll-closed");
  } else {
    gtk_widget_remove_css_class(self->time_label, "poll-closed");
    gtk_widget_remove_css_class(self->time_label, "poll-ending-soon");
  }
}

static gboolean time_update_callback(gpointer user_data) {
  GnostrPollCard *self = GNOSTR_POLL_CARD(user_data);

  if (!GNOSTR_IS_POLL_CARD(self)) {
    return G_SOURCE_REMOVE;
  }

  update_time_display(self);

  /* Check if poll just closed */
  if (gnostr_poll_card_is_closed(self)) {
    /* Disable voting */
    if (GTK_IS_WIDGET(self->vote_button)) {
      gtk_widget_set_sensitive(self->vote_button, FALSE);
    }
    /* Update to show results */
    update_results_display(self);
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

static void update_results_display(GnostrPollCard *self) {
  if (!self->options || self->options->len == 0) return;
  if (!self->option_bars || !self->option_count_labels) return;

  gboolean show_results = self->has_voted || gnostr_poll_card_is_closed(self);

  for (guint i = 0; i < self->options->len; i++) {
    GnostrPollCardOption *opt = g_ptr_array_index(self->options, i);

    GtkWidget *bar = (i < self->option_bars->len) ?
                      g_ptr_array_index(self->option_bars, i) : NULL;
    GtkWidget *count_label = (i < self->option_count_labels->len) ?
                              g_ptr_array_index(self->option_count_labels, i) : NULL;
    GtkWidget *percent_label = (i < self->option_percent_labels->len) ?
                                g_ptr_array_index(self->option_percent_labels, i) : NULL;

    if (show_results) {
      /* Calculate percentage */
      double fraction = 0.0;
      int percent = 0;
      if (self->total_votes > 0) {
        fraction = (double)opt->vote_count / (double)self->total_votes;
        percent = (int)(fraction * 100.0 + 0.5);
      }

      /* Show progress bar */
      if (GTK_IS_PROGRESS_BAR(bar)) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), fraction);
        gtk_widget_set_visible(bar, TRUE);

        /* Check if this was user's vote */
        gboolean is_user_vote = FALSE;
        if (self->user_vote_indices) {
          for (guint j = 0; j < self->user_vote_indices->len; j++) {
            int idx = g_array_index(self->user_vote_indices, int, j);
            if (idx == (int)i) {
              is_user_vote = TRUE;
              break;
            }
          }
        }

        if (is_user_vote) {
          gtk_widget_add_css_class(bar, "poll-option-voted");
        } else {
          gtk_widget_remove_css_class(bar, "poll-option-voted");
        }
      }

      /* Show vote count */
      if (GTK_IS_LABEL(count_label)) {
        gchar *count_str = g_strdup_printf("%u", opt->vote_count);
        gtk_label_set_text(GTK_LABEL(count_label), count_str);
        gtk_widget_set_visible(count_label, TRUE);
        g_free(count_str);
      }

      /* Show percentage */
      if (GTK_IS_LABEL(percent_label)) {
        gchar *percent_str = g_strdup_printf("%d%%", percent);
        gtk_label_set_text(GTK_LABEL(percent_label), percent_str);
        gtk_widget_set_visible(percent_label, TRUE);
        g_free(percent_str);
      }
    } else {
      /* Hide results */
      if (GTK_IS_WIDGET(bar)) {
        gtk_widget_set_visible(bar, FALSE);
      }
      if (GTK_IS_WIDGET(count_label)) {
        gtk_widget_set_visible(count_label, FALSE);
      }
      if (GTK_IS_WIDGET(percent_label)) {
        gtk_widget_set_visible(percent_label, FALSE);
      }
    }
  }

  /* Update total vote count label */
  if (GTK_IS_LABEL(self->vote_count_label)) {
    gchar *count_str = g_strdup_printf(g_dngettext(NULL, "%u vote", "%u votes",
                                        self->total_votes), self->total_votes);
    gtk_label_set_text(GTK_LABEL(self->vote_count_label), count_str);
    g_free(count_str);
  }

  /* Update vote button visibility and sensitivity */
  gboolean can_vote = self->is_logged_in &&
                      !self->has_voted &&
                      !gnostr_poll_card_is_closed(self);

  if (GTK_IS_WIDGET(self->vote_button)) {
    gtk_widget_set_visible(self->vote_button, can_vote);
    gtk_widget_set_sensitive(self->vote_button, can_vote);
  }

  /* Show refresh button when results are visible */
  if (GTK_IS_WIDGET(self->refresh_button)) {
    gtk_widget_set_visible(self->refresh_button, show_results);
  }

  /* Disable option buttons if user has voted or poll is closed */
  if (self->option_buttons) {
    gboolean sensitive = !self->has_voted && !gnostr_poll_card_is_closed(self);
    for (guint i = 0; i < self->option_buttons->len; i++) {
      GtkWidget *btn = g_ptr_array_index(self->option_buttons, i);
      if (GTK_IS_WIDGET(btn)) {
        gtk_widget_set_sensitive(btn, sensitive);
      }
    }
  }
}

static void on_option_toggled(GtkCheckButton *btn, gpointer user_data) {
  GnostrPollCard *self = GNOSTR_POLL_CARD(user_data);
  (void)btn;

  /* Update vote button sensitivity based on selection */
  GArray *selected = gnostr_poll_card_get_selected(self);
  if (GTK_IS_WIDGET(self->vote_button)) {
    gboolean has_selection = selected && selected->len > 0;
    gboolean can_vote = self->is_logged_in &&
                        !self->has_voted &&
                        !gnostr_poll_card_is_closed(self) &&
                        has_selection;
    gtk_widget_set_sensitive(self->vote_button, can_vote);
  }
  if (selected) g_array_unref(selected);
}

static void on_vote_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPollCard *self = GNOSTR_POLL_CARD(user_data);
  (void)btn;

  if (!self->poll_id || gnostr_poll_card_is_closed(self)) {
    return;
  }

  GArray *selected = gnostr_poll_card_get_selected(self);
  if (!selected || selected->len == 0) {
    if (selected) g_array_unref(selected);
    return;
  }

  /* Emit vote-clicked signal */
  g_signal_emit(self, signals[SIGNAL_VOTE_CLICKED], 0,
                self->poll_id, selected);

  g_array_unref(selected);
}

static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPollCard *self = GNOSTR_POLL_CARD(user_data);
  (void)btn;

  if (self->poll_id) {
    g_signal_emit(self, signals[SIGNAL_RESULTS_REQUESTED], 0, self->poll_id);
  }
}

static void on_author_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrPollCard *self = GNOSTR_POLL_CARD(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;

  if (self->author_pubkey && *self->author_pubkey) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->author_pubkey);
  }
}

static void rebuild_options_ui(GnostrPollCard *self) {
  /* Clear existing option widgets */
  if (self->options_box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->options_box)) != NULL) {
      gtk_box_remove(GTK_BOX(self->options_box), child);
    }
  }

  /* Reset arrays */
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

  if (self->option_percent_labels) {
    g_ptr_array_set_size(self->option_percent_labels, 0);
  } else {
    self->option_percent_labels = g_ptr_array_new();
  }

  if (!self->options || self->options->len == 0) return;

  GtkWidget *first_button = NULL;

  for (guint i = 0; i < self->options->len; i++) {
    GnostrPollCardOption *opt = g_ptr_array_index(self->options, i);

    /* Option container */
    GtkWidget *option_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(option_row, "poll-option-row");

    /* Top row: button + percentage + count */
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(top_row, TRUE);

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

    /* Percentage label (hidden until results shown) */
    GtkWidget *percent_label = gtk_label_new("0%");
    gtk_widget_add_css_class(percent_label, "poll-option-percent");
    gtk_widget_set_visible(percent_label, FALSE);
    g_ptr_array_add(self->option_percent_labels, percent_label);

    /* Vote count label (hidden until results shown) */
    GtkWidget *count_label = gtk_label_new("0");
    gtk_widget_add_css_class(count_label, "poll-option-count");
    gtk_widget_add_css_class(count_label, "dim-label");
    gtk_widget_set_visible(count_label, FALSE);
    g_ptr_array_add(self->option_count_labels, count_label);

    gtk_box_append(GTK_BOX(top_row), button);
    gtk_box_append(GTK_BOX(top_row), percent_label);
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

/* Set avatar initials fallback */
static void set_avatar_initials(GnostrPollCard *self, const char *display) {
  if (!GTK_IS_LABEL(self->author_avatar_initials)) return;

  const char *src = (display && *display) ? display : "AN";
  char initials[3] = {0};
  int idx = 0;

  for (const char *p = src; *p && idx < 2; p++) {
    if (g_ascii_isalnum(*p)) {
      initials[idx++] = g_ascii_toupper(*p);
    }
  }
  if (idx == 0) {
    initials[0] = 'A';
    initials[1] = 'N';
  }

  gtk_label_set_text(GTK_LABEL(self->author_avatar_initials), initials);
  if (GTK_IS_WIDGET(self->author_avatar)) {
    gtk_widget_set_visible(self->author_avatar, FALSE);
  }
  gtk_widget_set_visible(self->author_avatar_initials, TRUE);
}

static void gnostr_poll_card_class_init(GnostrPollCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_poll_card_dispose;
  gclass->finalize = gnostr_poll_card_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);

  /* Signals */
  signals[SIGNAL_VOTE_CLICKED] = g_signal_new("vote-clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_POINTER);

  signals[SIGNAL_RESULTS_REQUESTED] = g_signal_new("results-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_poll_card_init(GnostrPollCard *self) {
  GtkLayoutManager *layout = gtk_widget_get_layout_manager(GTK_WIDGET(self));
  gtk_orientable_set_orientation(GTK_ORIENTABLE(layout), GTK_ORIENTATION_VERTICAL);

  gtk_widget_add_css_class(GTK_WIDGET(self), "poll-card");

  /* Initialize state */
  self->multiple_choice = FALSE;
  self->end_time = 0;
  self->created_at = 0;
  self->total_votes = 0;
  self->has_voted = FALSE;
  self->is_logged_in = FALSE;
  self->options = g_ptr_array_new();
  self->user_vote_indices = NULL;

  /* Main container */
  self->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(self->root, 12);
  gtk_widget_set_margin_end(self->root, 12);
  gtk_widget_set_margin_top(self->root, 12);
  gtk_widget_set_margin_bottom(self->root, 12);
  gtk_widget_set_parent(self->root, GTK_WIDGET(self));

  /* Header: Poll icon + type label + time remaining */
  self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->header_box, "poll-header");

  self->poll_icon = gtk_image_new_from_icon_name("view-list-bullet-symbolic");
  gtk_widget_add_css_class(self->poll_icon, "poll-icon");
  gtk_box_append(GTK_BOX(self->header_box), self->poll_icon);

  self->poll_type_label = gtk_label_new(_("Poll"));
  gtk_widget_add_css_class(self->poll_type_label, "poll-type-label");
  gtk_box_append(GTK_BOX(self->header_box), self->poll_type_label);

  /* Spacer */
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(self->header_box), spacer);

  /* Time remaining label */
  self->time_label = gtk_label_new("");
  gtk_widget_add_css_class(self->time_label, "poll-time-label");
  gtk_widget_set_visible(self->time_label, FALSE);
  gtk_box_append(GTK_BOX(self->header_box), self->time_label);

  gtk_box_append(GTK_BOX(self->root), self->header_box);

  /* Question label */
  self->question_label = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->question_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->question_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(self->question_label), 0.0);
  gtk_widget_add_css_class(self->question_label, "poll-question");
  gtk_box_append(GTK_BOX(self->root), self->question_label);

  /* Options container */
  self->options_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(self->options_box, "poll-options");
  gtk_box_append(GTK_BOX(self->root), self->options_box);

  /* Footer: vote count + buttons */
  self->footer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->footer_box, "poll-footer");

  self->vote_count_label = gtk_label_new(_("0 votes"));
  gtk_widget_add_css_class(self->vote_count_label, "poll-vote-count");
  gtk_widget_set_hexpand(self->vote_count_label, TRUE);
  gtk_widget_set_halign(self->vote_count_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(self->footer_box), self->vote_count_label);

  /* Refresh button */
  self->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_add_css_class(self->refresh_button, "poll-refresh-button");
  gtk_widget_add_css_class(self->refresh_button, "flat");
  gtk_widget_set_tooltip_text(self->refresh_button, _("Refresh results"));
  gtk_widget_set_visible(self->refresh_button, FALSE);
  g_signal_connect(self->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), self);
  gtk_box_append(GTK_BOX(self->footer_box), self->refresh_button);

  /* Vote button */
  self->vote_button = gtk_button_new_with_label(_("Vote"));
  gtk_widget_add_css_class(self->vote_button, "poll-vote-button");
  gtk_widget_add_css_class(self->vote_button, "suggested-action");
  gtk_widget_set_sensitive(self->vote_button, FALSE);
  g_signal_connect(self->vote_button, "clicked", G_CALLBACK(on_vote_clicked), self);
  gtk_box_append(GTK_BOX(self->footer_box), self->vote_button);

  gtk_box_append(GTK_BOX(self->root), self->footer_box);

  /* Author info row */
  self->author_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->author_box, "poll-author-row");
  gtk_widget_set_margin_top(self->author_box, 8);

  /* Avatar container with overlay for initials */
  GtkWidget *avatar_overlay = gtk_overlay_new();
  gtk_widget_set_size_request(avatar_overlay, 24, 24);

  self->author_avatar = gtk_image_new_from_icon_name("avatar-default-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->author_avatar), 24);
  gtk_widget_add_css_class(self->author_avatar, "poll-author-avatar");
  gtk_overlay_set_child(GTK_OVERLAY(avatar_overlay), self->author_avatar);

  self->author_avatar_initials = gtk_label_new("AN");
  gtk_widget_add_css_class(self->author_avatar_initials, "poll-author-avatar-initials");
  gtk_widget_set_visible(self->author_avatar_initials, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(avatar_overlay), self->author_avatar_initials);

  gtk_box_append(GTK_BOX(self->author_box), avatar_overlay);

  /* Author name */
  self->author_name_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->author_name_label), 0.0);
  gtk_widget_add_css_class(self->author_name_label, "poll-author-name");
  gtk_box_append(GTK_BOX(self->author_box), self->author_name_label);

  /* Creation time */
  self->created_at_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->created_at_label, "dim-label");
  gtk_box_append(GTK_BOX(self->author_box), self->created_at_label);

  /* Make author row clickable */
  GtkGesture *author_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(author_click), GDK_BUTTON_PRIMARY);
  g_signal_connect(author_click, "released", G_CALLBACK(on_author_clicked), self);
  gtk_widget_add_controller(self->author_box, GTK_EVENT_CONTROLLER(author_click));
  gtk_widget_set_cursor_from_name(self->author_box, "pointer");

  gtk_box_append(GTK_BOX(self->root), self->author_box);

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  /* Uses shared session from gnostr_get_shared_soup_session() */
#endif
}

GnostrPollCard *gnostr_poll_card_new(void) {
  return g_object_new(GNOSTR_TYPE_POLL_CARD, NULL);
}

void gnostr_poll_card_set_poll(GnostrPollCard *self,
                                const char *event_id,
                                const char *question,
                                gint64 created_at) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));

  g_clear_pointer(&self->poll_id, g_free);
  self->poll_id = g_strdup(event_id);
  self->created_at = created_at;

  /* Set question */
  if (GTK_IS_LABEL(self->question_label)) {
    if (question && *question) {
      gtk_label_set_text(GTK_LABEL(self->question_label), question);
    } else {
      gtk_label_set_text(GTK_LABEL(self->question_label), _("Poll"));
    }
  }

  /* Update creation timestamp display */
  if (GTK_IS_LABEL(self->created_at_label)) {
    gchar *ts = format_timestamp(created_at);
    gtk_label_set_text(GTK_LABEL(self->created_at_label), ts);
    g_free(ts);
  }
}

void gnostr_poll_card_set_author(GnostrPollCard *self,
                                  const char *pubkey_hex,
                                  const char *display_name,
                                  const char *avatar_url) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));

  g_clear_pointer(&self->author_pubkey, g_free);
  self->author_pubkey = g_strdup(pubkey_hex);

  /* Set display name */
  if (GTK_IS_LABEL(self->author_name_label)) {
    if (display_name && *display_name) {
      gtk_label_set_text(GTK_LABEL(self->author_name_label), display_name);
    } else if (pubkey_hex && strlen(pubkey_hex) >= 8) {
      gchar *truncated = g_strdup_printf("%.8s...", pubkey_hex);
      gtk_label_set_text(GTK_LABEL(self->author_name_label), truncated);
      g_free(truncated);
    } else {
      gtk_label_set_text(GTK_LABEL(self->author_name_label), _("Anonymous"));
    }
  }

  /* Set avatar initials fallback */
  set_avatar_initials(self, display_name);

  /* Load avatar if available */
#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url && GTK_IS_IMAGE(self->author_avatar)) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_image_set_from_paintable(GTK_IMAGE(self->author_avatar), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->author_avatar, TRUE);
      gtk_widget_set_visible(self->author_avatar_initials, FALSE);
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->author_avatar, self->author_avatar_initials);
    }
  }
#else
  (void)avatar_url;
#endif
}

void gnostr_poll_card_set_options(GnostrPollCard *self,
                                   GnostrPollCardOption *options,
                                   gsize count) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));

  /* Clear existing options */
  if (self->options) {
    for (guint i = 0; i < self->options->len; i++) {
      GnostrPollCardOption *opt = g_ptr_array_index(self->options, i);
      g_free(opt->text);
      g_free(opt);
    }
    g_ptr_array_set_size(self->options, 0);
  }

  /* Add new options */
  for (gsize i = 0; i < count; i++) {
    GnostrPollCardOption *opt = g_new0(GnostrPollCardOption, 1);
    opt->index = options[i].index;
    opt->text = g_strdup(options[i].text);
    opt->vote_count = options[i].vote_count;
    g_ptr_array_add(self->options, opt);
  }

  rebuild_options_ui(self);
}

void gnostr_poll_card_set_multiple_choice(GnostrPollCard *self, gboolean multiple) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));

  if (self->multiple_choice != multiple) {
    self->multiple_choice = multiple;

    /* Update type label */
    if (GTK_IS_LABEL(self->poll_type_label)) {
      gtk_label_set_text(GTK_LABEL(self->poll_type_label),
        multiple ? _("Multiple Choice Poll") : _("Poll"));
    }

    /* Rebuild options UI to switch between radio/checkbox */
    rebuild_options_ui(self);
  }
}

gboolean gnostr_poll_card_is_multiple_choice(GnostrPollCard *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_CARD(self), FALSE);
  return self->multiple_choice;
}

void gnostr_poll_card_set_end_time(GnostrPollCard *self, gint64 end_time) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));

  self->end_time = end_time;

  /* Cancel existing timer */
  if (self->time_update_timer > 0) {
    g_source_remove(self->time_update_timer);
    self->time_update_timer = 0;
  }

  update_time_display(self);

  /* LEGITIMATE TIMEOUT - Periodic poll countdown update (30s intervals).
   * nostrc-b0h: Audited - UI countdown timer is appropriate. */
  if (end_time > 0 && !gnostr_poll_card_is_closed(self)) {
    self->time_update_timer = g_timeout_add_seconds(30, time_update_callback, self);
  }

  update_results_display(self);
}

gint64 gnostr_poll_card_get_end_time(GnostrPollCard *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_CARD(self), 0);
  return self->end_time;
}

gboolean gnostr_poll_card_is_closed(GnostrPollCard *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_CARD(self), TRUE);
  if (self->end_time <= 0) return FALSE;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  return now >= self->end_time;
}

void gnostr_poll_card_set_vote_counts(GnostrPollCard *self,
                                       guint *vote_counts,
                                       gsize count,
                                       guint total_votes) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));
  if (!self->options) return;

  for (gsize i = 0; i < count && i < self->options->len; i++) {
    GnostrPollCardOption *opt = g_ptr_array_index(self->options, i);
    opt->vote_count = vote_counts[i];
  }

  self->total_votes = total_votes;
  update_results_display(self);
}

void gnostr_poll_card_set_has_voted(GnostrPollCard *self, gboolean has_voted) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));
  self->has_voted = has_voted;
  update_results_display(self);
}

gboolean gnostr_poll_card_has_voted(GnostrPollCard *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_CARD(self), FALSE);
  return self->has_voted;
}

void gnostr_poll_card_set_user_votes(GnostrPollCard *self,
                                      int *indices,
                                      gsize count) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));

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

void gnostr_poll_card_set_logged_in(GnostrPollCard *self, gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_POLL_CARD(self));
  self->is_logged_in = logged_in;
  update_results_display(self);
}

GArray *gnostr_poll_card_get_selected(GnostrPollCard *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_CARD(self), NULL);

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

const char *gnostr_poll_card_get_poll_id(GnostrPollCard *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_CARD(self), NULL);
  return self->poll_id;
}

const char *gnostr_poll_card_get_author_pubkey(GnostrPollCard *self) {
  g_return_val_if_fail(GNOSTR_IS_POLL_CARD(self), NULL);
  return self->author_pubkey;
}
