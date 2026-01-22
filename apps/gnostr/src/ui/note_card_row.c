#include "note_card_row.h"
#include "og-preview-widget.h"
#include "gnostr-image-viewer.h"
#include "gnostr-video-player.h"
#include "gnostr-note-embed.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "gnostr-avatar-cache.h"
#include "../util/nip05.h"
#include "../util/imeta.h"
#include "../util/zap.h"
#include "../storage_ndb.h"
#include <nostr/nip19/nip19.h>
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
  GtkWidget *btn_repost;
  GtkWidget *btn_like;
  GtkWidget *lbl_like_count;
  GtkWidget *btn_zap;
  GtkWidget *lbl_zap_count;
  GtkWidget *btn_bookmark;
  GtkWidget *btn_thread;
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
  GtkWidget *repost_popover;  /* popover menu for repost/quote options */
  GtkWidget *menu_popover;    /* popover menu for more options (JSON, mute, etc.) */
  /* Reply indicator widgets */
  GtkWidget *reply_indicator_box;
  GtkWidget *reply_indicator_label;
  /* Reply count widgets (thread root indicator) */
  GtkWidget *reply_count_box;
  GtkWidget *reply_count_label;
  guint reply_count;
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
  char *parent_id;
  char *pubkey_hex;
  char *parent_pubkey;
  gint64 created_at;
  guint timestamp_timer_id;
  OgPreviewWidget *og_preview;
  /* NIP-21 embedded note widget */
  GnostrNoteEmbed *note_embed;
  /* NIP-05 verification state */
  char *nip05;
  GtkWidget *nip05_badge;
  GCancellable *nip05_cancellable;
  /* Reply state */
  gboolean is_reply;
  gboolean is_thread_root;
  /* Bookmark state */
  gboolean is_bookmarked;
  /* Like state (NIP-25 reactions) */
  gboolean is_liked;
  guint like_count;
  /* Zap state */
  gint64 zap_total_msat;
  guint zap_count;
  gchar *author_lud16;  /* Author's lightning address from profile */
  /* Content state (plain text for clipboard) */
  gchar *content_text;
  /* NIP-09: Track if this is the current user's own note (for delete option) */
  gboolean is_own_note;
  GtkWidget *delete_btn;  /* Reference to delete button for visibility toggle */
};

G_DEFINE_TYPE(GnostrNoteCardRow, gnostr_note_card_row, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_NOSTR_TARGET,
  SIGNAL_OPEN_URL,
  SIGNAL_REQUEST_EMBED,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_REPLY_REQUESTED,
  SIGNAL_REPOST_REQUESTED,
  SIGNAL_QUOTE_REQUESTED,
  SIGNAL_LIKE_REQUESTED,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_VIEW_THREAD_REQUESTED,
  SIGNAL_MUTE_USER_REQUESTED,
  SIGNAL_MUTE_THREAD_REQUESTED,
  SIGNAL_SHOW_TOAST,
  SIGNAL_BOOKMARK_TOGGLED,
  SIGNAL_REPORT_NOTE_REQUESTED,
  SIGNAL_SHARE_NOTE_REQUESTED,
  SIGNAL_SEARCH_HASHTAG,
  SIGNAL_NAVIGATE_TO_NOTE,
  SIGNAL_DELETE_NOTE_REQUESTED,
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

  /* Cancel NIP-05 verification */
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
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
  /* Clean up the repost popover before disposing template */
  if (self->repost_popover) {
    gtk_widget_unparent(self->repost_popover);
    self->repost_popover = NULL;
  }
  /* Clean up the menu popover before disposing template */
  if (self->menu_popover) {
    gtk_widget_unparent(self->menu_popover);
    self->menu_popover = NULL;
  }
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_NOTE_CARD_ROW);
  self->root = NULL; self->avatar_box = NULL; self->avatar_initials = NULL; self->avatar_image = NULL;
  self->lbl_display = NULL; self->lbl_handle = NULL; self->lbl_timestamp = NULL; self->content_label = NULL;
  self->media_box = NULL; self->embed_box = NULL; self->og_preview_container = NULL; self->actions_box = NULL;
  self->btn_repost = NULL; self->btn_like = NULL; self->btn_bookmark = NULL; self->btn_thread = NULL;
  self->reply_indicator_box = NULL; self->reply_indicator_label = NULL;
  self->reply_count_box = NULL; self->reply_count_label = NULL;
  G_OBJECT_CLASS(gnostr_note_card_row_parent_class)->dispose(obj);
}

static void gnostr_note_card_row_finalize(GObject *obj) {
  GnostrNoteCardRow *self = (GnostrNoteCardRow*)obj;
  g_clear_pointer(&self->avatar_url, g_free);
  g_clear_pointer(&self->id_hex, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->parent_id, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->parent_pubkey, g_free);
  g_clear_pointer(&self->nip05, g_free);
  g_clear_pointer(&self->author_lud16, g_free);
  g_clear_pointer(&self->content_text, g_free);
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

/* Callback for embedded profile click (NIP-21) */
static void on_embed_profile_clicked(GnostrNoteEmbed *embed, const char *pubkey_hex, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)embed;
  if (self && pubkey_hex && *pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
  }
}

static gboolean on_content_activate_link(GtkLabel *label, const char *uri, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)label;
  if (!self || !uri) return FALSE;
  /* Hashtag links - emit search signal */
  if (g_str_has_prefix(uri, "hashtag:")) {
    const char *tag = uri + 8; /* Skip "hashtag:" prefix */
    if (tag && *tag) {
      g_signal_emit(self, signals[SIGNAL_SEARCH_HASHTAG], 0, tag);
    }
    return TRUE;
  }
  /* nostr: URIs and bech32 entities */
  if (g_str_has_prefix(uri, "nostr:") || g_str_has_prefix(uri, "note1") || g_str_has_prefix(uri, "npub1") ||
      g_str_has_prefix(uri, "nevent1") || g_str_has_prefix(uri, "nprofile1") || g_str_has_prefix(uri, "naddr1")) {
    g_signal_emit(self, signals[SIGNAL_OPEN_NOSTR_TARGET], 0, uri);
    return TRUE;
  }
  if (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")) {
    /* Open URL in the default browser using GtkUriLauncher */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, parent, NULL, NULL, NULL);
    g_object_unref(launcher);
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

static void on_view_json_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }
  show_json_viewer(self);
}

static void on_mute_user_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->pubkey_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Emit signal to mute this user */
  g_signal_emit(self, signals[SIGNAL_MUTE_USER_REQUESTED], 0, self->pubkey_hex);
}

static void on_mute_thread_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Mute the root event of this thread (or self if it's the root) */
  const char *event_to_mute = self->root_id ? self->root_id : self->id_hex;
  if (event_to_mute) {
    g_signal_emit(self, signals[SIGNAL_MUTE_THREAD_REQUESTED], 0, event_to_mute);
  }
}

static void copy_to_clipboard(GnostrNoteCardRow *self, const char *text) {
  if (!text || !*text) return;

  GtkWidget *widget = GTK_WIDGET(self);
  GdkDisplay *display = gtk_widget_get_display(widget);
  if (display) {
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (clipboard) {
      gdk_clipboard_set_text(clipboard, text);
      g_signal_emit(self, signals[SIGNAL_SHOW_TOAST], 0, "Copied to clipboard");
    }
  }
}

static void on_copy_note_id_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || strlen(self->id_hex) != 64) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Encode as nevent with relay hints if available, otherwise as note1 */
  char *encoded = NULL;

  /* Try nevent first (includes more metadata) */
  NostrNEventConfig cfg = {
    .id = self->id_hex,
    .author = self->pubkey_hex,
    .kind = 1,  /* text note */
    .relays = NULL,
    .relays_count = 0
  };

  NostrPointer *ptr = NULL;
  if (nostr_pointer_from_nevent_config(&cfg, &ptr) == 0 && ptr) {
    nostr_pointer_to_bech32(ptr, &encoded);
    nostr_pointer_free(ptr);
  }

  /* Fallback to simple note1 if nevent encoding failed */
  if (!encoded) {
    uint8_t id_bytes[32];
    if (hex_to_bytes_32(self->id_hex, id_bytes)) {
      nostr_nip19_encode_note(id_bytes, &encoded);
    }
  }

  if (encoded) {
    copy_to_clipboard(self, encoded);
    free(encoded);
  }
}

static void on_copy_pubkey_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->pubkey_hex || strlen(self->pubkey_hex) != 64) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Encode as npub1 */
  uint8_t pubkey_bytes[32];
  if (hex_to_bytes_32(self->pubkey_hex, pubkey_bytes)) {
    char *npub = NULL;
    if (nostr_nip19_encode_npub(pubkey_bytes, &npub) == 0 && npub) {
      copy_to_clipboard(self, npub);
      free(npub);
    }
  }
}

static void on_copy_note_text_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->content_text) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  copy_to_clipboard(self, self->content_text);
}

static void on_report_note_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || !self->pubkey_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Emit signal to report this note (NIP-56) */
  g_signal_emit(self, signals[SIGNAL_REPORT_NOTE_REQUESTED], 0, self->id_hex, self->pubkey_hex);
}

static void on_share_note_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Build nostr: URI for sharing */
  char *encoded = NULL;
  NostrNEventConfig cfg = {
    .id = self->id_hex,
    .author = self->pubkey_hex,
    .kind = 1,
    .relays = NULL,
    .relays_count = 0
  };

  NostrPointer *ptr = NULL;
  if (nostr_pointer_from_nevent_config(&cfg, &ptr) == 0 && ptr) {
    nostr_pointer_to_bech32(ptr, &encoded);
    nostr_pointer_free(ptr);
  }

  /* Fallback to simple note1 */
  if (!encoded) {
    uint8_t id_bytes[32];
    if (hex_to_bytes_32(self->id_hex, id_bytes)) {
      nostr_nip19_encode_note(id_bytes, &encoded);
    }
  }

  if (encoded) {
    /* Create nostr: URI */
    char *uri = g_strdup_printf("nostr:%s", encoded);

    /* Use GtkUriLauncher or system share if available */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

    /* Copy to clipboard as fallback and show toast */
    copy_to_clipboard(self, uri);
    g_signal_emit(self, signals[SIGNAL_SHARE_NOTE_REQUESTED], 0, uri);

    g_free(uri);
    free(encoded);
  }
}

/* NIP-09: Delete note (only shown for own notes) */
static void on_delete_note_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || !self->pubkey_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Emit signal to request deletion (NIP-09) */
  g_signal_emit(self, signals[SIGNAL_DELETE_NOTE_REQUESTED], 0, self->id_hex, self->pubkey_hex);
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self) return;

  /* Create popover if not already created */
  if (!self->menu_popover) {
    self->menu_popover = gtk_popover_new();
    gtk_widget_set_parent(self->menu_popover, GTK_WIDGET(self->btn_menu));

    /* Create a vertical box for the menu items */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* View JSON button */
    GtkWidget *json_btn = gtk_button_new();
    GtkWidget *json_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *json_icon = gtk_image_new_from_icon_name("text-x-generic-symbolic");
    GtkWidget *json_label = gtk_label_new("View Raw JSON");
    gtk_box_append(GTK_BOX(json_box), json_icon);
    gtk_box_append(GTK_BOX(json_box), json_label);
    gtk_button_set_child(GTK_BUTTON(json_btn), json_box);
    gtk_button_set_has_frame(GTK_BUTTON(json_btn), FALSE);
    g_signal_connect(json_btn, "clicked", G_CALLBACK(on_view_json_clicked), self);
    gtk_box_append(GTK_BOX(box), json_btn);

    /* Copy Note ID button */
    GtkWidget *copy_note_btn = gtk_button_new();
    GtkWidget *copy_note_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_note_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_note_label = gtk_label_new("Copy Note ID");
    gtk_box_append(GTK_BOX(copy_note_box), copy_note_icon);
    gtk_box_append(GTK_BOX(copy_note_box), copy_note_label);
    gtk_button_set_child(GTK_BUTTON(copy_note_btn), copy_note_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_note_btn), FALSE);
    g_signal_connect(copy_note_btn, "clicked", G_CALLBACK(on_copy_note_id_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_note_btn);

    /* Copy Note Text button */
    GtkWidget *copy_text_btn = gtk_button_new();
    GtkWidget *copy_text_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_text_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_text_label = gtk_label_new("Copy Note Text");
    gtk_box_append(GTK_BOX(copy_text_box), copy_text_icon);
    gtk_box_append(GTK_BOX(copy_text_box), copy_text_label);
    gtk_button_set_child(GTK_BUTTON(copy_text_btn), copy_text_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_text_btn), FALSE);
    g_signal_connect(copy_text_btn, "clicked", G_CALLBACK(on_copy_note_text_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_text_btn);

    /* Copy Author Pubkey button */
    GtkWidget *copy_pubkey_btn = gtk_button_new();
    GtkWidget *copy_pubkey_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_pubkey_icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
    GtkWidget *copy_pubkey_label = gtk_label_new("Copy Author Pubkey");
    gtk_box_append(GTK_BOX(copy_pubkey_box), copy_pubkey_icon);
    gtk_box_append(GTK_BOX(copy_pubkey_box), copy_pubkey_label);
    gtk_button_set_child(GTK_BUTTON(copy_pubkey_btn), copy_pubkey_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_pubkey_btn), FALSE);
    g_signal_connect(copy_pubkey_btn, "clicked", G_CALLBACK(on_copy_pubkey_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_pubkey_btn);

    /* Separator - Share section */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep1, 4);
    gtk_widget_set_margin_bottom(sep1, 4);
    gtk_box_append(GTK_BOX(box), sep1);

    /* Share Note button */
    GtkWidget *share_btn = gtk_button_new();
    GtkWidget *share_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *share_icon = gtk_image_new_from_icon_name("emblem-shared-symbolic");
    GtkWidget *share_label = gtk_label_new("Share Note");
    gtk_box_append(GTK_BOX(share_box), share_icon);
    gtk_box_append(GTK_BOX(share_box), share_label);
    gtk_button_set_child(GTK_BUTTON(share_btn), share_box);
    gtk_button_set_has_frame(GTK_BUTTON(share_btn), FALSE);
    g_signal_connect(share_btn, "clicked", G_CALLBACK(on_share_note_clicked), self);
    gtk_box_append(GTK_BOX(box), share_btn);

    /* Separator - Moderation section */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep2, 4);
    gtk_widget_set_margin_bottom(sep2, 4);
    gtk_box_append(GTK_BOX(box), sep2);

    /* Mute Author button */
    GtkWidget *mute_btn = gtk_button_new();
    GtkWidget *mute_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mute_icon = gtk_image_new_from_icon_name("action-unavailable-symbolic");
    GtkWidget *mute_label = gtk_label_new("Mute Author");
    gtk_box_append(GTK_BOX(mute_box), mute_icon);
    gtk_box_append(GTK_BOX(mute_box), mute_label);
    gtk_button_set_child(GTK_BUTTON(mute_btn), mute_box);
    gtk_button_set_has_frame(GTK_BUTTON(mute_btn), FALSE);
    g_signal_connect(mute_btn, "clicked", G_CALLBACK(on_mute_user_clicked), self);
    gtk_box_append(GTK_BOX(box), mute_btn);

    /* Mute Thread button */
    GtkWidget *mute_thread_btn = gtk_button_new();
    GtkWidget *mute_thread_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mute_thread_icon = gtk_image_new_from_icon_name("mail-mark-junk-symbolic");
    GtkWidget *mute_thread_label = gtk_label_new("Mute Thread");
    gtk_box_append(GTK_BOX(mute_thread_box), mute_thread_icon);
    gtk_box_append(GTK_BOX(mute_thread_box), mute_thread_label);
    gtk_button_set_child(GTK_BUTTON(mute_thread_btn), mute_thread_box);
    gtk_button_set_has_frame(GTK_BUTTON(mute_thread_btn), FALSE);
    g_signal_connect(mute_thread_btn, "clicked", G_CALLBACK(on_mute_thread_clicked), self);
    gtk_box_append(GTK_BOX(box), mute_thread_btn);

    /* Report Note button */
    GtkWidget *report_btn = gtk_button_new();
    GtkWidget *report_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *report_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    GtkWidget *report_label = gtk_label_new("Report Note");
    gtk_box_append(GTK_BOX(report_box), report_icon);
    gtk_box_append(GTK_BOX(report_box), report_label);
    gtk_button_set_child(GTK_BUTTON(report_btn), report_box);
    gtk_button_set_has_frame(GTK_BUTTON(report_btn), FALSE);
    g_signal_connect(report_btn, "clicked", G_CALLBACK(on_report_note_clicked), self);
    gtk_box_append(GTK_BOX(box), report_btn);

    /* Separator before Delete section (NIP-09) */
    GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep3, 4);
    gtk_widget_set_margin_bottom(sep3, 4);
    gtk_box_append(GTK_BOX(box), sep3);

    /* Delete Note button (NIP-09) - only visible for own notes */
    GtkWidget *delete_btn = gtk_button_new();
    GtkWidget *delete_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *delete_icon = gtk_image_new_from_icon_name("user-trash-symbolic");
    GtkWidget *delete_label = gtk_label_new("Delete Note");
    gtk_box_append(GTK_BOX(delete_box), delete_icon);
    gtk_box_append(GTK_BOX(delete_box), delete_label);
    gtk_button_set_child(GTK_BUTTON(delete_btn), delete_box);
    gtk_button_set_has_frame(GTK_BUTTON(delete_btn), FALSE);
    gtk_widget_add_css_class(delete_btn, "destructive-action");
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_delete_note_clicked), self);
    gtk_box_append(GTK_BOX(box), delete_btn);
    /* Store reference for visibility toggle */
    self->delete_btn = delete_btn;
    /* Initially hide delete button - will be shown if is_own_note is set */
    gtk_widget_set_visible(delete_btn, self->is_own_note);
    gtk_widget_set_visible(sep3, self->is_own_note);
    /* Store separator reference for visibility toggle */
    g_object_set_data(G_OBJECT(delete_btn), "delete-separator", sep3);

    gtk_popover_set_child(GTK_POPOVER(self->menu_popover), box);
  }

  /* Show the popover */
  gtk_popover_popup(GTK_POPOVER(self->menu_popover));
}

/* Helper function to show context menu at a specific position */
static void show_context_menu_at_point(GnostrNoteCardRow *self, double x, double y) {
  if (!self) return;

  /* Create popover if needed (reuse the menu popover code) */
  if (!self->menu_popover) {
    /* Trigger creation by simulating a button click on the menu button */
    on_menu_clicked(GTK_BUTTON(self->btn_menu), self);
    /* Hide it immediately as we want to position it differently */
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Position the popover at the click point */
  GdkRectangle rect = { (int)x, (int)y, 1, 1 };
  gtk_popover_set_pointing_to(GTK_POPOVER(self->menu_popover), &rect);

  /* Re-parent to the note card widget for proper positioning */
  if (gtk_widget_get_parent(self->menu_popover) != GTK_WIDGET(self)) {
    gtk_widget_unparent(self->menu_popover);
    gtk_widget_set_parent(self->menu_popover, GTK_WIDGET(self));
  }

  gtk_popover_popup(GTK_POPOVER(self->menu_popover));
}

/* Right-click gesture handler */
static void on_right_click_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture;
  (void)n_press;

  show_context_menu_at_point(self, x, y);
}

/* Long-press gesture handler (for touch devices) */
static void on_long_press(GtkGestureLongPress *gesture, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture;

  show_context_menu_at_point(self, x, y);
}

static void on_reply_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_REPLY_REQUESTED], 0,
                  self->id_hex, self->root_id, self->pubkey_hex);
  }
}

static void on_repost_action_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* Hide popover first */
    if (self->repost_popover && GTK_IS_POPOVER(self->repost_popover)) {
      gtk_popover_popdown(GTK_POPOVER(self->repost_popover));
    }
    g_signal_emit(self, signals[SIGNAL_REPOST_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex);
  }
}

static void on_quote_action_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* Hide popover first */
    if (self->repost_popover && GTK_IS_POPOVER(self->repost_popover)) {
      gtk_popover_popdown(GTK_POPOVER(self->repost_popover));
    }
    g_signal_emit(self, signals[SIGNAL_QUOTE_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex);
  }
}

static void on_repost_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self) return;

  /* Create popover if not already created */
  if (!self->repost_popover) {
    self->repost_popover = gtk_popover_new();
    gtk_widget_set_parent(self->repost_popover, GTK_WIDGET(self->btn_repost));

    /* Create a vertical box for the menu items */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Repost button */
    GtkWidget *repost_btn = gtk_button_new();
    GtkWidget *repost_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *repost_icon = gtk_image_new_from_icon_name("object-rotate-right-symbolic");
    GtkWidget *repost_label = gtk_label_new("Repost");
    gtk_box_append(GTK_BOX(repost_box), repost_icon);
    gtk_box_append(GTK_BOX(repost_box), repost_label);
    gtk_button_set_child(GTK_BUTTON(repost_btn), repost_box);
    gtk_button_set_has_frame(GTK_BUTTON(repost_btn), FALSE);
    g_signal_connect(repost_btn, "clicked", G_CALLBACK(on_repost_action_clicked), self);
    gtk_box_append(GTK_BOX(box), repost_btn);

    /* Quote button */
    GtkWidget *quote_btn = gtk_button_new();
    GtkWidget *quote_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *quote_icon = gtk_image_new_from_icon_name("format-text-quote-symbolic");
    GtkWidget *quote_label = gtk_label_new("Quote");
    gtk_box_append(GTK_BOX(quote_box), quote_icon);
    gtk_box_append(GTK_BOX(quote_box), quote_label);
    gtk_button_set_child(GTK_BUTTON(quote_btn), quote_box);
    gtk_button_set_has_frame(GTK_BUTTON(quote_btn), FALSE);
    g_signal_connect(quote_btn, "clicked", G_CALLBACK(on_quote_action_clicked), self);
    gtk_box_append(GTK_BOX(box), quote_btn);

    gtk_popover_set_child(GTK_POPOVER(self->repost_popover), box);
  }

  /* Show the popover */
  gtk_popover_popup(GTK_POPOVER(self->repost_popover));
}

static void on_like_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_LIKE_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex);
  }
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* Emit zap requested signal with event_id, pubkey, and lud16 if available */
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex, self->author_lud16);
  }
}

static void on_bookmark_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex) {
    /* Toggle bookmark state */
    self->is_bookmarked = !self->is_bookmarked;

    /* Update button icon */
    if (GTK_IS_BUTTON(self->btn_bookmark)) {
      gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
        self->is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
    }

    /* Emit signal so main window can handle NIP-51 storage */
    g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0,
                  self->id_hex, self->is_bookmarked);
  }
}

static void on_thread_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self) {
    /* Use root_id if available, otherwise use the note's own id as thread root */
    const char *thread_root = self->root_id ? self->root_id : self->id_hex;
    if (thread_root) {
      g_signal_emit(self, signals[SIGNAL_VIEW_THREAD_REQUESTED], 0, thread_root);
    }
  }
}

/* Callback when reply indicator is clicked - navigate to parent note */
static void on_reply_indicator_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  if (!self) return;

  /* Navigate to parent note if available, otherwise to thread root */
  const char *target = self->parent_id ? self->parent_id : self->root_id;
  if (target && *target) {
    g_signal_emit(self, signals[SIGNAL_NAVIGATE_TO_NOTE], 0, target);
  }
}

/* Callback when reply count badge is clicked - open thread view */
static void on_reply_count_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  if (!self || !self->id_hex) return;

  /* Open thread view with this note as the root */
  g_signal_emit(self, signals[SIGNAL_VIEW_THREAD_REQUESTED], 0, self->id_hex);
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
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_repost);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_like);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_like_count);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_zap);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_zap_count);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_bookmark);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_thread);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_indicator_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_indicator_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_count_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_count_label);
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
  signals[SIGNAL_REPOST_REQUESTED] = g_signal_new("repost-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_QUOTE_REQUESTED] = g_signal_new("quote-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_LIKE_REQUESTED] = g_signal_new("like-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new("zap-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_VIEW_THREAD_REQUESTED] = g_signal_new("view-thread-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_MUTE_USER_REQUESTED] = g_signal_new("mute-user-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_MUTE_THREAD_REQUESTED] = g_signal_new("mute-thread-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_SHOW_TOAST] = g_signal_new("show-toast",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_BOOKMARK_TOGGLED] = g_signal_new("bookmark-toggled",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
  signals[SIGNAL_REPORT_NOTE_REQUESTED] = g_signal_new("report-note-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_SHARE_NOTE_REQUESTED] = g_signal_new("share-note-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_SEARCH_HASHTAG] = g_signal_new("search-hashtag",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_NAVIGATE_TO_NOTE] = g_signal_new("navigate-to-note",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  /* NIP-09 deletion request: id_hex, pubkey_hex */
  signals[SIGNAL_DELETE_NOTE_REQUESTED] = g_signal_new("delete-note-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
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
  /* Connect repost button */
  if (GTK_IS_BUTTON(self->btn_repost)) {
    g_signal_connect(self->btn_repost, "clicked", G_CALLBACK(on_repost_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_repost),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Repost Note", -1);
  }
  /* Connect like button */
  if (GTK_IS_BUTTON(self->btn_like)) {
    g_signal_connect(self->btn_like, "clicked", G_CALLBACK(on_like_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_like),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Like Note", -1);
  }
  /* Connect zap button */
  if (GTK_IS_BUTTON(self->btn_zap)) {
    g_signal_connect(self->btn_zap, "clicked", G_CALLBACK(on_zap_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_zap),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Zap Note", -1);
  }
  /* Connect bookmark button */
  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    g_signal_connect(self->btn_bookmark, "clicked", G_CALLBACK(on_bookmark_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_bookmark),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Bookmark Note", -1);
  }
  /* Connect view thread button */
  if (GTK_IS_BUTTON(self->btn_thread)) {
    g_signal_connect(self->btn_thread, "clicked", G_CALLBACK(on_thread_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_thread),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "View Thread", -1);
  }

  /* Make reply indicator clickable - navigate to parent note */
  if (GTK_IS_WIDGET(self->reply_indicator_box)) {
    GtkGesture *reply_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(reply_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(reply_click, "pressed", G_CALLBACK(on_reply_indicator_clicked), self);
    gtk_widget_add_controller(self->reply_indicator_box, GTK_EVENT_CONTROLLER(reply_click));
    /* Add CSS class for hover styling and cursor */
    gtk_widget_add_css_class(self->reply_indicator_box, "reply-indicator-clickable");
    gtk_widget_set_cursor_from_name(self->reply_indicator_box, "pointer");
  }

  /* Make reply count badge clickable - opens thread view */
  if (GTK_IS_WIDGET(self->reply_count_box)) {
    GtkGesture *count_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(count_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(count_click, "pressed", G_CALLBACK(on_reply_count_clicked), self);
    gtk_widget_add_controller(self->reply_count_box, GTK_EVENT_CONTROLLER(count_click));
    gtk_widget_set_cursor_from_name(self->reply_count_box, "pointer");
  }

  /* Add right-click gesture for context menu */
  GtkGesture *right_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
  g_signal_connect(right_click, "pressed", G_CALLBACK(on_right_click_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(right_click));

  /* Add long-press gesture for touch devices */
  GtkGesture *long_press = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(long_press), TRUE);
  g_signal_connect(long_press, "pressed", G_CALLBACK(on_long_press), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(long_press));

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

/* Load media image asynchronously - internal function that starts the actual fetch */
static void load_media_image_internal(GnostrNoteCardRow *self, const char *url, GtkPicture *picture) {
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

/* Lazy loading context for deferred media loading */
typedef struct {
  GnostrNoteCardRow *self;
  GtkPicture *picture;
  char *url;
  guint timeout_id;
  gulong map_handler_id;
  gulong unmap_handler_id;
  gboolean loaded;
} LazyLoadContext;

static void lazy_load_context_free(LazyLoadContext *ctx) {
  if (!ctx) return;
  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
    ctx->timeout_id = 0;
  }
  g_free(ctx->url);
  g_free(ctx);
}

/* Timeout callback - actually load the image after delay */
static gboolean on_lazy_load_timeout(gpointer user_data) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;

  if (!ctx || ctx->loaded) return G_SOURCE_REMOVE;

  /* Check if widgets are still valid */
  if (!GNOSTR_IS_NOTE_CARD_ROW(ctx->self) || !GTK_IS_PICTURE(ctx->picture)) {
    ctx->timeout_id = 0;
    return G_SOURCE_REMOVE;
  }

  /* Check if widget is still mapped (visible) */
  if (!gtk_widget_get_mapped(GTK_WIDGET(ctx->picture))) {
    ctx->timeout_id = 0;
    return G_SOURCE_REMOVE;
  }

  g_debug("Media: Lazy loading image: %s", ctx->url);
  ctx->loaded = TRUE;
  load_media_image_internal(ctx->self, ctx->url, ctx->picture);

  ctx->timeout_id = 0;
  return G_SOURCE_REMOVE;
}

/* Called when the picture widget becomes visible */
static void on_picture_mapped(GtkWidget *widget, gpointer user_data) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;
  (void)widget;

  if (!ctx || ctx->loaded) return;

  /* Cancel any pending timeout */
  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
    ctx->timeout_id = 0;
  }

  /* Schedule load after a short delay (150ms) to avoid loading during fast scrolling */
  ctx->timeout_id = g_timeout_add(150, on_lazy_load_timeout, ctx);
}

/* Called when the picture widget becomes hidden */
static void on_picture_unmapped(GtkWidget *widget, gpointer user_data) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;
  (void)widget;

  if (!ctx || ctx->loaded) return;

  /* Cancel pending load if user scrolled past */
  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
    ctx->timeout_id = 0;
    g_debug("Media: Cancelled lazy load (scrolled past): %s", ctx->url);
  }
}

/* Called when the picture widget is destroyed */
static void on_lazy_load_picture_destroyed(gpointer user_data, GObject *where_the_object_was) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;
  (void)where_the_object_was;
  lazy_load_context_free(ctx);
}

/* Load media image with lazy loading - defers actual loading until widget is visible */
static void load_media_image(GnostrNoteCardRow *self, const char *url, GtkPicture *picture) {
  if (!url || !*url || !GTK_IS_PICTURE(picture)) return;

  /* Create lazy loading context */
  LazyLoadContext *ctx = g_new0(LazyLoadContext, 1);
  ctx->self = self;
  ctx->picture = picture;
  ctx->url = g_strdup(url);
  ctx->loaded = FALSE;
  ctx->timeout_id = 0;

  /* Connect to map/unmap signals for visibility tracking */
  ctx->map_handler_id = g_signal_connect(picture, "map", G_CALLBACK(on_picture_mapped), ctx);
  ctx->unmap_handler_id = g_signal_connect(picture, "unmap", G_CALLBACK(on_picture_unmapped), ctx);

  /* Track widget destruction to free context */
  g_object_weak_ref(G_OBJECT(picture), on_lazy_load_picture_destroyed, ctx);

  /* If already mapped, start loading immediately */
  if (gtk_widget_get_mapped(GTK_WIDGET(picture))) {
    on_picture_mapped(GTK_WIDGET(picture), ctx);
  }
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

/* Image click handler - opens full-size image viewer with gallery support */
static void on_media_image_clicked(GtkGestureClick *gesture,
                                    int n_press,
                                    double x, double y,
                                    gpointer user_data) {
  (void)n_press;
  (void)x;
  (void)y;
  (void)user_data;

  /* Get the clicked picture widget */
  GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  if (!pic) return;

  /* Get the URL of this image */
  const char *clicked_url = g_object_get_data(G_OBJECT(pic), "image-url");
  if (!clicked_url || !*clicked_url) return;

  /* Get the parent media_box to find all images */
  GtkWidget *media_box = gtk_widget_get_parent(pic);
  if (!media_box || !GTK_IS_BOX(media_box)) {
    /* Fallback: single image mode */
    GtkRoot *root = gtk_widget_get_root(pic);
    GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
    GnostrImageViewer *viewer = gnostr_image_viewer_new(parent);
    gnostr_image_viewer_set_image_url(viewer, clicked_url);
    gnostr_image_viewer_present(viewer);
    return;
  }

  /* Collect all image URLs from media_box */
  GPtrArray *urls = g_ptr_array_new();
  guint clicked_index = 0;
  GtkWidget *child = gtk_widget_get_first_child(media_box);
  while (child) {
    if (GTK_IS_PICTURE(child)) {
      const char *url = g_object_get_data(G_OBJECT(child), "image-url");
      if (url && *url) {
        if (child == pic) {
          clicked_index = urls->len;
        }
        g_ptr_array_add(urls, (gpointer)url);
      }
    }
    child = gtk_widget_get_next_sibling(child);
  }
  g_ptr_array_add(urls, NULL);  /* NULL terminate */

  /* Get parent window */
  GtkRoot *root = gtk_widget_get_root(pic);
  GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

  /* Create and show the image viewer with gallery */
  GnostrImageViewer *viewer = gnostr_image_viewer_new(parent);
  if (urls->len > 2) {  /* More than just clicked + NULL terminator */
    gnostr_image_viewer_set_gallery(viewer, (const char * const *)urls->pdata, clicked_index);
  } else {
    gnostr_image_viewer_set_image_url(viewer, clicked_url);
  }
  gnostr_image_viewer_present(viewer);

  g_ptr_array_free(urls, TRUE);
}

static gboolean is_media_url(const char *u) {
  return is_image_url(u) || is_video_url(u);
}

/* Extract clean URL from token, stripping trailing punctuation.
 * Handles: trailing periods, commas, semicolons, unbalanced parens/brackets.
 * Returns newly allocated clean URL and sets suffix to trailing characters.
 */
static gchar *extract_clean_url(const char *token, gchar **suffix) {
  if (!token || !*token) {
    if (suffix) *suffix = g_strdup("");
    return NULL;
  }
  size_t len = strlen(token);
  if (len == 0) {
    if (suffix) *suffix = g_strdup("");
    return NULL;
  }
  size_t end = len;
  int paren_balance = 0, bracket_balance = 0;
  /* Count balanced parens/brackets */
  for (size_t i = 0; i < len; i++) {
    if (token[i] == '(') paren_balance++;
    else if (token[i] == ')') paren_balance--;
    else if (token[i] == '[') bracket_balance++;
    else if (token[i] == ']') bracket_balance--;
  }
  /* Trim trailing punctuation */
  while (end > 0) {
    char c = token[end - 1];
    if (c == ',' || c == ';' || c == '!' || c == '\'' || c == '"' || c == '.') {
      end--;
      continue;
    }
    if (c == ':' && end > 1 && !g_ascii_isdigit(token[end - 2])) {
      end--;
      continue;
    }
    if (c == ')' && paren_balance < 0) {
      paren_balance++;
      end--;
      continue;
    }
    if (c == ']' && bracket_balance < 0) {
      bracket_balance++;
      end--;
      continue;
    }
    break;
  }
  if (suffix) *suffix = g_strdup(token + end);
  return (end > 0) ? g_strndup(token, end) : NULL;
}

/* Check if token starts with URL prefix */
static gboolean token_is_url(const char *t) {
  return t && (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://") || g_str_has_prefix(t, "www."));
}

/* Check if token is a nostr entity */
static gboolean token_is_nostr(const char *t) {
  return t && (g_str_has_prefix(t, "nostr:") || g_str_has_prefix(t, "note1") || g_str_has_prefix(t, "npub1") ||
               g_str_has_prefix(t, "nevent1") || g_str_has_prefix(t, "nprofile1") || g_str_has_prefix(t, "naddr1"));
}

/* Check if token is a hashtag (#word) */
static gboolean token_is_hashtag(const char *t) {
  if (!t || t[0] != '#' || t[1] == '\0') return FALSE;
  /* Must have at least one alphanumeric char after # */
  return g_ascii_isalnum(t[1]) || (unsigned char)t[1] > 127; /* Allow Unicode */
}

/* Extract hashtag text (without # prefix and trailing punctuation) */
static gchar *extract_hashtag(const char *t, gchar **suffix) {
  if (!t || t[0] != '#') { if (suffix) *suffix = NULL; return NULL; }
  const char *start = t + 1; /* Skip # */
  size_t len = strlen(start);
  /* Find end of hashtag (alphanumeric, underscore, or high-byte UTF-8) */
  size_t end = 0;
  while (end < len) {
    unsigned char c = (unsigned char)start[end];
    if (g_ascii_isalnum(c) || c == '_' || c > 127) {
      end++;
    } else {
      break;
    }
  }
  if (end == 0) { if (suffix) *suffix = NULL; return NULL; }
  if (suffix) *suffix = (end < len) ? g_strdup(start + end) : NULL;
  return g_strndup(start, end);
}

void gnostr_note_card_row_set_content(GnostrNoteCardRow *self, const char *content) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->content_label)) return;

  /* Store plain text content for clipboard operations */
  g_clear_pointer(&self->content_text, g_free);
  self->content_text = g_strdup(content);

  /* Parse content: detect URLs and nostr entities, handle trailing punctuation */
  GString *out = g_string_new("");
  if (content && *content) {
    gchar **tokens = g_strsplit_set(content, " \n\t", -1);
    for (guint i=0; tokens && tokens[i]; i++) {
      const char *t = tokens[i];
      if (t[0]=='\0') { g_string_append(out, " "); continue; }
      gboolean is_url = token_is_url(t);
      gboolean is_nostr = token_is_nostr(t);
      gboolean is_hashtag = token_is_hashtag(t);
      if (is_url || is_nostr) {
        gchar *suffix = NULL;
        gchar *clean = extract_clean_url(t, &suffix);
        if (clean && *clean) {
          /* For www. URLs, use https:// in href */
          gchar *href = g_str_has_prefix(clean, "www.") ? g_strdup_printf("https://%s", clean) : g_strdup(clean);
          gchar *esc_href = g_markup_escape_text(href, -1);
          gchar *esc_display = g_markup_escape_text(clean, -1);
          g_string_append_printf(out, "<a href=\"%s\">%s</a>", esc_href, esc_display);
          g_free(esc_href);
          g_free(esc_display);
          g_free(href);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc);
          g_free(esc);
        }
        g_free(clean);
        g_free(suffix);
      } else if (is_hashtag) {
        gchar *suffix = NULL;
        gchar *tag = extract_hashtag(t, &suffix);
        if (tag && *tag) {
          gchar *esc_tag = g_markup_escape_text(tag, -1);
          /* Use hashtag: URI scheme for internal handling */
          g_string_append_printf(out, "<a href=\"hashtag:%s\">#%s</a>", esc_tag, esc_tag);
          g_free(esc_tag);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc);
          g_free(esc);
        }
        g_free(tag);
        g_free(suffix);
      } else {
        gchar *esc = g_markup_escape_text(t, -1);
        g_string_append(out, esc);
        g_free(esc);
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
            gtk_widget_add_css_class(pic, "clickable-image");
            gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
            gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
            gtk_widget_set_size_request(pic, -1, 300);
            gtk_widget_set_hexpand(pic, TRUE);
            gtk_widget_set_vexpand(pic, FALSE);
            gtk_widget_set_cursor_from_name(pic, "pointer");

            /* Add click gesture to open image viewer */
            GtkGesture *click_gesture = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
            /* Store URL in widget data for the click handler */
            g_object_set_data_full(G_OBJECT(pic), "image-url", g_strdup(url), g_free);
            g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_media_image_clicked),
                             g_object_get_data(G_OBJECT(pic), "image-url"));
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(click_gesture));

            gtk_box_append(GTK_BOX(self->media_box), pic);
            gtk_widget_set_visible(self->media_box, TRUE);

#ifdef HAVE_SOUP3
            /* Load image asynchronously */
            load_media_image(self, url, GTK_PICTURE(pic));
#endif
          }
          /* Handle videos - use enhanced video player with controls overlay */
          else if (is_video_url(url)) {
            GnostrVideoPlayer *player = gnostr_video_player_new();
            gtk_widget_add_css_class(GTK_WIDGET(player), "note-media-video");
            gtk_widget_set_size_request(GTK_WIDGET(player), -1, 300);
            gtk_widget_set_hexpand(GTK_WIDGET(player), TRUE);
            gtk_widget_set_vexpand(GTK_WIDGET(player), FALSE);

            /* Set video URI - settings (autoplay/loop) are read from GSettings */
            gnostr_video_player_set_uri(player, url);

            gtk_box_append(GTK_BOX(self->media_box), GTK_WIDGET(player));
            gtk_widget_set_visible(self->media_box, TRUE);
          }
        }
      }
      g_strfreev(tokens);
    }
  }

  /* Detect NIP-19/21 nostr: references and create embedded note widgets */
  if (self->embed_box && GTK_IS_WIDGET(self->embed_box)) {
    /* Clear existing embeds from the embed_box */
    if (GTK_IS_FRAME(self->embed_box)) {
      gtk_frame_set_child(GTK_FRAME(self->embed_box), NULL);
    }
    gtk_widget_set_visible(self->embed_box, FALSE);
    self->note_embed = NULL;

    const char *p = content;
    if (p && *p) {
      /* Scan for nostr: URIs and bech32 references */
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      const char *first_nostr_ref = NULL;

      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;

        /* Check for nostr: URI or bare bech32 (NIP-21) */
        if (g_str_has_prefix(t, "nostr:") ||
            g_str_has_prefix(t, "note1") ||
            g_str_has_prefix(t, "nevent1") ||
            g_str_has_prefix(t, "naddr1") ||
            g_str_has_prefix(t, "npub1") ||
            g_str_has_prefix(t, "nprofile1")) {
          first_nostr_ref = t;
          break;
        }
      }

      if (first_nostr_ref) {
        /* Create the NIP-21 embed widget */
        self->note_embed = gnostr_note_embed_new();

        /* Connect profile-clicked signal to relay to main window */
        g_signal_connect(self->note_embed, "profile-clicked",
                        G_CALLBACK(on_embed_profile_clicked), self);

        /* Set the nostr URI - this triggers async loading via NIP-19 decoding */
        gnostr_note_embed_set_nostr_uri(self->note_embed, first_nostr_ref);

        /* Add embed widget to the embed_box frame */
        if (GTK_IS_FRAME(self->embed_box)) {
          gtk_frame_set_child(GTK_FRAME(self->embed_box), GTK_WIDGET(self->note_embed));
        }
        gtk_widget_set_visible(self->embed_box, TRUE);

        /* Also emit the signal for timeline-level handling (backwards compatibility) */
        g_signal_emit(self, signals[SIGNAL_REQUEST_EMBED], 0, first_nostr_ref);
      }

      if (tokens) g_strfreev(tokens);
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

/* NIP-92 imeta-aware content setter */
void gnostr_note_card_row_set_content_with_imeta(GnostrNoteCardRow *self, const char *content, const char *tags_json) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->content_label)) return;

  /* Store plain text content for clipboard operations */
  g_clear_pointer(&self->content_text, g_free);
  self->content_text = g_strdup(content);

  GnostrImetaList *imeta_list = NULL;
  if (tags_json && *tags_json) {
    imeta_list = gnostr_imeta_parse_tags_json(tags_json);
    if (imeta_list) {
      g_debug("note_card: Parsed %zu imeta tags from event", imeta_list->count);
    }
  }

  GString *out = g_string_new("");
  if (content && *content) {
    gchar **tokens = g_strsplit_set(content, " \n\t", -1);
    for (guint i = 0; tokens && tokens[i]; i++) {
      const char *t = tokens[i];
      if (t[0] == '\0') { g_string_append(out, " "); continue; }
      gboolean is_link = g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://") ||
          g_str_has_prefix(t, "nostr:") || g_str_has_prefix(t, "note1") || g_str_has_prefix(t, "npub1") ||
          g_str_has_prefix(t, "nevent1") || g_str_has_prefix(t, "nprofile1") || g_str_has_prefix(t, "naddr1");
      gboolean is_hashtag = token_is_hashtag(t);
      if (is_link) {
        gchar *esc = g_markup_escape_text(t, -1);
        g_string_append_printf(out, "<a href=\"%s\">%s</a>", esc, esc);
        g_free(esc);
      } else if (is_hashtag) {
        gchar *suffix = NULL;
        gchar *tag = extract_hashtag(t, &suffix);
        if (tag && *tag) {
          gchar *esc_tag = g_markup_escape_text(tag, -1);
          g_string_append_printf(out, "<a href=\"hashtag:%s\">#%s</a>", esc_tag, esc_tag);
          g_free(esc_tag);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc); g_free(esc);
        }
        g_free(tag);
        g_free(suffix);
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
  if (markup_allocated || markup) g_free(markup);

  if (self->media_box && GTK_IS_BOX(self->media_box)) {
    GtkWidget *child = gtk_widget_get_first_child(self->media_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(self->media_box), child);
      child = next;
    }
    gtk_widget_set_visible(self->media_box, FALSE);

    if (content) {
      gchar **tokens = g_strsplit_set(content, " \n\t", -1);
      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *url = tokens[i];
        if (!url || !*url) continue;
        if (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")) {
          GnostrImeta *imeta = imeta_list ? gnostr_imeta_find_by_url(imeta_list, url) : NULL;
          GnostrMediaType media_type = GNOSTR_MEDIA_TYPE_UNKNOWN;
          if (imeta) {
            media_type = imeta->media_type;
            g_debug("note_card: imeta for %s: type=%d dim=%dx%d alt=%s",
                    url, media_type, imeta->width, imeta->height,
                    imeta->alt ? imeta->alt : "(none)");
          }
          if (media_type == GNOSTR_MEDIA_TYPE_UNKNOWN) {
            if (is_image_url(url)) media_type = GNOSTR_MEDIA_TYPE_IMAGE;
            else if (is_video_url(url)) media_type = GNOSTR_MEDIA_TYPE_VIDEO;
          }

          if (media_type == GNOSTR_MEDIA_TYPE_IMAGE) {
            GtkWidget *pic = gtk_picture_new();
            gtk_widget_add_css_class(pic, "note-media-image");
            gtk_widget_add_css_class(pic, "clickable-image");
            gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
            gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
            int height = 300;
            if (imeta && imeta->width > 0 && imeta->height > 0) {
              int cw = 400;
              height = imeta->width <= cw ? imeta->height : (int)((double)imeta->height * cw / imeta->width);
              if (height > 400) height = 400;
              if (height < 100) height = 100;
            }
            gtk_widget_set_size_request(pic, -1, height);
            if (imeta && imeta->alt && *imeta->alt) gtk_widget_set_tooltip_text(pic, imeta->alt);
            gtk_widget_set_hexpand(pic, TRUE);
            gtk_widget_set_vexpand(pic, FALSE);
            gtk_widget_set_cursor_from_name(pic, "pointer");

            /* Add click gesture to open image viewer */
            GtkGesture *click_gesture = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
            /* Store URL in widget data for the click handler */
            g_object_set_data_full(G_OBJECT(pic), "image-url", g_strdup(url), g_free);
            g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_media_image_clicked),
                             g_object_get_data(G_OBJECT(pic), "image-url"));
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(click_gesture));

            gtk_box_append(GTK_BOX(self->media_box), pic);
            gtk_widget_set_visible(self->media_box, TRUE);
#ifdef HAVE_SOUP3
            load_media_image(self, url, GTK_PICTURE(pic));
#endif
          } else if (media_type == GNOSTR_MEDIA_TYPE_VIDEO) {
            /* Use enhanced video player with controls overlay */
            GnostrVideoPlayer *player = gnostr_video_player_new();
            gtk_widget_add_css_class(GTK_WIDGET(player), "note-media-video");
            int height = 300;
            if (imeta && imeta->width > 0 && imeta->height > 0) {
              int cw = 400;
              height = imeta->width <= cw ? imeta->height : (int)((double)imeta->height * cw / imeta->width);
              if (height > 400) height = 400;
              if (height < 100) height = 100;
            }
            gtk_widget_set_size_request(GTK_WIDGET(player), -1, height);
            if (imeta && imeta->alt && *imeta->alt) gtk_widget_set_tooltip_text(GTK_WIDGET(player), imeta->alt);
            gtk_widget_set_hexpand(GTK_WIDGET(player), TRUE);
            gtk_widget_set_vexpand(GTK_WIDGET(player), FALSE);
            /* Set video URI - settings (autoplay/loop) are read from GSettings */
            gnostr_video_player_set_uri(player, url);
            gtk_box_append(GTK_BOX(self->media_box), GTK_WIDGET(player));
            gtk_widget_set_visible(self->media_box, TRUE);
          }
        }
      }
      g_strfreev(tokens);
    }
  }

  gnostr_imeta_list_free(imeta_list);

  if (self->embed_box && GTK_IS_WIDGET(self->embed_box)) {
    gtk_widget_set_visible(self->embed_box, FALSE);
    const char *p = content;
    const char *found = NULL;
    if (p && *p) {
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i]; if (!t || !*t) continue;
        if (g_str_has_prefix(t, "nostr:") || g_str_has_prefix(t, "note1") ||
            g_str_has_prefix(t, "nevent1") || g_str_has_prefix(t, "naddr1")) {
          found = t; break;
        }
      }
      if (tokens) g_strfreev(tokens);
    }
    if (found) {
      GtkWidget *sk = gtk_label_new("Loading embed...");
      gtk_widget_set_halign(sk, GTK_ALIGN_START);
      gtk_widget_set_valign(sk, GTK_ALIGN_START);
      gtk_widget_set_margin_start(sk, 6);
      gtk_widget_set_margin_end(sk, 6);
      gtk_widget_set_margin_top(sk, 4);
      gtk_widget_set_margin_bottom(sk, 4);
      if (GTK_IS_FRAME(self->embed_box)) gtk_frame_set_child(GTK_FRAME(self->embed_box), sk);
      gtk_widget_set_visible(self->embed_box, TRUE);
      g_signal_emit(self, signals[SIGNAL_REQUEST_EMBED], 0, found);
    }
  }

  if (self->og_preview_container && GTK_IS_BOX(self->og_preview_container)) {
    if (self->og_preview) {
      gtk_box_remove(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
      self->og_preview = NULL;
    }
    gtk_widget_set_visible(self->og_preview_container, FALSE);
    const char *p = content;
    const char *url_start = NULL;
    if (p && *p) {
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;
        if (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://")) {
          if (!is_media_url(t)) { url_start = t; break; }
        }
      }
      if (url_start) {
        self->og_preview = og_preview_widget_new();
        gtk_box_append(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
        gtk_widget_set_visible(self->og_preview_container, TRUE);
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

void gnostr_note_card_row_set_thread_info(GnostrNoteCardRow *self,
                                           const char *root_id,
                                           const char *parent_id,
                                           const char *parent_author_name,
                                           gboolean is_reply) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  g_free(self->root_id);
  self->root_id = g_strdup(root_id);
  g_free(self->parent_id);
  self->parent_id = g_strdup(parent_id);
  self->is_reply = is_reply;

  /* Update reply indicator visibility and text */
  if (GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, is_reply);
  }

  if (is_reply && GTK_IS_LABEL(self->reply_indicator_label)) {
    char *indicator_text = NULL;
    if (parent_author_name && *parent_author_name) {
      indicator_text = g_strdup_printf("In reply to %s", parent_author_name);
    } else {
      indicator_text = g_strdup("In reply to...");
    }
    gtk_label_set_text(GTK_LABEL(self->reply_indicator_label), indicator_text);
    g_free(indicator_text);
  }

  /* Show/hide view thread button - visible if this is a reply or has a root */
  if (GTK_IS_BUTTON(self->btn_thread)) {
    gboolean show_thread_btn = (is_reply || (root_id != NULL && *root_id));
    gtk_widget_set_visible(GTK_WIDGET(self->btn_thread), show_thread_btn);
  }
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

/* NIP-05 verification callback for note card */
static void on_note_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);

  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !result) {
    if (result) gnostr_nip05_result_free(result);
    return;
  }

  g_debug("note_card: NIP-05 verification result for %s: %s",
          result->identifier, gnostr_nip05_status_to_string(result->status));

  /* Show badge if verified */
  if (result->status == GNOSTR_NIP05_STATUS_VERIFIED && self->nip05_badge) {
    gtk_widget_set_visible(self->nip05_badge, TRUE);
    g_debug("note_card: showing NIP-05 verified badge for %s", result->identifier);
  }

  gnostr_nip05_result_free(result);
}

/* Set NIP-05 and trigger async verification */
void gnostr_note_card_row_set_nip05(GnostrNoteCardRow *self, const char *nip05, const char *pubkey_hex) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  /* Clear previous state */
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }
  g_clear_pointer(&self->nip05, g_free);

  /* Hide/remove previous badge */
  if (self->nip05_badge) {
    gtk_widget_set_visible(self->nip05_badge, FALSE);
  }

  if (!nip05 || !*nip05 || !pubkey_hex || strlen(pubkey_hex) != 64) {
    return;
  }

  /* Store NIP-05 identifier */
  self->nip05 = g_strdup(nip05);

  /* Create badge widget if needed (add next to handle label) */
  if (!self->nip05_badge && GTK_IS_LABEL(self->lbl_handle)) {
    /* Get parent of handle label */
    GtkWidget *parent = gtk_widget_get_parent(self->lbl_handle);
    if (GTK_IS_BOX(parent)) {
      /* Create the badge */
      self->nip05_badge = gnostr_nip05_create_badge();
      gtk_widget_set_visible(self->nip05_badge, FALSE);

      /* Insert badge after handle label */
      GtkWidget *next_sibling = gtk_widget_get_next_sibling(self->lbl_handle);
      if (next_sibling) {
        gtk_box_insert_child_after(GTK_BOX(parent), self->nip05_badge, self->lbl_handle);
      } else {
        gtk_box_append(GTK_BOX(parent), self->nip05_badge);
      }
    }
  }

  /* Check cache first for immediate display */
  GnostrNip05Result *cached = gnostr_nip05_cache_get(nip05);
  if (cached) {
    if (cached->status == GNOSTR_NIP05_STATUS_VERIFIED &&
        cached->pubkey_hex &&
        g_ascii_strcasecmp(cached->pubkey_hex, pubkey_hex) == 0) {
      if (self->nip05_badge) {
        gtk_widget_set_visible(self->nip05_badge, TRUE);
      }
      g_debug("note_card: NIP-05 verified from cache for %s", nip05);
    }
    gnostr_nip05_result_free(cached);
    return;
  }

  /* Verify async */
  self->nip05_cancellable = g_cancellable_new();
  gnostr_nip05_verify_async(nip05, pubkey_hex, on_note_nip05_verified, self, self->nip05_cancellable);
}

/* Set bookmark state and update button icon */
void gnostr_note_card_row_set_bookmarked(GnostrNoteCardRow *self, gboolean is_bookmarked) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_bookmarked = is_bookmarked;

  /* Update button icon */
  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
      is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
  }
}

/* Set like state and update button icon (NIP-25 reactions) */
void gnostr_note_card_row_set_liked(GnostrNoteCardRow *self, gboolean is_liked) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_liked = is_liked;

  /* Update button visual state */
  if (GTK_IS_BUTTON(self->btn_like)) {
    /* Use CSS class for visual differentiation - more reliable than icon switching.
     * CSS can style the "liked" class with different color (e.g., red/pink). */
    if (is_liked) {
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_like), "liked");
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(self->btn_like), "liked");
    }
  }
}

/* Set like count and update display (NIP-25 reactions) */
void gnostr_note_card_row_set_like_count(GnostrNoteCardRow *self, guint count) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->like_count = count;

  /* Update the like count label */
  if (GTK_IS_LABEL(self->lbl_like_count)) {
    if (count > 0) {
      gchar *text = g_strdup_printf("%u", count);
      gtk_label_set_text(GTK_LABEL(self->lbl_like_count), text);
      gtk_widget_set_visible(self->lbl_like_count, TRUE);
      g_free(text);
    } else {
      gtk_widget_set_visible(self->lbl_like_count, FALSE);
    }
  }
}

/* Set author's lightning address for NIP-57 zaps */
void gnostr_note_card_row_set_author_lud16(GnostrNoteCardRow *self, const char *lud16) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  g_clear_pointer(&self->author_lud16, g_free);
  self->author_lud16 = g_strdup(lud16);

  /* Update zap button sensitivity based on whether lud16 is available */
  if (GTK_IS_BUTTON(self->btn_zap)) {
    gboolean can_zap = (lud16 != NULL && *lud16 != '\0');
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_zap), can_zap);
    if (!can_zap) {
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_zap), "User has no lightning address");
    } else {
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_zap), "Zap");
    }
  }
}

/* Update zap statistics display */
void gnostr_note_card_row_set_zap_stats(GnostrNoteCardRow *self, guint zap_count, gint64 total_msat) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->zap_count = zap_count;
  self->zap_total_msat = total_msat;

  /* Update the zap count label */
  if (GTK_IS_LABEL(self->lbl_zap_count)) {
    if (zap_count > 0) {
      gchar *formatted = gnostr_zap_format_amount(total_msat);
      gtk_label_set_text(GTK_LABEL(self->lbl_zap_count), formatted);
      gtk_widget_set_visible(self->lbl_zap_count, TRUE);
      g_free(formatted);
    } else {
      gtk_widget_set_visible(self->lbl_zap_count, FALSE);
    }
  }
}

/* Set the reply count for thread root indicator */
void gnostr_note_card_row_set_reply_count(GnostrNoteCardRow *self, guint count) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->reply_count = count;
  self->is_thread_root = (count > 0);

  /* Update the reply count box visibility and label */
  if (GTK_IS_WIDGET(self->reply_count_box)) {
    gtk_widget_set_visible(self->reply_count_box, count > 0);
  }

  if (count > 0 && GTK_IS_LABEL(self->reply_count_label)) {
    gchar *text = NULL;
    if (count == 1) {
      text = g_strdup("1 reply");
    } else {
      text = g_strdup_printf("%u replies", count);
    }
    gtk_label_set_text(GTK_LABEL(self->reply_count_label), text);
    g_free(text);
  }

  /* Also show the thread button when there are replies */
  if (GTK_IS_BUTTON(self->btn_thread)) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_thread), count > 0);
  }
}

/* NIP-09: Set whether this is the current user's own note (enables delete option) */
void gnostr_note_card_row_set_is_own_note(GnostrNoteCardRow *self, gboolean is_own) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_own_note = is_own;

  /* Update delete button visibility if menu has been created */
  if (GTK_IS_BUTTON(self->delete_btn)) {
    gtk_widget_set_visible(self->delete_btn, is_own);
    /* Also update separator visibility */
    GtkWidget *sep = g_object_get_data(G_OBJECT(self->delete_btn), "delete-separator");
    if (GTK_IS_WIDGET(sep)) {
      gtk_widget_set_visible(sep, is_own);
    }
  }
}
