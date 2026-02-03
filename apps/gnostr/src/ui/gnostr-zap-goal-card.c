/**
 * GnostrZapGoalCard - NIP-75 Zap Goal Card Widget Implementation
 *
 * A card-style widget for displaying and interacting with zap goals.
 */

#include "gnostr-zap-goal-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip75_zap_goals.h"
#include <glib/gi18n.h>

/* Deadline timer interval in milliseconds (1 minute) */
#define DEADLINE_TIMER_INTERVAL_MS 60000

/* Celebration animation duration in milliseconds */
#define CELEBRATION_DURATION_MS 3000

struct _GnostrZapGoalCard {
  GtkWidget parent_instance;

  /* Main container */
  GtkWidget *card_box;

  /* Header section */
  GtkWidget *header_box;
  GtkWidget *goal_icon;
  GtkWidget *title_label;

  /* Author section */
  GtkWidget *author_box;
  GtkWidget *avatar_button;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *author_name_button;
  GtkWidget *author_name_label;

  /* Progress section */
  GtkWidget *progress_box;
  GtkWidget *progress_bar;
  GtkWidget *progress_label;
  GtkWidget *percent_label;

  /* Stats section */
  GtkWidget *stats_box;
  GtkWidget *zap_count_label;
  GtkWidget *deadline_box;
  GtkWidget *deadline_icon;
  GtkWidget *deadline_label;

  /* Status badge */
  GtkWidget *status_overlay;
  GtkWidget *status_badge;
  GtkWidget *status_label;

  /* Action section */
  GtkWidget *action_box;
  GtkWidget *zap_button;
  GtkWidget *zap_button_label;

  /* Celebration overlay */
  GtkWidget *celebration_overlay;
  GtkWidget *celebration_label;

  /* State */
  gchar *goal_id;
  gchar *pubkey;
  gchar *display_name;
  gchar *lud16;
  gint64 target_msats;
  gint64 current_msats;
  guint zap_count;
  gint64 end_time;
  gboolean is_complete;
  gboolean is_expired;
  gboolean logged_in;
  gboolean celebration_shown;

  /* Timer */
  guint deadline_timer_id;
};

G_DEFINE_TYPE(GnostrZapGoalCard, gnostr_zap_goal_card, GTK_TYPE_WIDGET)

/* Signals */
enum {
  SIGNAL_ZAP_CLICKED,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_GOAL_REACHED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_progress_display(GnostrZapGoalCard *self);
static void update_status_display(GnostrZapGoalCard *self);
static void update_deadline_display(GnostrZapGoalCard *self);
static void update_zap_button_state(GnostrZapGoalCard *self);
static void set_avatar_initials(GnostrZapGoalCard *self, const gchar *name);
static gboolean on_deadline_timer(gpointer user_data);
static void on_celebration_finished(gpointer user_data);

/* ============== GObject Lifecycle ============== */

static void gnostr_zap_goal_card_dispose(GObject *obj) {
  GnostrZapGoalCard *self = GNOSTR_ZAP_GOAL_CARD(obj);

  /* Stop timer */
  if (self->deadline_timer_id > 0) {
    g_source_remove(self->deadline_timer_id);
    self->deadline_timer_id = 0;
  }

  /* Clear child widgets */
  g_clear_pointer(&self->card_box, gtk_widget_unparent);

  G_OBJECT_CLASS(gnostr_zap_goal_card_parent_class)->dispose(obj);
}

static void gnostr_zap_goal_card_finalize(GObject *obj) {
  GnostrZapGoalCard *self = GNOSTR_ZAP_GOAL_CARD(obj);

  g_clear_pointer(&self->goal_id, g_free);
  g_clear_pointer(&self->pubkey, g_free);
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->lud16, g_free);

  G_OBJECT_CLASS(gnostr_zap_goal_card_parent_class)->finalize(obj);
}

/* ============== Signal Handlers ============== */

static void on_zap_button_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrZapGoalCard *self = GNOSTR_ZAP_GOAL_CARD(user_data);

  if (!self->pubkey || !self->lud16) {
    g_debug("NIP-75 Card: Cannot zap - missing creator info");
    return;
  }

  g_signal_emit(self, signals[SIGNAL_ZAP_CLICKED], 0,
                self->goal_id,
                self->pubkey,
                self->lud16);
}

static void on_author_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrZapGoalCard *self = GNOSTR_ZAP_GOAL_CARD(user_data);

  if (!self->pubkey) return;

  g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey);
}

/* ============== UI Construction ============== */

static GtkWidget *create_avatar_widget(GnostrZapGoalCard *self) {
  /* Overlay for avatar image + initials fallback */
  self->avatar_overlay = gtk_overlay_new();
  gtk_widget_set_size_request(self->avatar_overlay, 40, 40);

  /* Avatar image */
  self->avatar_image = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(self->avatar_image), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_size_request(self->avatar_image, 40, 40);
  gtk_widget_add_css_class(self->avatar_image, "avatar-image");
  gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(self->avatar_overlay), self->avatar_image);

  /* Initials fallback */
  self->avatar_initials = gtk_label_new("?");
  gtk_widget_add_css_class(self->avatar_initials, "avatar-initials");
  gtk_widget_set_halign(self->avatar_initials, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->avatar_initials, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->avatar_overlay), self->avatar_initials);

  /* Wrap in button for click handling */
  self->avatar_button = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->avatar_button), FALSE);
  gtk_button_set_child(GTK_BUTTON(self->avatar_button), self->avatar_overlay);
  gtk_widget_add_css_class(self->avatar_button, "avatar-button");
  g_signal_connect(self->avatar_button, "clicked", G_CALLBACK(on_author_clicked), self);

  return self->avatar_button;
}

static void build_card_ui(GnostrZapGoalCard *self) {
  /* Main card container */
  self->card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_parent(self->card_box, GTK_WIDGET(self));
  gtk_widget_add_css_class(self->card_box, "zap-goal-card");
  gtk_widget_set_margin_start(self->card_box, 12);
  gtk_widget_set_margin_end(self->card_box, 12);
  gtk_widget_set_margin_top(self->card_box, 12);
  gtk_widget_set_margin_bottom(self->card_box, 12);

  /* ---- Header Section ---- */
  self->header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(self->card_box), self->header_box);

  /* Goal icon */
  self->goal_icon = gtk_image_new_from_icon_name("starred-symbolic");
  gtk_widget_add_css_class(self->goal_icon, "zap-goal-icon");
  gtk_box_append(GTK_BOX(self->header_box), self->goal_icon);

  /* Title label */
  self->title_label = gtk_label_new(_("Zap Goal"));
  gtk_label_set_xalign(GTK_LABEL(self->title_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->title_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->title_label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_hexpand(self->title_label, TRUE);
  gtk_widget_add_css_class(self->title_label, "zap-goal-title");
  gtk_box_append(GTK_BOX(self->header_box), self->title_label);

  /* Status badge (overlay-style) */
  self->status_badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(self->status_badge, "zap-goal-status-badge");
  gtk_widget_set_visible(self->status_badge, FALSE);

  self->status_label = gtk_label_new("");
  gtk_widget_add_css_class(self->status_label, "zap-goal-status-label");
  gtk_box_append(GTK_BOX(self->status_badge), self->status_label);
  gtk_box_append(GTK_BOX(self->header_box), self->status_badge);

  /* ---- Author Section ---- */
  self->author_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->author_box, "zap-goal-author");
  gtk_box_append(GTK_BOX(self->card_box), self->author_box);

  /* Avatar */
  GtkWidget *avatar = create_avatar_widget(self);
  gtk_box_append(GTK_BOX(self->author_box), avatar);

  /* Author name button */
  self->author_name_label = gtk_label_new(_("Anonymous"));
  gtk_label_set_xalign(GTK_LABEL(self->author_name_label), 0.0);
  gtk_widget_add_css_class(self->author_name_label, "zap-goal-author-name");

  self->author_name_button = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->author_name_button), FALSE);
  gtk_button_set_child(GTK_BUTTON(self->author_name_button), self->author_name_label);
  g_signal_connect(self->author_name_button, "clicked", G_CALLBACK(on_author_clicked), self);
  gtk_box_append(GTK_BOX(self->author_box), self->author_name_button);

  /* ---- Progress Section ---- */
  self->progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(self->progress_box, "zap-goal-progress-section");
  gtk_box_append(GTK_BOX(self->card_box), self->progress_box);

  /* Progress bar */
  self->progress_bar = gtk_progress_bar_new();
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), 0.0);
  gtk_widget_add_css_class(self->progress_bar, "zap-goal-progress-bar");
  gtk_box_append(GTK_BOX(self->progress_box), self->progress_bar);

  /* Progress info row */
  GtkWidget *progress_info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(self->progress_box), progress_info_box);

  /* Progress label (e.g., "50K / 100K sats") */
  self->progress_label = gtk_label_new("0 / 0 sats");
  gtk_label_set_xalign(GTK_LABEL(self->progress_label), 0.0);
  gtk_widget_set_hexpand(self->progress_label, TRUE);
  gtk_widget_add_css_class(self->progress_label, "zap-goal-progress-text");
  gtk_box_append(GTK_BOX(progress_info_box), self->progress_label);

  /* Percent label */
  self->percent_label = gtk_label_new("0%");
  gtk_label_set_xalign(GTK_LABEL(self->percent_label), 1.0);
  gtk_widget_add_css_class(self->percent_label, "zap-goal-percent");
  gtk_box_append(GTK_BOX(progress_info_box), self->percent_label);

  /* ---- Stats Section ---- */
  self->stats_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class(self->stats_box, "zap-goal-stats");
  gtk_box_append(GTK_BOX(self->card_box), self->stats_box);

  /* Zap count */
  GtkWidget *zap_count_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *zap_count_icon = gtk_image_new_from_icon_name("emoji-people-symbolic");
  gtk_box_append(GTK_BOX(zap_count_box), zap_count_icon);
  self->zap_count_label = gtk_label_new("0 zaps");
  gtk_widget_add_css_class(self->zap_count_label, "zap-goal-stat-label");
  gtk_box_append(GTK_BOX(zap_count_box), self->zap_count_label);
  gtk_box_append(GTK_BOX(self->stats_box), zap_count_box);

  /* Deadline */
  self->deadline_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_visible(self->deadline_box, FALSE);
  self->deadline_icon = gtk_image_new_from_icon_name("alarm-symbolic");
  gtk_box_append(GTK_BOX(self->deadline_box), self->deadline_icon);
  self->deadline_label = gtk_label_new("");
  gtk_widget_add_css_class(self->deadline_label, "zap-goal-stat-label");
  gtk_box_append(GTK_BOX(self->deadline_box), self->deadline_label);
  gtk_box_append(GTK_BOX(self->stats_box), self->deadline_box);

  /* ---- Action Section ---- */
  self->action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(self->action_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(self->action_box, "zap-goal-actions");
  gtk_box_append(GTK_BOX(self->card_box), self->action_box);

  /* Large Zap button */
  self->zap_button = gtk_button_new();
  gtk_widget_add_css_class(self->zap_button, "zap-goal-zap-button");
  gtk_widget_add_css_class(self->zap_button, "suggested-action");
  gtk_widget_set_size_request(self->zap_button, 200, 48);

  GtkWidget *zap_btn_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(zap_btn_content, GTK_ALIGN_CENTER);
  GtkWidget *zap_icon = gtk_image_new_from_icon_name("weather-storm-symbolic");
  gtk_box_append(GTK_BOX(zap_btn_content), zap_icon);
  self->zap_button_label = gtk_label_new(_("Zap this Goal"));
  gtk_widget_add_css_class(self->zap_button_label, "zap-button-text");
  gtk_box_append(GTK_BOX(zap_btn_content), self->zap_button_label);
  gtk_button_set_child(GTK_BUTTON(self->zap_button), zap_btn_content);

  g_signal_connect(self->zap_button, "clicked", G_CALLBACK(on_zap_button_clicked), self);
  gtk_box_append(GTK_BOX(self->action_box), self->zap_button);

  /* ---- Celebration Overlay (hidden by default) ---- */
  self->celebration_overlay = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(self->celebration_overlay, "zap-goal-celebration");
  gtk_widget_set_visible(self->celebration_overlay, FALSE);
  gtk_widget_set_halign(self->celebration_overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->celebration_overlay, GTK_ALIGN_CENTER);

  GtkWidget *celebration_icon = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(celebration_icon), 64);
  gtk_widget_add_css_class(celebration_icon, "celebration-icon");
  gtk_box_append(GTK_BOX(self->celebration_overlay), celebration_icon);

  self->celebration_label = gtk_label_new(_("Goal Reached!"));
  gtk_widget_add_css_class(self->celebration_label, "celebration-text");
  gtk_box_append(GTK_BOX(self->celebration_overlay), self->celebration_label);

  /* Note: celebration_overlay is shown on top when triggered */
}

/* ============== Class Init ============== */

static void gnostr_zap_goal_card_class_init(GnostrZapGoalCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_zap_goal_card_dispose;
  gclass->finalize = gnostr_zap_goal_card_finalize;

  /* Use bin layout for single child */
  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BIN_LAYOUT);

  /* CSS class for styling */
  gtk_widget_class_set_css_name(wclass, "gnostr-zap-goal-card");

  /**
   * GnostrZapGoalCard::zap-clicked:
   * @self: the widget
   * @goal_id: goal event ID (hex)
   * @pubkey: creator's pubkey (hex)
   * @lud16: creator's lightning address
   */
  signals[SIGNAL_ZAP_CLICKED] = g_signal_new(
    "zap-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  /**
   * GnostrZapGoalCard::open-profile:
   * @self: the widget
   * @pubkey: creator's pubkey (hex)
   */
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new(
    "open-profile",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  /**
   * GnostrZapGoalCard::goal-reached:
   * @self: the widget
   * @goal_id: goal event ID (hex)
   */
  signals[SIGNAL_GOAL_REACHED] = g_signal_new(
    "goal-reached",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_zap_goal_card_init(GnostrZapGoalCard *self) {
  self->logged_in = FALSE;
  self->is_complete = FALSE;
  self->is_expired = FALSE;
  self->celebration_shown = FALSE;
  self->deadline_timer_id = 0;

  build_card_ui(self);
  update_zap_button_state(self);
}

/* ============== Public API ============== */

GnostrZapGoalCard *gnostr_zap_goal_card_new(void) {
  return g_object_new(GNOSTR_TYPE_ZAP_GOAL_CARD, NULL);
}

void gnostr_zap_goal_card_set_goal_id(GnostrZapGoalCard *self,
                                       const gchar *goal_id_hex) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  g_clear_pointer(&self->goal_id, g_free);
  self->goal_id = g_strdup(goal_id_hex);
}

const gchar *gnostr_zap_goal_card_get_goal_id(GnostrZapGoalCard *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self), NULL);
  return self->goal_id;
}

void gnostr_zap_goal_card_set_title(GnostrZapGoalCard *self,
                                     const gchar *title) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  if (GTK_IS_LABEL(self->title_label)) {
    gtk_label_set_text(GTK_LABEL(self->title_label),
                       (title && *title) ? title : _("Zap Goal"));
  }
}

void gnostr_zap_goal_card_set_target(GnostrZapGoalCard *self,
                                      gint64 target_msats) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  self->target_msats = target_msats;
  update_progress_display(self);
}

void gnostr_zap_goal_card_set_progress(GnostrZapGoalCard *self,
                                        gint64 current_msats,
                                        guint zap_count) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  gboolean was_complete = self->is_complete;
  self->current_msats = current_msats;
  self->zap_count = zap_count;

  /* Check if just completed */
  if (self->target_msats > 0 && current_msats >= self->target_msats) {
    self->is_complete = TRUE;

    /* Emit goal-reached signal and trigger celebration on first completion */
    if (!was_complete && !self->celebration_shown) {
      g_signal_emit(self, signals[SIGNAL_GOAL_REACHED], 0, self->goal_id);
      gnostr_zap_goal_card_trigger_celebration(self);
    }
  }

  update_progress_display(self);
  update_status_display(self);
}

void gnostr_zap_goal_card_set_deadline(GnostrZapGoalCard *self,
                                        gint64 end_time) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  self->end_time = end_time;
  update_deadline_display(self);

  /* Start or stop timer based on whether there's a deadline */
  if (end_time > 0) {
    gnostr_zap_goal_card_start_deadline_timer(self);
  } else {
    gnostr_zap_goal_card_stop_deadline_timer(self);
  }
}

void gnostr_zap_goal_card_set_author(GnostrZapGoalCard *self,
                                      const gchar *pubkey_hex,
                                      const gchar *display_name,
                                      const gchar *lud16) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  g_clear_pointer(&self->pubkey, g_free);
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->lud16, g_free);

  self->pubkey = g_strdup(pubkey_hex);
  self->display_name = g_strdup(display_name);
  self->lud16 = g_strdup(lud16);

  /* Update name label */
  if (GTK_IS_LABEL(self->author_name_label)) {
    if (display_name && *display_name) {
      gtk_label_set_text(GTK_LABEL(self->author_name_label), display_name);
    } else if (pubkey_hex && strlen(pubkey_hex) >= 12) {
      /* Show truncated pubkey as fallback */
      gchar *truncated = g_strdup_printf("%.8s...%.4s",
                                          pubkey_hex,
                                          pubkey_hex + strlen(pubkey_hex) - 4);
      gtk_label_set_text(GTK_LABEL(self->author_name_label), truncated);
      g_free(truncated);
    } else {
      gtk_label_set_text(GTK_LABEL(self->author_name_label), _("Anonymous"));
    }
  }

  /* Update initials */
  set_avatar_initials(self, display_name);
  update_zap_button_state(self);
}

void gnostr_zap_goal_card_set_avatar(GnostrZapGoalCard *self,
                                      const gchar *avatar_url) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  if (!avatar_url || !*avatar_url) return;
  if (!GTK_IS_PICTURE(self->avatar_image)) return;

  /* Try cached avatar first */
  GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
  if (cached) {
    gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
    gtk_widget_set_visible(self->avatar_image, TRUE);
    gtk_widget_set_visible(self->avatar_initials, FALSE);
    g_object_unref(cached);
  } else {
    /* Async download */
    gnostr_avatar_download_async(avatar_url, self->avatar_image, self->avatar_initials);
  }
}

void gnostr_zap_goal_card_set_logged_in(GnostrZapGoalCard *self,
                                         gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  self->logged_in = logged_in;
  update_zap_button_state(self);
}

void gnostr_zap_goal_card_set_complete(GnostrZapGoalCard *self,
                                        gboolean is_complete) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  gboolean was_complete = self->is_complete;
  self->is_complete = is_complete;

  if (is_complete && !was_complete && !self->celebration_shown) {
    gnostr_zap_goal_card_trigger_celebration(self);
  }

  update_status_display(self);
}

void gnostr_zap_goal_card_set_expired(GnostrZapGoalCard *self,
                                       gboolean is_expired) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  self->is_expired = is_expired;
  update_status_display(self);
  update_zap_button_state(self);
}

gdouble gnostr_zap_goal_card_get_progress_percent(GnostrZapGoalCard *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self), 0.0);

  if (self->target_msats <= 0) return 0.0;
  return ((gdouble)self->current_msats / (gdouble)self->target_msats) * 100.0;
}

gboolean gnostr_zap_goal_card_is_complete(GnostrZapGoalCard *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self), FALSE);
  return self->is_complete;
}

gboolean gnostr_zap_goal_card_is_expired(GnostrZapGoalCard *self) {
  g_return_val_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self), FALSE);
  return self->is_expired;
}

void gnostr_zap_goal_card_trigger_celebration(GnostrZapGoalCard *self) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  if (self->celebration_shown) return;
  self->celebration_shown = TRUE;

  /* Show celebration overlay */
  if (GTK_IS_WIDGET(self->celebration_overlay)) {
    gtk_widget_set_visible(self->celebration_overlay, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(self), "celebrating");

    /* LEGITIMATE TIMEOUT - Hide celebration overlay after animation.
     * nostrc-b0h: Audited - animation timing is appropriate. */
    g_timeout_add(CELEBRATION_DURATION_MS, (GSourceFunc)on_celebration_finished, self);
  }
}

void gnostr_zap_goal_card_start_deadline_timer(GnostrZapGoalCard *self) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  if (self->deadline_timer_id > 0) return;
  if (self->end_time <= 0) return;

  /* LEGITIMATE TIMEOUT - Periodic deadline countdown update.
   * nostrc-b0h: Audited - UI countdown timer is appropriate. */
  self->deadline_timer_id = g_timeout_add(DEADLINE_TIMER_INTERVAL_MS,
                                           on_deadline_timer,
                                           self);
}

void gnostr_zap_goal_card_stop_deadline_timer(GnostrZapGoalCard *self) {
  g_return_if_fail(GNOSTR_IS_ZAP_GOAL_CARD(self));

  if (self->deadline_timer_id > 0) {
    g_source_remove(self->deadline_timer_id);
    self->deadline_timer_id = 0;
  }
}

/* ============== Internal Helpers ============== */

static void set_avatar_initials(GnostrZapGoalCard *self, const gchar *name) {
  if (!GTK_IS_LABEL(self->avatar_initials)) return;

  const gchar *src = (name && *name) ? name : "?";
  gchar initials[3] = {0};
  int i = 0;

  for (const gchar *p = src; *p && i < 2; p++) {
    if (g_ascii_isalnum(*p)) {
      initials[i++] = g_ascii_toupper(*p);
    }
  }
  if (i == 0) {
    initials[0] = '?';
  }

  gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
}

static void update_progress_display(GnostrZapGoalCard *self) {
  /* Update progress bar */
  if (GTK_IS_PROGRESS_BAR(self->progress_bar)) {
    gdouble fraction = 0.0;
    if (self->target_msats > 0) {
      fraction = (gdouble)self->current_msats / (gdouble)self->target_msats;
      if (fraction > 1.0) fraction = 1.0;  /* Cap visual at 100% */
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress_bar), fraction);
  }

  /* Update progress text (e.g., "50K / 100K sats") */
  if (GTK_IS_LABEL(self->progress_label)) {
    gchar *progress_str = gnostr_zap_goal_format_progress(self->current_msats,
                                                           self->target_msats);
    gtk_label_set_text(GTK_LABEL(self->progress_label), progress_str);
    g_free(progress_str);
  }

  /* Update percentage */
  if (GTK_IS_LABEL(self->percent_label)) {
    gdouble percent = gnostr_zap_goal_card_get_progress_percent(self);
    gchar *percent_str = g_strdup_printf("%.0f%%", percent);
    gtk_label_set_text(GTK_LABEL(self->percent_label), percent_str);
    g_free(percent_str);
  }

  /* Update zap count */
  if (GTK_IS_LABEL(self->zap_count_label)) {
    gchar *count_str = g_strdup_printf("%u zap%s",
                                        self->zap_count,
                                        self->zap_count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(self->zap_count_label), count_str);
    g_free(count_str);
  }
}

static void update_status_display(GnostrZapGoalCard *self) {
  if (!GTK_IS_WIDGET(self->status_badge)) return;

  gboolean show_badge = FALSE;
  const gchar *status_text = NULL;

  /* Remove old CSS classes */
  gtk_widget_remove_css_class(self->status_badge, "status-complete");
  gtk_widget_remove_css_class(self->status_badge, "status-expired");
  gtk_widget_remove_css_class(GTK_WIDGET(self), "goal-complete");
  gtk_widget_remove_css_class(GTK_WIDGET(self), "goal-expired");

  if (self->is_complete) {
    show_badge = TRUE;
    status_text = _("Goal Reached!");
    gtk_widget_add_css_class(self->status_badge, "status-complete");
    gtk_widget_add_css_class(GTK_WIDGET(self), "goal-complete");
  } else if (self->is_expired) {
    show_badge = TRUE;
    status_text = _("Ended");
    gtk_widget_add_css_class(self->status_badge, "status-expired");
    gtk_widget_add_css_class(GTK_WIDGET(self), "goal-expired");
  }

  gtk_widget_set_visible(self->status_badge, show_badge);
  if (show_badge && GTK_IS_LABEL(self->status_label)) {
    gtk_label_set_text(GTK_LABEL(self->status_label), status_text);
  }

  /* Update progress bar styling */
  if (GTK_IS_PROGRESS_BAR(self->progress_bar)) {
    gtk_widget_remove_css_class(self->progress_bar, "progress-complete");
    gtk_widget_remove_css_class(self->progress_bar, "progress-expired");

    if (self->is_complete) {
      gtk_widget_add_css_class(self->progress_bar, "progress-complete");
    } else if (self->is_expired) {
      gtk_widget_add_css_class(self->progress_bar, "progress-expired");
    }
  }

  update_zap_button_state(self);
}

static void update_deadline_display(GnostrZapGoalCard *self) {
  if (!GTK_IS_WIDGET(self->deadline_box)) return;

  if (self->end_time > 0) {
    gchar *remaining = gnostr_zap_goal_format_time_remaining(self->end_time);
    if (remaining) {
      if (g_strcmp0(remaining, "Ended") == 0) {
        self->is_expired = TRUE;
        update_status_display(self);
      }
      gtk_label_set_text(GTK_LABEL(self->deadline_label), remaining);
      g_free(remaining);
    }
    gtk_widget_set_visible(self->deadline_box, TRUE);
  } else {
    gtk_widget_set_visible(self->deadline_box, FALSE);
  }
}

static void update_zap_button_state(GnostrZapGoalCard *self) {
  if (!GTK_IS_WIDGET(self->zap_button)) return;

  gboolean can_zap = self->logged_in &&
                     self->lud16 != NULL &&
                     *self->lud16 != '\0' &&
                     !self->is_expired;

  gtk_widget_set_sensitive(self->zap_button, can_zap);

  /* Update button text based on state */
  if (GTK_IS_LABEL(self->zap_button_label)) {
    if (!self->logged_in) {
      gtk_label_set_text(GTK_LABEL(self->zap_button_label), _("Login to Zap"));
    } else if (!self->lud16 || !*self->lud16) {
      gtk_label_set_text(GTK_LABEL(self->zap_button_label), _("No Lightning Address"));
    } else if (self->is_expired) {
      gtk_label_set_text(GTK_LABEL(self->zap_button_label), _("Goal Ended"));
    } else if (self->is_complete) {
      gtk_label_set_text(GTK_LABEL(self->zap_button_label), _("Zap Anyway!"));
    } else {
      gtk_label_set_text(GTK_LABEL(self->zap_button_label), _("Zap this Goal"));
    }
  }
}

static gboolean on_deadline_timer(gpointer user_data) {
  GnostrZapGoalCard *self = GNOSTR_ZAP_GOAL_CARD(user_data);

  if (!GNOSTR_IS_ZAP_GOAL_CARD(self)) {
    return G_SOURCE_REMOVE;
  }

  update_deadline_display(self);

  /* Check if deadline passed */
  if (self->end_time > 0) {
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
    if (now >= self->end_time) {
      self->is_expired = TRUE;
      update_status_display(self);
      gnostr_zap_goal_card_stop_deadline_timer(self);
      return G_SOURCE_REMOVE;
    }
  }

  return G_SOURCE_CONTINUE;
}

static void on_celebration_finished(gpointer user_data) {
  GnostrZapGoalCard *self = GNOSTR_ZAP_GOAL_CARD(user_data);

  if (!GNOSTR_IS_ZAP_GOAL_CARD(self)) return;

  /* Hide celebration overlay */
  if (GTK_IS_WIDGET(self->celebration_overlay)) {
    gtk_widget_set_visible(self->celebration_overlay, FALSE);
  }
  gtk_widget_remove_css_class(GTK_WIDGET(self), "celebrating");
}
