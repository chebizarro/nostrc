/*
 * gnostr-calendar-event-card.c - NIP-52 Calendar Event Card Widget
 *
 * Displays kind 31922/31923 calendar events.
 */

#include "gnostr-calendar-event-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip05.h"
#include "../util/utils.h"
#include <glib/gi18n.h>
#include <nostr-gobject-1.0/nostr_nip19.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Maximum participants to show before "and N more" */
#define MAX_VISIBLE_PARTICIPANTS 5

/* Status badge colors */
#define STATUS_UPCOMING_CLASS "badge-upcoming"
#define STATUS_ONGOING_CLASS "badge-ongoing"
#define STATUS_PAST_CLASS "badge-past"

struct _GnostrCalendarEventCard {
  GtkWidget parent_instance;

  /* Header section */
  GtkWidget *root;
  GtkWidget *event_image_box;
  GtkWidget *event_image;
  GtkWidget *status_badge;
  GtkWidget *lbl_status;
  GtkWidget *type_icon;

  /* Title and time */
  GtkWidget *btn_title;
  GtkWidget *lbl_title;
  GtkWidget *lbl_date_range;
  GtkWidget *lbl_time_until;

  /* Location */
  GtkWidget *location_row;
  GtkWidget *location_icon;
  GtkWidget *lbl_location;
  GtkWidget *btn_open_map;

  /* Organizer */
  GtkWidget *organizer_row;
  GtkWidget *btn_organizer_avatar;
  GtkWidget *organizer_avatar;
  GtkWidget *organizer_initials;
  GtkWidget *btn_organizer_name;
  GtkWidget *lbl_organizer_name;
  GtkWidget *nip05_badge;

  /* Participants */
  GtkWidget *participants_section;
  GtkWidget *lbl_participants_header;
  GtkWidget *participants_flow;
  GtkWidget *lbl_more_participants;

  /* Description */
  GtkWidget *description_box;
  GtkWidget *lbl_description;

  /* Hashtags */
  GtkWidget *hashtags_box;

  /* Action buttons */
  GtkWidget *btn_rsvp;
  GtkWidget *btn_share;
  GtkWidget *btn_menu;
  GtkWidget *menu_popover;

  /* State */
  gchar *event_id;
  gchar *d_tag;
  gchar *pubkey_hex;
  GnostrCalendarEventType event_type;
  gint64 start_time;
  gint64 end_time;
  gboolean has_rsvp;
  gboolean is_logged_in;
  guint participants_count;

#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  GCancellable *image_cancellable;
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
#endif

  GCancellable *nip05_cancellable;
};

G_DEFINE_TYPE(GnostrCalendarEventCard, gnostr_calendar_event_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_EVENT,
  SIGNAL_OPEN_URL,
  SIGNAL_OPEN_MAP,
  SIGNAL_RSVP_REQUESTED,
  SIGNAL_SHARE_EVENT,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void gnostr_calendar_event_card_dispose(GObject *object) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(object);

  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
  }
  if (self->image_cancellable) {
    g_cancellable_cancel(self->image_cancellable);
    g_clear_object(&self->image_cancellable);
  }
  /* Shared session is managed globally - do not clear here */
#endif

  if (self->menu_popover) {
    if (GTK_IS_POPOVER(self->menu_popover)) {
      gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
    }
    gtk_widget_unparent(self->menu_popover);
    self->menu_popover = NULL;
  }

  /* Clear all child widgets */
  GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self));
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_widget_unparent(child);
    child = next;
  }

  G_OBJECT_CLASS(gnostr_calendar_event_card_parent_class)->dispose(object);
}

static void gnostr_calendar_event_card_finalize(GObject *obj) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(obj);

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);

  G_OBJECT_CLASS(gnostr_calendar_event_card_parent_class)->finalize(obj);
}

/* Click handlers */
static void on_organizer_avatar_clicked(GtkButton *btn, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_organizer_name_clicked(GtkButton *btn, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);
  (void)btn;
  if (self->pubkey_hex && *self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_title_clicked(GtkButton *btn, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);
  (void)btn;
  if (self->event_id && *self->event_id) {
    g_signal_emit(self, signals[SIGNAL_OPEN_EVENT], 0, self->event_id);
  }
}

static void on_rsvp_clicked(GtkButton *btn, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);
  (void)btn;
  if (self->event_id && self->d_tag && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_RSVP_REQUESTED], 0,
                  self->event_id, self->d_tag, self->pubkey_hex);
  }
}

static void on_share_clicked(GtkButton *btn, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);
  (void)btn;

  if (!self->d_tag || !self->pubkey_hex) return;

  /* Build naddr for NIP-33 addressable event */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_naddr(
    self->d_tag, self->pubkey_hex, (gint)self->event_type, NULL, NULL);

  if (n19) {
    char *uri = g_strdup_printf("nostr:%s", gnostr_nip19_get_bech32(n19));
    g_signal_emit(self, signals[SIGNAL_SHARE_EVENT], 0, uri);
    g_free(uri);
  }
}

static void on_open_map_clicked(GtkButton *btn, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);
  (void)btn;

  const gchar *location = gtk_label_get_text(GTK_LABEL(self->lbl_location));
  if (location && *location) {
    /* URL-encode the location for OpenStreetMap search */
    gchar *encoded = g_uri_escape_string(location, NULL, TRUE);
    gchar *url = g_strdup_printf("https://www.openstreetmap.org/search?query=%s", encoded);
    g_signal_emit(self, signals[SIGNAL_OPEN_URL], 0, url);
    g_free(url);
    g_free(encoded);
  }
}

static void on_participant_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  GtkWidget *avatar = GTK_WIDGET(user_data);
  const gchar *pubkey = g_object_get_data(G_OBJECT(avatar), "pubkey");
  GnostrCalendarEventCard *self = g_object_get_data(G_OBJECT(avatar), "card");

  if (pubkey && self) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey);
  }
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);
  (void)btn;

  if (!self->menu_popover) {
    self->menu_popover = gtk_popover_new();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Copy Event Link */
    GtkWidget *copy_btn = gtk_button_new();
    GtkWidget *copy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_label = gtk_label_new(_("Copy Event Link"));
    gtk_box_append(GTK_BOX(copy_box), copy_icon);
    gtk_box_append(GTK_BOX(copy_box), copy_label);
    gtk_button_set_child(GTK_BUTTON(copy_btn), copy_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_btn), FALSE);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_share_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_btn);

    /* View Organizer Profile */
    GtkWidget *profile_btn = gtk_button_new();
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *profile_icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
    GtkWidget *profile_label = gtk_label_new(_("View Organizer Profile"));
    gtk_box_append(GTK_BOX(profile_box), profile_icon);
    gtk_box_append(GTK_BOX(profile_box), profile_label);
    gtk_button_set_child(GTK_BUTTON(profile_btn), profile_box);
    gtk_button_set_has_frame(GTK_BUTTON(profile_btn), FALSE);
    g_signal_connect(profile_btn, "clicked", G_CALLBACK(on_organizer_avatar_clicked), self);
    gtk_box_append(GTK_BOX(box), profile_btn);

    gtk_popover_set_child(GTK_POPOVER(self->menu_popover), box);
    gtk_widget_set_parent(self->menu_popover, GTK_WIDGET(self->btn_menu));
  }

  gtk_popover_popup(GTK_POPOVER(self->menu_popover));
}

/* Set avatar initials fallback */
static void set_avatar_initials(GtkWidget *initials_label, const char *display, const char *handle) {
  if (!GTK_IS_LABEL(initials_label)) return;

  const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
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

  gtk_label_set_text(GTK_LABEL(initials_label), initials);
}

/* Update status badge */
static void update_status_badge(GnostrCalendarEventCard *self) {
  if (!GTK_IS_LABEL(self->lbl_status)) return;

  gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
  gint64 end = (self->end_time > 0) ? self->end_time : (self->start_time + 86400);

  /* Remove old classes */
  gtk_widget_remove_css_class(self->status_badge, STATUS_UPCOMING_CLASS);
  gtk_widget_remove_css_class(self->status_badge, STATUS_ONGOING_CLASS);
  gtk_widget_remove_css_class(self->status_badge, STATUS_PAST_CLASS);

  if (now < self->start_time) {
    gtk_label_set_text(GTK_LABEL(self->lbl_status), _("Upcoming"));
    gtk_widget_add_css_class(self->status_badge, STATUS_UPCOMING_CLASS);
  } else if (now <= end) {
    gtk_label_set_text(GTK_LABEL(self->lbl_status), _("Ongoing"));
    gtk_widget_add_css_class(self->status_badge, STATUS_ONGOING_CLASS);
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_status), _("Past"));
    gtk_widget_add_css_class(self->status_badge, STATUS_PAST_CLASS);
  }
}

/* Build the widget UI */
static void build_ui(GnostrCalendarEventCard *self) {
  /* Main container */
  self->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_parent(self->root, GTK_WIDGET(self));
  gtk_widget_add_css_class(self->root, "calendar-event-card");
  gtk_widget_add_css_class(self->root, "card");
  gtk_widget_set_margin_start(self->root, 12);
  gtk_widget_set_margin_end(self->root, 12);
  gtk_widget_set_margin_top(self->root, 12);
  gtk_widget_set_margin_bottom(self->root, 12);

  /* Header with image placeholder and status */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(header, "event-header");
  gtk_box_append(GTK_BOX(self->root), header);

  /* Type icon (calendar for date, clock for time) */
  self->type_icon = gtk_image_new_from_icon_name("x-office-calendar-symbolic");
  gtk_widget_add_css_class(self->type_icon, "event-type-icon");
  gtk_box_append(GTK_BOX(header), self->type_icon);

  /* Status badge */
  self->status_badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(self->status_badge, "status-badge");
  self->lbl_status = gtk_label_new(_("Upcoming"));
  gtk_widget_add_css_class(self->lbl_status, "status-label");
  gtk_box_append(GTK_BOX(self->status_badge), self->lbl_status);
  gtk_box_append(GTK_BOX(header), self->status_badge);

  /* Spacer */
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(header), spacer);

  /* Menu button */
  self->btn_menu = gtk_button_new_from_icon_name("view-more-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_menu), FALSE);
  gtk_widget_add_css_class(self->btn_menu, "flat");
  g_signal_connect(self->btn_menu, "clicked", G_CALLBACK(on_menu_clicked), self);
  gtk_box_append(GTK_BOX(header), self->btn_menu);

  /* Event image (hidden by default) */
  self->event_image_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_visible(self->event_image_box, FALSE);
  gtk_widget_add_css_class(self->event_image_box, "event-image-box");
  self->event_image = gtk_picture_new();
  gtk_widget_set_size_request(self->event_image, -1, 150);
  gtk_picture_set_content_fit(GTK_PICTURE(self->event_image), GTK_CONTENT_FIT_COVER);
  gtk_box_append(GTK_BOX(self->event_image_box), self->event_image);
  gtk_box_append(GTK_BOX(self->root), self->event_image_box);

  /* Title (clickable) */
  self->btn_title = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_title), FALSE);
  gtk_widget_add_css_class(self->btn_title, "title-button");
  self->lbl_title = gtk_label_new(_("Untitled Event"));
  gtk_label_set_wrap(GTK_LABEL(self->lbl_title), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_title), 0);
  gtk_widget_add_css_class(self->lbl_title, "event-title");
  gtk_button_set_child(GTK_BUTTON(self->btn_title), self->lbl_title);
  g_signal_connect(self->btn_title, "clicked", G_CALLBACK(on_title_clicked), self);
  gtk_box_append(GTK_BOX(self->root), self->btn_title);

  /* Date/time range */
  GtkWidget *time_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(time_row, "time-row");

  GtkWidget *time_icon = gtk_image_new_from_icon_name("alarm-symbolic");
  gtk_widget_add_css_class(time_icon, "dim-label");
  gtk_box_append(GTK_BOX(time_row), time_icon);

  self->lbl_date_range = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(self->lbl_date_range), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_date_range), 0);
  gtk_widget_add_css_class(self->lbl_date_range, "date-range");
  gtk_box_append(GTK_BOX(time_row), self->lbl_date_range);

  self->lbl_time_until = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_time_until, "time-until");
  gtk_widget_add_css_class(self->lbl_time_until, "dim-label");
  gtk_box_append(GTK_BOX(time_row), self->lbl_time_until);

  gtk_box_append(GTK_BOX(self->root), time_row);

  /* Location row (hidden by default) */
  self->location_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_visible(self->location_row, FALSE);
  gtk_widget_add_css_class(self->location_row, "location-row");

  self->location_icon = gtk_image_new_from_icon_name("mark-location-symbolic");
  gtk_widget_add_css_class(self->location_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->location_row), self->location_icon);

  self->lbl_location = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(self->lbl_location), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_location), 0);
  gtk_widget_set_hexpand(self->lbl_location, TRUE);
  gtk_box_append(GTK_BOX(self->location_row), self->lbl_location);

  self->btn_open_map = gtk_button_new_from_icon_name("map-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_open_map), FALSE);
  gtk_widget_set_tooltip_text(self->btn_open_map, _("Open in Maps"));
  g_signal_connect(self->btn_open_map, "clicked", G_CALLBACK(on_open_map_clicked), self);
  gtk_box_append(GTK_BOX(self->location_row), self->btn_open_map);

  gtk_box_append(GTK_BOX(self->root), self->location_row);

  /* Organizer row */
  self->organizer_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(self->organizer_row, "organizer-row");

  GtkWidget *organized_label = gtk_label_new(_("Organized by"));
  gtk_widget_add_css_class(organized_label, "dim-label");
  gtk_box_append(GTK_BOX(self->organizer_row), organized_label);

  /* Organizer avatar button */
  self->btn_organizer_avatar = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_organizer_avatar), FALSE);
  gtk_widget_add_css_class(self->btn_organizer_avatar, "avatar-button");

  GtkWidget *avatar_overlay = gtk_overlay_new();
  self->organizer_initials = gtk_label_new("AN");
  gtk_widget_add_css_class(self->organizer_initials, "avatar-initials");
  gtk_widget_set_size_request(self->organizer_initials, 28, 28);
  gtk_overlay_set_child(GTK_OVERLAY(avatar_overlay), self->organizer_initials);

  self->organizer_avatar = gtk_picture_new();
  gtk_widget_set_size_request(self->organizer_avatar, 28, 28);
  gtk_widget_set_visible(self->organizer_avatar, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(avatar_overlay), self->organizer_avatar);

  gtk_button_set_child(GTK_BUTTON(self->btn_organizer_avatar), avatar_overlay);
  g_signal_connect(self->btn_organizer_avatar, "clicked", G_CALLBACK(on_organizer_avatar_clicked), self);
  gtk_box_append(GTK_BOX(self->organizer_row), self->btn_organizer_avatar);

  /* Organizer name button */
  self->btn_organizer_name = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_organizer_name), FALSE);
  self->lbl_organizer_name = gtk_label_new(_("Anonymous"));
  gtk_widget_add_css_class(self->lbl_organizer_name, "organizer-name");
  gtk_button_set_child(GTK_BUTTON(self->btn_organizer_name), self->lbl_organizer_name);
  g_signal_connect(self->btn_organizer_name, "clicked", G_CALLBACK(on_organizer_name_clicked), self);
  gtk_box_append(GTK_BOX(self->organizer_row), self->btn_organizer_name);

  /* NIP-05 badge */
  self->nip05_badge = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_widget_add_css_class(self->nip05_badge, "nip05-badge");
  gtk_widget_set_visible(self->nip05_badge, FALSE);
  gtk_box_append(GTK_BOX(self->organizer_row), self->nip05_badge);

  gtk_box_append(GTK_BOX(self->root), self->organizer_row);

  /* Participants section (hidden by default) */
  self->participants_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_visible(self->participants_section, FALSE);
  gtk_widget_add_css_class(self->participants_section, "participants-section");

  self->lbl_participants_header = gtk_label_new(_("Participants"));
  gtk_widget_add_css_class(self->lbl_participants_header, "participants-header");
  gtk_label_set_xalign(GTK_LABEL(self->lbl_participants_header), 0);
  gtk_box_append(GTK_BOX(self->participants_section), self->lbl_participants_header);

  self->participants_flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->participants_flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->participants_flow), 10);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->participants_flow), 4);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->participants_flow), 4);
  gtk_box_append(GTK_BOX(self->participants_section), self->participants_flow);

  self->lbl_more_participants = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_more_participants, "dim-label");
  gtk_widget_set_visible(self->lbl_more_participants, FALSE);
  gtk_box_append(GTK_BOX(self->participants_section), self->lbl_more_participants);

  gtk_box_append(GTK_BOX(self->root), self->participants_section);

  /* Description (hidden by default) */
  self->description_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_visible(self->description_box, FALSE);
  gtk_widget_add_css_class(self->description_box, "description-box");

  self->lbl_description = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(self->lbl_description), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->lbl_description), 0);
  gtk_label_set_max_width_chars(GTK_LABEL(self->lbl_description), 80);
  gtk_label_set_ellipsize(GTK_LABEL(self->lbl_description), PANGO_ELLIPSIZE_END);
  gtk_label_set_lines(GTK_LABEL(self->lbl_description), 3);
  gtk_box_append(GTK_BOX(self->description_box), self->lbl_description);

  gtk_box_append(GTK_BOX(self->root), self->description_box);

  /* Hashtags (hidden by default) */
  self->hashtags_box = gtk_flow_box_new();
  gtk_widget_set_visible(self->hashtags_box, FALSE);
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->hashtags_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->hashtags_box), 10);
  gtk_widget_add_css_class(self->hashtags_box, "hashtags-box");
  gtk_box_append(GTK_BOX(self->root), self->hashtags_box);

  /* Action buttons */
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(actions, "action-buttons");
  gtk_widget_set_halign(actions, GTK_ALIGN_END);

  self->btn_share = gtk_button_new_from_icon_name("emblem-shared-symbolic");
  gtk_button_set_has_frame(GTK_BUTTON(self->btn_share), FALSE);
  gtk_widget_set_tooltip_text(self->btn_share, _("Share Event"));
  g_signal_connect(self->btn_share, "clicked", G_CALLBACK(on_share_clicked), self);
  gtk_box_append(GTK_BOX(actions), self->btn_share);

  self->btn_rsvp = gtk_button_new_with_label(_("RSVP"));
  gtk_widget_add_css_class(self->btn_rsvp, "suggested-action");
  gtk_widget_set_sensitive(self->btn_rsvp, FALSE);
  g_signal_connect(self->btn_rsvp, "clicked", G_CALLBACK(on_rsvp_clicked), self);
  gtk_box_append(GTK_BOX(actions), self->btn_rsvp);

  gtk_box_append(GTK_BOX(self->root), actions);
}

static void gnostr_calendar_event_card_class_init(GnostrCalendarEventCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_calendar_event_card_dispose;
  gclass->finalize = gnostr_calendar_event_card_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BIN_LAYOUT);

  /* Signals */
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_EVENT] = g_signal_new("open-event",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_URL] = g_signal_new("open-url",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_MAP] = g_signal_new("open-map",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_RSVP_REQUESTED] = g_signal_new("rsvp-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_SHARE_EVENT] = g_signal_new("share-event",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_calendar_event_card_init(GnostrCalendarEventCard *self) {
  gtk_widget_add_css_class(GTK_WIDGET(self), "calendar-event-card-widget");

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  self->image_cancellable = g_cancellable_new();
  /* Uses shared session from gnostr_get_shared_soup_session() */
#endif

  build_ui(self);
}

GnostrCalendarEventCard *gnostr_calendar_event_card_new(void) {
  return g_object_new(GNOSTR_TYPE_CALENDAR_EVENT_CARD, NULL);
}

void gnostr_calendar_event_card_set_event(GnostrCalendarEventCard *self,
                                           const GnostrNip52CalendarEvent *event) {
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self));
  if (!event) return;

  /* Store event identifiers */
  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->d_tag, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);

  self->event_id = g_strdup(event->event_id);
  self->d_tag = g_strdup(event->d_tag);
  self->pubkey_hex = g_strdup(event->pubkey);
  self->event_type = event->type;
  self->start_time = event->start;
  self->end_time = event->end;

  /* Update type icon */
  if (GTK_IS_IMAGE(self->type_icon)) {
    if (event->type == GNOSTR_CALENDAR_EVENT_DATE_BASED) {
      gtk_image_set_from_icon_name(GTK_IMAGE(self->type_icon), "x-office-calendar-symbolic");
      gtk_widget_set_tooltip_text(self->type_icon, _("Date-based event"));
    } else {
      gtk_image_set_from_icon_name(GTK_IMAGE(self->type_icon), "alarm-symbolic");
      gtk_widget_set_tooltip_text(self->type_icon, _("Time-based event"));
    }
  }

  /* Set title */
  if (GTK_IS_LABEL(self->lbl_title)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_title),
      (event->title && *event->title) ? event->title : _("Untitled Event"));
  }

  /* Set date/time range */
  if (GTK_IS_LABEL(self->lbl_date_range)) {
    gchar *range = gnostr_nip52_format_date_range(event);
    gtk_label_set_text(GTK_LABEL(self->lbl_date_range), range);
    g_free(range);
  }

  /* Set time until */
  if (GTK_IS_LABEL(self->lbl_time_until)) {
    gchar *time_until = gnostr_nip52_format_time_until(event);
    if (time_until) {
      gtk_label_set_text(GTK_LABEL(self->lbl_time_until), time_until);
      gtk_widget_set_visible(self->lbl_time_until, TRUE);
      g_free(time_until);
    } else {
      gtk_widget_set_visible(self->lbl_time_until, FALSE);
    }
  }

  /* Set location */
  const gchar *location = gnostr_nip52_get_primary_location(event);
  if (location && *location) {
    gtk_label_set_text(GTK_LABEL(self->lbl_location), location);
    gtk_widget_set_visible(self->location_row, TRUE);
  } else {
    gtk_widget_set_visible(self->location_row, FALSE);
  }

  /* Set description */
  if (event->description && *event->description) {
    gtk_label_set_text(GTK_LABEL(self->lbl_description), event->description);
    gtk_widget_set_visible(self->description_box, TRUE);
  } else {
    gtk_widget_set_visible(self->description_box, FALSE);
  }

  /* Set hashtags */
  if (event->hashtags && event->hashtags_count > 0) {
    /* Clear existing hashtags */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->hashtags_box))) {
      gtk_flow_box_remove(GTK_FLOW_BOX(self->hashtags_box), child);
    }

    for (gsize i = 0; i < event->hashtags_count && event->hashtags[i]; i++) {
      gchar *tag_text = g_strdup_printf("#%s", event->hashtags[i]);
      GtkWidget *tag_label = gtk_label_new(tag_text);
      gtk_widget_add_css_class(tag_label, "hashtag");
      gtk_flow_box_append(GTK_FLOW_BOX(self->hashtags_box), tag_label);
      g_free(tag_text);
    }
    gtk_widget_set_visible(self->hashtags_box, TRUE);
  } else {
    gtk_widget_set_visible(self->hashtags_box, FALSE);
  }

  /* Update status badge */
  update_status_badge(self);

  /* Reset participant count */
  self->participants_count = 0;
}

void gnostr_calendar_event_card_set_organizer(GnostrCalendarEventCard *self,
                                               const char *display_name,
                                               const char *handle,
                                               const char *avatar_url,
                                               const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self));

  if (pubkey_hex) {
    g_clear_pointer(&self->pubkey_hex, g_free);
    self->pubkey_hex = g_strdup(pubkey_hex);
  }

  /* Set organizer name */
  if (GTK_IS_LABEL(self->lbl_organizer_name)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_organizer_name),
      (display_name && *display_name) ? display_name : (handle ? handle : _("Anonymous")));
  }

  /* Set avatar initials */
  set_avatar_initials(self->organizer_initials, display_name, handle);

#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url && GTK_IS_PICTURE(self->organizer_avatar)) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->organizer_avatar), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->organizer_avatar, TRUE);
      gtk_widget_set_visible(self->organizer_initials, FALSE);
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, self->organizer_avatar, self->organizer_initials);
    }
  }
#else
  (void)avatar_url;
#endif
}

/* NIP-05 verification callback */
static void on_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GnostrCalendarEventCard *self = GNOSTR_CALENDAR_EVENT_CARD(user_data);

  if (!GNOSTR_IS_CALENDAR_EVENT_CARD(self) || !GTK_IS_IMAGE(self->nip05_badge)) {
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

void gnostr_calendar_event_card_set_nip05(GnostrCalendarEventCard *self,
                                           const char *nip05,
                                           const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self));

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

void gnostr_calendar_event_card_add_participant(GnostrCalendarEventCard *self,
                                                 const char *display_name,
                                                 const char *avatar_url,
                                                 const char *pubkey_hex,
                                                 const char *role) {
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self));
  if (!pubkey_hex) return;

  /* Show participants section */
  gtk_widget_set_visible(self->participants_section, TRUE);

  /* Check if we've reached max visible */
  if (self->participants_count >= MAX_VISIBLE_PARTICIPANTS) {
    self->participants_count++;
    gchar *more_text = g_strdup_printf(_("and %u more..."), self->participants_count - MAX_VISIBLE_PARTICIPANTS);
    gtk_label_set_text(GTK_LABEL(self->lbl_more_participants), more_text);
    gtk_widget_set_visible(self->lbl_more_participants, TRUE);
    g_free(more_text);
    return;
  }

  self->participants_count++;

  /* Create participant avatar */
  GtkWidget *avatar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_size_request(avatar_box, 40, 50);

  GtkWidget *avatar_overlay = gtk_overlay_new();

  GtkWidget *initials = gtk_label_new("AN");
  set_avatar_initials(initials, display_name, pubkey_hex);
  gtk_widget_add_css_class(initials, "avatar-initials-small");
  gtk_widget_set_size_request(initials, 32, 32);
  gtk_overlay_set_child(GTK_OVERLAY(avatar_overlay), initials);

  GtkWidget *avatar_pic = gtk_picture_new();
  gtk_widget_set_size_request(avatar_pic, 32, 32);
  gtk_widget_set_visible(avatar_pic, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(avatar_overlay), avatar_pic);

  gtk_box_append(GTK_BOX(avatar_box), avatar_overlay);

  /* Role label if provided */
  if (role && *role) {
    GtkWidget *role_label = gtk_label_new(role);
    gtk_widget_add_css_class(role_label, "participant-role");
    gtk_widget_add_css_class(role_label, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(role_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(role_label), 8);
    gtk_box_append(GTK_BOX(avatar_box), role_label);
  }

  /* Store pubkey and card reference for click handler */
  g_object_set_data_full(G_OBJECT(avatar_box), "pubkey", g_strdup(pubkey_hex), g_free);
  g_object_set_data(G_OBJECT(avatar_box), "card", self);

  /* Make clickable */
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_participant_clicked), avatar_box);
  gtk_widget_add_controller(avatar_box, GTK_EVENT_CONTROLLER(click));

  gtk_widget_set_cursor_from_name(avatar_box, "pointer");
  gtk_widget_set_tooltip_text(avatar_box, display_name ? display_name : pubkey_hex);

  gtk_flow_box_append(GTK_FLOW_BOX(self->participants_flow), avatar_box);

#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url) {
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(avatar_pic), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(avatar_pic, TRUE);
      gtk_widget_set_visible(initials, FALSE);
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, avatar_pic, initials);
    }
  }
#else
  (void)avatar_url;
#endif
}

void gnostr_calendar_event_card_set_logged_in(GnostrCalendarEventCard *self,
                                               gboolean logged_in) {
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self));

  self->is_logged_in = logged_in;

  if (GTK_IS_WIDGET(self->btn_rsvp)) {
    gtk_widget_set_sensitive(self->btn_rsvp, logged_in);
  }
}

void gnostr_calendar_event_card_set_rsvp_status(GnostrCalendarEventCard *self,
                                                 gboolean has_rsvp) {
  g_return_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self));

  self->has_rsvp = has_rsvp;

  if (GTK_IS_BUTTON(self->btn_rsvp)) {
    if (has_rsvp) {
      gtk_button_set_label(GTK_BUTTON(self->btn_rsvp), _("Going"));
      gtk_widget_remove_css_class(self->btn_rsvp, "suggested-action");
      gtk_widget_add_css_class(self->btn_rsvp, "success");
    } else {
      gtk_button_set_label(GTK_BUTTON(self->btn_rsvp), _("RSVP"));
      gtk_widget_remove_css_class(self->btn_rsvp, "success");
      gtk_widget_add_css_class(self->btn_rsvp, "suggested-action");
    }
  }
}

const char *gnostr_calendar_event_card_get_event_id(GnostrCalendarEventCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self), NULL);
  return self->event_id;
}

const char *gnostr_calendar_event_card_get_d_tag(GnostrCalendarEventCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self), NULL);
  return self->d_tag;
}

char *gnostr_calendar_event_card_get_a_tag(GnostrCalendarEventCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self), NULL);

  if (!self->pubkey_hex || !self->d_tag) return NULL;

  return gnostr_nip52_build_a_tag((gint)self->event_type, self->pubkey_hex, self->d_tag);
}

GnostrCalendarEventType gnostr_calendar_event_card_get_event_type(GnostrCalendarEventCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self), GNOSTR_CALENDAR_EVENT_DATE_BASED);
  return self->event_type;
}

gboolean gnostr_calendar_event_card_is_date_based(GnostrCalendarEventCard *self) {
  g_return_val_if_fail(GNOSTR_IS_CALENDAR_EVENT_CARD(self), FALSE);
  return self->event_type == GNOSTR_CALENDAR_EVENT_DATE_BASED;
}
