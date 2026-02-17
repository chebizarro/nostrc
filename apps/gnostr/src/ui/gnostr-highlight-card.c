/*
 * gnostr-highlight-card.c - NIP-84 Highlight Card Widget Implementation
 */

#include "gnostr-highlight-card.h"
#include "gnostr-avatar-cache.h"
#include "../util/nip84_highlights.h"
#include "../util/utils.h"
#include <glib/gi18n.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

struct _GnostrHighlightCard {
  GtkWidget parent_instance;

  /* Widgets */
  GtkWidget *root;
  GtkWidget *quote_mark;           /* Left border / quote indicator */
  GtkWidget *highlighted_text;     /* Main highlighted text */
  GtkWidget *context_label;        /* Surrounding context (collapsed) */
  GtkWidget *comment_label;        /* User's annotation */
  GtkWidget *source_link;          /* Link to source content */
  GtkWidget *source_icon;          /* Icon indicating source type */
  GtkWidget *highlighter_box;      /* Highlighter info row */
  GtkWidget *highlighter_avatar;   /* Highlighter's avatar */
  GtkWidget *highlighter_name;     /* Highlighter's name */
  GtkWidget *timestamp_label;      /* When highlighted */
  GtkWidget *author_label;         /* "from [author]" text */

  /* State */
  gchar *event_id;
  gchar *highlighter_pubkey;
  gchar *author_pubkey;
  gchar *source_event_id;
  gchar *source_a_tag;
  gchar *source_url;
  gchar *source_relay_hint;
  GnostrHighlightSource source_type;

#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
#endif
};

G_DEFINE_TYPE(GnostrHighlightCard, gnostr_highlight_card, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_SOURCE,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_OPEN_AUTHOR_PROFILE,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void gnostr_highlight_card_dispose(GObject *obj) {
  GnostrHighlightCard *self = GNOSTR_HIGHLIGHT_CARD(obj);

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

  G_OBJECT_CLASS(gnostr_highlight_card_parent_class)->dispose(obj);
}

static void gnostr_highlight_card_finalize(GObject *obj) {
  GnostrHighlightCard *self = GNOSTR_HIGHLIGHT_CARD(obj);

  g_clear_pointer(&self->event_id, g_free);
  g_clear_pointer(&self->highlighter_pubkey, g_free);
  g_clear_pointer(&self->author_pubkey, g_free);
  g_clear_pointer(&self->source_event_id, g_free);
  g_clear_pointer(&self->source_a_tag, g_free);
  g_clear_pointer(&self->source_url, g_free);
  g_clear_pointer(&self->source_relay_hint, g_free);

  G_OBJECT_CLASS(gnostr_highlight_card_parent_class)->finalize(obj);
}

/* Format timestamp for display */
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

/* Click handler for source link */
static void on_source_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrHighlightCard *self = GNOSTR_HIGHLIGHT_CARD(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;

  const char *source_ref = NULL;
  switch (self->source_type) {
    case GNOSTR_HIGHLIGHT_SOURCE_NOTE:
      source_ref = self->source_event_id;
      break;
    case GNOSTR_HIGHLIGHT_SOURCE_ARTICLE:
      source_ref = self->source_a_tag;
      break;
    case GNOSTR_HIGHLIGHT_SOURCE_URL:
      source_ref = self->source_url;
      break;
    default:
      return;
  }

  if (source_ref && *source_ref) {
    g_signal_emit(self, signals[SIGNAL_OPEN_SOURCE], 0, source_ref);
  }
}

/* Click handler for highlighter profile */
static void on_highlighter_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrHighlightCard *self = GNOSTR_HIGHLIGHT_CARD(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;

  if (self->highlighter_pubkey && *self->highlighter_pubkey) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->highlighter_pubkey);
  }
}

/* Click handler for author profile */
static void on_author_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrHighlightCard *self = GNOSTR_HIGHLIGHT_CARD(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;

  if (self->author_pubkey && *self->author_pubkey) {
    g_signal_emit(self, signals[SIGNAL_OPEN_AUTHOR_PROFILE], 0, self->author_pubkey);
  }
}

static void gnostr_highlight_card_class_init(GnostrHighlightCardClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_highlight_card_dispose;
  gclass->finalize = gnostr_highlight_card_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);

  /* Signals */
  signals[SIGNAL_OPEN_SOURCE] = g_signal_new("open-source",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_OPEN_AUTHOR_PROFILE] = g_signal_new("open-author-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_highlight_card_init(GnostrHighlightCard *self) {
  GtkLayoutManager *layout = gtk_widget_get_layout_manager(GTK_WIDGET(self));
  gtk_orientable_set_orientation(GTK_ORIENTABLE(layout), GTK_ORIENTATION_VERTICAL);

  gtk_widget_add_css_class(GTK_WIDGET(self), "highlight-card");

  /* Create main container with left border (quote style) */
  self->root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(self->root, 12);
  gtk_widget_set_margin_end(self->root, 12);
  gtk_widget_set_margin_top(self->root, 8);
  gtk_widget_set_margin_bottom(self->root, 8);
  gtk_widget_set_parent(self->root, GTK_WIDGET(self));

  /* Quote mark / left border indicator */
  self->quote_mark = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(self->quote_mark, 4, -1);
  gtk_widget_add_css_class(self->quote_mark, "highlight-quote-border");
  gtk_box_append(GTK_BOX(self->root), self->quote_mark);

  /* Content area */
  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(content_box, TRUE);
  gtk_box_append(GTK_BOX(self->root), content_box);

  /* Highlighted text (main content) */
  self->highlighted_text = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->highlighted_text), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->highlighted_text), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(self->highlighted_text), 0.0);
  gtk_label_set_selectable(GTK_LABEL(self->highlighted_text), TRUE);
  gtk_widget_add_css_class(self->highlighted_text, "highlight-text");
  gtk_box_append(GTK_BOX(content_box), self->highlighted_text);

  /* Context label (collapsed, shown on expand) */
  self->context_label = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->context_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(self->context_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(self->context_label), 0.0);
  gtk_widget_add_css_class(self->context_label, "highlight-context");
  gtk_widget_add_css_class(self->context_label, "dim-label");
  gtk_widget_set_visible(self->context_label, FALSE);
  gtk_box_append(GTK_BOX(content_box), self->context_label);

  /* Comment label (user's annotation) */
  self->comment_label = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(self->comment_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(self->comment_label), 0.0);
  gtk_widget_add_css_class(self->comment_label, "highlight-comment");
  gtk_widget_set_visible(self->comment_label, FALSE);
  gtk_box_append(GTK_BOX(content_box), self->comment_label);

  /* Source link row */
  GtkWidget *source_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(source_box, "highlight-source-row");

  self->source_icon = gtk_image_new_from_icon_name("document-open-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->source_icon), 12);
  gtk_box_append(GTK_BOX(source_box), self->source_icon);

  self->source_link = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->source_link), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(self->source_link), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(self->source_link, "highlight-source-link");
  gtk_widget_set_cursor_from_name(self->source_link, "pointer");
  gtk_box_append(GTK_BOX(source_box), self->source_link);

  /* Add click gesture to source link */
  GtkGesture *source_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(source_click), GDK_BUTTON_PRIMARY);
  g_signal_connect(source_click, "released", G_CALLBACK(on_source_clicked), self);
  gtk_widget_add_controller(source_box, GTK_EVENT_CONTROLLER(source_click));

  gtk_widget_set_visible(source_box, FALSE);
  self->author_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->author_label, "dim-label");
  gtk_widget_set_visible(self->author_label, FALSE);
  gtk_box_append(GTK_BOX(source_box), self->author_label);

  gtk_box_append(GTK_BOX(content_box), source_box);
  g_object_set_data(G_OBJECT(self), "source-box", source_box);

  /* Highlighter info row */
  self->highlighter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(self->highlighter_box, "highlight-meta-row");
  gtk_widget_set_margin_top(self->highlighter_box, 4);

  /* Highlighter avatar */
  self->highlighter_avatar = gtk_image_new_from_icon_name("avatar-default-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->highlighter_avatar), 20);
  gtk_widget_add_css_class(self->highlighter_avatar, "highlight-avatar");
  gtk_box_append(GTK_BOX(self->highlighter_box), self->highlighter_avatar);

  /* Highlighter name */
  self->highlighter_name = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(self->highlighter_name), 0.0);
  gtk_widget_add_css_class(self->highlighter_name, "highlight-author-name");
  gtk_widget_set_cursor_from_name(self->highlighter_name, "pointer");
  gtk_box_append(GTK_BOX(self->highlighter_box), self->highlighter_name);

  /* Add click gesture to highlighter */
  GtkGesture *highlighter_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(highlighter_click), GDK_BUTTON_PRIMARY);
  g_signal_connect(highlighter_click, "released", G_CALLBACK(on_highlighter_clicked), self);
  gtk_widget_add_controller(self->highlighter_box, GTK_EVENT_CONTROLLER(highlighter_click));

  /* Timestamp */
  self->timestamp_label = gtk_label_new(NULL);
  gtk_widget_add_css_class(self->timestamp_label, "dim-label");
  gtk_box_append(GTK_BOX(self->highlighter_box), self->timestamp_label);

  gtk_box_append(GTK_BOX(content_box), self->highlighter_box);

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  /* Uses shared session from gnostr_get_shared_soup_session() */
#endif

  self->source_type = GNOSTR_HIGHLIGHT_SOURCE_NONE;
}

GnostrHighlightCard *gnostr_highlight_card_new(void) {
  return g_object_new(GNOSTR_TYPE_HIGHLIGHT_CARD, NULL);
}

void gnostr_highlight_card_set_highlight(GnostrHighlightCard *self,
                                          const char *event_id,
                                          const char *highlighted_text,
                                          const char *context,
                                          const char *comment,
                                          gint64 created_at) {
  g_return_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self));

  g_clear_pointer(&self->event_id, g_free);
  self->event_id = g_strdup(event_id);

  /* Set highlighted text with markup for emphasis */
  if (GTK_IS_LABEL(self->highlighted_text)) {
    if (highlighted_text && *highlighted_text) {
      gchar *escaped = g_markup_escape_text(highlighted_text, -1);
      gchar *markup = g_strdup_printf("<i>\"%s\"</i>", escaped);
      gtk_label_set_markup(GTK_LABEL(self->highlighted_text), markup);
      g_free(markup);
      g_free(escaped);
    } else {
      gtk_label_set_text(GTK_LABEL(self->highlighted_text), _("(empty highlight)"));
    }
  }

  /* Set context if available */
  if (GTK_IS_LABEL(self->context_label)) {
    if (context && *context) {
      gchar *escaped = g_markup_escape_text(context, -1);
      gtk_label_set_text(GTK_LABEL(self->context_label), escaped);
      gtk_widget_set_visible(self->context_label, TRUE);
      g_free(escaped);
    } else {
      gtk_widget_set_visible(self->context_label, FALSE);
    }
  }

  /* Set comment if available */
  if (GTK_IS_LABEL(self->comment_label)) {
    if (comment && *comment) {
      gchar *escaped = g_markup_escape_text(comment, -1);
      gchar *markup = g_strdup_printf("<b>Note:</b> %s", escaped);
      gtk_label_set_markup(GTK_LABEL(self->comment_label), markup);
      gtk_widget_set_visible(self->comment_label, TRUE);
      g_free(markup);
      g_free(escaped);
    } else {
      gtk_widget_set_visible(self->comment_label, FALSE);
    }
  }

  /* Set timestamp */
  if (GTK_IS_LABEL(self->timestamp_label)) {
    gchar *ts = format_timestamp(created_at);
    gtk_label_set_text(GTK_LABEL(self->timestamp_label), ts);
    g_free(ts);
  }
}

void gnostr_highlight_card_set_source_note(GnostrHighlightCard *self,
                                            const char *source_event_id,
                                            const char *relay_hint) {
  g_return_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self));

  self->source_type = GNOSTR_HIGHLIGHT_SOURCE_NOTE;
  g_clear_pointer(&self->source_event_id, g_free);
  g_clear_pointer(&self->source_relay_hint, g_free);
  self->source_event_id = g_strdup(source_event_id);
  self->source_relay_hint = g_strdup(relay_hint);

  /* Update UI */
  GtkWidget *source_box = g_object_get_data(G_OBJECT(self), "source-box");
  if (source_box) {
    gtk_widget_set_visible(source_box, TRUE);
  }

  if (GTK_IS_IMAGE(self->source_icon)) {
    gtk_image_set_from_icon_name(GTK_IMAGE(self->source_icon), "mail-unread-symbolic");
  }

  if (GTK_IS_LABEL(self->source_link)) {
    if (source_event_id && strlen(source_event_id) >= 8) {
      gchar *truncated = g_strdup_printf("From note: %.8s...", source_event_id);
      gtk_label_set_text(GTK_LABEL(self->source_link), truncated);
      g_free(truncated);
    } else {
      gtk_label_set_text(GTK_LABEL(self->source_link), _("From a note"));
    }
  }
}

void gnostr_highlight_card_set_source_article(GnostrHighlightCard *self,
                                               const char *a_tag,
                                               const char *relay_hint) {
  g_return_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self));

  self->source_type = GNOSTR_HIGHLIGHT_SOURCE_ARTICLE;
  g_clear_pointer(&self->source_a_tag, g_free);
  g_clear_pointer(&self->source_relay_hint, g_free);
  self->source_a_tag = g_strdup(a_tag);
  self->source_relay_hint = g_strdup(relay_hint);

  /* Update UI */
  GtkWidget *source_box = g_object_get_data(G_OBJECT(self), "source-box");
  if (source_box) {
    gtk_widget_set_visible(source_box, TRUE);
  }

  if (GTK_IS_IMAGE(self->source_icon)) {
    gtk_image_set_from_icon_name(GTK_IMAGE(self->source_icon), "document-open-symbolic");
  }

  if (GTK_IS_LABEL(self->source_link)) {
    /* Parse a-tag to extract d-tag for display */
    if (a_tag) {
      gchar **parts = g_strsplit(a_tag, ":", 3);
      if (parts && parts[2] && *parts[2]) {
        gchar *text = g_strdup_printf("From article: %s", parts[2]);
        gtk_label_set_text(GTK_LABEL(self->source_link), text);
        g_free(text);
      } else {
        gtk_label_set_text(GTK_LABEL(self->source_link), _("From an article"));
      }
      g_strfreev(parts);
    } else {
      gtk_label_set_text(GTK_LABEL(self->source_link), _("From an article"));
    }
  }
}

void gnostr_highlight_card_set_source_url(GnostrHighlightCard *self,
                                           const char *url) {
  g_return_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self));

  self->source_type = GNOSTR_HIGHLIGHT_SOURCE_URL;
  g_clear_pointer(&self->source_url, g_free);
  self->source_url = g_strdup(url);

  /* Update UI */
  GtkWidget *source_box = g_object_get_data(G_OBJECT(self), "source-box");
  if (source_box) {
    gtk_widget_set_visible(source_box, TRUE);
  }

  if (GTK_IS_IMAGE(self->source_icon)) {
    gtk_image_set_from_icon_name(GTK_IMAGE(self->source_icon), "web-browser-symbolic");
  }

  if (GTK_IS_LABEL(self->source_link)) {
    if (url && *url) {
      /* Try to show just the domain */
      GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
      if (uri) {
        const char *host = g_uri_get_host(uri);
        gchar *text = g_strdup_printf("From: %s", host ? host : url);
        gtk_label_set_text(GTK_LABEL(self->source_link), text);
        g_free(text);
        g_uri_unref(uri);
      } else {
        gtk_label_set_text(GTK_LABEL(self->source_link), url);
      }
    }
  }
}

void gnostr_highlight_card_set_highlighter(GnostrHighlightCard *self,
                                            const char *pubkey_hex,
                                            const char *display_name,
                                            const char *avatar_url) {
  g_return_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self));

  g_clear_pointer(&self->highlighter_pubkey, g_free);
  self->highlighter_pubkey = g_strdup(pubkey_hex);

  /* Set display name */
  if (GTK_IS_LABEL(self->highlighter_name)) {
    if (display_name && *display_name) {
      gtk_label_set_text(GTK_LABEL(self->highlighter_name), display_name);
    } else if (pubkey_hex && strlen(pubkey_hex) >= 8) {
      gchar *truncated = g_strdup_printf("%.8s...", pubkey_hex);
      gtk_label_set_text(GTK_LABEL(self->highlighter_name), truncated);
      g_free(truncated);
    }
  }

  /* Load avatar if available */
  if (avatar_url && *avatar_url && GTK_IS_IMAGE(self->highlighter_avatar)) {
    /* Try cache first, then async download */
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      gtk_image_set_from_paintable(GTK_IMAGE(self->highlighter_avatar), GDK_PAINTABLE(cached));
      g_object_unref(cached);
    } else {
      gnostr_avatar_download_async(avatar_url, GTK_WIDGET(self->highlighter_avatar), NULL);
    }
  }
}

void gnostr_highlight_card_set_author(GnostrHighlightCard *self,
                                       const char *pubkey_hex,
                                       const char *display_name) {
  g_return_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self));

  g_clear_pointer(&self->author_pubkey, g_free);
  self->author_pubkey = g_strdup(pubkey_hex);

  if (GTK_IS_LABEL(self->author_label)) {
    if (display_name && *display_name) {
      gchar *text = g_strdup_printf("by %s", display_name);
      gtk_label_set_text(GTK_LABEL(self->author_label), text);
      gtk_widget_set_visible(self->author_label, TRUE);

      /* Make it clickable */
      gtk_widget_set_cursor_from_name(self->author_label, "pointer");

      /* Add click gesture */
      GtkGesture *author_click = gtk_gesture_click_new();
      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(author_click), GDK_BUTTON_PRIMARY);
      g_signal_connect(author_click, "released", G_CALLBACK(on_author_clicked), self);
      gtk_widget_add_controller(self->author_label, GTK_EVENT_CONTROLLER(author_click));

      g_free(text);
    } else if (pubkey_hex && strlen(pubkey_hex) >= 8) {
      gchar *text = g_strdup_printf("by %.8s...", pubkey_hex);
      gtk_label_set_text(GTK_LABEL(self->author_label), text);
      gtk_widget_set_visible(self->author_label, TRUE);
      g_free(text);
    } else {
      gtk_widget_set_visible(self->author_label, FALSE);
    }
  }
}

const char *gnostr_highlight_card_get_event_id(GnostrHighlightCard *self) {
  g_return_val_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self), NULL);
  return self->event_id;
}

const char *gnostr_highlight_card_get_highlighter_pubkey(GnostrHighlightCard *self) {
  g_return_val_if_fail(GNOSTR_IS_HIGHLIGHT_CARD(self), NULL);
  return self->highlighter_pubkey;
}
