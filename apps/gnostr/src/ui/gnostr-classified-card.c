/*
 * gnostr-classified-card.c - NIP-99 Classified Listing Card Widget
 *
 * Displays a single kind 30402 classified listing.
 */

#include "gnostr-classified-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip05.h"
#include "../util/markdown_pango.h"
#include <glib/gi18n.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

struct _GnostrClassifiedCard {
  GtkWidget parent_instance;

  /* Main layout widgets */
  GtkWidget *root_box;
  GtkWidget *image_overlay;
  GtkWidget *image_stack;
  GtkWidget *image_placeholder;
  GtkWidget *btn_prev_image;
  GtkWidget *btn_next_image;
  GtkWidget *image_dots_box;

  /* Content widgets */
  GtkWidget *content_box;
  GtkWidget *price_label;
  GtkWidget *title_button;
  GtkWidget *title_label;
  GtkWidget *summary_label;
  GtkWidget *location_box;
  GtkWidget *location_icon;
  GtkWidget *location_label;
  GtkWidget *categories_box;
  GtkWidget *published_label;

  /* Seller info widgets */
  GtkWidget *seller_box;
  GtkWidget *btn_seller;
  GtkWidget *seller_avatar;
  GtkWidget *seller_initials;
  GtkWidget *seller_name_label;
  GtkWidget *nip05_badge;

  /* Action buttons */
  GtkWidget *btn_contact;
  GtkWidget *btn_share;
  GtkWidget *btn_details;

  /* State */
  gchar *event_id;
  gchar *d_tag;
  gchar *pubkey_hex;
  gchar *seller_lud16;
  gchar *nip05;
  gint64 published_at;
  gboolean is_logged_in;
  gboolean is_compact;

  /* Images */
  GPtrArray *images;        /* Array of gchar* URLs */
  GPtrArray *image_widgets; /* Array of GtkPicture* */
  guint current_image_index;

#ifdef HAVE_SOUP3
  GCancellable *image_cancellable;
  SoupSession *session;
#endif

  GCancellable *nip05_cancellable;
};

G_DEFINE_TYPE(GnostrClassifiedCard, gnostr_classified_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CONTACT_SELLER,
  SIGNAL_VIEW_DETAILS,
  SIGNAL_IMAGE_CLICKED,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_CATEGORY_CLICKED,
  SIGNAL_SHARE_LISTING,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void update_image_carousel(GnostrClassifiedCard *self);
static void update_image_dots(GnostrClassifiedCard *self);

/* ============== Disposal ============== */

static void
gnostr_classified_card_dispose(GObject *obj)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(obj);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
  }
  g_clear_object(&self->session);
#endif

  /* Unparent root */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  if (child)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(gnostr_classified_card_parent_class)->dispose(obj);
}

static void
gnostr_classified_card_finalize(GObject *obj)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(obj);

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->seller_lud16, g_free);
  g_clear_pointer(&self->nip05, g_free);

  if (self->images) g_ptr_array_unref(self->images);
  if (self->image_widgets) g_ptr_array_unref(self->image_widgets);

  G_OBJECT_CLASS(gnostr_classified_card_parent_class)->finalize(obj);
}

/* ============== Helper Functions ============== */

static gchar *
format_publish_date(gint64 published_at)
{
  if (published_at <= 0) return g_strdup(_("Recently"));

  GDateTime *dt = g_date_time_new_from_unix_local(published_at);
  if (!dt) return g_strdup(_("Recently"));

  GDateTime *now = g_date_time_new_now_local();
  GTimeSpan diff = g_date_time_difference(now, dt);
  g_date_time_unref(now);

  gchar *result;
  gint64 seconds = diff / G_TIME_SPAN_SECOND;

  if (seconds < 60) {
    result = g_strdup(_("Just now"));
  } else if (seconds < 3600) {
    gint minutes = (gint)(seconds / 60);
    result = g_strdup_printf(g_dngettext(NULL, "%d minute ago", "%d minutes ago", minutes), minutes);
  } else if (seconds < 86400) {
    gint hours = (gint)(seconds / 3600);
    result = g_strdup_printf(g_dngettext(NULL, "%d hour ago", "%d hours ago", hours), hours);
  } else if (seconds < 604800) {
    gint days = (gint)(seconds / 86400);
    result = g_strdup_printf(g_dngettext(NULL, "%d day ago", "%d days ago", days), days);
  } else {
    result = g_date_time_format(dt, "%B %d, %Y");
  }

  g_date_time_unref(dt);
  return result;
}

static void
set_seller_initials(GnostrClassifiedCard *self, const char *display_name)
{
  if (!GTK_IS_LABEL(self->seller_initials)) return;

  const char *src = (display_name && *display_name) ? display_name : "AN";
  char initials[3] = {0};
  int i = 0;

  for (const char *p = src; *p && i < 2; p++) {
    if (g_ascii_isalnum(*p)) {
      initials[i++] = g_ascii_toupper(*p);
    }
  }
  if (i == 0) {
    initials[0] = 'A';
    initials[1] = 'N';
  }

  gtk_label_set_text(GTK_LABEL(self->seller_initials), initials);
  if (self->seller_avatar) gtk_widget_set_visible(self->seller_avatar, FALSE);
  gtk_widget_set_visible(self->seller_initials, TRUE);
}

/* ============== Click Handlers ============== */

static void
on_seller_clicked(GtkButton *btn, gpointer user_data)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void
on_title_clicked(GtkButton *btn, gpointer user_data)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);
  (void)btn;
  if (self->event_id && *self->event_id) {
    gchar *naddr = NULL;
    if (self->pubkey_hex && self->d_tag) {
      naddr = g_strdup_printf("%d:%s:%s", NIP99_KIND_CLASSIFIED_LISTING,
                              self->pubkey_hex, self->d_tag);
    }
    g_signal_emit(self, signals[SIGNAL_VIEW_DETAILS], 0, self->event_id, naddr);
    g_free(naddr);
  }
}

static void
on_contact_clicked(GtkButton *btn, gpointer user_data)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_CONTACT_SELLER], 0,
                  self->pubkey_hex, self->seller_lud16);
  }
}

static void
on_share_clicked(GtkButton *btn, gpointer user_data)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);
  (void)btn;

  if (!self->pubkey_hex || !self->d_tag) return;

  /* Build naddr URI */
  gchar *naddr = g_strdup_printf("%d:%s:%s", NIP99_KIND_CLASSIFIED_LISTING,
                                  self->pubkey_hex, self->d_tag);
  gchar *uri = g_strdup_printf("nostr:naddr1%s", naddr); /* Simplified - should use proper bech32 */
  g_signal_emit(self, signals[SIGNAL_SHARE_LISTING], 0, uri);
  g_free(uri);
  g_free(naddr);
}

static void
on_details_clicked(GtkButton *btn, gpointer user_data)
{
  on_title_clicked(GTK_BUTTON(btn), user_data);
}

static void
on_prev_image_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);
  gnostr_classified_card_prev_image(self);
}

static void
on_next_image_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);
  gnostr_classified_card_next_image(self);
}

static void
on_image_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                 gpointer user_data)
{
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);

  if (self->images && self->current_image_index < self->images->len) {
    const gchar *url = g_ptr_array_index(self->images, self->current_image_index);
    g_signal_emit(self, signals[SIGNAL_IMAGE_CLICKED], 0, url, self->current_image_index);
  }
}

static void
on_category_clicked(GtkButton *btn, gpointer user_data)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);
  const gchar *category = gtk_button_get_label(btn);
  if (category && *category) {
    /* Remove '#' prefix if present */
    if (category[0] == '#') category++;
    g_signal_emit(self, signals[SIGNAL_CATEGORY_CLICKED], 0, category);
  }
}

/* ============== Widget Construction ============== */

static GtkWidget *
create_image_carousel(GnostrClassifiedCard *self)
{
  /* Overlay for image + navigation buttons */
  self->image_overlay = gtk_overlay_new();
  gtk_widget_add_css_class(self->image_overlay, "classified-image-overlay");

  /* Stack for images */
  self->image_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->image_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
  gtk_stack_set_transition_duration(GTK_STACK(self->image_stack), 200);
  gtk_widget_set_size_request(self->image_stack, -1, 200);
  gtk_overlay_set_child(GTK_OVERLAY(self->image_overlay), self->image_stack);

  /* Placeholder image */
  self->image_placeholder = gtk_image_new_from_icon_name("image-x-generic-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->image_placeholder), 64);
  gtk_widget_add_css_class(self->image_placeholder, "dim-label");
  gtk_stack_add_named(GTK_STACK(self->image_stack), self->image_placeholder, "placeholder");

  /* Previous button */
  self->btn_prev_image = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_add_css_class(self->btn_prev_image, "osd");
  gtk_widget_add_css_class(self->btn_prev_image, "circular");
  gtk_widget_set_halign(self->btn_prev_image, GTK_ALIGN_START);
  gtk_widget_set_valign(self->btn_prev_image, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start(self->btn_prev_image, 8);
  gtk_widget_set_visible(self->btn_prev_image, FALSE);
  g_signal_connect(self->btn_prev_image, "clicked", G_CALLBACK(on_prev_image_clicked), self);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->image_overlay), self->btn_prev_image);

  /* Next button */
  self->btn_next_image = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_add_css_class(self->btn_next_image, "osd");
  gtk_widget_add_css_class(self->btn_next_image, "circular");
  gtk_widget_set_halign(self->btn_next_image, GTK_ALIGN_END);
  gtk_widget_set_valign(self->btn_next_image, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end(self->btn_next_image, 8);
  gtk_widget_set_visible(self->btn_next_image, FALSE);
  g_signal_connect(self->btn_next_image, "clicked", G_CALLBACK(on_next_image_clicked), self);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->image_overlay), self->btn_next_image);

  /* Dots indicator */
  self->image_dots_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(self->image_dots_box, "osd");
  gtk_widget_set_halign(self->image_dots_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(self->image_dots_box, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(self->image_dots_box, 8);
  gtk_widget_set_visible(self->image_dots_box, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->image_overlay), self->image_dots_box);

  /* Click gesture for full-size view */
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_image_clicked), self);
  gtk_widget_add_controller(self->image_stack, GTK_EVENT_CONTROLLER(click));

  return self->image_overlay;
}

static GtkWidget *
create_content_section(GnostrClassifiedCard *self)
{
  self->content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(self->content_box, "classified-content");
  gtk_widget_set_margin_start(self->content_box, 12);
  gtk_widget_set_margin_end(self->content_box, 12);
  gtk_widget_set_margin_top(self->content_box, 12);
  gtk_widget_set_margin_bottom(self->content_box, 12);

  /* Price label - prominent */
  self->price_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->price_label), 0);
  gtk_widget_add_css_class(self->price_label, "title-1");
  gtk_widget_add_css_class(self->price_label, "classified-price");
  gtk_box_append(GTK_BOX(self->content_box), self->price_label);

  /* Title button */
  self->title_button = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->title_button), FALSE);
  gtk_widget_add_css_class(self->title_button, "flat");
  g_signal_connect(self->title_button, "clicked", G_CALLBACK(on_title_clicked), self);

  self->title_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->title_label), 0);
  gtk_label_set_wrap(GTK_LABEL(self->title_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->title_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(self->title_label), 40);
  gtk_label_set_ellipsize(GTK_LABEL(self->title_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_lines(GTK_LABEL(self->title_label), 2);
  gtk_widget_add_css_class(self->title_label, "title-3");
  gtk_button_set_child(GTK_BUTTON(self->title_button), self->title_label);
  gtk_box_append(GTK_BOX(self->content_box), self->title_button);

  /* Summary */
  self->summary_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->summary_label), 0);
  gtk_label_set_wrap(GTK_LABEL(self->summary_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->summary_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(self->summary_label), 50);
  gtk_label_set_ellipsize(GTK_LABEL(self->summary_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_lines(GTK_LABEL(self->summary_label), 3);
  gtk_widget_add_css_class(self->summary_label, "dim-label");
  gtk_box_append(GTK_BOX(self->content_box), self->summary_label);

  /* Location box */
  self->location_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_visible(self->location_box, FALSE);

  self->location_icon = gtk_image_new_from_icon_name("mark-location-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->location_icon), 16);
  gtk_widget_add_css_class(self->location_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->location_box), self->location_icon);

  self->location_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->location_label, "dim-label");
  gtk_box_append(GTK_BOX(self->location_box), self->location_label);

  gtk_box_append(GTK_BOX(self->content_box), self->location_box);

  /* Categories box */
  self->categories_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->categories_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->categories_box), 10);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->categories_box), 4);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->categories_box), 4);
  gtk_widget_set_visible(self->categories_box, FALSE);
  gtk_box_append(GTK_BOX(self->content_box), self->categories_box);

  /* Published date */
  self->published_label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->published_label), 0);
  gtk_widget_add_css_class(self->published_label, "dim-label");
  gtk_widget_add_css_class(self->published_label, "caption");
  gtk_box_append(GTK_BOX(self->content_box), self->published_label);

  return self->content_box;
}

static GtkWidget *
create_seller_section(GnostrClassifiedCard *self)
{
  self->seller_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->seller_box, "classified-seller");
  gtk_widget_set_margin_start(self->seller_box, 12);
  gtk_widget_set_margin_end(self->seller_box, 12);

  /* Seller button (avatar + name) */
  self->btn_seller = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_seller), FALSE);
  gtk_widget_add_css_class(self->btn_seller, "flat");
  g_signal_connect(self->btn_seller, "clicked", G_CALLBACK(on_seller_clicked), self);

  GtkWidget *seller_inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  /* Avatar overlay */
  GtkWidget *avatar_overlay = gtk_overlay_new();
  gtk_widget_set_size_request(avatar_overlay, 32, 32);

  self->seller_initials = gtk_label_new("AN");
  gtk_widget_add_css_class(self->seller_initials, "avatar-initials");
  gtk_overlay_set_child(GTK_OVERLAY(avatar_overlay), self->seller_initials);

  self->seller_avatar = gtk_picture_new();
  gtk_widget_set_size_request(self->seller_avatar, 32, 32);
  gtk_widget_add_css_class(self->seller_avatar, "avatar");
  gtk_widget_set_visible(self->seller_avatar, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(avatar_overlay), self->seller_avatar);

  gtk_box_append(GTK_BOX(seller_inner), avatar_overlay);

  /* Seller name */
  self->seller_name_label = gtk_label_new(_("Seller"));
  gtk_widget_add_css_class(self->seller_name_label, "heading");
  gtk_box_append(GTK_BOX(seller_inner), self->seller_name_label);

  /* NIP-05 badge */
  self->nip05_badge = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->nip05_badge), 16);
  gtk_widget_add_css_class(self->nip05_badge, "success");
  gtk_widget_set_visible(self->nip05_badge, FALSE);
  gtk_box_append(GTK_BOX(seller_inner), self->nip05_badge);

  gtk_button_set_child(GTK_BUTTON(self->btn_seller), seller_inner);
  gtk_box_append(GTK_BOX(self->seller_box), self->btn_seller);

  return self->seller_box;
}

static GtkWidget *
create_action_buttons(GnostrClassifiedCard *self)
{
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(button_box, 12);
  gtk_widget_set_margin_end(button_box, 12);
  gtk_widget_set_margin_bottom(button_box, 12);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);

  /* Share button */
  self->btn_share = gtk_button_new_from_icon_name("emblem-shared-symbolic");
  gtk_widget_add_css_class(self->btn_share, "flat");
  gtk_widget_set_tooltip_text(self->btn_share, _("Share listing"));
  g_signal_connect(self->btn_share, "clicked", G_CALLBACK(on_share_clicked), self);
  gtk_box_append(GTK_BOX(button_box), self->btn_share);

  /* Contact seller button */
  self->btn_contact = gtk_button_new_with_label(_("Contact Seller"));
  gtk_widget_add_css_class(self->btn_contact, "suggested-action");
  g_signal_connect(self->btn_contact, "clicked", G_CALLBACK(on_contact_clicked), self);
  gtk_box_append(GTK_BOX(button_box), self->btn_contact);

  /* View details button */
  self->btn_details = gtk_button_new_with_label(_("View Details"));
  g_signal_connect(self->btn_details, "clicked", G_CALLBACK(on_details_clicked), self);
  gtk_box_append(GTK_BOX(button_box), self->btn_details);

  return button_box;
}

/* ============== Class Init ============== */

static void
gnostr_classified_card_class_init(GnostrClassifiedCardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = gnostr_classified_card_dispose;
  object_class->finalize = gnostr_classified_card_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_css_name(widget_class, "classified-card");

  /* Signals */
  signals[SIGNAL_CONTACT_SELLER] = g_signal_new("contact-seller",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_VIEW_DETAILS] = g_signal_new("view-details",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_IMAGE_CLICKED] = g_signal_new("image-clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_UINT);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_CATEGORY_CLICKED] = g_signal_new("category-clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_SHARE_LISTING] = g_signal_new("share-listing",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gnostr_classified_card_init(GnostrClassifiedCard *self)
{
  /* Initialize arrays */
  self->images = g_ptr_array_new_with_free_func(g_free);
  self->image_widgets = g_ptr_array_new();
  self->current_image_index = 0;

#ifdef HAVE_SOUP3
  self->image_cancellable = g_cancellable_new();
  self->session = soup_session_new();
  soup_session_set_timeout(self->session, 30);
#endif

  /* Build main layout */
  self->root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(self->root_box, "card");
  gtk_widget_add_css_class(self->root_box, "classified-card");
  gtk_widget_set_parent(self->root_box, GTK_WIDGET(self));

  /* Image carousel */
  GtkWidget *image_section = create_image_carousel(self);
  gtk_box_append(GTK_BOX(self->root_box), image_section);

  /* Content section */
  GtkWidget *content_section = create_content_section(self);
  gtk_box_append(GTK_BOX(self->root_box), content_section);

  /* Seller section */
  GtkWidget *seller_section = create_seller_section(self);
  gtk_box_append(GTK_BOX(self->root_box), seller_section);

  /* Separator */
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(sep, 8);
  gtk_widget_set_margin_bottom(sep, 8);
  gtk_box_append(GTK_BOX(self->root_box), sep);

  /* Action buttons */
  GtkWidget *buttons = create_action_buttons(self);
  gtk_box_append(GTK_BOX(self->root_box), buttons);
}

/* ============== Public API ============== */

GnostrClassifiedCard *
gnostr_classified_card_new(void)
{
  return g_object_new(GNOSTR_TYPE_CLASSIFIED_CARD, NULL);
}

void
gnostr_classified_card_set_listing(GnostrClassifiedCard *self,
                                    const GnostrClassified *classified)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));
  g_return_if_fail(classified != NULL);

  gnostr_classified_card_set_title(self, classified->title);
  gnostr_classified_card_set_summary(self, classified->summary);
  gnostr_classified_card_set_price(self, classified->price);
  gnostr_classified_card_set_location(self, classified->location);
  gnostr_classified_card_set_images(self, classified->images);
  gnostr_classified_card_set_categories(self, classified->categories);
  gnostr_classified_card_set_event_id(self, classified->event_id, classified->d_tag);
  gnostr_classified_card_set_published_at(self, classified->published_at);

  if (classified->pubkey) {
    gnostr_classified_card_set_seller(self,
      classified->seller_name,
      classified->seller_avatar,
      classified->pubkey);
  }

  if (classified->seller_nip05) {
    gnostr_classified_card_set_seller_nip05(self,
      classified->seller_nip05,
      classified->pubkey);
  }

  if (classified->seller_lud16) {
    gnostr_classified_card_set_seller_lud16(self, classified->seller_lud16);
  }
}

void
gnostr_classified_card_set_title(GnostrClassifiedCard *self, const char *title)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  if (GTK_IS_LABEL(self->title_label)) {
    gtk_label_set_text(GTK_LABEL(self->title_label),
      (title && *title) ? title : _("Untitled Listing"));
  }
}

void
gnostr_classified_card_set_summary(GnostrClassifiedCard *self, const char *summary)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  if (GTK_IS_LABEL(self->summary_label)) {
    if (summary && *summary) {
      gtk_label_set_text(GTK_LABEL(self->summary_label), summary);
      gtk_widget_set_visible(self->summary_label, TRUE);
    } else {
      gtk_widget_set_visible(self->summary_label, FALSE);
    }
  }
}

void
gnostr_classified_card_set_price(GnostrClassifiedCard *self,
                                  const GnostrClassifiedPrice *price)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  if (GTK_IS_LABEL(self->price_label)) {
    if (price) {
      gchar *formatted = gnostr_classified_price_format(price);
      gtk_label_set_text(GTK_LABEL(self->price_label), formatted);
      g_free(formatted);
    } else {
      gtk_label_set_text(GTK_LABEL(self->price_label), _("Price on request"));
    }
  }
}

void
gnostr_classified_card_set_location(GnostrClassifiedCard *self, const char *location)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  if (location && *location) {
    gtk_label_set_text(GTK_LABEL(self->location_label), location);
    gtk_widget_set_visible(self->location_box, TRUE);
  } else {
    gtk_widget_set_visible(self->location_box, FALSE);
  }
}

void
gnostr_classified_card_set_images(GnostrClassifiedCard *self, GPtrArray *images)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  /* Clear existing images */
  g_ptr_array_set_size(self->images, 0);
  g_ptr_array_set_size(self->image_widgets, 0);
  self->current_image_index = 0;

  /* Remove old image widgets from stack (except placeholder) */
  GtkWidget *child = gtk_widget_get_first_child(self->image_stack);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    if (child != self->image_placeholder) {
      gtk_stack_remove(GTK_STACK(self->image_stack), child);
    }
    child = next;
  }

  if (!images || images->len == 0) {
    gtk_stack_set_visible_child(GTK_STACK(self->image_stack), self->image_placeholder);
    gtk_widget_set_visible(self->btn_prev_image, FALSE);
    gtk_widget_set_visible(self->btn_next_image, FALSE);
    gtk_widget_set_visible(self->image_dots_box, FALSE);
    return;
  }

  /* Add new images */
  for (guint i = 0; i < images->len; i++) {
    const gchar *url = g_ptr_array_index(images, i);
    if (!url || !*url) continue;

    g_ptr_array_add(self->images, g_strdup(url));

    /* Create picture widget */
    GtkWidget *picture = gtk_picture_new();
    gtk_widget_add_css_class(picture, "classified-image");
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);

    gchar *name = g_strdup_printf("image_%u", i);
    gtk_stack_add_named(GTK_STACK(self->image_stack), picture, name);
    g_free(name);

    g_ptr_array_add(self->image_widgets, picture);

    /* Try loading from cache */
    GdkTexture *cached = gnostr_classified_get_cached_image(url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(cached));
      g_object_unref(cached);
    } else {
      /* Async download */
      gnostr_avatar_download_async(url, picture, NULL);
    }
  }

  update_image_carousel(self);
  update_image_dots(self);
}

void
gnostr_classified_card_set_categories(GnostrClassifiedCard *self, GPtrArray *categories)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  /* Clear existing categories */
  GtkWidget *child = gtk_widget_get_first_child(self->categories_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_flow_box_remove(GTK_FLOW_BOX(self->categories_box), child);
    child = next;
  }

  if (!categories || categories->len == 0) {
    gtk_widget_set_visible(self->categories_box, FALSE);
    return;
  }

  for (guint i = 0; i < categories->len; i++) {
    const gchar *cat = g_ptr_array_index(categories, i);
    if (!cat || !*cat) continue;

    gchar *label = g_strdup_printf("#%s", cat);
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    gtk_widget_add_css_class(btn, "pill");
    gtk_widget_add_css_class(btn, "small");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_category_clicked), self);
    gtk_flow_box_append(GTK_FLOW_BOX(self->categories_box), btn);
    g_free(label);
  }

  gtk_widget_set_visible(self->categories_box, TRUE);
}

void
gnostr_classified_card_set_seller(GnostrClassifiedCard *self,
                                   const char *display_name,
                                   const char *avatar_url,
                                   const char *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  g_clear_pointer(&self->pubkey_hex, g_free);
  self->pubkey_hex = g_strdup(pubkey_hex);

  /* Set name */
  if (GTK_IS_LABEL(self->seller_name_label)) {
    gtk_label_set_text(GTK_LABEL(self->seller_name_label),
      (display_name && *display_name) ? display_name : _("Seller"));
  }

  /* Set avatar */
  set_seller_initials(self, display_name);

#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url && GTK_IS_PICTURE(self->seller_avatar)) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->seller_avatar), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->seller_avatar, TRUE);
      gtk_widget_set_visible(self->seller_initials, FALSE);
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->seller_avatar, self->seller_initials);
    }
  }
#endif
}

static void
on_nip05_verified(GnostrNip05Result *result, gpointer user_data)
{
  GnostrClassifiedCard *self = GNOSTR_CLASSIFIED_CARD(user_data);

  if (!GNOSTR_IS_CLASSIFIED_CARD(self) || !GTK_IS_IMAGE(self->nip05_badge)) {
    gnostr_nip05_result_free(result);
    return;
  }

  gboolean verified = (result && result->status == GNOSTR_NIP05_STATUS_VERIFIED);
  gtk_widget_set_visible(self->nip05_badge, verified);

  if (verified && result->identifier) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->nip05_badge), result->identifier);
  }

  gnostr_nip05_result_free(result);
}

void
gnostr_classified_card_set_seller_nip05(GnostrClassifiedCard *self,
                                         const char *nip05,
                                         const char *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  g_clear_pointer(&self->nip05, g_free);
  self->nip05 = g_strdup(nip05);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

  if (!nip05 || !*nip05 || !pubkey_hex) {
    gtk_widget_set_visible(self->nip05_badge, FALSE);
    return;
  }

  self->nip05_cancellable = g_cancellable_new();
  gnostr_nip05_verify_async(nip05, pubkey_hex, on_nip05_verified, self, self->nip05_cancellable);
}

void
gnostr_classified_card_set_seller_lud16(GnostrClassifiedCard *self, const char *lud16)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  g_clear_pointer(&self->seller_lud16, g_free);
  self->seller_lud16 = g_strdup(lud16);
}

void
gnostr_classified_card_set_event_id(GnostrClassifiedCard *self,
                                     const char *event_id,
                                     const char *d_tag)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  self->event_id = g_strdup(event_id);
  self->d_tag = g_strdup(d_tag);
}

void
gnostr_classified_card_set_published_at(GnostrClassifiedCard *self, gint64 published_at)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  self->published_at = published_at;

  if (GTK_IS_LABEL(self->published_label)) {
    gchar *date_str = format_publish_date(published_at);
    gtk_label_set_text(GTK_LABEL(self->published_label), date_str);
    g_free(date_str);
  }
}

void
gnostr_classified_card_set_compact(GnostrClassifiedCard *self, gboolean compact)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  self->is_compact = compact;

  if (compact) {
    gtk_widget_add_css_class(self->root_box, "compact");
    /* Hide some elements in compact mode */
    gtk_widget_set_visible(self->summary_label, FALSE);
    gtk_widget_set_visible(self->categories_box, FALSE);
  } else {
    gtk_widget_remove_css_class(self->root_box, "compact");
  }
}

void
gnostr_classified_card_set_logged_in(GnostrClassifiedCard *self, gboolean logged_in)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  self->is_logged_in = logged_in;
  gtk_widget_set_sensitive(self->btn_contact, logged_in);
}

const char *
gnostr_classified_card_get_event_id(GnostrClassifiedCard *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self), NULL);
  return self->event_id;
}

const char *
gnostr_classified_card_get_d_tag(GnostrClassifiedCard *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self), NULL);
  return self->d_tag;
}

const char *
gnostr_classified_card_get_seller_pubkey(GnostrClassifiedCard *self)
{
  g_return_val_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self), NULL);
  return self->pubkey_hex;
}

/* ============== Image Carousel ============== */

static void
update_image_carousel(GnostrClassifiedCard *self)
{
  if (!self->images || self->images->len == 0) {
    gtk_stack_set_visible_child(GTK_STACK(self->image_stack), self->image_placeholder);
    gtk_widget_set_visible(self->btn_prev_image, FALSE);
    gtk_widget_set_visible(self->btn_next_image, FALSE);
    return;
  }

  /* Show current image */
  if (self->current_image_index < self->image_widgets->len) {
    gchar *name = g_strdup_printf("image_%u", self->current_image_index);
    gtk_stack_set_visible_child_name(GTK_STACK(self->image_stack), name);
    g_free(name);
  }

  /* Show/hide navigation buttons */
  gboolean show_nav = self->images->len > 1;
  gtk_widget_set_visible(self->btn_prev_image, show_nav);
  gtk_widget_set_visible(self->btn_next_image, show_nav);
  gtk_widget_set_visible(self->image_dots_box, show_nav);

  /* Update button sensitivity */
  gtk_widget_set_sensitive(self->btn_prev_image, self->current_image_index > 0);
  gtk_widget_set_sensitive(self->btn_next_image,
    self->current_image_index < self->images->len - 1);
}

static void
update_image_dots(GnostrClassifiedCard *self)
{
  /* Clear existing dots */
  GtkWidget *child = gtk_widget_get_first_child(self->image_dots_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(self->image_dots_box), child);
    child = next;
  }

  if (!self->images || self->images->len <= 1) return;

  for (guint i = 0; i < self->images->len; i++) {
    GtkWidget *dot = gtk_drawing_area_new();
    gtk_widget_set_size_request(dot, 8, 8);
    gtk_widget_add_css_class(dot, "image-dot");
    if (i == self->current_image_index) {
      gtk_widget_add_css_class(dot, "active");
    }
    gtk_box_append(GTK_BOX(self->image_dots_box), dot);
  }
}

void
gnostr_classified_card_next_image(GnostrClassifiedCard *self)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  if (!self->images || self->images->len <= 1) return;

  if (self->current_image_index < self->images->len - 1) {
    self->current_image_index++;
    update_image_carousel(self);
    update_image_dots(self);
  }
}

void
gnostr_classified_card_prev_image(GnostrClassifiedCard *self)
{
  g_return_if_fail(GNOSTR_IS_CLASSIFIED_CARD(self));

  if (!self->images || self->images->len <= 1) return;

  if (self->current_image_index > 0) {
    self->current_image_index--;
    update_image_carousel(self);
    update_image_dots(self);
  }
}
