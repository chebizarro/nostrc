#include "note_card_row.h"
#include "og-preview-widget.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/note-card-row.ui"

struct _GnostrNoteCardRow {
  GtkWidget parent_instance;
  // template children
  GtkWidget *root;
  GtkWidget *btn_avatar;
  GtkWidget *btn_display_name;
  GtkWidget *avatar_box;
  GtkWidget *avatar_initials;
  GtkWidget *avatar_image;
  GtkWidget *lbl_display;
  GtkWidget *lbl_handle;
  GtkWidget *lbl_timestamp;
  GtkWidget *content_label;
  GtkWidget *media_box;
  GtkWidget *embed_box;
  GtkWidget *og_preview_container;
  GtkWidget *actions_box;
  // state
  char *avatar_url;
#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
#endif
  guint depth;
  char *id_hex;
  char *root_id;
  char *pubkey_hex;
  OgPreviewWidget *og_preview;
};

G_DEFINE_TYPE(GnostrNoteCardRow, gnostr_note_card_row, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_NOSTR_TARGET,
  SIGNAL_OPEN_URL,
  SIGNAL_REQUEST_EMBED,
  SIGNAL_OPEN_PROFILE,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void gnostr_note_card_row_dispose(GObject *obj) {
  GnostrNoteCardRow *self = (GnostrNoteCardRow*)obj;
#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) { g_cancellable_cancel(self->avatar_cancellable); g_clear_object(&self->avatar_cancellable); }
#endif
  g_clear_object(&self->og_preview);
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_NOTE_CARD_ROW);
  self->root = NULL; self->avatar_box = NULL; self->avatar_initials = NULL; self->avatar_image = NULL;
  self->lbl_display = NULL; self->lbl_handle = NULL; self->lbl_timestamp = NULL; self->content_label = NULL;
  self->media_box = NULL; self->embed_box = NULL; self->og_preview_container = NULL; self->actions_box = NULL;
  G_OBJECT_CLASS(gnostr_note_card_row_parent_class)->dispose(obj);
}

static void gnostr_note_card_row_finalize(GObject *obj) {
  GnostrNoteCardRow *self = (GnostrNoteCardRow*)obj;
  g_clear_pointer(&self->avatar_url, g_free);
  g_clear_pointer(&self->id_hex, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  G_OBJECT_CLASS(gnostr_note_card_row_parent_class)->finalize(obj);
}

static void on_avatar_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_display_name_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static gboolean on_content_activate_link(GtkLabel *label, const char *uri, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)label;
  if (!self || !uri) return FALSE;
  /* nostr: URIs and bech32 entities */
  if (g_str_has_prefix(uri, "nostr:") || g_str_has_prefix(uri, "note1") || g_str_has_prefix(uri, "npub1") ||
      g_str_has_prefix(uri, "nevent1") || g_str_has_prefix(uri, "nprofile1") || g_str_has_prefix(uri, "naddr1")) {
    g_signal_emit(self, signals[SIGNAL_OPEN_NOSTR_TARGET], 0, uri);
    return TRUE;
  }
  if (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")) {
    g_signal_emit(self, signals[SIGNAL_OPEN_URL], 0, uri);
    return TRUE;
  }
  return FALSE;
}

static void gnostr_note_card_row_class_init(GnostrNoteCardRowClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  gclass->dispose = gnostr_note_card_row_dispose;
  gclass->finalize = gnostr_note_card_row_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, root);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_avatar);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_display_name);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, avatar_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, avatar_initials);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, avatar_image);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_display);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_handle);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_timestamp);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, content_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, media_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, embed_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, og_preview_container);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, actions_box);

  signals[SIGNAL_OPEN_NOSTR_TARGET] = g_signal_new("open-nostr-target",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_OPEN_URL] = g_signal_new("open-url",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_REQUEST_EMBED] = g_signal_new("request-embed",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_note_card_row_init(GnostrNoteCardRow *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_widget_add_css_class(GTK_WIDGET(self), "note-card");
  if (GTK_IS_LABEL(self->content_label)) {
    gtk_label_set_wrap(GTK_LABEL(self->content_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->content_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(self->content_label), FALSE);
    g_signal_connect(self->content_label, "activate-link", G_CALLBACK(on_content_activate_link), self);
  }
  /* Connect profile click handlers */
  if (GTK_IS_BUTTON(self->btn_avatar)) {
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_display_name)) {
    g_signal_connect(self->btn_display_name, "clicked", G_CALLBACK(on_display_name_clicked), self);
  }
#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
#endif
}

GnostrNoteCardRow *gnostr_note_card_row_new(void) {
  return g_object_new(GNOSTR_TYPE_NOTE_CARD_ROW, NULL);
}

static void set_avatar_initials(GnostrNoteCardRow *self, const char *display, const char *handle) {
  if (!self || !GTK_IS_LABEL(self->avatar_initials)) return;
  const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
  char initials[3] = {0}; int i = 0;
  for (const char *p = src; *p && i < 2; p++) if (g_ascii_isalnum(*p)) initials[i++] = g_ascii_toupper(*p);
  if (i == 0) { initials[0] = 'A'; initials[1] = 'N'; }
  gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
  if (self->avatar_image) gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
}

#ifdef HAVE_SOUP3
static void on_avatar_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
  if (!bytes) { g_clear_error(&error); set_avatar_initials(self, NULL, NULL); return; }
  GdkTexture *tex = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);
  if (!tex) { g_clear_error(&error); set_avatar_initials(self, NULL, NULL); return; }
  if (GTK_IS_PICTURE(self->avatar_image)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(tex));
    gtk_widget_set_visible(self->avatar_image, TRUE);
  }
  if (GTK_IS_WIDGET(self->avatar_initials)) gtk_widget_set_visible(self->avatar_initials, FALSE);
  g_object_unref(tex);
}
#endif

void gnostr_note_card_row_set_author(GnostrNoteCardRow *self, const char *display_name, const char *handle, const char *avatar_url) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (GTK_IS_LABEL(self->lbl_display)) gtk_label_set_text(GTK_LABEL(self->lbl_display), (display_name && *display_name) ? display_name : (handle ? handle : _("Anonymous")));
  if (GTK_IS_LABEL(self->lbl_handle))  gtk_label_set_text(GTK_LABEL(self->lbl_handle), (handle && *handle) ? handle : "@anon");
  g_clear_pointer(&self->avatar_url, g_free);
  self->avatar_url = g_strdup(avatar_url);
  set_avatar_initials(self, display_name, handle);
#ifdef HAVE_SOUP3
  if (avatar_url && *avatar_url) {
    SoupSession *sess = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", avatar_url);
    soup_session_send_and_read_async(sess, msg, G_PRIORITY_DEFAULT, self->avatar_cancellable, on_avatar_http_done, self);
    g_object_unref(msg);
    g_object_unref(sess);
  }
#endif
}

void gnostr_note_card_row_set_timestamp(GnostrNoteCardRow *self, gint64 created_at, const char *fallback_ts) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->lbl_timestamp)) return;
  if (created_at > 0) {
    time_t now = time(NULL);
    long diff = (long)(now - (time_t)created_at);
    if (diff < 0) diff = 0;
    char buf[32];
    if (diff < 5) g_strlcpy(buf, "now", sizeof(buf));
    else if (diff < 3600) g_snprintf(buf, sizeof(buf), "%ldm", diff/60);
    else if (diff < 86400) g_snprintf(buf, sizeof(buf), "%ldh", diff/3600);
    else g_snprintf(buf, sizeof(buf), "%ldd", diff/86400);
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), buf);
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), fallback_ts ? fallback_ts : "now");
  }
}

static gchar *escape_markup(const char *s) {
  if (!s) return g_strdup("");
  return g_markup_escape_text(s, -1);
}

static gboolean is_media_url(const char *u) {
  if (!u) return FALSE;
  const char *exts[] = {".jpg",".jpeg",".png",".gif",".webp",".mp4",".webm"};
  for (guint i=0;i<G_N_ELEMENTS(exts);i++) if (g_str_has_suffix(g_ascii_strdown(u,-1), exts[i])) return TRUE;
  return FALSE;
}

void gnostr_note_card_row_set_content(GnostrNoteCardRow *self, const char *content) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->content_label)) return;
  /* naive parse: split by space and wrap URLs and nostr ids as links */
  GString *out = g_string_new("");
  if (content && *content) {
    gchar **tokens = g_strsplit_set(content, " \n\t", -1);
    for (guint i=0; tokens && tokens[i]; i++) {
      const char *t = tokens[i];
      if (t[0]=='\0') { g_string_append(out, " "); continue; }
      gboolean link = FALSE;
      if (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://") ||
          g_str_has_prefix(t, "nostr:") || g_str_has_prefix(t, "note1") || g_str_has_prefix(t, "npub1") ||
          g_str_has_prefix(t, "nevent1") || g_str_has_prefix(t, "nprofile1") || g_str_has_prefix(t, "naddr1"))
        link = TRUE;
      if (link) {
        gchar *esc = g_markup_escape_text(t, -1);
        g_string_append_printf(out, "<a href=\"%s\">%s</a>", esc, esc);
        g_free(esc);
      } else {
        gchar *esc = g_markup_escape_text(t, -1);
        g_string_append(out, esc); g_free(esc);
      }
      g_string_append_c(out, ' ');
    }
    g_strfreev(tokens);
  }
  gchar *markup = out->len ? g_string_free(out, FALSE) : g_string_free(out, TRUE), *empty = NULL;
  if (!markup) markup = empty = escape_markup(content);
  gtk_label_set_use_markup(GTK_LABEL(self->content_label), TRUE);
  gtk_label_set_markup(GTK_LABEL(self->content_label), markup ? markup : "");
  g_free(markup);
  g_free(empty);

  /* Very simple media detection: if any line looks like media URL, create a GtkPicture under media_box */
  if (self->media_box && GTK_IS_BOX(self->media_box)) {
    gtk_widget_set_visible(self->media_box, FALSE);
    if (content) {
      gchar **lines = g_strsplit(content, "\n", -1);
      for (guint i=0; lines && lines[i]; i++) {
        const char *u = lines[i];
        if (g_str_has_prefix(u, "http://") || g_str_has_prefix(u, "https://")) {
          if (is_media_url(u)) {
            GtkWidget *pic = gtk_picture_new(); /* TODO: fetch via libsoup and set paintable async */
            gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
            gtk_widget_set_hexpand(pic, TRUE);
            gtk_widget_set_vexpand(pic, FALSE);
            gtk_box_append(GTK_BOX(self->media_box), pic);
            gtk_widget_set_visible(self->media_box, TRUE);
            break;
          }
        }
      }
      g_strfreev(lines);
    }
  }

  /* Detect first NIP-19/21 reference and request an embed */
  if (self->embed_box && GTK_IS_WIDGET(self->embed_box)) {
    gtk_widget_set_visible(self->embed_box, FALSE);
    const char *p = content;
    const char *found = NULL;
    if (p && *p) {
      /* naive token scan */
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      for (guint i=0; tokens && tokens[i]; i++) {
        const char *t = tokens[i]; if (!t || !*t) continue;
        if (g_str_has_prefix(t, "nostr:") || g_str_has_prefix(t, "note1") || g_str_has_prefix(t, "nevent1") || g_str_has_prefix(t, "naddr1")) { found = t; break; }
      }
      if (tokens) g_strfreev(tokens);
    }
    if (found) {
      /* show skeleton */
      GtkWidget *sk = gtk_label_new("Loading embed…");
      gtk_widget_set_halign(sk, GTK_ALIGN_START);
      gtk_widget_set_valign(sk, GTK_ALIGN_START);
      gtk_widget_set_margin_start(sk, 6);
      gtk_widget_set_margin_end(sk, 6);
      gtk_widget_set_margin_top(sk, 4);
      gtk_widget_set_margin_bottom(sk, 4);
      /* Replace child of frame */
      if (GTK_IS_FRAME(self->embed_box)) gtk_frame_set_child(GTK_FRAME(self->embed_box), sk);
      gtk_widget_set_visible(self->embed_box, TRUE);
      g_signal_emit(self, signals[SIGNAL_REQUEST_EMBED], 0, found);
    }
  }

  /* Detect first HTTP(S) URL and create OG preview */
  if (self->og_preview_container && GTK_IS_BOX(self->og_preview_container)) {
    /* Clear any existing preview */
    if (self->og_preview) {
      gtk_box_remove(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
      self->og_preview = NULL;
    }
    gtk_widget_set_visible(self->og_preview_container, FALSE);
    
    const char *p = content;
    const char *url_start = NULL;
    if (p && *p) {
      /* Find first HTTP(S) URL */
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;
        if (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://")) {
          /* Skip media URLs */
          if (!is_media_url(t)) {
            url_start = t;
            break;
          }
        }
      }
      
      if (url_start) {
        /* Create OG preview widget */
        self->og_preview = og_preview_widget_new();
        gtk_box_append(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
        gtk_widget_set_visible(self->og_preview_container, TRUE);
        
        /* Set URL to fetch metadata */
        og_preview_widget_set_url(self->og_preview, url_start);
      }
      
      if (tokens) g_strfreev(tokens);
    }
  }
}

void gnostr_note_card_row_set_depth(GnostrNoteCardRow *self, guint depth) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  self->depth = depth;
  gtk_widget_set_margin_start(GTK_WIDGET(self), depth * 16);
}

void gnostr_note_card_row_set_ids(GnostrNoteCardRow *self, const char *id_hex, const char *root_id, const char *pubkey_hex) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  g_free(self->id_hex); self->id_hex = g_strdup(id_hex);
  g_free(self->root_id); self->root_id = g_strdup(root_id);
  g_free(self->pubkey_hex); self->pubkey_hex = g_strdup(pubkey_hex);
}

/* Public helper to set the embed mini-card content */
void gnostr_note_card_row_set_embed(GnostrNoteCardRow *self, const char *title, const char *snippet) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_FRAME(self->embed_box)) return;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *lbl_title = gtk_label_new(title ? title : "");
  GtkWidget *lbl_snip = gtk_label_new(snippet ? snippet : "");
  gtk_widget_add_css_class(lbl_title, "note-author");
  gtk_widget_add_css_class(lbl_snip, "note-content");
  gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
  gtk_label_set_xalign(GTK_LABEL(lbl_snip), 0.0);
  gtk_box_append(GTK_BOX(box), lbl_title);
  gtk_box_append(GTK_BOX(box), lbl_snip);
  gtk_frame_set_child(GTK_FRAME(self->embed_box), box);
  gtk_widget_set_visible(self->embed_box, TRUE);
}

/* Rich embed variant: adds meta line between title and snippet */
void gnostr_note_card_row_set_embed_rich(GnostrNoteCardRow *self, const char *title, const char *meta, const char *snippet) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_FRAME(self->embed_box)) return;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *lbl_title = gtk_label_new(title ? title : "");
  GtkWidget *lbl_meta  = gtk_label_new(meta ? meta : "");
  GtkWidget *lbl_snip  = gtk_label_new(snippet ? snippet : "");
  gtk_widget_add_css_class(lbl_title, "note-author");
  gtk_widget_add_css_class(lbl_meta,  "note-meta");
  gtk_widget_add_css_class(lbl_snip,  "note-content");
  gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
  gtk_label_set_xalign(GTK_LABEL(lbl_meta),  0.0);
  gtk_label_set_xalign(GTK_LABEL(lbl_snip),  0.0);
  gtk_box_append(GTK_BOX(box), lbl_title);
  gtk_box_append(GTK_BOX(box), lbl_meta);
  gtk_box_append(GTK_BOX(box), lbl_snip);
  gtk_frame_set_child(GTK_FRAME(self->embed_box), box);
  gtk_widget_set_visible(self->embed_box, TRUE);
}
