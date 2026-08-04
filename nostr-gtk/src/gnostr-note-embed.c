/**
 * gnostr-note-embed.c - NIP-21 embedded note widget implementation
 *
 * Renders nostr: URI references as compact embedded cards.
 */

#include <nostr-gtk-1.0/gnostr-note-embed.h>
#include <nostr-gtk-1.0/content_renderer.h>
#include "gnostr-avatar-cache.h"
#include "../util/utils.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/nostr_profile_service.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
/* nostr_pool.h provided via utils.h */
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "gnostr-label-guard.h"
#include <string.h>
#include <time.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/nostr/gtk/ui/widgets/gnostr-note-embed.ui"
#define MAX_EMBED_HINT_RELAYS 8

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
  gint address_kind;      /* naddr kind, or -1 */
  char *address_identifier; /* naddr d-tag identifier */
  char **relay_hints;     /* NULL-terminated array of relay URLs */
  size_t relay_hints_count;

  /* Cancellable for async operations */
  GCancellable *cancellable;
  
  /* External cancellable from parent widget (not owned, just referenced) */
  GCancellable *external_cancellable;

  /* Track whether relay hints have been attempted (for fallback to main pool) */
  gboolean hints_attempted;
  gboolean main_pool_attempted;

  /* Disposal flag - set during prepare_for_unbind to prevent callbacks from accessing widget */
  gboolean disposed;

#ifdef HAVE_SOUP3
  /* Uses gnostr_get_shared_soup_session() instead of per-widget session */
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
static void fetch_address_from_local(GnostrNoteEmbed *self);
static void fetch_event_from_relays(GnostrNoteEmbed *self, const char *id_hex);
static void request_profile_via_service(GnostrNoteEmbed *self, const char *pubkey_hex);
static void apply_relay_event_json(GnostrNoteEmbed *self, const char *json);
static void on_profile_service_result(const char *pubkey_hex,
                                       const GnostrProfileMeta *meta,
                                       gpointer user_data);
static void update_ui_state(GnostrNoteEmbed *self);

/* Helper: get effective cancellable (external from parent if set, otherwise internal) */
static GCancellable *get_effective_cancellable(GnostrNoteEmbed *self) {
  return self->external_cancellable ? self->external_cancellable : self->cancellable;
}

static void gnostr_note_embed_dispose(GObject *obj) {
  GnostrNoteEmbed *self = GNOSTR_NOTE_EMBED(obj);

  self->disposed = TRUE;

  /* nostrc-pb01: Cancel any pending profile service callbacks for this widget.
   * get_default() is documented as always returning a valid singleton. */
  gnostr_profile_service_cancel_for_user_data(
      gnostr_profile_service_get_default(), self);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

#ifdef HAVE_SOUP3
  /* Shared session is managed globally - do not clear here */
#endif

  /* Clear layout manager to prevent measurement during disposal cascade. */
  gtk_widget_set_layout_manager(GTK_WIDGET(self), NULL);

  /* Safe label cleanup: clear text when native is available (resets
   * PangoLayout), ref-leak when native is gone to prevent finalization
   * crash in pango_layout_clear_lines.  Same pattern as note_card_row. */
#define EMBED_DISPOSE_LABEL(lbl) \
  do { \
    if (GNOSTR_LABEL_SAFE(lbl)) { \
      gtk_label_set_text(GTK_LABEL(lbl), ""); \
    } else if (GTK_IS_LABEL(lbl)) { \
      const char *_t = gtk_label_get_text(GTK_LABEL(lbl)); \
      if (_t && *_t) g_object_ref(lbl); \
    } \
  } while (0)

  EMBED_DISPOSE_LABEL(self->content_label);
  EMBED_DISPOSE_LABEL(self->author_label);
  EMBED_DISPOSE_LABEL(self->handle_label);
  EMBED_DISPOSE_LABEL(self->timestamp_label);
  EMBED_DISPOSE_LABEL(self->error_label);
  EMBED_DISPOSE_LABEL(self->profile_about_label);

#undef EMBED_DISPOSE_LABEL

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_NOTE_EMBED);

  G_OBJECT_CLASS(gnostr_note_embed_parent_class)->dispose(obj);
}

static void gnostr_note_embed_finalize(GObject *obj) {
  GnostrNoteEmbed *self = GNOSTR_NOTE_EMBED(obj);

  g_clear_pointer(&self->target_id, g_free);
  g_clear_pointer(&self->original_uri, g_free);
  g_clear_pointer(&self->address_identifier, g_free);

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

static void
gnostr_note_embed_measure(GtkWidget      *widget,
                           GtkOrientation  orientation,
                           int             for_size,
                           int            *minimum,
                           int            *natural,
                           int            *minimum_baseline,
                           int            *natural_baseline)
{
  GnostrNoteEmbed *self = GNOSTR_NOTE_EMBED(widget);

  /* Guard: skip parent measure when disposed — child GtkLabels in liminal
   * state can have NULL PangoLayout, causing SEGV in pango_layout_set_width
   * during the layout traversal.  Return zeros; the widget is being recycled. */
  if (self->disposed) {
    *minimum = 0;
    *natural = 0;
    *minimum_baseline = -1;
    *natural_baseline = -1;
    return;
  }

  GTK_WIDGET_CLASS(gnostr_note_embed_parent_class)->measure(
      widget, orientation, for_size,
      minimum, natural, minimum_baseline, natural_baseline);

  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    const int MAX_EMBED_WIDTH = 700;
    if (*natural > MAX_EMBED_WIDTH) {
      *natural = MAX_EMBED_WIDTH;
    }
    if (*minimum > MAX_EMBED_WIDTH) {
      *minimum = MAX_EMBED_WIDTH;
    }
  }
}

static void gnostr_note_embed_class_init(GnostrNoteEmbedClass *klass) {
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

  gclass->dispose = gnostr_note_embed_dispose;
  gclass->finalize = gnostr_note_embed_finalize;
  wclass->measure = gnostr_note_embed_measure;

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
  self->address_kind = -1;
  self->cancellable = g_cancellable_new();

#ifdef HAVE_SOUP3
  /* Uses shared session from gnostr_get_shared_soup_session() */
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
  if (!GTK_IS_WIDGET(self->main_box)) {
    g_warning("NOTE_EMBED: update_ui_state - main_box is NOT a widget!");
    return;
  }

  gboolean show_main = (self->state == EMBED_STATE_LOADED);
  gboolean show_loading = (self->state == EMBED_STATE_LOADING);
  gboolean show_error = (self->state == EMBED_STATE_ERROR);

  g_debug("NOTE_EMBED: update_ui_state - state=%d show_main=%d show_loading=%d show_error=%d",
          self->state, show_main, show_loading, show_error);

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
    snprintf(out + i*2, 3, "%02x", bytes[i]);
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
                                 gint *address_kind, char **address_identifier,
                                 char ***relay_hints, size_t *relay_count) {
  if (!uri) return FALSE;

  const char *ref = uri;
  if (g_str_has_prefix(ref, "nostr:")) {
    ref = uri + 6;
  }

  *type = EMBED_TYPE_UNKNOWN;
  *target_hex = NULL;
  *address_kind = -1;
  *address_identifier = NULL;
  *relay_hints = NULL;
  *relay_count = 0;

  /* Decode once, then inspect the entity type */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(ref, NULL);
  if (!n19) {
    return FALSE;
  }

  GNostrBech32Type b32_type = gnostr_nip19_get_entity_type(n19);

  switch (b32_type) {
    case GNOSTR_BECH32_NOTE: {
      const gchar *event_id = gnostr_nip19_get_event_id(n19);
      if (event_id) {
        *type = EMBED_TYPE_NOTE;
        *target_hex = g_strdup(event_id);
        return TRUE;
      }
      break;
    }

    case GNOSTR_BECH32_NPUB: {
      const gchar *pubkey = gnostr_nip19_get_pubkey(n19);
      if (pubkey) {
        *type = EMBED_TYPE_PROFILE;
        *target_hex = g_strdup(pubkey);
        return TRUE;
      }
      break;
    }

    case GNOSTR_BECH32_NEVENT: {
      const gchar *event_id = gnostr_nip19_get_event_id(n19);
      if (event_id) {
        *type = EMBED_TYPE_NOTE;
        *target_hex = g_strdup(event_id);

        /* Copy relay hints */
        const gchar *const *relays = gnostr_nip19_get_relays(n19);
        if (relays) {
          *relay_hints = g_strdupv((gchar **)relays);
          *relay_count = g_strv_length((gchar **)relays);
        }

        return TRUE;
      }
      break;
    }

    case GNOSTR_BECH32_NPROFILE: {
      const gchar *pubkey = gnostr_nip19_get_pubkey(n19);
      if (pubkey) {
        *type = EMBED_TYPE_PROFILE;
        *target_hex = g_strdup(pubkey);

        /* Copy relay hints */
        const gchar *const *relays = gnostr_nip19_get_relays(n19);
        if (relays) {
          *relay_hints = g_strdupv((gchar **)relays);
          *relay_count = g_strv_length((gchar **)relays);
        }

        return TRUE;
      }
      break;
    }

    case GNOSTR_BECH32_NADDR: {
      const gchar *pubkey = gnostr_nip19_get_pubkey(n19);
      const gchar *identifier = gnostr_nip19_get_identifier(n19);
      gint kind = gnostr_nip19_get_kind(n19);
      if (pubkey && identifier && kind >= 0) {
        *type = EMBED_TYPE_ADDR;
        *target_hex = g_strdup(pubkey);
        *address_kind = kind;
        *address_identifier = g_strdup(identifier);

        /* Copy relay hints */
        const gchar *const *relays = gnostr_nip19_get_relays(n19);
        if (relays) {
          *relay_hints = g_strdupv((gchar **)relays);
          *relay_count = g_strv_length((gchar **)relays);
        }

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

  /* nostrc-pb01: clear disposed flag in case prepare_for_unbind set it before
   * this list-view recycling rebind.  Async profile-service callbacks check
   * this flag and drop results when it is TRUE. */
  self->disposed = FALSE;

  /* Cancel any pending operations */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
    self->cancellable = g_cancellable_new();
  }

  /* Clear previous state */
  g_clear_pointer(&self->target_id, g_free);
  g_clear_pointer(&self->original_uri, g_free);
  g_clear_pointer(&self->address_identifier, g_free);
  self->address_kind = -1;
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
  gint address_kind = -1;
  char *address_identifier = NULL;
  char **hints = NULL;
  size_t hint_count = 0;

  if (!parse_nostr_uri(uri, &type, &target_hex,
                       &address_kind, &address_identifier,
                       &hints, &hint_count)) {
    self->state = EMBED_STATE_ERROR;
    if (GTK_IS_LABEL(self->error_label)) {
      gtk_label_set_text(GTK_LABEL(self->error_label), "Invalid nostr URI");
    }
    update_ui_state(self);
    return;
  }

  self->embed_type = type;
  self->target_id = target_hex;
  self->address_kind = address_kind;
  self->address_identifier = address_identifier;
  self->relay_hints = hints;
  self->relay_hints_count = hint_count;

  /* Set loading state */
  self->state = EMBED_STATE_LOADING;
  update_ui_state(self);

  /* Start fetching */
  unsigned char bytes32[32];
  if (hex_to_bytes_32(target_hex, bytes32)) {
    if (type == EMBED_TYPE_PROFILE) {
      /* nostrc-pb01: delegate profile loading to centralized service */
      request_profile_via_service(self, target_hex);
    } else if (type == EMBED_TYPE_ADDR) {
      fetch_address_from_local(self);
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

  /* nostrc-pb01: clear disposed flag on rebind (see set_nostr_uri) */
  self->disposed = FALSE;

  g_clear_pointer(&self->target_id, g_free);
  g_clear_pointer(&self->address_identifier, g_free);
  self->address_kind = -1;
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

  /* nostrc-pb01: clear disposed flag on rebind (see set_nostr_uri) */
  self->disposed = FALSE;

  /* nostrc-akyz: defensively normalize npub/nprofile to hex */
  g_autofree gchar *hex = gnostr_ensure_hex_pubkey(pubkey_hex);
  g_clear_pointer(&self->target_id, g_free);
  g_clear_pointer(&self->address_identifier, g_free);
  self->address_kind = -1;
  self->target_id = hex ? g_strdup(hex) : g_strdup(pubkey_hex);
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

  /* nostrc-pb01: delegate to centralized profile service (checks LRU cache,
   * NDB, then batches network fetches). */
  request_profile_via_service(self, self->target_id);
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

  g_debug("NOTE_EMBED: set_content called - author='%s' content='%.50s...'",
          author_display ? author_display : "(null)",
          content ? content : "(null)");

  self->embed_type = EMBED_TYPE_NOTE;
  self->state = EMBED_STATE_LOADED;

  if (GTK_IS_LABEL(self->author_label)) {
    gtk_label_set_text(GTK_LABEL(self->author_label),
                       author_display && *author_display ? author_display : "Anonymous");
  }

  if (GTK_IS_LABEL(self->handle_label)) {
    g_autofree char *handle_text = NULL;
    if (author_handle && *author_handle) {
      if (author_handle[0] != '@') {
        handle_text = g_strdup_printf("@%s", author_handle);
      } else {
        handle_text = g_strdup(author_handle);
      }
    }
    gtk_label_set_text(GTK_LABEL(self->handle_label), handle_text ? handle_text : "");
  }

  if (GTK_IS_LABEL(self->timestamp_label)) {
    gtk_label_set_text(GTK_LABEL(self->timestamp_label), timestamp ? timestamp : "");
  }

  if (GTK_IS_LABEL(self->content_label)) {
    /* Use content renderer to format nostr: URIs and other special content.
     * Don't pre-truncate - let the label's lines/ellipsize handle visual truncation
     * so URLs and nostr: refs are properly formatted before being cut off. */
    if (content && *content) {
      GnContentRenderResult *render = gnostr_render_content(content, -1, NULL);
      /* nostrc-csaf: Use safe markup setter for relay content */
      gnostr_safe_set_markup(GTK_LABEL(self->content_label), render->markup);
      gnostr_content_render_result_free(render);
    } else {
      gtk_label_set_text(GTK_LABEL(self->content_label), "");
    }
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

  /* Duplicate pubkey_hex BEFORE freeing target_id in case they point to same memory.
   * After this point, use self->target_id instead of pubkey_hex parameter! */
  char *new_target_id = g_strdup(pubkey_hex);
  g_clear_pointer(&self->target_id, g_free);
  self->target_id = new_target_id;

  if (GTK_IS_LABEL(self->author_label)) {
    gtk_label_set_text(GTK_LABEL(self->author_label),
                       display_name && *display_name ? display_name : "Anonymous");
  }

  if (GTK_IS_LABEL(self->handle_label)) {
    g_autofree char *handle_text = NULL;
    if (handle && *handle) {
      if (handle[0] != '@') {
        handle_text = g_strdup_printf("@%s", handle);
      } else {
        handle_text = g_strdup(handle);
      }
    } else if (self->target_id && strlen(self->target_id) >= 8) {
      handle_text = g_strdup_printf("%.8s...", self->target_id);
    }
    gtk_label_set_text(GTK_LABEL(self->handle_label), handle_text ? handle_text : "");
  }

  if (GTK_IS_LABEL(self->profile_about_label)) {
    g_autofree char *truncated = truncate_content(about, 150);
    gtk_label_set_text(GTK_LABEL(self->profile_about_label), truncated ? truncated : "");
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

/* Return an owned copy of the newest valid event in a result array. */
static char *
copy_latest_event_json(const char *const *jsons, size_t count)
{
  const char *latest_json = NULL;
  gint64 latest_created_at = G_MININT64;

  for (size_t i = 0; i < count; i++) {
    const char *json = jsons[i];
    if (!json) continue;

    NostrEvent *event = nostr_event_new();
    if (event && nostr_event_deserialize(event, json) == 0) {
      gint64 created_at = (gint64)nostr_event_get_created_at(event);
      if (!latest_json || created_at > latest_created_at) {
        latest_json = json;
        latest_created_at = created_at;
      }
    }
    if (event) nostr_event_free(event);
  }

  return latest_json ? g_strdup(latest_json) : NULL;
}

/* Local database fetch for events */
static void fetch_event_from_local(GnostrNoteEmbed *self, const unsigned char id32[32]) {
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    fetch_event_from_relays(self, self->target_id);
    return;
  }

  char *json = NULL;
  int json_len = 0;
  if (storage_ndb_get_note_by_id(txn, id32, &json, &json_len, NULL) == 0 && json) {
    /* Parse and display */
    NostrEvent *evt = nostr_event_new();
    if (evt && nostr_event_deserialize(evt, json) == 0) {
      const char *content = nostr_event_get_content(evt);
      const char *author_hex = nostr_event_get_pubkey(evt);
      gint64 created_at = (gint64)nostr_event_get_created_at(evt);

      char *ts = format_timestamp(created_at);

      /* Try to get author profile */
      unsigned char author_pk[32];
      g_autofree char *author_display = NULL;
      char *author_handle = NULL;
      char *avatar_url = NULL;

      if (author_hex && hex_to_bytes_32(author_hex, author_pk)) {
        char *profile_event_json = NULL;
        int profile_event_len = 0;
        if (storage_ndb_get_profile_by_pubkey(txn, author_pk, &profile_event_json, &profile_event_len, NULL) == 0 && profile_event_json) {
          /* Parse kind-0 event to get profile content JSON */
          NostrEvent *profile_evt = nostr_event_new();
          if (profile_evt && nostr_event_deserialize(profile_evt, profile_event_json) == 0) {
            const char *profile_content = nostr_event_get_content(profile_evt);
            if (profile_content && *profile_content) {
              /* Parse the profile content JSON using nostr_json helpers */
              char *dn = NULL;
              if ((dn = gnostr_json_get_string(profile_content, "display_name", NULL)) != NULL && dn && *dn) {
                author_display = g_strdup(dn);
              }
              free(dn);
              if (!author_display) {
                char *nm = NULL;
                if ((nm = gnostr_json_get_string(profile_content, "name", NULL)) != NULL && nm && *nm) {
                  author_display = g_strdup(nm);
                }
                free(nm);
              }
              /* Get handle/name */
              char *name = NULL;
              if ((name = gnostr_json_get_string(profile_content, "name", NULL)) != NULL && name && *name) {
                author_handle = g_strdup(name);
              }
              free(name);
              /* Get picture URL */
              char *pic = NULL;
              if ((pic = gnostr_json_get_string(profile_content, "picture", NULL)) != NULL && pic && *pic) {
                avatar_url = g_strdup(pic);
              }
              free(pic);
            }
          }
          if (profile_evt) nostr_event_free(profile_evt);
          g_free(profile_event_json);
        }
      }

      if (!author_display && author_hex && strlen(author_hex) >= 8) {
        author_display = g_strdup_printf("%.8s...", author_hex);
      }

      gnostr_note_embed_set_content(self, author_display, author_handle,
                                     content, ts, avatar_url);

      g_free(ts);
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

/* Resolve an naddr from nostrdb using its complete NIP-33 address. */
static void
fetch_address_from_local(GnostrNoteEmbed *self)
{
  if (!self->target_id || self->address_kind < 0 ||
      !self->address_identifier) {
    gnostr_note_embed_set_error(self, "Invalid naddr");
    return;
  }

  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { self->address_kind };
  const char *authors[] = { self->target_id };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_tags_append(filter, "d", self->address_identifier, NULL);
  /* Bound corrupted/legacy duplicate rows while still selecting the newest. */
  nostr_filter_set_limit(filter, 64);

  char *filter_object = nostr_filter_serialize(filter);
  nostr_filter_free(filter);
  if (!filter_object) {
    fetch_event_from_relays(self, NULL);
    return;
  }
  g_autofree char *filters_json = g_strdup_printf("[%s]", filter_object);
  free(filter_object);

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    fetch_event_from_relays(self, NULL);
    return;
  }

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filters_json, &results, &count, NULL);
  storage_ndb_end_query(txn);

  g_autofree char *latest = NULL;
  if (rc == 0 && results && count > 0)
    latest = copy_latest_event_json((const char *const *)results, (size_t)count);
  if (results)
    storage_ndb_free_results(results, count);

  if (latest)
    apply_relay_event_json(self, latest);
  else
    fetch_event_from_relays(self, NULL);
}

typedef struct {
  GWeakRef embed_ref;
  gchar *request_key;
  GCancellable *cancellable;
  gulong cancelled_id;
  gint cancelled;
} EventRequestSubscriber;

typedef struct {
  gchar *request_key;
  gchar *event_id;
  gint address_kind;
  gchar *address_author;
  gchar *address_identifier;
  gchar **relay_hints;
  gsize relay_hints_count;
  GPtrArray *subscribers;
  GCancellable *cancellable;
  gboolean hints_attempted;
  gboolean main_pool_attempted;
} PendingEventRequest;

/* Canonical event/address key -> PendingEventRequest. Entries exist only
 * while a relay query is active, so identical embeds share one resolution. */
static GHashTable *pending_event_requests;

static void start_pending_event_query(PendingEventRequest *request,
                                      gboolean use_hints);

static void
on_event_subscriber_cancelled(GCancellable *cancellable, gpointer user_data)
{
  (void)cancellable;
  EventRequestSubscriber *subscriber = user_data;
  g_atomic_int_set(&subscriber->cancelled, TRUE);
}

static void
event_request_subscriber_free(gpointer data)
{
  EventRequestSubscriber *subscriber = data;
  if (!subscriber) return;
  if (subscriber->cancellable && subscriber->cancelled_id)
    g_cancellable_disconnect(subscriber->cancellable,
                             subscriber->cancelled_id);
  g_clear_object(&subscriber->cancellable);
  g_weak_ref_clear(&subscriber->embed_ref);
  g_free(subscriber->request_key);
  g_free(subscriber);
}

static void
pending_event_request_free(gpointer data)
{
  PendingEventRequest *request = data;
  if (!request) return;
  g_clear_object(&request->cancellable);
  g_strfreev(request->relay_hints);
  g_clear_pointer(&request->subscribers, g_ptr_array_unref);
  g_free(request->request_key);
  g_free(request->event_id);
  g_free(request->address_author);
  g_free(request->address_identifier);
  g_free(request);
}

static void
apply_relay_event_json(GnostrNoteEmbed *self, const char *json)
{
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize(event, json) != 0) {
    if (event) nostr_event_free(event);
    gnostr_note_embed_set_error(self, "Failed to parse");
    return;
  }

  const char *content = nostr_event_get_content(event);
  const char *author_hex = nostr_event_get_pubkey(event);
  gint64 created_at = (gint64)nostr_event_get_created_at(event);
  g_autofree char *timestamp = format_timestamp(created_at);
  g_autofree char *author_display = NULL;
  if (author_hex && strlen(author_hex) >= 8)
    author_display = g_strdup_printf("%.8s...", author_hex);

  gnostr_note_embed_set_content(self, author_display, NULL, content,
                                timestamp, NULL);
  nostr_event_free(event);
}

static void
complete_pending_event_request(PendingEventRequest *request,
                               const char *json,
                               const char *error_message)
{
  if (json) {
    GPtrArray *batch = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(batch, g_strdup(json));
    storage_ndb_ingest_events_async(batch);
  }

  for (guint i = 0; i < request->subscribers->len; i++) {
    EventRequestSubscriber *subscriber =
        g_ptr_array_index(request->subscribers, i);
    if (g_atomic_int_get(&subscriber->cancelled))
      continue;

    GnostrNoteEmbed *embed = g_weak_ref_get(&subscriber->embed_ref);
    if (!embed)
      continue;
    g_autofree char *current_key = NULL;
    if (embed->embed_type == EMBED_TYPE_ADDR &&
        embed->target_id && embed->address_identifier) {
      current_key = g_strdup_printf("a:%d:%s:%s", embed->address_kind,
                                    embed->target_id,
                                    embed->address_identifier);
    } else if (embed->target_id) {
      current_key = g_strdup_printf("e:%s", embed->target_id);
    }
    if (!embed->disposed && current_key &&
        g_strcmp0(current_key, subscriber->request_key) == 0) {
      if (json)
        apply_relay_event_json(embed, json);
      else
        gnostr_note_embed_set_error(
            embed, error_message ? error_message : "Not found");
    }
    g_object_unref(embed);
  }

  g_hash_table_remove(pending_event_requests, request->request_key);
}

static void
on_pending_event_query_done(GObject *source,
                            GAsyncResult *result,
                            gpointer user_data)
{
  PendingEventRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  GPtrArray *results =
      gnostr_pool_query_finish(GNOSTR_POOL(source), result, &error);

  if ((error || !results || results->len == 0) &&
      request->hints_attempted &&
      !request->main_pool_attempted) {
    if (results)
      g_ptr_array_unref(results);
    start_pending_event_query(request, FALSE);
    return;
  }

  const char *json = NULL;
  g_autofree char *latest = NULL;
  if (!error && results && results->len > 0) {
    if (request->address_identifier) {
      latest = copy_latest_event_json(
          (const char *const *)results->pdata, results->len);
      json = latest;
    } else {
      json = g_ptr_array_index(results, 0);
    }
  }

  if (json)
    complete_pending_event_request(request, json, NULL);
  else
    complete_pending_event_request(
        request, NULL,
        error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
            ? "Network error" : "Not found");

  if (results)
    g_ptr_array_unref(results);
}

/* Shared pool for embed queries - initialized lazily with pre-connected relays */
static GNostrPool *embed_pool = NULL;
static gboolean embed_pool_initialized = FALSE;
static gulong embed_relay_change_handler_id = 0;

/* Relay change callback for embed pool (nostrc-36y.4: live relay switching) */
static void on_embed_relay_config_changed(gpointer user_data) {
  (void)user_data;
  if (!embed_pool) return;

  GPtrArray *read_relays = gnostr_get_read_relay_urls();
  if (read_relays->len > 0) {
    const char **urls = g_new0(const char*, read_relays->len);
    for (guint i = 0; i < read_relays->len; i++) {
      urls[i] = g_ptr_array_index(read_relays, i);
    }
    g_message("[EMBED_POOL] Syncing embed pool with %u read relays", read_relays->len);
    gnostr_pool_sync_relays(embed_pool, (const gchar **)urls, read_relays->len);
    g_free(urls);
  }
  g_ptr_array_unref(read_relays);
}

/* Initialize the shared embed pool with pre-connected relays */
static void ensure_embed_pool_initialized(void) {
  if (embed_pool_initialized) return;
  embed_pool_initialized = TRUE;

  if (!embed_pool) {
    embed_pool = gnostr_pool_new();
  }

  /* Pre-load read-capable relays into pool for embed queries (NIP-65) */
  GPtrArray *urls = gnostr_get_read_relay_urls();

  if (urls->len > 0) {
    g_debug("embed_pool: Pre-connecting %u read relays for embed queries", urls->len);
    /* The pool's query_single will connect and add relays as needed,
     * but we can prime the process by logging the relay count */
  }

  g_ptr_array_unref(urls);

  /* Register for relay configuration changes (nostrc-36y.4: live relay switching) */
  if (embed_relay_change_handler_id == 0) {
    embed_relay_change_handler_id = gnostr_relay_change_connect(on_embed_relay_config_changed, NULL);
    g_debug("[EMBED_POOL] Registered relay change handler (id=%lu)", embed_relay_change_handler_id);
  }
}

static void
start_pending_event_query(PendingEventRequest *request,
                          gboolean use_hints)
{
  ensure_embed_pool_initialized();

  g_autoptr(GPtrArray) urls = NULL;
  if (use_hints && request->relay_hints_count > 0) {
    request->hints_attempted = TRUE;
    urls = g_ptr_array_new_with_free_func(g_free);
    for (gsize i = 0; i < request->relay_hints_count; i++)
      g_ptr_array_add(urls, g_strdup(request->relay_hints[i]));
    g_debug("note_embed: shared request trying %zu relay hints for %s",
            request->relay_hints_count, request->request_key);
  } else {
    request->main_pool_attempted = TRUE;
    urls = gnostr_get_read_relay_urls();
    g_debug("note_embed: shared request trying %u read relays for %s",
            urls->len, request->request_key);
  }

  NostrFilter *filter = nostr_filter_new();
  if (request->address_identifier) {
    const int kinds[] = { request->address_kind };
    const char *authors[] = { request->address_author };
    nostr_filter_set_kinds(filter, kinds, 1);
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_tags_append(filter, "d", request->address_identifier, NULL);
    nostr_filter_set_limit(filter, 64);
  } else {
    const char *ids[] = { request->event_id };
    nostr_filter_set_ids(filter, ids, 1);
  }

  NostrFilters *filters = nostr_filters_new();
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  gnostr_pool_query_urls_async(
      embed_pool, (const gchar **)urls->pdata, urls->len,
      filters, request->cancellable, on_pending_event_query_done, request);
}

static EventRequestSubscriber *
event_request_subscriber_new(GnostrNoteEmbed *self, const char *request_key)
{
  EventRequestSubscriber *subscriber =
      g_new0(EventRequestSubscriber, 1);
  g_weak_ref_init(&subscriber->embed_ref, self);
  subscriber->request_key = g_strdup(request_key);

  GCancellable *effective = get_effective_cancellable(self);
  if (effective) {
    subscriber->cancellable = g_object_ref(effective);
    subscriber->cancelled_id = g_cancellable_connect(
        effective, G_CALLBACK(on_event_subscriber_cancelled),
        subscriber, NULL);
    if (g_cancellable_is_cancelled(effective))
      g_atomic_int_set(&subscriber->cancelled, TRUE);
  }
  return subscriber;
}

/* Nostrdb is checked synchronously before this function. The shared table
 * coalesces only relay work and keeps each widget as an independently
 * cancellable weak subscriber. */
static void
fetch_event_from_relays(GnostrNoteEmbed *self, const char *id_hex)
{
  gboolean is_address =
      self->embed_type == EMBED_TYPE_ADDR &&
      self->target_id && self->address_identifier &&
      self->address_kind >= 0;

  if (!is_address && (!id_hex || !*id_hex)) {
    gnostr_note_embed_set_error(self, "No event ID");
    return;
  }

  g_autofree char *request_key =
      is_address
          ? g_strdup_printf("a:%d:%s:%s", self->address_kind,
                            self->target_id, self->address_identifier)
          : g_strdup_printf("e:%s", id_hex);

  if (!pending_event_requests) {
    pending_event_requests = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, pending_event_request_free);
  }

  PendingEventRequest *request =
      g_hash_table_lookup(pending_event_requests, request_key);
  if (request) {
    g_ptr_array_add(request->subscribers,
                    event_request_subscriber_new(self, request_key));
    return;
  }

  request = g_new0(PendingEventRequest, 1);
  request->request_key = g_strdup(request_key);
  if (is_address) {
    request->address_kind = self->address_kind;
    request->address_author = g_strdup(self->target_id);
    request->address_identifier = g_strdup(self->address_identifier);
  } else {
    request->event_id = g_strdup(id_hex);
  }
  request->relay_hints_count = MIN(self->relay_hints_count, (size_t)MAX_EMBED_HINT_RELAYS);
  if (request->relay_hints_count > 0) {
    request->relay_hints = g_new0(char *, request->relay_hints_count + 1);
    for (gsize i = 0; i < request->relay_hints_count; i++)
      request->relay_hints[i] = g_strdup(self->relay_hints[i]);
  }
  request->subscribers = g_ptr_array_new_with_free_func(
      event_request_subscriber_free);
  request->cancellable = g_cancellable_new();
  g_ptr_array_add(request->subscribers,
                  event_request_subscriber_new(self, request_key));
  g_hash_table_insert(pending_event_requests,
                      g_strdup(request->request_key), request);

  start_pending_event_query(request, request->relay_hints_count > 0);
}

/* nostrc-pb01: Profile loading now goes through the centralized profile
 * service (nostr-gobject/src/nostr_profile_service.c) which:
 *   1. Checks the in-memory LRU cache (GnostrProfileProvider)
 *   2. Falls back to NostrDB (local persisted profiles)
 *   3. Batches 150ms-debounced network fetches across all widgets
 *   4. Deduplicates concurrent requests for the same pubkey
 *
 * The callback is invoked on the main thread. Widget disposal cancels
 * any pending callbacks via gnostr_profile_service_cancel_for_user_data.
 */

/* Callback invoked by the profile service when a profile request completes. */
static void on_profile_service_result(const char *pubkey_hex,
                                       const GnostrProfileMeta *meta,
                                       gpointer user_data) {
  GnostrNoteEmbed *self = (GnostrNoteEmbed *)user_data;

  /* Widget may have been destroyed; cancel_for_user_data removes the
   * callback during dispose so we shouldn't get here, but defend anyway. */
  if (!self || !GNOSTR_IS_NOTE_EMBED(self) || self->disposed) return;

  /* Only accept results for the pubkey this widget is currently bound to. */
  if (!self->target_id || !pubkey_hex ||
      g_ascii_strcasecmp(self->target_id, pubkey_hex) != 0) {
    return;
  }

  if (!meta) {
    /* Profile not found - show basic profile with just pubkey */
    gnostr_note_embed_set_profile(self, NULL, NULL, NULL, NULL, self->target_id);
    return;
  }

  /* Prefer display_name, fall back to name for the display string. */
  const char *display_name = NULL;
  if (meta->display_name && *meta->display_name) {
    display_name = meta->display_name;
  } else if (meta->name && *meta->name) {
    display_name = meta->name;
  }

  gnostr_note_embed_set_profile(self,
                                 display_name,
                                 meta->name,
                                 meta->about,
                                 meta->picture,
                                 self->target_id);
}

/* Request a profile for the given pubkey via the centralized profile service.
 * Cancels any prior in-flight request for this widget to avoid stale results.
 * NIP-19 hint relays augment the service's configured relay batch. */
static void request_profile_via_service(GnostrNoteEmbed *self, const char *pubkey_hex) {
  if (!pubkey_hex || !*pubkey_hex) {
    gnostr_note_embed_set_profile(self, NULL, NULL, NULL, NULL, self->target_id);
    return;
  }

  /* Fast-path: synchronous check of the provider cache.  This restores the
   * old behaviour where a cache hit rendered immediately without waiting for
   * the 150 ms service debounce, and avoids dropping results on widgets that
   * were marked disposed by a recent prepare_for_unbind + rebind. */
  GnostrProfileMeta *cached = gnostr_profile_provider_get(pubkey_hex);
  if (cached) {
    on_profile_service_result(pubkey_hex, cached, self);
    gnostr_profile_meta_free(cached);
    return;
  }

  /* get_default() is documented as always returning a valid singleton. */
  gpointer svc = gnostr_profile_service_get_default();

  /* Drop any pending callbacks for this widget before queuing a new request,
   * in case set_pubkey/set_nostr_uri is called repeatedly on the same widget. */
  gnostr_profile_service_cancel_for_user_data(svc, self);

  gnostr_profile_service_request_with_hints(
      svc, pubkey_hex,
      (const char *const *)self->relay_hints, self->relay_hints_count,
      on_profile_service_result, self);
}

/**
 * gnostr_note_embed_set_cancellable:
 *
 * Sets an external cancellable for all async operations.
 */
void gnostr_note_embed_set_cancellable(GnostrNoteEmbed *self, GCancellable *cancellable) {
  g_return_if_fail(GNOSTR_IS_NOTE_EMBED(self));
  self->external_cancellable = cancellable;
}

/**
 * gnostr_note_embed_prepare_for_unbind:
 *
 * Prepares the widget for unbinding from a list item. This cancels all async
 * operations and marks the widget as disposed to prevent callbacks from
 * accessing widget state during the unbind/dispose process.
 */
void gnostr_note_embed_prepare_for_unbind(GnostrNoteEmbed *self) {
  /* NOTE: Do NOT use type-checking macros (GNOSTR_IS_NOTE_EMBED) here - they
   * dereference the pointer which crashes if it contains garbage. The pointer
   * may be stale if the widget was destroyed or the row is being recycled.
   * Just check for NULL and trust the caller (note_card_row) checked validity.
   * See nostrc-ofq crash fix for this pattern. */
  g_return_if_fail(self != NULL);

  /* Clear ALL labels BEFORE setting disposed — PangoLayouts must be reset
   * while the widget still has a native surface.  Without this, a layout pass
   * between unbind and dispose can reach a child GtkLabel whose PangoLayout is
   * NULL, causing SEGV in pango_layout_set_width. */
  if (GNOSTR_LABEL_SAFE(self->content_label))
    gtk_label_set_text(GTK_LABEL(self->content_label), "");
  if (GNOSTR_LABEL_SAFE(self->author_label))
    gtk_label_set_text(GTK_LABEL(self->author_label), "");
  if (GNOSTR_LABEL_SAFE(self->handle_label))
    gtk_label_set_text(GTK_LABEL(self->handle_label), "");
  if (GNOSTR_LABEL_SAFE(self->timestamp_label))
    gtk_label_set_text(GTK_LABEL(self->timestamp_label), "");
  if (GNOSTR_LABEL_SAFE(self->error_label))
    gtk_label_set_text(GTK_LABEL(self->error_label), "");
  if (GNOSTR_LABEL_SAFE(self->profile_about_label))
    gtk_label_set_text(GTK_LABEL(self->profile_about_label), "");

  /* Mark as disposed to prevent any async callbacks from running.
   * This is the same pattern as note_card_row_prepare_for_unbind.
   * If the widget is later rebound via set_pubkey/set_nostr_uri/set_event_id,
   * those setters clear this flag so new async callbacks can run. */
  self->disposed = TRUE;

  /* nostrc-pb01: Cancel any pending profile service callbacks for this widget */
  gnostr_profile_service_cancel_for_user_data(
      gnostr_profile_service_get_default(), self);

  /* Cancel internal cancellable - external one will be cancelled by parent */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
  }

  /* Clear external cancellable reference - it's owned by the parent */
  self->external_cancellable = NULL;
}
