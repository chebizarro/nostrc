/**
 * gnostr-picture-card.c - NIP-68 Picture Card Widget Implementation
 *
 * A GTK4 widget for displaying NIP-68 picture posts.
 */

#include "gnostr-picture-card.h"
#include "gnostr-avatar-cache.h"
#include "gnostr-image-viewer.h"
#include "../util/nip05.h"
#include "../util/utils.h"
#include <glib/gi18n.h>
#include <string.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Maximum caption length for display */
#define MAX_CAPTION_LENGTH 200

/* Thumbnail aspect ratio (square for grid) */
#define THUMBNAIL_SIZE 280

struct _GnostrPictureCard {
  GtkWidget parent_instance;

  /* Main layout widgets */
  GtkWidget *root_box;
  GtkWidget *card_frame;

  /* Image area */
  GtkWidget *image_overlay;
  GtkWidget *image_picture;
  GtkWidget *image_spinner;
  GtkWidget *content_warning_box;
  GtkWidget *content_warning_label;
  GtkWidget *content_warning_btn;
  GtkWidget *gallery_indicator;
  GtkWidget *gallery_count_label;

  /* Author row */
  GtkWidget *author_box;
  GtkWidget *avatar_btn;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *author_info_box;
  GtkWidget *author_name_btn;
  GtkWidget *author_name_label;
  GtkWidget *nip05_badge;
  GtkWidget *timestamp_label;

  /* Caption */
  GtkWidget *caption_label;

  /* Hashtags */
  GtkWidget *hashtags_box;

  /* Action buttons */
  GtkWidget *actions_box;
  GtkWidget *like_btn;
  GtkWidget *like_icon;
  GtkWidget *like_count_label;
  GtkWidget *zap_btn;
  GtkWidget *zap_count_label;
  GtkWidget *reply_btn;
  GtkWidget *reply_count_label;
  GtkWidget *repost_btn;
  GtkWidget *repost_icon;
  GtkWidget *repost_count_label;
  GtkWidget *share_btn;
  GtkWidget *menu_btn;

  /* Data */
  GnostrPictureMeta *picture;
  gchar *author_lud16;
  gboolean is_logged_in;
  gboolean is_compact;
  gboolean is_liked;
  gboolean is_reposted;
  gboolean content_revealed;

  /* Async operations */
  GCancellable *image_cancellable;
  GCancellable *nip05_cancellable;

#ifdef HAVE_SOUP3
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
#endif
};

G_DEFINE_TYPE(GnostrPictureCard, gnostr_picture_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_IMAGE_CLICKED,
  SIGNAL_AUTHOR_CLICKED,
  SIGNAL_LIKE_CLICKED,
  SIGNAL_ZAP_CLICKED,
  SIGNAL_REPLY_CLICKED,
  SIGNAL_REPOST_CLICKED,
  SIGNAL_SHARE_CLICKED,
  SIGNAL_HASHTAG_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_display(GnostrPictureCard *self);
static void load_image(GnostrPictureCard *self);
static void update_reaction_display(GnostrPictureCard *self);
static gchar *format_relative_time(gint64 timestamp);
static gchar *format_count(int count);

static void
gnostr_picture_card_dispose(GObject *object) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(object);

  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
  }

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  /* Shared session is managed globally - do not clear here */
#endif

  /* Unparent all children */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child) {
    gtk_widget_unparent(child);
  }

  G_OBJECT_CLASS(gnostr_picture_card_parent_class)->dispose(object);
}

static void
gnostr_picture_card_finalize(GObject *object) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(object);

  if (self->picture) {
    gnostr_picture_meta_free(self->picture);
    self->picture = NULL;
  }

  g_free(self->author_lud16);

  G_OBJECT_CLASS(gnostr_picture_card_parent_class)->finalize(object);
}

/* Click handlers */
static void
on_image_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                 gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;

  /* If content warning is active and not revealed, reveal it */
  if (gnostr_picture_has_content_warning(self->picture) && !self->content_revealed) {
    gnostr_picture_card_reveal_content(self);
    return;
  }

  g_signal_emit(self, signals[SIGNAL_IMAGE_CLICKED], 0);
}

static void
on_author_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)btn;

  if (self->picture && self->picture->pubkey) {
    g_signal_emit(self, signals[SIGNAL_AUTHOR_CLICKED], 0, self->picture->pubkey);
  }
}

static void
on_like_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)btn;

  g_signal_emit(self, signals[SIGNAL_LIKE_CLICKED], 0);
}

static void
on_zap_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)btn;

  g_signal_emit(self, signals[SIGNAL_ZAP_CLICKED], 0);
}

static void
on_reply_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)btn;

  g_signal_emit(self, signals[SIGNAL_REPLY_CLICKED], 0);
}

static void
on_repost_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)btn;

  g_signal_emit(self, signals[SIGNAL_REPOST_CLICKED], 0);
}

static void
on_share_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)btn;

  g_signal_emit(self, signals[SIGNAL_SHARE_CLICKED], 0);
}

static void
on_reveal_content_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  (void)btn;

  gnostr_picture_card_reveal_content(self);
}

static void
on_hashtag_clicked(GtkButton *btn, gpointer user_data) {
  GnostrPictureCard *self = GNOSTR_PICTURE_CARD(user_data);
  const char *tag = g_object_get_data(G_OBJECT(btn), "hashtag");

  if (tag) {
    g_signal_emit(self, signals[SIGNAL_HASHTAG_CLICKED], 0, tag);
  }
}

static void
gnostr_picture_card_class_init(GnostrPictureCardClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_picture_card_dispose;
  object_class->finalize = gnostr_picture_card_finalize;

  /* CSS name */
  gtk_widget_class_set_css_name(widget_class, "picture-card");

  /* Layout manager */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

  /* Signals */
  signals[SIGNAL_IMAGE_CLICKED] = g_signal_new(
    "image-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_AUTHOR_CLICKED] = g_signal_new(
    "author-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_LIKE_CLICKED] = g_signal_new(
    "like-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_ZAP_CLICKED] = g_signal_new(
    "zap-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_REPLY_CLICKED] = g_signal_new(
    "reply-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_REPOST_CLICKED] = g_signal_new(
    "repost-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_SHARE_CLICKED] = g_signal_new(
    "share-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_HASHTAG_CLICKED] = g_signal_new(
    "hashtag-clicked",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_picture_card_init(GnostrPictureCard *self) {
  self->picture = NULL;
  self->author_lud16 = NULL;
  self->is_logged_in = FALSE;
  self->is_compact = FALSE;
  self->is_liked = FALSE;
  self->is_reposted = FALSE;
  self->content_revealed = FALSE;

#ifdef HAVE_SOUP3
  /* Uses shared session from gnostr_get_shared_soup_session() */
#endif

  self->image_cancellable = g_cancellable_new();

  /* Build UI programmatically */

  /* Root box */
  self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));
  gtk_widget_add_css_class(self->root_box, "picture-card-root");

  /* Card frame */
  self->card_frame = gtk_frame_new(NULL);
  gtk_widget_add_css_class(self->card_frame, "picture-card-frame");
  gtk_box_append(GTK_BOX(self->root_box), self->card_frame);

  GtkWidget *card_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_frame_set_child(GTK_FRAME(self->card_frame), card_content);

  /* Image overlay */
  self->image_overlay = gtk_overlay_new();
  gtk_widget_add_css_class(self->image_overlay, "picture-image-overlay");
  gtk_box_append(GTK_BOX(card_content), self->image_overlay);

  /* Main image */
  self->image_picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(self->image_picture), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_size_request(self->image_picture, THUMBNAIL_SIZE, THUMBNAIL_SIZE);
  gtk_widget_add_css_class(self->image_picture, "picture-main-image");
  gtk_overlay_set_child(GTK_OVERLAY(self->image_overlay), self->image_picture);

  /* Image click gesture */
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_image_clicked), self);
  gtk_widget_add_controller(self->image_overlay, GTK_EVENT_CONTROLLER(click));

  /* Loading spinner */
  self->image_spinner = gtk_spinner_new();
  gtk_widget_set_halign(self->image_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->image_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(self->image_spinner, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->image_overlay), self->image_spinner);

  /* Content warning overlay */
  self->content_warning_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign(self->content_warning_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->content_warning_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(self->content_warning_box, "picture-content-warning");
  gtk_widget_set_visible(self->content_warning_box, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->image_overlay), self->content_warning_box);

  GtkWidget *cw_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(cw_icon), 48);
  gtk_box_append(GTK_BOX(self->content_warning_box), cw_icon);

  self->content_warning_label = gtk_label_new(_("Content Warning"));
  gtk_widget_add_css_class(self->content_warning_label, "picture-cw-label");
  gtk_box_append(GTK_BOX(self->content_warning_box), self->content_warning_label);

  self->content_warning_btn = gtk_button_new_with_label(_("Show Content"));
  gtk_widget_add_css_class(self->content_warning_btn, "picture-cw-button");
  g_signal_connect(self->content_warning_btn, "clicked",
                   G_CALLBACK(on_reveal_content_clicked), self);
  gtk_box_append(GTK_BOX(self->content_warning_box), self->content_warning_btn);

  /* Gallery indicator (top-right overlay) */
  self->gallery_indicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign(self->gallery_indicator, GTK_ALIGN_END);
  gtk_widget_set_valign(self->gallery_indicator, GTK_ALIGN_START);
  gtk_widget_set_margin_end(self->gallery_indicator, 8);
  gtk_widget_set_margin_top(self->gallery_indicator, 8);
  gtk_widget_add_css_class(self->gallery_indicator, "picture-gallery-indicator");
  gtk_widget_set_visible(self->gallery_indicator, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->image_overlay), self->gallery_indicator);

  GtkWidget *gallery_icon = gtk_image_new_from_icon_name("view-grid-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(gallery_icon), 12);
  gtk_box_append(GTK_BOX(self->gallery_indicator), gallery_icon);

  self->gallery_count_label = gtk_label_new("1");
  gtk_box_append(GTK_BOX(self->gallery_indicator), self->gallery_count_label);

  /* Content area */
  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(content_box, 12);
  gtk_widget_set_margin_end(content_box, 12);
  gtk_widget_set_margin_top(content_box, 10);
  gtk_widget_set_margin_bottom(content_box, 10);
  gtk_box_append(GTK_BOX(card_content), content_box);

  /* Author row */
  self->author_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(content_box), self->author_box);

  /* Avatar button */
  self->avatar_btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->avatar_btn), FALSE);
  gtk_widget_add_css_class(self->avatar_btn, "flat");
  g_signal_connect(self->avatar_btn, "clicked", G_CALLBACK(on_author_clicked), self);
  gtk_box_append(GTK_BOX(self->author_box), self->avatar_btn);

  self->avatar_overlay = gtk_overlay_new();
  gtk_widget_set_size_request(self->avatar_overlay, 32, 32);
  gtk_widget_add_css_class(self->avatar_overlay, "avatar");
  gtk_button_set_child(GTK_BUTTON(self->avatar_btn), self->avatar_overlay);

  self->avatar_image = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(self->avatar_image), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_size_request(self->avatar_image, 32, 32);
  gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(self->avatar_overlay), self->avatar_image);

  self->avatar_initials = gtk_label_new("AN");
  gtk_widget_add_css_class(self->avatar_initials, "avatar-initials");
  gtk_overlay_add_overlay(GTK_OVERLAY(self->avatar_overlay), self->avatar_initials);

  /* Author info */
  self->author_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(self->author_info_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(self->author_info_box, TRUE);
  gtk_box_append(GTK_BOX(self->author_box), self->author_info_box);

  /* Name row with NIP-05 badge */
  GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append(GTK_BOX(self->author_info_box), name_row);

  self->author_name_btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->author_name_btn), FALSE);
  gtk_widget_add_css_class(self->author_name_btn, "flat");
  g_signal_connect(self->author_name_btn, "clicked", G_CALLBACK(on_author_clicked), self);
  gtk_box_append(GTK_BOX(name_row), self->author_name_btn);

  self->author_name_label = gtk_label_new(_("Anonymous"));
  gtk_label_set_xalign(GTK_LABEL(self->author_name_label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->author_name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->author_name_label, "picture-author-name");
  gtk_button_set_child(GTK_BUTTON(self->author_name_btn), self->author_name_label);

  self->nip05_badge = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->nip05_badge), 14);
  gtk_widget_add_css_class(self->nip05_badge, "nip05-verified-badge");
  gtk_widget_set_visible(self->nip05_badge, FALSE);
  gtk_box_append(GTK_BOX(name_row), self->nip05_badge);

  /* Timestamp */
  self->timestamp_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->timestamp_label), 0.0);
  gtk_widget_add_css_class(self->timestamp_label, "picture-timestamp");
  gtk_widget_add_css_class(self->timestamp_label, "dim-label");
  gtk_box_append(GTK_BOX(self->author_info_box), self->timestamp_label);

  /* Menu button */
  self->menu_btn = gtk_button_new_from_icon_name("open-menu-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->menu_btn), FALSE);
  gtk_widget_add_css_class(self->menu_btn, "flat");
  gtk_widget_set_tooltip_text(self->menu_btn, _("More options"));
  gtk_box_append(GTK_BOX(self->author_box), self->menu_btn);

  /* Caption */
  self->caption_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->caption_label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(self->caption_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->caption_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_lines(GTK_LABEL(self->caption_label), 3);
  gtk_label_set_ellipsize(GTK_LABEL(self->caption_label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(self->caption_label, "picture-caption");
  gtk_widget_set_visible(self->caption_label, FALSE);
  gtk_box_append(GTK_BOX(content_box), self->caption_label);

  /* Hashtags */
  self->hashtags_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->hashtags_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->hashtags_box), 6);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->hashtags_box), 4);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->hashtags_box), 6);
  gtk_widget_add_css_class(self->hashtags_box, "picture-hashtags");
  gtk_widget_set_visible(self->hashtags_box, FALSE);
  gtk_box_append(GTK_BOX(content_box), self->hashtags_box);

  /* Action buttons */
  self->actions_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_top(self->actions_box, 4);
  gtk_box_append(GTK_BOX(content_box), self->actions_box);

  /* Like button */
  self->like_btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->like_btn), FALSE);
  gtk_widget_add_css_class(self->like_btn, "flat");
  gtk_widget_set_tooltip_text(self->like_btn, _("Like"));
  g_signal_connect(self->like_btn, "clicked", G_CALLBACK(on_like_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->like_btn);

  GtkWidget *like_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_button_set_child(GTK_BUTTON(self->like_btn), like_box);

  self->like_icon = gtk_image_new_from_icon_name("emblem-favorite-symbolic");
  gtk_box_append(GTK_BOX(like_box), self->like_icon);

  self->like_count_label = gtk_label_new("");
  gtk_widget_add_css_class(self->like_count_label, "reaction-count");
  gtk_box_append(GTK_BOX(like_box), self->like_count_label);

  /* Zap button */
  self->zap_btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->zap_btn), FALSE);
  gtk_widget_add_css_class(self->zap_btn, "flat");
  gtk_widget_set_tooltip_text(self->zap_btn, _("Zap"));
  g_signal_connect(self->zap_btn, "clicked", G_CALLBACK(on_zap_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->zap_btn);

  GtkWidget *zap_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_button_set_child(GTK_BUTTON(self->zap_btn), zap_box);

  GtkWidget *zap_icon = gtk_image_new_from_icon_name("camera-flash-symbolic");
  gtk_box_append(GTK_BOX(zap_box), zap_icon);

  self->zap_count_label = gtk_label_new("");
  gtk_widget_add_css_class(self->zap_count_label, "reaction-count");
  gtk_box_append(GTK_BOX(zap_box), self->zap_count_label);

  /* Reply button */
  self->reply_btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->reply_btn), FALSE);
  gtk_widget_add_css_class(self->reply_btn, "flat");
  gtk_widget_set_tooltip_text(self->reply_btn, _("Reply"));
  g_signal_connect(self->reply_btn, "clicked", G_CALLBACK(on_reply_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->reply_btn);

  GtkWidget *reply_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_button_set_child(GTK_BUTTON(self->reply_btn), reply_box);

  GtkWidget *reply_icon = gtk_image_new_from_icon_name("mail-reply-sender-symbolic");
  gtk_box_append(GTK_BOX(reply_box), reply_icon);

  self->reply_count_label = gtk_label_new("");
  gtk_widget_add_css_class(self->reply_count_label, "reaction-count");
  gtk_box_append(GTK_BOX(reply_box), self->reply_count_label);

  /* Repost button */
  self->repost_btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->repost_btn), FALSE);
  gtk_widget_add_css_class(self->repost_btn, "flat");
  gtk_widget_set_tooltip_text(self->repost_btn, _("Repost"));
  g_signal_connect(self->repost_btn, "clicked", G_CALLBACK(on_repost_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->repost_btn);

  GtkWidget *repost_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_button_set_child(GTK_BUTTON(self->repost_btn), repost_box);

  self->repost_icon = gtk_image_new_from_icon_name("media-playlist-repeat-symbolic");
  gtk_box_append(GTK_BOX(repost_box), self->repost_icon);

  self->repost_count_label = gtk_label_new("");
  gtk_widget_add_css_class(self->repost_count_label, "reaction-count");
  gtk_box_append(GTK_BOX(repost_box), self->repost_count_label);

  /* Spacer */
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(self->actions_box), spacer);

  /* Share button */
  self->share_btn = gtk_button_new_from_icon_name("emblem-shared-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->share_btn), FALSE);
  gtk_widget_add_css_class(self->share_btn, "flat");
  gtk_widget_set_tooltip_text(self->share_btn, _("Share"));
  g_signal_connect(self->share_btn, "clicked", G_CALLBACK(on_share_clicked), self);
  gtk_box_append(GTK_BOX(self->actions_box), self->share_btn);
}

GnostrPictureCard *
gnostr_picture_card_new(void) {
  return g_object_new(GNOSTR_TYPE_PICTURE_CARD, NULL);
}

void
gnostr_picture_card_set_picture(GnostrPictureCard *self,
                                 const GnostrPictureMeta *meta) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  /* Free previous data */
  if (self->picture) {
    gnostr_picture_meta_free(self->picture);
    self->picture = NULL;
  }

  /* Cancel pending image load */
  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
    self->image_cancellable = g_cancellable_new();
  }

  /* Copy new data */
  if (meta) {
    self->picture = gnostr_picture_meta_copy(meta);
  }

  self->content_revealed = FALSE;

  /* Update display */
  update_display(self);
}

const GnostrPictureMeta *
gnostr_picture_card_get_picture(GnostrPictureCard *self) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_CARD(self), NULL);
  return self->picture;
}

void
gnostr_picture_card_set_author(GnostrPictureCard *self,
                                const char *display_name,
                                const char *handle,
                                const char *avatar_url,
                                const char *nip05) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  /* Set author name */
  const char *name = (display_name && *display_name) ? display_name :
                     (handle && *handle) ? handle : _("Anonymous");
  gtk_label_set_text(GTK_LABEL(self->author_name_label), name);

  /* Set avatar initials */
  char initials[3] = {0};
  int i = 0;
  for (const char *p = name; *p && i < 2; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isalnum(c)) {
      initials[i++] = g_ascii_toupper(c);
    }
  }
  if (i == 0) {
    initials[0] = 'A';
    initials[1] = 'N';
  }
  gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
  gtk_widget_set_visible(self->avatar_image, FALSE);

  /* Load avatar image if URL provided */
#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->avatar_image, TRUE);
      gtk_widget_set_visible(self->avatar_initials, FALSE);
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->avatar_image, self->avatar_initials);
    }
  }
#else
  (void)avatar_url;
#endif

  /* NIP-05 verification */
  if (nip05 && *nip05 && self->picture && self->picture->pubkey) {
    if (self->nip05_cancellable) {
      g_cancellable_cancel(self->nip05_cancellable);
      g_clear_object(&self->nip05_cancellable);
    }
    self->nip05_cancellable = g_cancellable_new();

    /* Start async verification - callback would update badge visibility */
    gtk_widget_set_visible(self->nip05_badge, FALSE);
    gtk_widget_set_tooltip_text(self->nip05_badge, nip05);
    /* Note: actual verification would be done by the caller
     * and they would show/hide the badge */
  } else {
    gtk_widget_set_visible(self->nip05_badge, FALSE);
  }
}

void
gnostr_picture_card_set_author_lud16(GnostrPictureCard *self,
                                      const char *lud16) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  g_free(self->author_lud16);
  self->author_lud16 = g_strdup(lud16);

  /* Enable/disable zap button */
  gtk_widget_set_sensitive(self->zap_btn, lud16 && *lud16 && self->is_logged_in);
}

void
gnostr_picture_card_set_reaction_counts(GnostrPictureCard *self,
                                         int likes,
                                         int zaps,
                                         gint64 zap_sats,
                                         int reposts,
                                         int replies) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  if (self->picture) {
    self->picture->like_count = likes;
    self->picture->zap_count = zaps;
    self->picture->zap_amount = zap_sats;
    self->picture->repost_count = reposts;
    self->picture->reply_count = replies;
  }

  update_reaction_display(self);
}

void
gnostr_picture_card_set_user_reaction(GnostrPictureCard *self,
                                       gboolean liked,
                                       gboolean reposted) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  self->is_liked = liked;
  self->is_reposted = reposted;

  /* Update like button appearance */
  if (liked) {
    gtk_widget_add_css_class(self->like_btn, "liked");
    gtk_image_set_from_icon_name(GTK_IMAGE(self->like_icon), "emblem-favorite-symbolic");
  } else {
    gtk_widget_remove_css_class(self->like_btn, "liked");
    gtk_image_set_from_icon_name(GTK_IMAGE(self->like_icon), "emblem-favorite-symbolic");
  }

  /* Update repost button appearance */
  if (reposted) {
    gtk_widget_add_css_class(self->repost_btn, "reposted");
  } else {
    gtk_widget_remove_css_class(self->repost_btn, "reposted");
  }
}

void
gnostr_picture_card_set_logged_in(GnostrPictureCard *self,
                                   gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  self->is_logged_in = logged_in;

  /* Update button sensitivity */
  gtk_widget_set_sensitive(self->like_btn, logged_in);
  gtk_widget_set_sensitive(self->zap_btn, logged_in && self->author_lud16 && *self->author_lud16);
  gtk_widget_set_sensitive(self->reply_btn, logged_in);
  gtk_widget_set_sensitive(self->repost_btn, logged_in);
}

void
gnostr_picture_card_set_loading(GnostrPictureCard *self,
                                 gboolean loading) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  gtk_widget_set_visible(self->image_spinner, loading);
  if (loading) {
    gtk_spinner_start(GTK_SPINNER(self->image_spinner));
  } else {
    gtk_spinner_stop(GTK_SPINNER(self->image_spinner));
  }
}

void
gnostr_picture_card_reveal_content(GnostrPictureCard *self) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  self->content_revealed = TRUE;
  gtk_widget_set_visible(self->content_warning_box, FALSE);
  gtk_widget_set_visible(self->image_picture, TRUE);

  /* Load the actual image now */
  load_image(self);
}

void
gnostr_picture_card_set_compact(GnostrPictureCard *self,
                                 gboolean compact) {
  g_return_if_fail(GNOSTR_IS_PICTURE_CARD(self));

  if (self->is_compact == compact) return;

  self->is_compact = compact;

  if (compact) {
    gtk_widget_add_css_class(GTK_WIDGET(self), "compact");
    gtk_widget_set_visible(self->caption_label, FALSE);
    gtk_widget_set_visible(self->hashtags_box, FALSE);
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self), "compact");
    update_display(self);
  }
}

const char *
gnostr_picture_card_get_event_id(GnostrPictureCard *self) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_CARD(self), NULL);
  return self->picture ? self->picture->event_id : NULL;
}

const char *
gnostr_picture_card_get_pubkey(GnostrPictureCard *self) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_CARD(self), NULL);
  return self->picture ? self->picture->pubkey : NULL;
}

char **
gnostr_picture_card_get_image_urls(GnostrPictureCard *self,
                                    size_t *count) {
  g_return_val_if_fail(GNOSTR_IS_PICTURE_CARD(self), NULL);
  return gnostr_picture_get_all_image_urls(self->picture, count);
}

/* Internal: Update the entire display */
static void
update_display(GnostrPictureCard *self) {
  if (!self->picture) {
    gtk_widget_set_visible(self->image_picture, FALSE);
    gtk_widget_set_visible(self->caption_label, FALSE);
    gtk_widget_set_visible(self->hashtags_box, FALSE);
    return;
  }

  /* Check for content warning */
  if (gnostr_picture_has_content_warning(self->picture) && !self->content_revealed) {
    /* Show content warning overlay instead of image */
    gtk_widget_set_visible(self->image_picture, FALSE);
    gtk_widget_set_visible(self->content_warning_box, TRUE);

    const char *cw = self->picture->content_warning;
    gtk_label_set_text(GTK_LABEL(self->content_warning_label), cw);
  } else {
    gtk_widget_set_visible(self->content_warning_box, FALSE);
    gtk_widget_set_visible(self->image_picture, TRUE);
    load_image(self);
  }

  /* Gallery indicator */
  if (self->picture->image_count > 1) {
    gchar *count_str = g_strdup_printf("%zu", self->picture->image_count);
    gtk_label_set_text(GTK_LABEL(self->gallery_count_label), count_str);
    gtk_widget_set_visible(self->gallery_indicator, TRUE);
    g_free(count_str);
  } else {
    gtk_widget_set_visible(self->gallery_indicator, FALSE);
  }

  /* Caption */
  if (self->picture->caption && *self->picture->caption && !self->is_compact) {
    gchar *formatted = gnostr_picture_format_caption(self->picture->caption, MAX_CAPTION_LENGTH);
    gtk_label_set_text(GTK_LABEL(self->caption_label), formatted);
    gtk_widget_set_visible(self->caption_label, TRUE);
    g_free(formatted);
  } else {
    gtk_widget_set_visible(self->caption_label, FALSE);
  }

  /* Timestamp */
  if (self->picture->created_at > 0) {
    gchar *time_str = format_relative_time(self->picture->created_at);
    gtk_label_set_text(GTK_LABEL(self->timestamp_label), time_str);
    g_free(time_str);
  }

  /* Hashtags */
  /* First clear existing hashtags */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->hashtags_box)) != NULL) {
    gtk_flow_box_remove(GTK_FLOW_BOX(self->hashtags_box), child);
  }

  if (self->picture->hashtags && self->picture->hashtag_count > 0 && !self->is_compact) {
    for (size_t i = 0; i < self->picture->hashtag_count && i < 5; i++) {
      gchar *tag_text = g_strdup_printf("#%s", self->picture->hashtags[i]);
      GtkWidget *tag_btn = gtk_button_new_with_label(tag_text);
      gtk_button_set_has_frame(GTK_BUTTON(tag_btn), FALSE);
      gtk_widget_add_css_class(tag_btn, "flat");
      gtk_widget_add_css_class(tag_btn, "picture-hashtag");

      g_object_set_data_full(G_OBJECT(tag_btn), "hashtag",
                             g_strdup(self->picture->hashtags[i]), g_free);
      g_signal_connect(tag_btn, "clicked", G_CALLBACK(on_hashtag_clicked), self);

      gtk_flow_box_insert(GTK_FLOW_BOX(self->hashtags_box), tag_btn, -1);
      g_free(tag_text);
    }
    gtk_widget_set_visible(self->hashtags_box, TRUE);
  } else {
    gtk_widget_set_visible(self->hashtags_box, FALSE);
  }

  /* Update reaction counts */
  update_reaction_display(self);
}

/* Internal: Load the primary image */
static void
load_image(GnostrPictureCard *self) {
  const char *url = gnostr_picture_get_thumbnail_url(self->picture);
  if (!url || !*url) {
    gtk_widget_set_visible(self->image_picture, FALSE);
    return;
  }

  /* Use GFile to load from URI */
  GFile *file = g_file_new_for_uri(url);
  gtk_picture_set_file(GTK_PICTURE(self->image_picture), file);
  gtk_widget_set_visible(self->image_picture, TRUE);
  g_object_unref(file);
}

/* Internal: Update reaction count display */
static void
update_reaction_display(GnostrPictureCard *self) {
  if (!self->picture) return;

  /* Likes */
  if (self->picture->like_count > 0) {
    gchar *count = format_count(self->picture->like_count);
    gtk_label_set_text(GTK_LABEL(self->like_count_label), count);
    gtk_widget_set_visible(self->like_count_label, TRUE);
    g_free(count);
  } else {
    gtk_widget_set_visible(self->like_count_label, FALSE);
  }

  /* Zaps */
  if (self->picture->zap_count > 0) {
    gchar *count;
    if (self->picture->zap_amount >= 1000000) {
      count = g_strdup_printf("%.1fM", self->picture->zap_amount / 1000000.0);
    } else if (self->picture->zap_amount >= 1000) {
      count = g_strdup_printf("%.1fk", self->picture->zap_amount / 1000.0);
    } else {
      count = g_strdup_printf("%d", (int)self->picture->zap_amount);
    }
    gtk_label_set_text(GTK_LABEL(self->zap_count_label), count);
    gtk_widget_set_visible(self->zap_count_label, TRUE);
    g_free(count);
  } else {
    gtk_widget_set_visible(self->zap_count_label, FALSE);
  }

  /* Replies */
  if (self->picture->reply_count > 0) {
    gchar *count = format_count(self->picture->reply_count);
    gtk_label_set_text(GTK_LABEL(self->reply_count_label), count);
    gtk_widget_set_visible(self->reply_count_label, TRUE);
    g_free(count);
  } else {
    gtk_widget_set_visible(self->reply_count_label, FALSE);
  }

  /* Reposts */
  if (self->picture->repost_count > 0) {
    gchar *count = format_count(self->picture->repost_count);
    gtk_label_set_text(GTK_LABEL(self->repost_count_label), count);
    gtk_widget_set_visible(self->repost_count_label, TRUE);
    g_free(count);
  } else {
    gtk_widget_set_visible(self->repost_count_label, FALSE);
  }
}

/* Helper: Format relative time */
static gchar *
format_relative_time(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup("");

  GDateTime *then = g_date_time_new_from_unix_local(timestamp);
  if (!then) return g_strdup("");

  GDateTime *now = g_date_time_new_now_local();
  GTimeSpan diff = g_date_time_difference(now, then);
  g_date_time_unref(now);
  g_date_time_unref(then);

  gint64 seconds = diff / G_TIME_SPAN_SECOND;

  if (seconds < 60) {
    return g_strdup(_("now"));
  } else if (seconds < 3600) {
    gint minutes = (gint)(seconds / 60);
    return g_strdup_printf("%dm", minutes);
  } else if (seconds < 86400) {
    gint hours = (gint)(seconds / 3600);
    return g_strdup_printf("%dh", hours);
  } else if (seconds < 604800) {
    gint days = (gint)(seconds / 86400);
    return g_strdup_printf("%dd", days);
  } else {
    gint weeks = (gint)(seconds / 604800);
    return g_strdup_printf("%dw", weeks);
  }
}

/* Helper: Format count with K/M suffix */
static gchar *
format_count(int count) {
  if (count >= 1000000) {
    return g_strdup_printf("%.1fM", count / 1000000.0);
  } else if (count >= 1000) {
    return g_strdup_printf("%.1fk", count / 1000.0);
  } else {
    return g_strdup_printf("%d", count);
  }
}
