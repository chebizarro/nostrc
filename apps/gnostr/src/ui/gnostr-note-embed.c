/**
 * gnostr-note-embed.c - NIP-21 embedded note widget implementation
 *
 * Renders nostr: URI references as compact embedded cards.
 */

#include "gnostr-note-embed.h"
#include "gnostr-avatar-cache.h"
#include "../storage_ndb.h"
#include <nostr/nip19/nip19.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <json.h>
#include <nostr_simple_pool.h>
#include "../util/relays.h"
#include <string.h>
#include <time.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-note-embed.ui"

typedef enum {
  EMBED_TYPE_UNKNOWN,
  EMBED_TYPE_NOTE,     /* note1 or nevent */
  EMBED_TYPE_PROFILE,  /* npub or nprofile */
  EMBED_TYPE_ADDR,     /* naddr */
} EmbedType;

typedef enum {
  EMBED_STATE_EMPTY,
  EMBED_STATE_LOADING,
  EMBED_STATE_LOADED,
  EMBED_STATE_ERROR,
} EmbedState;

struct _GnostrNoteEmbed {
  GtkWidget parent_instance;

  /* Template children */
  GtkWidget *root_frame;
  GtkWidget *main_box;
  GtkWidget *header_box;
  GtkWidget *avatar_overlay;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *author_label;
  GtkWidget *handle_label;
  GtkWidget *timestamp_label;
  GtkWidget *content_label;
  GtkWidget *loading_spinner;
  GtkWidget *error_label;
  GtkWidget *profile_about_label;

  /* State */
  EmbedType embed_type;
  EmbedState state;
  char *target_id;        /* event ID hex or pubkey hex */
  char *original_uri;     /* original nostr: URI */
  char **relay_hints;     /* NULL-terminated array of relay URLs */
  size_t relay_hints_count;

  /* Cancellable for async operations */
  GCancellable *cancellable;

  /* Track whether relay hints have been attempted (for fallback to main pool) */
  gboolean hints_attempted;
  gboolean main_pool_attempted;

#ifdef HAVE_SOUP3
  SoupSession *session;
#endif
};

G_DEFINE_TYPE(GnostrNoteEmbed, gnostr_note_embed, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CLICKED,
  SIGNAL_PROFILE_CLICKED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void fetch_event_from_local(GnostrNoteEmbed *self, const unsigned char id32[32]);
static void fetch_event_from_relays(GnostrNoteEmbed *self, const char *id_hex);
static void fetch_profile_from_local(GnostrNoteEmbed *self, const unsigned char pk32[32]);
static void update_ui_state(GnostrNoteEmbed *self);

static void gnostr_note_embed_dispose(GObject *obj) {
  GnostrNoteEmbed *self = GNOSTR_NOTE_EMBED(obj);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

#ifdef HAVE_SOUP3
  g_clear_object(&self->session);
#endif

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_NOTE_EMBED);

  G_OBJECT_CLASS(gnostr_note_embed_parent_class)->dispose(obj);
}

static void gnostr_note_embed_finalize(GObject *obj) {
  GnostrNoteEmbed *self = GNOSTR_NOTE_EMBED(obj);

  g_clear_pointer(&self->target_id, g_free);
  g_clear_pointer(&self->original_uri, g_free);

  if (self->relay_hints) {
    for (size_t i = 0; i < self->relay_hints_count; i++) {
      g_free(self->relay_hints[i]);
    }
    g_free(self->relay_hints);
    self->relay_hints = NULL;
  }

  G_OBJECT_CLASS(gnostr_note_embed_parent_class)->finalize(obj);
}

static void on_embed_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
  (void)gesture; (void)n_press; (void)x; (void)y;
  GnostrNoteEmbed *self = GNOSTR_NOTE_EMBED(user_data);

  if (self->embed_type == EMBED_TYPE_PROFILE && self->target_id) {
    g_signal_emit(self, signals[SIGNAL_PROFILE_CLICKED], 0, self->target_id);
  } else {
    g_signal_emit(self, signals[SIGNAL_CLICKED], 0);
  }
}

static void gnostr_note_embed_class_init(GnostrNoteEmbedClass *klass) {
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

  gclass->dispose = gnostr_note_embed_dispose;
  gclass->finalize = gnostr_note_embed_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, root_frame);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, main_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, header_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, avatar_overlay);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, avatar_image);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, avatar_initials);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, author_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, handle_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, timestamp_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, content_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, loading_spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, error_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteEmbed, profile_about_label);

  signals[SIGNAL_CLICKED] = g_signal_new("clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_PROFILE_CLICKED] = g_signal_new("profile-clicked",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_note_embed_init(GnostrNoteEmbed *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->embed_type = EMBED_TYPE_UNKNOWN;
  self->state = EMBED_STATE_EMPTY;
  self->cancellable = g_cancellable_new();

#ifdef HAVE_SOUP3
  self->session = soup_session_new();
  soup_session_set_timeout(self->session, 15);
#endif

  /* Add click gesture */
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "released", G_CALLBACK(on_embed_clicked), self);
  gtk_widget_add_controller(GTK_WIDGET(self->root_frame), GTK_EVENT_CONTROLLER(click));

  /* Add hover cursor */
  gtk_widget_set_cursor_from_name(GTK_WIDGET(self->root_frame), "pointer");

  /* Add CSS class */
  gtk_widget_add_css_class(GTK_WIDGET(self), "note-embed");

  update_ui_state(self);
}

GnostrNoteEmbed *gnostr_note_embed_new(void) {
  return g_object_new(GNOSTR_TYPE_NOTE_EMBED, NULL);
}

static void update_ui_state(GnostrNoteEmbed *self) {
  if (!GTK_IS_WIDGET(self->main_box)) return;

  gboolean show_main = (self->state == EMBED_STATE_LOADED);
  gboolean show_loading = (self->state == EMBED_STATE_LOADING);
  gboolean show_error = (self->state == EMBED_STATE_ERROR);

  if (GTK_IS_WIDGET(self->main_box))
    gtk_widget_set_visible(self->main_box, show_main);
  if (GTK_IS_WIDGET(self->loading_spinner)) {
    gtk_widget_set_visible(self->loading_spinner, show_loading);
    if (show_loading)
      gtk_spinner_start(GTK_SPINNER(self->loading_spinner));
    else
      gtk_spinner_stop(GTK_SPINNER(self->loading_spinner));
  }
  if (GTK_IS_WIDGET(self->error_label))
    gtk_widget_set_visible(self->error_label, show_error);

  /* Show/hide profile-specific elements */
  if (GTK_IS_WIDGET(self->profile_about_label)) {
    gtk_widget_set_visible(self->profile_about_label,
                           show_main && self->embed_type == EMBED_TYPE_PROFILE);
  }
  if (GTK_IS_WIDGET(self->content_label)) {
    gtk_widget_set_visible(self->content_label,
                           show_main && self->embed_type != EMBED_TYPE_PROFILE);
  }
  if (GTK_IS_WIDGET(self->timestamp_label)) {
    gtk_widget_set_visible(self->timestamp_label,
                           show_main && self->embed_type != EMBED_TYPE_PROFILE);
  }
}

static void set_avatar_initials(GnostrNoteEmbed *self, const char *display, const char *handle) {
  if (!GTK_IS_LABEL(self->avatar_initials)) return;

  const char *src = (display && *display) ? display : (handle && *handle ? handle : "?");
  char initials[3] = {0};
  int i = 0;
  for (const char *p = src; *p && i < 2; p++) {
    if (g_ascii_isalnum(*p)) {
      initials[i++] = g_ascii_toupper(*p);
    }
  }
  if (i == 0) {
    initials[0] = '?';
  }
  gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);

  if (GTK_IS_WIDGET(self->avatar_image))
    gtk_widget_set_visible(self->avatar_image, FALSE);
  if (GTK_IS_WIDGET(self->avatar_initials))
    gtk_widget_set_visible(self->avatar_initials, TRUE);
}

static void load_avatar(GnostrNoteEmbed *self, const char *url, const char *display, const char *handle) {
  set_avatar_initials(self, display, handle);

  if (!url || !*url || !GTK_IS_PICTURE(self->avatar_image)) return;

  /* Try cache first */
  GdkTexture *cached = gnostr_avatar_try_load_cached(url);
  if (cached) {
    gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
    gtk_widget_set_visible(self->avatar_image, TRUE);
    gtk_widget_set_visible(self->avatar_initials, FALSE);
    g_object_unref(cached);
    return;
  }

  /* Download async */
  gnostr_avatar_download_async(url, self->avatar_image, self->avatar_initials);
}

/* Convert hex string to binary */
static gboolean hex_to_bytes_32(const char *hex, unsigned char out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + i*2, "%2x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* Convert 32-byte binary to hex string */
static void bytes_to_hex(const unsigned char *bytes, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) {
    sprintf(out + i*2, "%02x", bytes[i]);
  }
  out[len*2] = '\0';
}

/* Format timestamp */
static char *format_timestamp(gint64 created_at) {
  if (created_at <= 0) return g_strdup("");

  time_t now = time(NULL);
  long diff = (long)(now - (time_t)created_at);
  if (diff < 0) diff = 0;

  char buf[32];
  if (diff < 60) {
    g_snprintf(buf, sizeof(buf), "now");
  } else if (diff < 3600) {
    g_snprintf(buf, sizeof(buf), "%ldm", diff/60);
  } else if (diff < 86400) {
    g_snprintf(buf, sizeof(buf), "%ldh", diff/3600);
  } else {
    g_snprintf(buf, sizeof(buf), "%ldd", diff/86400);
  }
  return g_strdup(buf);
}

/* Truncate content for embed display */
static char *truncate_content(const char *content, size_t max_len) {
  if (!content || !*content) return g_strdup("");

  GString *out = g_string_new("");
  gboolean prev_space = FALSE;
  size_t n = 0;

  for (const char *p = content; *p && n < max_len; p++) {
    char c = *p;
    /* Normalize whitespace */
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (g_ascii_isspace(c)) {
      if (prev_space) continue;
      c = ' ';
      prev_space = TRUE;
    } else {
      prev_space = FALSE;
    }
    g_string_append_c(out, c);
    n++;
  }

  if (strlen(content) > max_len) {
    g_string_append(out, "...");
  }

  return g_string_free(out, FALSE);
}

/* Parse nostr: URI and extract type + data */
static gboolean parse_nostr_uri(const char *uri, EmbedType *type, char **target_hex,
                                 char ***relay_hints, size_t *relay_count) {
  if (!uri) return FALSE;

  const char *ref = uri;
  if (g_str_has_prefix(ref, "nostr:")) {
    ref = uri + 6;
  }

  *type = EMBED_TYPE_UNKNOWN;
  *target_hex = NULL;
  *relay_hints = NULL;
  *relay_count = 0;

  NostrBech32Type b32_type;
  if (nostr_nip19_inspect(ref, &b32_type) != 0) {
    return FALSE;
  }

  switch (b32_type) {
    case NOSTR_B32_NOTE: {
      unsigned char id32[32];
      if (nostr_nip19_decode_note(ref, id32) == 0) {
        *type = EMBED_TYPE_NOTE;
        *target_hex = g_malloc(65);
        bytes_to_hex(id32, 32, *target_hex);
        return TRUE;
      }
      break;
    }

    case NOSTR_B32_NPUB: {
      unsigned char pk32[32];
      if (nostr_nip19_decode_npub(ref, pk32) == 0) {
        *type = EMBED_TYPE_PROFILE;
        *target_hex = g_malloc(65);
        bytes_to_hex(pk32, 32, *target_hex);
        return TRUE;
      }
      break;
    }

    case NOSTR_B32_NEVENT: {
      NostrEventPointer *ptr = NULL;
      if (nostr_nip19_decode_nevent(ref, &ptr) == 0 && ptr) {
        *type = EMBED_TYPE_NOTE;
        *target_hex = g_strdup(ptr->id);

        /* Copy relay hints */
        if (ptr->relays_count > 0) {
          *relay_hints = g_new0(char*, ptr->relays_count + 1);
          for (size_t i = 0; i < ptr->relays_count; i++) {
            (*relay_hints)[i] = g_strdup(ptr->relays[i]);
          }
          *relay_count = ptr->relays_count;
        }

        nostr_event_pointer_free(ptr);
        return TRUE;
      }
      break;
    }

    case NOSTR_B32_NPROFILE: {
      NostrProfilePointer *ptr = NULL;
      if (nostr_nip19_decode_nprofile(ref, &ptr) == 0 && ptr) {
        *type = EMBED_TYPE_PROFILE;
        *target_hex = g_strdup(ptr->public_key);

        /* Copy relay hints */
        if (ptr->relays_count > 0) {
          *relay_hints = g_new0(char*, ptr->relays_count + 1);
          for (size_t i = 0; i < ptr->relays_count; i++) {
            (*relay_hints)[i] = g_strdup(ptr->relays[i]);
          }
          *relay_count = ptr->relays_count;
        }

        nostr_profile_pointer_free(ptr);
        return TRUE;
      }
      break;
    }

    case NOSTR_B32_NADDR: {
      NostrEntityPointer *ptr = NULL;
      if (nostr_nip19_decode_naddr(ref, &ptr) == 0 && ptr) {
        *type = EMBED_TYPE_ADDR;
        /* For naddr, we use author pubkey as target (will need special handling) */
        *target_hex = g_strdup(ptr->public_key);

        /* Copy relay hints */
        if (ptr->relays_count > 0) {
          *relay_hints = g_new0(char*, ptr->relays_count + 1);
          for (size_t i = 0; i < ptr->relays_count; i++) {
            (*relay_hints)[i] = g_strdup(ptr->relays[i]);
          }
          *relay_count = ptr->relays_count;
        }

        nostr_entity_pointer_free(ptr);
        return TRUE;
      }
      break;
    }

    default:
      break;
  }

  return FALSE;
}

void gnostr_note_embed_set_nostr_uri(GnostrNoteEmbed *self, const char *uri) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));

  /* Cancel any pending operations */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
    self->cancellable = g_cancellable_new();
  }

  /* Clear previous state */
  g_clear_pointer(&self->target_id, g_free);
  g_clear_pointer(&self->original_uri, g_free);
  self->hints_attempted = FALSE;
  self->main_pool_attempted = FALSE;
  if (self->relay_hints) {
    for (size_t i = 0; i < self->relay_hints_count; i++) {
      g_free(self->relay_hints[i]);
    }
    g_free(self->relay_hints);
    self->relay_hints = NULL;
    self->relay_hints_count = 0;
  }

  if (!uri || !*uri) {
    self->state = EMBED_STATE_EMPTY;
    update_ui_state(self);
    return;
  }

  self->original_uri = g_strdup(uri);

  /* Parse the URI */
  EmbedType type;
  char *target_hex = NULL;
  char **hints = NULL;
  size_t hint_count = 0;

  if (!parse_nostr_uri(uri, &type, &target_hex, &hints, &hint_count)) {
    self->state = EMBED_STATE_ERROR;
    if (GTK_IS_LABEL(self->error_label)) {
      gtk_label_set_text(GTK_LABEL(self->error_label), "Invalid nostr URI");
    }
    update_ui_state(self);
    return;
  }

  self->embed_type = type;
  self->target_id = target_hex;
  self->relay_hints = hints;
  self->relay_hints_count = hint_count;

  /* Set loading state */
  self->state = EMBED_STATE_LOADING;
  update_ui_state(self);

  /* Start fetching */
  unsigned char bytes32[32];
  if (hex_to_bytes_32(target_hex, bytes32)) {
    if (type == EMBED_TYPE_PROFILE) {
      fetch_profile_from_local(self, bytes32);
    } else {
      fetch_event_from_local(self, bytes32);
    }
  } else {
    self->state = EMBED_STATE_ERROR;
    if (GTK_IS_LABEL(self->error_label)) {
      gtk_label_set_text(GTK_LABEL(self->error_label), "Invalid hex ID");
    }
    update_ui_state(self);
  }
}

void gnostr_note_embed_set_event_id(GnostrNoteEmbed *self,
                                     const char *event_id_hex,
                                     const char * const *relay_hints) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));

  g_clear_pointer(&self->target_id, g_free);
  self->target_id = g_strdup(event_id_hex);
  self->embed_type = EMBED_TYPE_NOTE;

  /* Copy relay hints */
  if (self->relay_hints) {
    for (size_t i = 0; i < self->relay_hints_count; i++) {
      g_free(self->relay_hints[i]);
    }
    g_free(self->relay_hints);
    self->relay_hints = NULL;
    self->relay_hints_count = 0;
  }

  if (relay_hints) {
    size_t count = 0;
    while (relay_hints[count]) count++;
    if (count > 0) {
      self->relay_hints = g_new0(char*, count + 1);
      for (size_t i = 0; i < count; i++) {
        self->relay_hints[i] = g_strdup(relay_hints[i]);
      }
      self->relay_hints_count = count;
    }
  }

  self->state = EMBED_STATE_LOADING;
  update_ui_state(self);

  unsigned char id32[32];
  if (hex_to_bytes_32(event_id_hex, id32)) {
    fetch_event_from_local(self, id32);
  }
}

void gnostr_note_embed_set_pubkey(GnostrNoteEmbed *self,
                                   const char *pubkey_hex,
                                   const char * const *relay_hints) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));

  g_clear_pointer(&self->target_id, g_free);
  self->target_id = g_strdup(pubkey_hex);
  self->embed_type = EMBED_TYPE_PROFILE;

  /* Copy relay hints */
  if (self->relay_hints) {
    for (size_t i = 0; i < self->relay_hints_count; i++) {
      g_free(self->relay_hints[i]);
    }
    g_free(self->relay_hints);
    self->relay_hints = NULL;
    self->relay_hints_count = 0;
  }

  if (relay_hints) {
    size_t count = 0;
    while (relay_hints[count]) count++;
    if (count > 0) {
      self->relay_hints = g_new0(char*, count + 1);
      for (size_t i = 0; i < count; i++) {
        self->relay_hints[i] = g_strdup(relay_hints[i]);
      }
      self->relay_hints_count = count;
    }
  }

  self->state = EMBED_STATE_LOADING;
  update_ui_state(self);

  unsigned char pk32[32];
  if (hex_to_bytes_32(pubkey_hex, pk32)) {
    fetch_profile_from_local(self, pk32);
  }
}

void gnostr_note_embed_set_loading(GnostrNoteEmbed *self, gboolean loading) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));
  self->state = loading ? EMBED_STATE_LOADING : EMBED_STATE_EMPTY;
  update_ui_state(self);
}

void gnostr_note_embed_set_error(GnostrNoteEmbed *self, const char *error_message) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));
  self->state = EMBED_STATE_ERROR;
  if (GTK_IS_LABEL(self->error_label)) {
    gtk_label_set_text(GTK_LABEL(self->error_label),
                       error_message ? error_message : "Failed to load");
  }
  update_ui_state(self);
}

void gnostr_note_embed_set_content(GnostrNoteEmbed *self,
                                    const char *author_display,
                                    const char *author_handle,
                                    const char *content,
                                    const char *timestamp,
                                    const char *avatar_url) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));

  self->embed_type = EMBED_TYPE_NOTE;
  self->state = EMBED_STATE_LOADED;

  if (GTK_IS_LABEL(self->author_label)) {
    gtk_label_set_text(GTK_LABEL(self->author_label),
                       author_display && *author_display ? author_display : "Anonymous");
  }

  if (GTK_IS_LABEL(self->handle_label)) {
    char *handle_text = NULL;
    if (author_handle && *author_handle) {
      if (author_handle[0] != '@') {
        handle_text = g_strdup_printf("@%s", author_handle);
      } else {
        handle_text = g_strdup(author_handle);
      }
    }
    gtk_label_set_text(GTK_LABEL(self->handle_label), handle_text ? handle_text : "");
    g_free(handle_text);
  }

  if (GTK_IS_LABEL(self->timestamp_label)) {
    gtk_label_set_text(GTK_LABEL(self->timestamp_label), timestamp ? timestamp : "");
  }

  if (GTK_IS_LABEL(self->content_label)) {
    char *truncated = truncate_content(content, 200);
    gtk_label_set_text(GTK_LABEL(self->content_label), truncated);
    g_free(truncated);
  }

  load_avatar(self, avatar_url, author_display, author_handle);

  update_ui_state(self);
}

void gnostr_note_embed_set_profile(GnostrNoteEmbed *self,
                                    const char *display_name,
                                    const char *handle,
                                    const char *about,
                                    const char *avatar_url,
                                    const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));

  self->embed_type = EMBED_TYPE_PROFILE;
  self->state = EMBED_STATE_LOADED;

  g_clear_pointer(&self->target_id, g_free);
  self->target_id = g_strdup(pubkey_hex);

  if (GTK_IS_LABEL(self->author_label)) {
    gtk_label_set_text(GTK_LABEL(self->author_label),
                       display_name && *display_name ? display_name : "Anonymous");
  }

  if (GTK_IS_LABEL(self->handle_label)) {
    char *handle_text = NULL;
    if (handle && *handle) {
      if (handle[0] != '@') {
        handle_text = g_strdup_printf("@%s", handle);
      } else {
        handle_text = g_strdup(handle);
      }
    } else if (pubkey_hex && strlen(pubkey_hex) >= 8) {
      handle_text = g_strdup_printf("%.8s...", pubkey_hex);
    }
    gtk_label_set_text(GTK_LABEL(self->handle_label), handle_text ? handle_text : "");
    g_free(handle_text);
  }

  if (GTK_IS_LABEL(self->profile_about_label)) {
    char *truncated = truncate_content(about, 150);
    gtk_label_set_text(GTK_LABEL(self->profile_about_label), truncated ? truncated : "");
    g_free(truncated);
  }

  load_avatar(self, avatar_url, display_name, handle);

  update_ui_state(self);
}

const char *gnostr_note_embed_get_target_id(GnostrNoteEmbed *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_EMBED(self), NULL);
  return self->target_id;
}

gboolean gnostr_note_embed_is_profile(GnostrNoteEmbed *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_EMBED(self), FALSE);
  return self->embed_type == EMBED_TYPE_PROFILE;
}

/* Local database fetch for events */
static void fetch_event_from_local(GnostrNoteEmbed *self, const unsigned char id32[32]) {
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    fetch_event_from_relays(self, self->target_id);
    return;
  }

  char *json = NULL;
  int json_len = 0;
  if (storage_ndb_get_note_by_id(txn, id32, &json, &json_len) == 0 && json) {
    /* Parse and display */
    NostrEvent *evt = nostr_event_new();
    if (evt && nostr_event_deserialize(evt, json) == 0) {
      const char *content = nostr_event_get_content(evt);
      const char *author_hex = nostr_event_get_pubkey(evt);
      gint64 created_at = (gint64)nostr_event_get_created_at(evt);

      char *ts = format_timestamp(created_at);

      /* Try to get author profile */
      unsigned char author_pk[32];
      char *author_display = NULL;
      char *author_handle = NULL;
      char *avatar_url = NULL;

      if (author_hex && hex_to_bytes_32(author_hex, author_pk)) {
        char *profile_json = NULL;
        int profile_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, author_pk, &profile_json, &profile_len) == 0 && profile_json) {
          /* Parse profile JSON to extract name/picture */
          /* Simple JSON extraction - could use json-glib for robustness */
          /* For now, just use author hex prefix */
          g_free(profile_json);
        }
      }

      if (!author_display && author_hex && strlen(author_hex) >= 8) {
        author_display = g_strdup_printf("%.8s...", author_hex);
      }

      gnostr_note_embed_set_content(self, author_display, author_handle,
                                     content, ts, avatar_url);

      g_free(ts);
      g_free(author_display);
      g_free(author_handle);
      g_free(avatar_url);
    } else {
      gnostr_note_embed_set_error(self, "Failed to parse event");
    }
    if (evt) nostr_event_free(evt);
    g_free(json);
  } else {
    /* Not in local cache, try relays */
    storage_ndb_end_query(txn);
    fetch_event_from_relays(self, self->target_id);
    return;
  }

  storage_ndb_end_query(txn);
}

/* Forward declaration for fallback */
static void fetch_event_from_main_pool(GnostrNoteEmbed *self, const char *id_hex);

/* Callback for relay query */
static void on_relay_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrNoteEmbed *self = GNOSTR_NOTE_EMBED(user_data);

  if (!GNOSTR_IS_NOTE_EMBED(self)) return;

  GError *err = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      /* If hints were tried and failed (but main pool not yet), fall back to main pool */
      if (self->hints_attempted && !self->main_pool_attempted && self->relay_hints_count > 0) {
        g_debug("note_embed: hint relays failed, falling back to main pool");
        g_error_free(err);
        fetch_event_from_main_pool(self, self->target_id);
        return;
      }
      gnostr_note_embed_set_error(self, "Network error");
    }
    g_error_free(err);
    return;
  }

  if (!results || results->len == 0) {
    /* If hints were tried and found nothing (but main pool not yet), fall back to main pool */
    if (self->hints_attempted && !self->main_pool_attempted && self->relay_hints_count > 0) {
      g_debug("note_embed: not found on hint relays, falling back to main pool");
      if (results) g_ptr_array_unref(results);
      fetch_event_from_main_pool(self, self->target_id);
      return;
    }
    gnostr_note_embed_set_error(self, "Not found");
    if (results) g_ptr_array_unref(results);
    return;
  }

  const char *json = (const char*)g_ptr_array_index(results, 0);
  if (!json) {
    gnostr_note_embed_set_error(self, "Empty result");
    g_ptr_array_unref(results);
    return;
  }

  /* Ingest into local store */
  storage_ndb_ingest_event_json(json, NULL);

  /* Parse and display */
  NostrEvent *evt = nostr_event_new();
  if (evt && nostr_event_deserialize(evt, json) == 0) {
    const char *content = nostr_event_get_content(evt);
    const char *author_hex = nostr_event_get_pubkey(evt);
    gint64 created_at = (gint64)nostr_event_get_created_at(evt);

    char *ts = format_timestamp(created_at);
    char *author_display = NULL;
    if (author_hex && strlen(author_hex) >= 8) {
      author_display = g_strdup_printf("%.8s...", author_hex);
    }

    gnostr_note_embed_set_content(self, author_display, NULL, content, ts, NULL);

    g_free(ts);
    g_free(author_display);
  } else {
    gnostr_note_embed_set_error(self, "Failed to parse");
  }

  if (evt) nostr_event_free(evt);
  g_ptr_array_unref(results);
}

/* Shared pool for embed queries - initialized lazily with pre-connected relays */
static GnostrSimplePool *embed_pool = NULL;
static gboolean embed_pool_initialized = FALSE;

/* Initialize the shared embed pool with pre-connected relays */
static void ensure_embed_pool_initialized(void) {
  if (embed_pool_initialized) return;
  embed_pool_initialized = TRUE;

  if (!embed_pool) {
    embed_pool = gnostr_simple_pool_new();
  }

  /* Pre-load configured relays into pool for faster first queries */
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(urls);

  if (urls->len > 0) {
    g_debug("embed_pool: Pre-connecting %u relays for embed queries", urls->len);
    /* The pool's query_single will connect and add relays as needed,
     * but we can prime the process by logging the relay count */
  }

  g_ptr_array_free(urls, TRUE);
}

/* Fetch event from relays - try hints first, then main pool */
static void fetch_event_from_relays(GnostrNoteEmbed *self, const char *id_hex) {
  if (!id_hex) {
    gnostr_note_embed_set_error(self, "No event ID");
    return;
  }

  /* Ensure pool is initialized */
  ensure_embed_pool_initialized();

  /* If we have relay hints and haven't tried them yet, use hints only first */
  if (self->relay_hints && self->relay_hints_count > 0 && !self->hints_attempted) {
    self->hints_attempted = TRUE;

    GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
    for (size_t i = 0; i < self->relay_hints_count; i++) {
      g_ptr_array_add(urls, g_strdup(self->relay_hints[i]));
    }

    g_debug("note_embed: trying %zu relay hints first for %s", self->relay_hints_count, id_hex);

    /* Build filter */
    NostrFilter *filter = nostr_filter_new();
    const char *ids[1] = { id_hex };
    nostr_filter_set_ids(filter, ids, 1);

    /* Convert GPtrArray to const char** */
    const char **url_arr = g_new0(const char*, urls->len);
    for (guint i = 0; i < urls->len; i++) {
      url_arr[i] = (const char*)g_ptr_array_index(urls, i);
    }

    gnostr_simple_pool_query_single_async(embed_pool, url_arr, urls->len, filter,
                                           self->cancellable, on_relay_query_done, self);

    g_free(url_arr);
    g_ptr_array_free(urls, TRUE);
    nostr_filter_free(filter);
    return;
  }

  /* No hints or hints already tried - use main pool */
  fetch_event_from_main_pool(self, id_hex);
}

/* Fetch event from main relay pool (fallback) */
static void fetch_event_from_main_pool(GnostrNoteEmbed *self, const char *id_hex) {
  self->main_pool_attempted = TRUE;

  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(urls);

  if (urls->len == 0) {
    /* Add default relays as last resort */
    g_ptr_array_add(urls, g_strdup("wss://relay.damus.io"));
    g_ptr_array_add(urls, g_strdup("wss://nos.lol"));
    g_ptr_array_add(urls, g_strdup("wss://relay.nostr.band"));
  }

  g_debug("note_embed: trying %u main pool relays for %s", urls->len, id_hex);

  /* Build filter */
  NostrFilter *filter = nostr_filter_new();
  const char *ids[1] = { id_hex };
  nostr_filter_set_ids(filter, ids, 1);

  /* Convert GPtrArray to const char** */
  const char **url_arr = g_new0(const char*, urls->len);
  for (guint i = 0; i < urls->len; i++) {
    url_arr[i] = (const char*)g_ptr_array_index(urls, i);
  }

  gnostr_simple_pool_query_single_async(embed_pool, url_arr, urls->len, filter,
                                         self->cancellable, on_relay_query_done, self);

  g_free(url_arr);
  g_ptr_array_free(urls, TRUE);
  nostr_filter_free(filter);
}

/* Local database fetch for profiles */
static void fetch_profile_from_local(GnostrNoteEmbed *self, const unsigned char pk32[32]) {
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    /* Show basic profile with just pubkey */
    gnostr_note_embed_set_profile(self, NULL, NULL, NULL, NULL, self->target_id);
    return;
  }

  char *json = NULL;
  int json_len = 0;
  if (storage_ndb_get_profile_by_pubkey(txn, pk32, &json, &json_len) == 0 && json) {
    /* Parse profile - this is kind 0 content JSON */
    /* For now, just show as loaded with pubkey */
    /* TODO: Parse JSON to extract name, about, picture */
    gnostr_note_embed_set_profile(self, NULL, NULL, NULL, NULL, self->target_id);
    g_free(json);
  } else {
    /* Not in cache, show basic profile */
    gnostr_note_embed_set_profile(self, NULL, NULL, NULL, NULL, self->target_id);
  }

  storage_ndb_end_query(txn);
}
