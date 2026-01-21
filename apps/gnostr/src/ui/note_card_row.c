#include "note_card_row.h"
#include "og-preview-widget.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "gnostr-avatar-cache.h"
#include "../storage_ndb.h"
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/note-card-row.ui"

/* No longer using mutex - proper fix is at backend level */

struct _GnostrNoteCardRow {
  GtkWidget parent_instance;
  // template children
  GtkWidget *root;
  GtkWidget *btn_avatar;
  GtkWidget *btn_display_name;
  GtkWidget *btn_menu;
  GtkWidget *btn_reply;
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
  SoupSession *media_session;
  GHashTable *media_cancellables; /* URL -> GCancellable */
#endif
  guint depth;
  char *id_hex;
  char *root_id;
  char *pubkey_hex;
  gint64 created_at;
  guint timestamp_timer_id;
  OgPreviewWidget *og_preview;
};

G_DEFINE_TYPE(GnostrNoteCardRow, gnostr_note_card_row, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_NOSTR_TARGET,
  SIGNAL_OPEN_URL,
  SIGNAL_REQUEST_EMBED,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_REPLY_REQUESTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void gnostr_note_card_row_dispose(GObject *obj) {
  GnostrNoteCardRow *self = (GnostrNoteCardRow*)obj;
  
  /* Remove timestamp timer */
  if (self->timestamp_timer_id > 0) {
    g_source_remove(self->timestamp_timer_id);
    self->timestamp_timer_id = 0;
  }
  
#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) { g_cancellable_cancel(self->avatar_cancellable); g_clear_object(&self->avatar_cancellable); }
  /* Cancel all media fetches */
  if (self->media_cancellables) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->media_cancellables);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      GCancellable *cancellable = G_CANCELLABLE(value);
      if (cancellable) g_cancellable_cancel(cancellable);
    }
    g_clear_pointer(&self->media_cancellables, g_hash_table_unref);
  }
  g_clear_object(&self->media_session);
#endif
  /* Don't manually clear og_preview - it's a child widget that will be
   * automatically disposed when the template is disposed */
  self->og_preview = NULL;
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

/* Helper: convert 64-char hex string to 32 bytes */
static gboolean hex_to_bytes_32(const char *hex, unsigned char out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + i*2, "%2x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* Ensure NostrDB is initialized (idempotent). Mirrors logic in main_app.c */
static void ensure_ndb_initialized(void) {
  /* storage_ndb_init is idempotent; if already initialized it returns 1 */
  gchar *dbdir = g_build_filename(g_get_user_cache_dir(), "gnostr", "ndb", NULL);
  (void)g_mkdir_with_parents(dbdir, 0700);
  const char *opts = "{\"mapsize\":1073741824,\"ingester_threads\":4,\"ingest_skip_validation\":1}";
  storage_ndb_init(dbdir, opts);
  g_free(dbdir);
}

static void show_json_viewer(GnostrNoteCardRow *self) {
  if (!self || !self->id_hex) {
    g_warning("No event ID available to fetch JSON");
    return;
  }
  
  /* Ensure DB is initialized (safe if already initialized) */
  ensure_ndb_initialized();
  
  /* Fetch event JSON from NostrDB using nontxn helper with built-in retries */
  char *event_json = NULL;
  int json_len = 0;
  
  int rc = storage_ndb_get_note_by_id_nontxn(self->id_hex, &event_json, &json_len);

  if (rc != 0 || !event_json) {
    g_warning("Failed to fetch event JSON from NostrDB (id=%s, rc=%d)", 
              self->id_hex, rc);
    return;
  }

  /* Get the toplevel window */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

  /* Create dialog */
  GtkWidget *dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Event JSON");
  gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 500);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  if (parent) gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);

  /* Create scrolled window */
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), 
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  /* Create text view */
  GtkWidget *text_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_NONE);
  gtk_widget_set_margin_start(text_view, 12);
  gtk_widget_set_margin_end(text_view, 12);
  gtk_widget_set_margin_top(text_view, 12);
  gtk_widget_set_margin_bottom(text_view, 12);

  /* Set the JSON content */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
  gtk_text_buffer_set_text(buffer, event_json, -1);
  
  /* Free the fetched JSON (allocated with malloc in storage_ndb_get_note_by_id_nontxn) */
  free(event_json);

  /* Assemble the dialog */
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text_view);
  gtk_window_set_child(GTK_WINDOW(dialog), scrolled);

  gtk_window_present(GTK_WINDOW(dialog));
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  show_json_viewer(self);
}

static void on_reply_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_REPLY_REQUESTED], 0,
                  self->id_hex, self->root_id, self->pubkey_hex);
  }
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
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_menu);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_reply);
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
  signals[SIGNAL_REPLY_REQUESTED] = g_signal_new("reply-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void gnostr_note_card_row_init(GnostrNoteCardRow *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_reply),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Note Reply", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_menu),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Note More", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_avatar),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Open Profile", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_display_name),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Open Profile", -1);
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
  /* Connect menu button to show JSON viewer */
  if (GTK_IS_BUTTON(self->btn_menu)) {
    g_signal_connect(self->btn_menu, "clicked", G_CALLBACK(on_menu_clicked), self);
  }
  /* Connect reply button */
  if (GTK_IS_BUTTON(self->btn_reply)) {
    g_signal_connect(self->btn_reply, "clicked", G_CALLBACK(on_reply_clicked), self);
  }
#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  self->media_session = soup_session_new();
  soup_session_set_timeout(self->media_session, 30); /* 30 second timeout for media */
  self->media_cancellables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
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

/* Callback for media image loading */
static void on_media_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  GtkPicture *picture = GTK_PICTURE(user_data);
  GError *error = NULL;
  
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("Media: Failed to load image: %s", error->message);
    }
    g_error_free(error);
    /* Release the reference we took in load_media_image */
    g_object_unref(picture);
    return;
  }
  
  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    /* Release the reference we took in load_media_image */
    g_object_unref(picture);
    return;
  }
  
  /* Create texture from bytes */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);
  
  if (error) {
    g_debug("Media: Failed to create texture: %s", error->message);
    g_error_free(error);
    /* Release the reference we took in load_media_image */
    g_object_unref(picture);
    return;
  }
  
  /* Update picture widget - check if still valid */
  if (GTK_IS_PICTURE(picture)) {
    gtk_picture_set_paintable(picture, GDK_PAINTABLE(texture));
  }
  
  g_object_unref(texture);
  /* Release the reference we took in load_media_image */
  g_object_unref(picture);
}

/* Load media image asynchronously */
static void load_media_image(GnostrNoteCardRow *self, const char *url, GtkPicture *picture) {
  if (!url || !*url || !GTK_IS_PICTURE(picture)) return;
  
  /* Create cancellable for this request */
  GCancellable *cancellable = g_cancellable_new();
  g_hash_table_insert(self->media_cancellables, g_strdup(url), cancellable);
  
  /* Create HTTP request */
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_debug("Media: Invalid image URL: %s", url);
    return;
  }
  
  /* Take a reference to the picture to keep it alive during async operation */
  g_object_ref(picture);
  
  /* Start async fetch */
  soup_session_send_and_read_async(
    self->media_session,
    msg,
    G_PRIORITY_LOW,
    cancellable,
    on_media_image_loaded,
    picture
  );
  
  g_object_unref(msg);
}
#endif

/* Use centralized avatar cache API (avatar_cache.h) */

void gnostr_note_card_row_set_author(GnostrNoteCardRow *self, const char *display_name, const char *handle, const char *avatar_url) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (GTK_IS_LABEL(self->lbl_display)) gtk_label_set_text(GTK_LABEL(self->lbl_display), (display_name && *display_name) ? display_name : (handle ? handle : _("Anonymous")));
  if (GTK_IS_LABEL(self->lbl_handle))  gtk_label_set_text(GTK_LABEL(self->lbl_handle), (handle && *handle) ? handle : "@anon");
  g_clear_pointer(&self->avatar_url, g_free);
  self->avatar_url = g_strdup(avatar_url);
  set_avatar_initials(self, display_name, handle);
  
#ifdef HAVE_SOUP3
  /* OPTIMIZATION: Check cache before downloading */
  if (avatar_url && *avatar_url && GTK_IS_PICTURE(self->avatar_image)) {
    g_message("note_card: set_author called with avatar_url=%s", avatar_url);
    /* First, try to load from cache (memory or disk) */
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      /* Cache hit! Apply immediately without HTTP request */
      g_message("note_card: avatar cache HIT, displaying url=%s", avatar_url);
      gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->avatar_image, TRUE);
      if (GTK_IS_WIDGET(self->avatar_initials)) {
        gtk_widget_set_visible(self->avatar_initials, FALSE);
      }
      g_object_unref(cached);
    } else {
      /* Cache miss - download asynchronously */
      g_message("note_card: avatar cache MISS, downloading url=%s", avatar_url);
      gnostr_avatar_download_async(avatar_url, self->avatar_image, self->avatar_initials);
    }
  } else {
    if (!avatar_url || !*avatar_url) {
      g_debug("note_card: set_author called with NO avatar_url");
    } else if (!GTK_IS_PICTURE(self->avatar_image)) {
      g_warning("note_card: avatar_image is not a GtkPicture!");
    }
  }
#endif
}

/* Timer callback to update timestamp display */
static gboolean update_timestamp_tick(gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->lbl_timestamp)) {
    return G_SOURCE_REMOVE;
  }
  
  if (self->created_at > 0) {
    time_t now = time(NULL);
    long diff = (long)(now - (time_t)self->created_at);
    if (diff < 0) diff = 0;
    char buf[32];
    if (diff < 5) g_strlcpy(buf, "now", sizeof(buf));
    else if (diff < 3600) g_snprintf(buf, sizeof(buf), "%ldm", diff/60);
    else if (diff < 86400) g_snprintf(buf, sizeof(buf), "%ldh", diff/3600);
    else g_snprintf(buf, sizeof(buf), "%ldd", diff/86400);
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), buf);
  }
  
  return G_SOURCE_CONTINUE;
}

void gnostr_note_card_row_set_timestamp(GnostrNoteCardRow *self, gint64 created_at, const char *fallback_ts) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->lbl_timestamp)) return;
  
  /* Store the created_at timestamp */
  self->created_at = created_at;
  
  /* Update the display immediately */
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
    
    /* Remove old timer if exists */
    if (self->timestamp_timer_id > 0) {
      g_source_remove(self->timestamp_timer_id);
    }
    
    /* Add timer to update every 60 seconds */
    self->timestamp_timer_id = g_timeout_add_seconds(60, update_timestamp_tick, self);
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), fallback_ts ? fallback_ts : "now");
  }
}

static gchar *escape_markup(const char *s) {
  if (!s) return g_strdup("");
  return g_markup_escape_text(s, -1);
}

static gboolean is_image_url(const char *u) {
  if (!u) return FALSE;
  gchar *lower = g_ascii_strdown(u, -1);
  const char *exts[] = {".jpg",".jpeg",".png",".gif",".webp",".bmp",".svg"};
  gboolean result = FALSE;
  for (guint i=0; i<G_N_ELEMENTS(exts); i++) {
    if (g_str_has_suffix(lower, exts[i])) {
      result = TRUE;
      break;
    }
  }
  g_free(lower);
  return result;
}

static gboolean is_video_url(const char *u) {
  if (!u) return FALSE;
  gchar *lower = g_ascii_strdown(u, -1);
  const char *exts[] = {".mp4",".webm",".mov",".avi",".mkv",".m4v"};
  gboolean result = FALSE;
  for (guint i=0; i<G_N_ELEMENTS(exts); i++) {
    if (g_str_has_suffix(lower, exts[i])) {
      result = TRUE;
      break;
    }
  }
  g_free(lower);
  return result;
}

static gboolean is_media_url(const char *u) {
  return is_image_url(u) || is_video_url(u);
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
  gchar *markup = out->len ? g_string_free(out, FALSE) : g_string_free(out, TRUE);
  gboolean markup_allocated = (markup != NULL);
  if (!markup) markup = escape_markup(content);
  gtk_label_set_use_markup(GTK_LABEL(self->content_label), TRUE);
  gtk_label_set_markup(GTK_LABEL(self->content_label), markup ? markup : "");
  if (markup_allocated || markup) g_free(markup); /* Only free once */

  /* Media detection: detect images and videos in content and display them */
  if (self->media_box && GTK_IS_BOX(self->media_box)) {
    /* Clear existing media widgets */
    GtkWidget *child = gtk_widget_get_first_child(self->media_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(self->media_box), child);
      child = next;
    }
    gtk_widget_set_visible(self->media_box, FALSE);
    
    if (content) {
      gchar **tokens = g_strsplit_set(content, " \n\t", -1);
      for (guint i=0; tokens && tokens[i]; i++) {
        const char *url = tokens[i];
        if (!url || !*url) continue;
        
        /* Check if it's an HTTP(S) URL */
        if (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")) {
          /* Handle images */
          if (is_image_url(url)) {
            GtkWidget *pic = gtk_picture_new();
            gtk_widget_add_css_class(pic, "note-media-image");
            gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
            gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
            gtk_widget_set_size_request(pic, -1, 300);
            gtk_widget_set_hexpand(pic, TRUE);
            gtk_widget_set_vexpand(pic, FALSE);
            gtk_box_append(GTK_BOX(self->media_box), pic);
            gtk_widget_set_visible(self->media_box, TRUE);
            
#ifdef HAVE_SOUP3
            /* Load image asynchronously */
            load_media_image(self, url, GTK_PICTURE(pic));
#endif
          }
          /* Handle videos */
          else if (is_video_url(url)) {
            GtkWidget *video = gtk_video_new();
            gtk_widget_add_css_class(video, "note-media-video");
            gtk_video_set_autoplay(GTK_VIDEO(video), FALSE);
            gtk_video_set_loop(GTK_VIDEO(video), TRUE);
            gtk_widget_set_size_request(video, -1, 300);
            gtk_widget_set_hexpand(video, TRUE);
            gtk_widget_set_vexpand(video, FALSE);
            
            /* Set video file */
            GFile *file = g_file_new_for_uri(url);
            gtk_video_set_file(GTK_VIDEO(video), file);
            g_object_unref(file);
            
            gtk_box_append(GTK_BOX(self->media_box), video);
            gtk_widget_set_visible(self->media_box, TRUE);
          }
        }
      }
      g_strfreev(tokens);
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
  
  /* Apply CSS class for depth styling using GTK4 API */
  GtkWidget *widget = GTK_WIDGET(self);
  
  /* Remove existing depth classes */
  for (guint i = 1; i <= 4; i++) {
    gchar *class_name = g_strdup_printf("thread-depth-%u", i);
    gtk_widget_remove_css_class(widget, class_name);
    g_free(class_name);
  }
  
  /* Add appropriate depth class */
  if (depth > 0 && depth <= 4) {
    gchar *class_name = g_strdup_printf("thread-depth-%u", depth);
    gtk_widget_add_css_class(widget, class_name);
    g_free(class_name);
  }
  
  /* Add thread-reply class for any depth > 0 */
  if (depth > 0) {
    gtk_widget_add_css_class(widget, "thread-reply");
  } else {
    gtk_widget_remove_css_class(widget, "thread-reply");
  }
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
