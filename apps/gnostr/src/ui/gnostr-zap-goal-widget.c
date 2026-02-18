/**
 * GnostrZapGoalWidget - NIP-75 Zap Goal Display Widget
 *
 * Displays a zap goal with progress bar and funding status.
 */

#include "gnostr-zap-goal-widget.h"
#include "../util/nip75_goals.h"
#include <glib/gi18n.h>

#define UI_RESOURCE "/org/gnostr/ui/widgets/gnostr-zap-goal-widget.ui"

struct _GnostrZapGoalWidget {
  GtkWidget parent_instance;

  /* Template children */
  GtkWidget *main_box;
  GtkWidget *header_box;
  GtkWidget *icon_image;
  GtkWidget *title_label;
  GtkWidget *creator_button;
  GtkWidget *creator_label;
  GtkWidget *description_label;
  GtkWidget *progress_bar;
  GtkWidget *progress_label;
  GtkWidget *stats_box;
  GtkWidget *zap_count_label;
  GtkWidget *deadline_label;
  GtkWidget *status_badge;
  GtkWidget *status_label;
  GtkWidget *zap_button;

  /* State */
  gchar *goal_id;
  gchar *creator_pubkey;
  gchar *creator_name;
  gchar *creator_lud16;
  gint64 target_msat;
  gint64 received_msat;
  guint zap_count;
  gint64 closed_at;
  gboolean is_complete;
  gboolean is_expired;
  gboolean logged_in;
  gchar *linked_event_id;
};

G_DEFINE_TYPE(GnostrZapGoalWidget, gnostr_zap_goal_widget, GTK_TYPE_WIDGET)

/* Signals */
enum {
  SIGNAL_ZAP_TO_GOAL,
  SIGNAL_GOAL_CLICKED,
  SIGNAL_CREATOR_CLICKED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_zap_button_clicked(GtkButton *btn, gpointer user_data);
static void on_creator_clicked(GtkButton *btn, gpointer user_data);
static void update_progress_display(GnostrZapGoalWidget *self);
static void update_status_display(GnostrZapGoalWidget *self);
static void update_deadline_display(GnostrZapGoalWidget *self);

static void gnostr_zap_goal_widget_dispose(GObject *obj) {
  GnostrZapGoalWidget *self = GNOSTR_ZAP_GOAL_WIDGET(obj);

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_ZAP_GOAL_WIDGET);
  G_OBJECT_CLASS(gnostr_zap_goal_widget_parent_class)->dispose(obj);
}

static void gnostr_zap_goal_widget_finalize(GObject *obj) {
  GnostrZapGoalWidget *self = GNOSTR_ZAP_GOAL_WIDGET(obj);

  g_clear_pointer(&self->goal_id, g_free);
  g_clear_pointer(&self->creator_pubkey, g_free);
  g_clear_pointer(&self->creator_name, g_free);
  g_clear_pointer(&self->creator_lud16, g_free);
  g_clear_pointer(&self->linked_event_id, g_free);

  G_OBJECT_CLASS(gnostr_zap_goal_widget_parent_class)->finalize(obj);
}

static void gnostr_zap_goal_widget_class_init(GnostrZapGoalWidgetClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_zap_goal_widget_dispose;
  gclass->finalize = gnostr_zap_goal_widget_finalize;

  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  /* Layout manager for composite widget */
  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BIN_LAYOUT);

  /* Bind template children */
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, main_box);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, header_box);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, icon_image);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, title_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, creator_button);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, creator_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, description_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, progress_bar);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, progress_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, stats_box);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, zap_count_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, deadline_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, status_badge);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, status_label);
  gtk_widget_class_bind_template_child(wclass, GnostrZapGoalWidget, zap_button);

  /* Bind template callbacks */
  gtk_widget_class_bind_template_callback(wclass, on_zap_button_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_creator_clicked);

  /**
   * GnostrZapGoalWidget::zap-to-goal:
   * @self: the widget
   * @goal_id: goal event ID (hex)
   * @creator_pubkey: creator's pubkey (hex)
   * @lud16: creator's lightning address
   */
  signals[SIGNAL_ZAP_TO_GOAL] = g_signal_new(
    "zap-to-goal",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * GnostrZapGoalWidget::goal-clicked:
   * @self: the widget
   * @goal_id: goal event ID (hex)
   */
  signals[SIGNAL_GOAL_CLICKED] = g_signal_new(
    "goal-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  /**
   * GnostrZapGoalWidget::creator-clicked:
   * @self: the widget
   * @pubkey: creator's pubkey (hex)
   */
  signals[SIGNAL_CREATOR_CLICKED] = g_signal_new(
    "creator-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_zap_goal_widget_init(GnostrZapGoalWidget *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->logged_in = FALSE;
  self->is_complete = FALSE;
  self->is_expired = FALSE;

  /* Initial state */
  update_progress_display(self);
  update_status_display(self);
}

GnostrZapGoalWidget *gnostr_zap_goal_widget_new(void) {
  return g_object_new(GNOSTR_TYPE_ZAP_GOAL_WIDGET, NULL);
}

void gnostr_zap_goal_widget_set_goal_id(GnostrZapGoalWidget *self,
                                         const gchar *goal_id_hex) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  g_clear_pointer(&self->goal_id, g_free);
  self->goal_id = g_strdup(goal_id_hex);
}

const gchar *gnostr_zap_goal_widget_get_goal_id(GnostrZapGoalWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self), NULL);
  return self->goal_id;
}

void gnostr_zap_goal_widget_set_creator(GnostrZapGoalWidget *self,
                                         const gchar *pubkey_hex,
                                         const gchar *display_name,
                                         const gchar *lud16) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  g_clear_pointer(&self->creator_pubkey, g_free);
  g_clear_pointer(&self->creator_name, g_free);
  g_clear_pointer(&self->creator_lud16, g_free);

  self->creator_pubkey = g_strdup(pubkey_hex);
  self->creator_name = g_strdup(display_name);
  self->creator_lud16 = g_strdup(lud16);

  /* Update UI */
  if (GTK_IS_LABEL(self->creator_label)) {
    if (display_name && *display_name) {
      gtk_label_set_text(GTK_LABEL(self->creator_label), display_name);
    } else if (pubkey_hex) {
      /* Show truncated pubkey */
      g_autofree gchar *truncated = g_strdup_printf("%.8s...%.4s", pubkey_hex, pubkey_hex + 60);
      gtk_label_set_text(GTK_LABEL(self->creator_label), truncated);
    }
  }

  /* Enable/disable zap button based on lud16 availability */
  update_status_display(self);
}

void gnostr_zap_goal_widget_set_description(GnostrZapGoalWidget *self,
                                             const gchar *description) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  if (GTK_IS_LABEL(self->description_label)) {
    gtk_label_set_text(GTK_LABEL(self->description_label),
                       description ? description : "");
    gtk_widget_set_visible(self->description_label,
                           description && *description);
  }
}

void gnostr_zap_goal_widget_set_target(GnostrZapGoalWidget *self,
                                        gint64 target_msat) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  self->target_msat = target_msat;

  /* Update title with target */
  if (GTK_IS_LABEL(self->title_label)) {
    gchar *target_str = gnostr_nip75_format_target(target_msat);
    g_autofree gchar *title = g_strdup_printf("Zap Goal: %s", target_str);
    gtk_label_set_text(GTK_LABEL(self->title_label), title);
    g_free(target_str);
  }

  update_progress_display(self);
}

void gnostr_zap_goal_widget_set_progress(GnostrZapGoalWidget *self,
                                          gint64 received_msat,
                                          guint zap_count) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  self->received_msat = received_msat;
  self->zap_count = zap_count;

  /* Check if complete */
  if (self->target_msat > 0 && received_msat >= self->target_msat) {
    self->is_complete = TRUE;
  }

  update_progress_display(self);
  update_status_display(self);
}

void gnostr_zap_goal_widget_set_deadline(GnostrZapGoalWidget *self,
                                          gint64 closed_at) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  self->closed_at = closed_at;
  update_deadline_display(self);
  update_status_display(self);
}

void gnostr_zap_goal_widget_set_complete(GnostrZapGoalWidget *self,
                                          gboolean is_complete) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  self->is_complete = is_complete;
  update_status_display(self);
}

void gnostr_zap_goal_widget_set_expired(GnostrZapGoalWidget *self,
                                         gboolean is_expired) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  self->is_expired = is_expired;
  update_status_display(self);
}

void gnostr_zap_goal_widget_set_linked_event(GnostrZapGoalWidget *self,
                                              const gchar *event_id) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  g_clear_pointer(&self->linked_event_id, g_free);
  self->linked_event_id = g_strdup(event_id);
}

void gnostr_zap_goal_widget_set_logged_in(GnostrZapGoalWidget *self,
                                           gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self));

  self->logged_in = logged_in;
  update_status_display(self);
}

gdouble gnostr_zap_goal_widget_get_progress_percent(GnostrZapGoalWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self), 0.0);

  if (self->target_msat <= 0) return 0.0;
  return ((gdouble)self->received_msat / (gdouble)self->target_msat) * 100.0;
}

gboolean gnostr_zap_goal_widget_is_complete(GnostrZapGoalWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self), FALSE);
  return self->is_complete;
}

gboolean gnostr_zap_goal_widget_is_expired(GnostrZapGoalWidget *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_WIDGET(self), FALSE);
  return self->is_expired;
}

/* Internal helpers */

static void update_progress_display(GnostrZapGoalWidget *self) {
  if (!GTK_IS_PROGRESS_BAR(self->progress_bar)) return;

  gdouble fraction = 0.0;
  if (self->target_msat > 0) {
    fraction = (gdouble)self->received_msat / (gdouble)self->target_msat;
    /* Cap at 1.0 for display but allow overfunding */
    if (fraction > 1.0) fraction = 1.0;
  }

  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), fraction);

  /* Update progress label */
  if (GTK_IS_LABEL(self->progress_label)) {
    gchar *progress_str = gnostr_nip75_format_progress(self->received_msat,
                                                        self->target_msat);
    gdouble percent = gnostr_zap_goal_widget_get_progress_percent(self);
    g_autofree gchar *full_str = g_strdup_printf("%s (%.0f%%)", progress_str, percent);
    gtk_label_set_text(GTK_LABEL(self->progress_label), full_str);
    g_free(progress_str);
  }

  /* Update zap count */
  if (GTK_IS_LABEL(self->zap_count_label)) {
    g_autofree gchar *count_str = g_strdup_printf("%u zap%s",
                                       self->zap_count,
                                       self->zap_count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(self->zap_count_label), count_str);
  }
}

static void update_deadline_display(GnostrZapGoalWidget *self) {
  if (!GTK_IS_LABEL(self->deadline_label)) return;

  if (self->closed_at > 0) {
    gchar *remaining = gnostr_nip75_format_time_remaining(self->closed_at);
    if (remaining) {
      g_autofree gchar *deadline_text = g_strdup_printf("%s remaining", remaining);
      gtk_label_set_text(GTK_LABEL(self->deadline_label), deadline_text);
      g_free(remaining);
    } else {
      gtk_label_set_text(GTK_LABEL(self->deadline_label), "Ended");
    }
    gtk_widget_set_visible(self->deadline_label, TRUE);
  } else {
    gtk_widget_set_visible(self->deadline_label, FALSE);
  }
}

static void update_status_display(GnostrZapGoalWidget *self) {
  /* Update status badge */
  if (GTK_IS_WIDGET(self->status_badge)) {
    gboolean show_badge = FALSE;
    const gchar *status_text = NULL;
    const gchar *css_class = NULL;

    if (self->is_complete) {
      show_badge = TRUE;
      status_text = "Goal Reached!";
      css_class = "zap-goal-complete";
    } else if (self->is_expired) {
      show_badge = TRUE;
      status_text = "Ended";
      css_class = "zap-goal-expired";
    }

    gtk_widget_set_visible(self->status_badge, show_badge);
    if (show_badge && GTK_IS_LABEL(self->status_label)) {
      gtk_label_set_text(GTK_LABEL(self->status_label), status_text);
    }

    /* Update CSS class */
    gtk_widget_remove_css_class(self->status_badge, "zap-goal-complete");
    gtk_widget_remove_css_class(self->status_badge, "zap-goal-expired");
    if (css_class) {
      gtk_widget_add_css_class(self->status_badge, css_class);
    }
  }

  /* Update zap button state */
  if (GTK_IS_WIDGET(self->zap_button)) {
    gboolean can_zap = self->logged_in &&
                       self->creator_lud16 != NULL &&
                       *self->creator_lud16 != '\0' &&
                       !self->is_expired;
    gtk_widget_set_sensitive(self->zap_button, can_zap);

    /* Add completed styling */
    if (self->is_complete) {
      gtk_widget_add_css_class(GTK_WIDGET(self), "zap-goal-funded");
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(self), "zap-goal-funded");
    }
  }

  /* Update progress bar styling */
  if (GTK_IS_PROGRESS_BAR(self->progress_bar)) {
    gtk_widget_remove_css_class(self->progress_bar, "zap-goal-bar-complete");
    gtk_widget_remove_css_class(self->progress_bar, "zap-goal-bar-expired");

    if (self->is_complete) {
      gtk_widget_add_css_class(self->progress_bar, "zap-goal-bar-complete");
    } else if (self->is_expired) {
      gtk_widget_add_css_class(self->progress_bar, "zap-goal-bar-expired");
    }
  }
}

/* Signal handlers */

static void on_zap_button_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrZapGoalWidget *self = GNOSTR_ZAP_GOAL_WIDGET(user_data);

  if (!self->creator_pubkey || !self->creator_lud16) {
    g_debug("NIP-75: Cannot zap - missing creator info");
    return;
  }

  g_signal_emit(self, signals[SIGNAL_ZAP_TO_GOAL], 0,
                self->goal_id,
                self->creator_pubkey,
                self->creator_lud16);
}

static void on_creator_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrZapGoalWidget *self = GNOSTR_ZAP_GOAL_WIDGET(user_data);

  if (!self->creator_pubkey) return;

  g_signal_emit(self, signals[SIGNAL_CREATOR_CLICKED], 0,
                self->creator_pubkey);
}
