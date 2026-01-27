#include "gnostr-profile-pane.h"
#include "gnostr-profile-edit.h"
#include "gnostr-status-dialog.h"
#include "gnostr-image-viewer.h"
#include "note_card_row.h"
#include "gnostr-highlight-card.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include "gnostr-avatar-cache.h"
#include "../util/nip05.h"
#include "../util/relays.h"
#include "../util/nip58_badges.h"
#include "../util/user_status.h"
#include "../util/nip39_identity.h"
#include "../util/nip84_highlights.h"
#include "../storage_ndb.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr_simple_pool.h"
#include <jansson.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-profile-pane.ui"
#define DEFAULT_BANNER_RESOURCE "/org/gnostr/assets/assets/background.png"

/* Check if user is logged in by checking GSettings current-npub.
 * Returns TRUE if logged in, FALSE otherwise. */
static gboolean is_user_logged_in(void) {
  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (!settings) return FALSE;
  char *npub = g_settings_get_string(settings, "current-npub");
  g_object_unref(settings);
  gboolean logged_in = (npub && *npub);
  g_free(npub);
  return logged_in;
}

/* Default banner texture loaded from GResource */
static GdkTexture *default_banner_texture = NULL;

/* Lazy-load the default banner texture from GResource */
static GdkTexture *get_default_banner_texture(void) {
  if (default_banner_texture == NULL) {
    GError *error = NULL;
    default_banner_texture = gdk_texture_new_from_resource(DEFAULT_BANNER_RESOURCE);
    if (!default_banner_texture) {
      g_warning("Failed to load default banner from resource: %s",
                error ? error->message : "unknown error");
      g_clear_error(&error);
    }
  }
  return default_banner_texture;
}

/* Maximum posts to fetch per page */
#define POSTS_PAGE_SIZE 20

/* Maximum cached images per profile pane to prevent unbounded memory growth */
#define IMAGE_CACHE_MAX 50

/* Post item for the list model */
typedef struct _ProfilePostItem {
  GObject parent_instance;
  char *id_hex;
  char *pubkey_hex;
  char *content;
  gint64 created_at;
  char *display_name;
  char *handle;
  char *avatar_url;
} ProfilePostItem;

typedef struct _ProfilePostItemClass {
  GObjectClass parent_class;
} ProfilePostItemClass;

G_DEFINE_TYPE(ProfilePostItem, profile_post_item, G_TYPE_OBJECT)

enum {
  POST_PROP_0,
  POST_PROP_ID_HEX,
  POST_PROP_PUBKEY_HEX,
  POST_PROP_CONTENT,
  POST_PROP_CREATED_AT,
  POST_PROP_DISPLAY_NAME,
  POST_PROP_HANDLE,
  POST_PROP_AVATAR_URL,
  POST_N_PROPS
};

static GParamSpec *post_props[POST_N_PROPS];

static void profile_post_item_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
  ProfilePostItem *self = (ProfilePostItem*)obj;
  switch (prop_id) {
    case POST_PROP_ID_HEX:       g_free(self->id_hex);       self->id_hex       = g_value_dup_string(value); break;
    case POST_PROP_PUBKEY_HEX:   g_free(self->pubkey_hex);   self->pubkey_hex   = g_value_dup_string(value); break;
    case POST_PROP_CONTENT:      g_free(self->content);      self->content      = g_value_dup_string(value); break;
    case POST_PROP_CREATED_AT:   self->created_at            = g_value_get_int64(value); break;
    case POST_PROP_DISPLAY_NAME: g_free(self->display_name); self->display_name = g_value_dup_string(value); break;
    case POST_PROP_HANDLE:       g_free(self->handle);       self->handle       = g_value_dup_string(value); break;
    case POST_PROP_AVATAR_URL:   g_free(self->avatar_url);   self->avatar_url   = g_value_dup_string(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void profile_post_item_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
  ProfilePostItem *self = (ProfilePostItem*)obj;
  switch (prop_id) {
    case POST_PROP_ID_HEX:       g_value_set_string(value, self->id_hex); break;
    case POST_PROP_PUBKEY_HEX:   g_value_set_string(value, self->pubkey_hex); break;
    case POST_PROP_CONTENT:      g_value_set_string(value, self->content); break;
    case POST_PROP_CREATED_AT:   g_value_set_int64(value, self->created_at); break;
    case POST_PROP_DISPLAY_NAME: g_value_set_string(value, self->display_name); break;
    case POST_PROP_HANDLE:       g_value_set_string(value, self->handle); break;
    case POST_PROP_AVATAR_URL:   g_value_set_string(value, self->avatar_url); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void profile_post_item_dispose(GObject *obj) {
  ProfilePostItem *self = (ProfilePostItem*)obj;
  g_clear_pointer(&self->id_hex, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->content, g_free);
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->handle, g_free);
  g_clear_pointer(&self->avatar_url, g_free);
  G_OBJECT_CLASS(profile_post_item_parent_class)->dispose(obj);
}

static void profile_post_item_class_init(ProfilePostItemClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->set_property = profile_post_item_set_property;
  oc->get_property = profile_post_item_get_property;
  oc->dispose = profile_post_item_dispose;

  post_props[POST_PROP_ID_HEX]       = g_param_spec_string("id-hex",       "id-hex",       "Event ID",      NULL, G_PARAM_READWRITE);
  post_props[POST_PROP_PUBKEY_HEX]   = g_param_spec_string("pubkey-hex",   "pubkey-hex",   "Pubkey",        NULL, G_PARAM_READWRITE);
  post_props[POST_PROP_CONTENT]      = g_param_spec_string("content",      "content",      "Content",       NULL, G_PARAM_READWRITE);
  post_props[POST_PROP_CREATED_AT]   = g_param_spec_int64 ("created-at",   "created-at",   "Created At",    0, G_MAXINT64, 0, G_PARAM_READWRITE);
  post_props[POST_PROP_DISPLAY_NAME] = g_param_spec_string("display-name", "display-name", "Display Name",  NULL, G_PARAM_READWRITE);
  post_props[POST_PROP_HANDLE]       = g_param_spec_string("handle",       "handle",       "Handle",        NULL, G_PARAM_READWRITE);
  post_props[POST_PROP_AVATAR_URL]   = g_param_spec_string("avatar-url",   "avatar-url",   "Avatar URL",    NULL, G_PARAM_READWRITE);
  g_object_class_install_properties(oc, POST_N_PROPS, post_props);
}

static void profile_post_item_init(ProfilePostItem *self) { (void)self; }

static ProfilePostItem *profile_post_item_new(const char *id_hex, const char *pubkey_hex, const char *content, gint64 created_at) {
  return g_object_new(profile_post_item_get_type(),
                      "id-hex", id_hex,
                      "pubkey-hex", pubkey_hex,
                      "content", content,
                      "created-at", created_at,
                      NULL);
}

/* Media item for the grid model */
typedef struct _ProfileMediaItem {
  GObject parent_instance;
  char *url;           /* Media URL (image/video) */
  char *thumb_url;     /* Thumbnail URL (may be same as url) */
  char *event_id_hex;  /* Source event ID */
  char *mime_type;     /* MIME type if known */
  gint64 created_at;
} ProfileMediaItem;

typedef struct _ProfileMediaItemClass {
  GObjectClass parent_class;
} ProfileMediaItemClass;

G_DEFINE_TYPE(ProfileMediaItem, profile_media_item, G_TYPE_OBJECT)

enum {
  MEDIA_PROP_0,
  MEDIA_PROP_URL,
  MEDIA_PROP_THUMB_URL,
  MEDIA_PROP_EVENT_ID_HEX,
  MEDIA_PROP_MIME_TYPE,
  MEDIA_PROP_CREATED_AT,
  MEDIA_N_PROPS
};

static GParamSpec *media_props[MEDIA_N_PROPS];

static void profile_media_item_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
  ProfileMediaItem *self = (ProfileMediaItem*)obj;
  switch (prop_id) {
    case MEDIA_PROP_URL:         g_free(self->url);         self->url         = g_value_dup_string(value); break;
    case MEDIA_PROP_THUMB_URL:   g_free(self->thumb_url);   self->thumb_url   = g_value_dup_string(value); break;
    case MEDIA_PROP_EVENT_ID_HEX: g_free(self->event_id_hex); self->event_id_hex = g_value_dup_string(value); break;
    case MEDIA_PROP_MIME_TYPE:   g_free(self->mime_type);   self->mime_type   = g_value_dup_string(value); break;
    case MEDIA_PROP_CREATED_AT:  self->created_at           = g_value_get_int64(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void profile_media_item_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
  ProfileMediaItem *self = (ProfileMediaItem*)obj;
  switch (prop_id) {
    case MEDIA_PROP_URL:          g_value_set_string(value, self->url); break;
    case MEDIA_PROP_THUMB_URL:    g_value_set_string(value, self->thumb_url); break;
    case MEDIA_PROP_EVENT_ID_HEX: g_value_set_string(value, self->event_id_hex); break;
    case MEDIA_PROP_MIME_TYPE:    g_value_set_string(value, self->mime_type); break;
    case MEDIA_PROP_CREATED_AT:   g_value_set_int64(value, self->created_at); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void profile_media_item_dispose(GObject *obj) {
  ProfileMediaItem *self = (ProfileMediaItem*)obj;
  g_clear_pointer(&self->url, g_free);
  g_clear_pointer(&self->thumb_url, g_free);
  g_clear_pointer(&self->event_id_hex, g_free);
  g_clear_pointer(&self->mime_type, g_free);
  G_OBJECT_CLASS(profile_media_item_parent_class)->dispose(obj);
}

static void profile_media_item_class_init(ProfileMediaItemClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->set_property = profile_media_item_set_property;
  oc->get_property = profile_media_item_get_property;
  oc->dispose = profile_media_item_dispose;

  media_props[MEDIA_PROP_URL]          = g_param_spec_string("url",          "url",          "Media URL",     NULL, G_PARAM_READWRITE);
  media_props[MEDIA_PROP_THUMB_URL]    = g_param_spec_string("thumb-url",    "thumb-url",    "Thumbnail URL", NULL, G_PARAM_READWRITE);
  media_props[MEDIA_PROP_EVENT_ID_HEX] = g_param_spec_string("event-id-hex", "event-id-hex", "Event ID",      NULL, G_PARAM_READWRITE);
  media_props[MEDIA_PROP_MIME_TYPE]    = g_param_spec_string("mime-type",    "mime-type",    "MIME Type",     NULL, G_PARAM_READWRITE);
  media_props[MEDIA_PROP_CREATED_AT]   = g_param_spec_int64 ("created-at",   "created-at",   "Created At",    0, G_MAXINT64, 0, G_PARAM_READWRITE);
  g_object_class_install_properties(oc, MEDIA_N_PROPS, media_props);
}

static void profile_media_item_init(ProfileMediaItem *self) { (void)self; }

static ProfileMediaItem *profile_media_item_new(const char *url, const char *thumb_url, const char *event_id_hex, const char *mime_type, gint64 created_at) {
  return g_object_new(profile_media_item_get_type(),
                      "url", url,
                      "thumb-url", thumb_url ? thumb_url : url,
                      "event-id-hex", event_id_hex,
                      "mime-type", mime_type,
                      "created-at", created_at,
                      NULL);
}

/* Maximum media items to fetch per page */
#define MEDIA_PAGE_SIZE 30

struct _GnostrProfilePane {
  GtkWidget parent_instance;

  /* Template children */
  GtkWidget *root;
  GtkWidget *btn_close;
  GtkWidget *btn_avatar;
  GtkWidget *banner_image;
  GtkWidget *avatar_box;
  GtkWidget *avatar_image;
  GtkWidget *avatar_initials;
  GtkWidget *lbl_display_name;
  GtkWidget *lbl_handle;
  GtkWidget *lbl_bio;
  GtkWidget *metadata_box;
  GtkWidget *stats_box;
  GtkWidget *lbl_notes_count;
  GtkWidget *lbl_followers_count;
  GtkWidget *lbl_following_count;
  GtkWidget *btn_follow;
  GtkWidget *btn_message;
  GtkWidget *other_profile_actions;
  GtkWidget *own_profile_actions;
  GtkWidget *btn_edit_profile;
  GtkWidget *btn_set_status;

  /* Tab widgets */
  GtkWidget *tab_switcher;
  GtkWidget *content_stack;
  GtkWidget *about_scroll;
  GtkWidget *about_content;

  /* Posts tab widgets */
  GtkWidget *posts_container;
  GtkWidget *posts_scroll;
  GtkWidget *posts_list;
  GtkWidget *posts_loading_box;
  GtkWidget *posts_spinner;
  GtkWidget *posts_empty_box;
  GtkWidget *posts_empty_label;
  GtkWidget *btn_load_more;

  /* Posts model */
  GListStore *posts_model;
  GtkSelectionModel *posts_selection;

  /* Media tab widgets */
  GtkWidget *media_container;
  GtkWidget *media_scroll;
  GtkWidget *media_grid;
  GtkWidget *media_loading_box;
  GtkWidget *media_spinner;
  GtkWidget *media_empty_box;
  GtkWidget *media_empty_label;
  GtkWidget *btn_media_load_more;

  /* Media model */
  GListStore *media_model;
  GtkSelectionModel *media_selection;
  GCancellable *media_cancellable;
  gboolean media_loaded;
  gint64 media_oldest_timestamp;

  /* NIP-84 Highlights tab widgets */
  GtkWidget *highlights_container;
  GtkWidget *highlights_scroll;
  GtkWidget *highlights_list;
  GtkWidget *highlights_loading_box;
  GtkWidget *highlights_spinner;
  GtkWidget *highlights_empty_box;
  GtkWidget *highlights_empty_label;
  GCancellable *highlights_cancellable;
  gboolean highlights_loaded;

  /* State */
  char *current_pubkey;
  char *own_pubkey;              /* Current user's pubkey */
  char *current_profile_json;    /* Raw profile JSON for edit dialog */
  char *current_nip05;           /* NIP-05 identifier from profile */
  char *current_display_name;    /* Display name for posts */
  char *current_handle;          /* Handle for posts */
  char *current_avatar_url;      /* Avatar URL for posts */
  GtkWidget *nip05_badge;        /* Verification badge widget */
  GtkWidget *nip05_row;          /* NIP-05 metadata row */
  GtkWidget *bot_badge;          /* NIP-24 Bot indicator badge */
  GCancellable *nip05_cancellable;
  GCancellable *posts_cancellable;
  GCancellable *nip65_cancellable;
  gboolean posts_loaded;
  gint64 posts_oldest_timestamp;  /* For pagination */
  GPtrArray *nip65_relays;        /* Cached NIP-65 relay list (GnostrNip65Relay*) */
  gboolean nip65_fetched;         /* Whether NIP-65 lookup was attempted */
#ifdef HAVE_SOUP3
  SoupSession *soup_session;
  GCancellable *banner_cancellable;
  GCancellable *avatar_cancellable;
  GHashTable *image_cache; /* URL -> GdkTexture */
  GQueue *image_cache_lru; /* LRU queue of URL keys (head=oldest) */
#endif

  /* SimplePool for fetching posts */
  GnostrSimplePool *simple_pool;

  /* Profile fetch state */
  GCancellable *profile_cancellable;  /* For network profile fetch */
  gboolean profile_loaded_from_cache; /* Whether we showed cached data */

  /* NIP-58 Badge state */
  GtkWidget *badges_box;              /* Container for badge icons */
  GPtrArray *profile_badges;          /* GnostrProfileBadge* array */
  GCancellable *badges_cancellable;   /* For badge fetch */
  gboolean badges_loaded;             /* Whether badges have been fetched */

  /* NIP-38 User Status state */
  GtkWidget *status_box;              /* Container for status display */
  GtkWidget *status_general_row;      /* General status row widget */
  GtkWidget *status_music_row;        /* Music status row widget */
  GnostrUserStatus *current_general_status;  /* Cached general status */
  GnostrUserStatus *current_music_status;    /* Cached music status */
  GCancellable *status_cancellable;   /* For status fetch */
  gboolean status_loaded;             /* Whether status has been fetched */

  /* NIP-39 External Identity state */
  char *current_event_json;           /* Full event JSON (for accessing tags) */
  GPtrArray *external_identities;     /* GnostrExternalIdentity* array */
  GtkWidget *identities_box;          /* Container for identity display */
};

G_DEFINE_TYPE(GnostrProfilePane, gnostr_profile_pane, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CLOSE_REQUESTED,
  SIGNAL_NOTE_ACTIVATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
#ifdef HAVE_SOUP3
static void load_image_async(GnostrProfilePane *self, const char *url, GtkPicture *picture, GCancellable **cancellable_slot);
static void load_banner_async(GnostrProfilePane *self, const char *url);
#endif
static void update_profile_ui(GnostrProfilePane *self, json_t *profile_json);
static void load_posts(GnostrProfilePane *self);
static void load_posts_with_relays(GnostrProfilePane *self, GPtrArray *relay_urls);
static void load_media(GnostrProfilePane *self);
static void on_stack_visible_child_changed(GtkStack *stack, GParamSpec *pspec, gpointer user_data);
static void fetch_profile_from_cache_or_network(GnostrProfilePane *self);
static void load_badges(GnostrProfilePane *self);
static void on_badges_fetched(GPtrArray *badges, gpointer user_data);
static void fetch_user_status(GnostrProfilePane *self);

static void gnostr_profile_pane_dispose(GObject *obj) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(obj);
  /* Cancel profile loading */
  if (self->profile_cancellable) {
    g_cancellable_cancel(self->profile_cancellable);
    g_clear_object(&self->profile_cancellable);
  }
  /* Cancel NIP-58 badge loading */
  if (self->badges_cancellable) {
    g_cancellable_cancel(self->badges_cancellable);
    g_clear_object(&self->badges_cancellable);
  }
  /* Clear badge data */
  g_clear_pointer(&self->profile_badges, g_ptr_array_unref);
  /* Cancel NIP-05 verification if in progress */
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }
  /* Cancel posts loading */
  if (self->posts_cancellable) {
    g_cancellable_cancel(self->posts_cancellable);
    g_clear_object(&self->posts_cancellable);
  }
  /* Cancel NIP-65 relay lookup */
  if (self->nip65_cancellable) {
    g_cancellable_cancel(self->nip65_cancellable);
    g_clear_object(&self->nip65_cancellable);
  }
  /* Clear NIP-65 relay cache */
  g_clear_pointer(&self->nip65_relays, g_ptr_array_unref);
#ifdef HAVE_SOUP3
  if (self->banner_cancellable) {
    g_cancellable_cancel(self->banner_cancellable);
    g_clear_object(&self->banner_cancellable);
  }
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
  }
  g_clear_object(&self->soup_session);
  g_clear_pointer(&self->image_cache, g_hash_table_unref);
  if (self->image_cache_lru) {
    g_queue_free_full(self->image_cache_lru, g_free);
    self->image_cache_lru = NULL;
  }
#endif
  /* Clear posts model */
  if (self->posts_list && GTK_IS_LIST_VIEW(self->posts_list)) {
    gtk_list_view_set_model(GTK_LIST_VIEW(self->posts_list), NULL);
  }
  g_clear_object(&self->posts_selection);
  g_clear_object(&self->posts_model);

  /* Cancel media loading */
  if (self->media_cancellable) {
    g_cancellable_cancel(self->media_cancellable);
    g_clear_object(&self->media_cancellable);
  }
  /* Clear media model */
  if (self->media_grid && GTK_IS_GRID_VIEW(self->media_grid)) {
    gtk_grid_view_set_model(GTK_GRID_VIEW(self->media_grid), NULL);
  }
  g_clear_object(&self->media_selection);
  g_clear_object(&self->media_model);

  g_clear_object(&self->simple_pool);

  /* Cancel NIP-38 user status loading */
  if (self->status_cancellable) {
    g_cancellable_cancel(self->status_cancellable);
    g_clear_object(&self->status_cancellable);
  }
  /* Clear user status data */
  g_clear_pointer(&self->current_general_status, gnostr_user_status_free);
  g_clear_pointer(&self->current_music_status, gnostr_user_status_free);

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_PROFILE_PANE);
  G_OBJECT_CLASS(gnostr_profile_pane_parent_class)->dispose(obj);
}

static void gnostr_profile_pane_finalize(GObject *obj) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(obj);
  g_clear_pointer(&self->current_pubkey, g_free);
  g_clear_pointer(&self->own_pubkey, g_free);
  g_clear_pointer(&self->current_profile_json, g_free);
  g_clear_pointer(&self->current_event_json, g_free);
  g_clear_pointer(&self->current_nip05, g_free);
  g_clear_pointer(&self->current_display_name, g_free);
  g_clear_pointer(&self->current_handle, g_free);
  g_clear_pointer(&self->current_avatar_url, g_free);
  g_clear_pointer(&self->external_identities, g_ptr_array_unref);
  G_OBJECT_CLASS(gnostr_profile_pane_parent_class)->finalize(obj);
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)btn;
  g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
}

static void on_avatar_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)btn;

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;
  if (!self->current_avatar_url || !*self->current_avatar_url) return;

  /* Open avatar image in image viewer */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
  GnostrImageViewer *viewer = gnostr_image_viewer_new(parent);
  gnostr_image_viewer_set_image_url(viewer, self->current_avatar_url);
  gnostr_image_viewer_present(viewer);
}

/* Callback when profile is saved from edit dialog */
static void on_profile_saved(GnostrProfileEdit *edit_dialog, const char *profile_json, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)edit_dialog;

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  /* Update the profile pane with new data */
  if (profile_json && *profile_json) {
    gnostr_profile_pane_update_from_json(self, profile_json);
  }
}

static void on_edit_profile_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)btn;

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  /* Find the parent window */
  GtkWidget *parent_widget = GTK_WIDGET(self);
  GtkWindow *parent_window = NULL;
  while (parent_widget) {
    if (GTK_IS_WINDOW(parent_widget)) {
      parent_window = GTK_WINDOW(parent_widget);
      break;
    }
    parent_widget = gtk_widget_get_parent(parent_widget);
  }

  /* Create edit dialog */
  GnostrProfileEdit *edit_dialog = gnostr_profile_edit_new(parent_window);

  /* Populate with current profile data */
  if (self->current_profile_json && *self->current_profile_json) {
    gnostr_profile_edit_set_profile_json(edit_dialog, self->current_profile_json);
  }

  /* Populate external identities from full event JSON (NIP-39) */
  if (self->current_event_json && *self->current_event_json) {
    gnostr_profile_edit_set_event_json(edit_dialog, self->current_event_json);
  }

  /* Connect to profile-saved signal */
  g_signal_connect(edit_dialog, "profile-saved", G_CALLBACK(on_profile_saved), self);

  /* Show the dialog */
  gtk_window_present(GTK_WINDOW(edit_dialog));
}

/* Callback when status dialog updates status */
static void on_status_dialog_status_updated(GnostrStatusDialog *dialog, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)dialog;

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  /* Refresh user status display */
  fetch_user_status(self);
}

static void on_set_status_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)btn;

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  /* Create status dialog */
  GnostrStatusDialog *status_dialog = gnostr_status_dialog_new();

  /* Pre-fill with current status if available */
  const gchar *general = self->current_general_status ? self->current_general_status->content : NULL;
  const gchar *music = self->current_music_status ? self->current_music_status->content : NULL;
  gnostr_status_dialog_set_current_status(status_dialog, general, music);

  /* Connect to status-updated signal */
  g_signal_connect(status_dialog, "status-updated", G_CALLBACK(on_status_dialog_status_updated), self);

  /* Present the dialog */
  gnostr_status_dialog_present(status_dialog, GTK_WIDGET(self));
}

static void on_load_more_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)btn;
  load_posts(self);
}

/* Post row factory setup */
static void posts_factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *row = GTK_WIDGET(gnostr_note_card_row_new());
  gtk_list_item_set_child(item, row);
}

/* Post row factory bind */
static void posts_factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GObject *obj = gtk_list_item_get_item(item);
  if (!obj) return;

  ProfilePostItem *post = (ProfilePostItem*)obj;
  GtkWidget *row = gtk_list_item_get_child(item);
  if (!GNOSTR_IS_NOTE_CARD_ROW(row)) return;

  /* Set author info from stored profile data */
  gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row),
                                   post->display_name,
                                   post->handle,
                                   post->avatar_url);

  /* Set timestamp */
  gnostr_note_card_row_set_timestamp(GNOSTR_NOTE_CARD_ROW(row), post->created_at, NULL);

  /* Set content */
  gnostr_note_card_row_set_content(GNOSTR_NOTE_CARD_ROW(row), post->content);

  /* Set IDs */
  gnostr_note_card_row_set_ids(GNOSTR_NOTE_CARD_ROW(row), post->id_hex, NULL, post->pubkey_hex);

  /* Set depth to 0 (top-level posts) */
  gnostr_note_card_row_set_depth(GNOSTR_NOTE_CARD_ROW(row), 0);

  /* Set login state for authentication-required buttons */
  gnostr_note_card_row_set_logged_in(GNOSTR_NOTE_CARD_ROW(row), is_user_logged_in());

  gtk_widget_set_visible(row, TRUE);
}

/* Post row factory unbind */
static void posts_factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)item; (void)data;
  /* No special cleanup needed */
}

/* Handle post activation (click) */
static void on_posts_list_activate(GtkListView *list_view, guint position, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)list_view;

  if (!self->posts_model) return;

  ProfilePostItem *post = g_list_model_get_item(G_LIST_MODEL(self->posts_model), position);
  if (post && post->id_hex) {
    g_signal_emit(self, signals[SIGNAL_NOTE_ACTIVATED], 0, post->id_hex);
    g_object_unref(post);
  }
}

static void setup_posts_list(GnostrProfilePane *self) {
  if (!self->posts_list || !GTK_IS_LIST_VIEW(self->posts_list)) return;

  /* Create model if not exists */
  if (!self->posts_model) {
    self->posts_model = g_list_store_new(profile_post_item_get_type());
  }

  /* Create selection model */
  if (!self->posts_selection) {
    self->posts_selection = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->posts_model)));
  }

  /* Create factory */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(posts_factory_setup_cb), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(posts_factory_bind_cb), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(posts_factory_unbind_cb), NULL);

  /* Set up list view */
  gtk_list_view_set_model(GTK_LIST_VIEW(self->posts_list), self->posts_selection);
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->posts_list), factory);
  g_object_unref(factory);

  /* Connect activation signal */
  g_signal_connect(self->posts_list, "activate", G_CALLBACK(on_posts_list_activate), self);
}

static void gnostr_profile_pane_class_init(GnostrProfilePaneClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  
  gclass->dispose = gnostr_profile_pane_dispose;
  gclass->finalize = gnostr_profile_pane_finalize;
  
  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);
  
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, root);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_close);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_avatar);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, banner_image);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, avatar_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, avatar_image);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, avatar_initials);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, lbl_display_name);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, bot_badge);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, lbl_handle);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, lbl_bio);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, metadata_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, stats_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, lbl_notes_count);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, lbl_followers_count);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, lbl_following_count);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_follow);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_message);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, other_profile_actions);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, own_profile_actions);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_edit_profile);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_set_status);

  /* Tab widgets */
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, tab_switcher);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, content_stack);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, about_scroll);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, about_content);

  /* Posts tab widgets */
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, posts_container);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, posts_scroll);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, posts_list);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, posts_loading_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, posts_spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, posts_empty_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, posts_empty_label);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_load_more);

  /* Media tab widgets */
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, media_container);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, media_scroll);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, media_grid);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, media_loading_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, media_spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, media_empty_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, media_empty_label);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, btn_media_load_more);
  /* NIP-84 Highlights tab */
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, highlights_container);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, highlights_scroll);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, highlights_list);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, highlights_loading_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, highlights_spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, highlights_empty_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, highlights_empty_label);

  gtk_widget_class_bind_template_callback(wclass, on_close_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_edit_profile_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_set_status_clicked);

  signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new(
    "close-requested",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_NOTE_ACTIVATED] = g_signal_new(
    "note-activated",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_profile_pane_init(GnostrProfilePane *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_close),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Close Profile", -1);
  g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);

  /* Connect avatar button to open image viewer */
  if (self->btn_avatar) {
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);
  }

  /* Connect edit profile button */
  if (self->btn_edit_profile) {
    g_signal_connect(self->btn_edit_profile, "clicked", G_CALLBACK(on_edit_profile_clicked), self);
  }

  /* Connect set status button (NIP-38) */
  if (self->btn_set_status) {
    g_signal_connect(self->btn_set_status, "clicked", G_CALLBACK(on_set_status_clicked), self);
  }

  /* Connect load more button */
  if (self->btn_load_more) {
    g_signal_connect(self->btn_load_more, "clicked", G_CALLBACK(on_load_more_clicked), self);
  }

  /* Connect stack visible child changed to lazy load posts */
  if (self->content_stack) {
    g_signal_connect(self->content_stack, "notify::visible-child-name",
                     G_CALLBACK(on_stack_visible_child_changed), self);
  }

  /* Setup posts list */
  setup_posts_list(self);

#ifdef HAVE_SOUP3
  /* Create session with connection limits to avoid overwhelming the TLS stack */
  self->soup_session = soup_session_new_with_options(
    "max-conns", 2,
    "max-conns-per-host", 1,
    NULL);
  self->image_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  self->image_cache_lru = g_queue_new();
#endif

  /* Create SimplePool for fetching posts */
  self->simple_pool = gnostr_simple_pool_new();

  /* Show default banner from GResource */
  GdkTexture *default_banner = get_default_banner_texture();
  if (default_banner && self->banner_image) {
    gtk_picture_set_paintable(GTK_PICTURE(self->banner_image), GDK_PAINTABLE(default_banner));
    gtk_widget_set_visible(self->banner_image, TRUE);
  }
}

/* Helper to update action button visibility based on whether viewing own profile */
static void update_action_buttons_visibility(GnostrProfilePane *self) {
  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  gboolean is_own_profile = FALSE;
  if (self->current_pubkey && self->own_pubkey) {
    is_own_profile = (g_ascii_strcasecmp(self->current_pubkey, self->own_pubkey) == 0);
  }

  /* Show edit button only for own profile */
  if (self->own_profile_actions) {
    gtk_widget_set_visible(self->own_profile_actions, is_own_profile);
  }

  /* Show follow/message buttons only for other profiles */
  if (self->other_profile_actions) {
    gtk_widget_set_visible(self->other_profile_actions, !is_own_profile);
  }
}

GnostrProfilePane *gnostr_profile_pane_new(void) {
  return g_object_new(GNOSTR_TYPE_PROFILE_PANE, NULL);
}

void gnostr_profile_pane_clear(GnostrProfilePane *self) {
  g_return_if_fail(GNOSTR_IS_PROFILE_PANE(self));
  
  gtk_label_set_text(GTK_LABEL(self->lbl_display_name), "");
  gtk_label_set_text(GTK_LABEL(self->lbl_handle), "");
  gtk_widget_set_visible(self->lbl_bio, FALSE);
  gtk_widget_set_visible(self->metadata_box, FALSE);
  
  /* Clear images and show initials; restore default banner */
  gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
  /* Show default banner instead of hiding */
  GdkTexture *default_banner = get_default_banner_texture();
  if (default_banner && self->banner_image) {
    gtk_picture_set_paintable(GTK_PICTURE(self->banner_image), GDK_PAINTABLE(default_banner));
    gtk_widget_set_visible(self->banner_image, TRUE);
  } else {
    gtk_widget_set_visible(self->banner_image, FALSE);
  }
  
#ifdef HAVE_SOUP3
  /* Cancel any pending loads */
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
    g_clear_object(&self->avatar_cancellable);
  }
  if (self->banner_cancellable) {
    g_cancellable_cancel(self->banner_cancellable);
    g_clear_object(&self->banner_cancellable);
  }
#endif
  
  /* Reset UI to loading state */
  gtk_label_set_text(GTK_LABEL(self->lbl_display_name), "Loading...");
  gtk_label_set_text(GTK_LABEL(self->lbl_handle), "@loading");
  gtk_label_set_text(GTK_LABEL(self->lbl_bio), "");
  gtk_widget_set_visible(self->lbl_bio, FALSE);
  gtk_widget_set_visible(self->metadata_box, FALSE);
  gtk_label_set_text(GTK_LABEL(self->avatar_initials), "?");
  gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), NULL);
  gtk_widget_set_visible(self->avatar_image, FALSE);
  /* Banner already set to default above, don't clear it */
  
  /* Clear metadata box children */
  GtkWidget *child = gtk_widget_get_first_child(self->metadata_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(self->metadata_box), child);
    child = next;
  }

  /* Clear NIP-05 state */
  self->nip05_row = NULL;
  self->nip05_badge = NULL;
  g_clear_pointer(&self->current_nip05, g_free);

  /* Hide NIP-24 bot badge */
  if (self->bot_badge) {
    gtk_widget_set_visible(self->bot_badge, FALSE);
  }
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

  /* Cancel profile loading */
  if (self->profile_cancellable) {
    g_cancellable_cancel(self->profile_cancellable);
    g_clear_object(&self->profile_cancellable);
  }
  self->profile_loaded_from_cache = FALSE;

  /* Cancel posts loading */
  if (self->posts_cancellable) {
    g_cancellable_cancel(self->posts_cancellable);
    g_clear_object(&self->posts_cancellable);
  }

  /* Clear posts */
  if (self->posts_model) {
    g_list_store_remove_all(self->posts_model);
  }
  self->posts_loaded = FALSE;
  self->posts_oldest_timestamp = 0;

  /* Clear NIP-65 relay cache */
  if (self->nip65_cancellable) {
    g_cancellable_cancel(self->nip65_cancellable);
    g_clear_object(&self->nip65_cancellable);
  }
  g_clear_pointer(&self->nip65_relays, g_ptr_array_unref);
  self->nip65_fetched = FALSE;

  /* Reset posts UI state */
  if (self->posts_loading_box)
    gtk_widget_set_visible(self->posts_loading_box, FALSE);
  if (self->posts_empty_box)
    gtk_widget_set_visible(self->posts_empty_box, FALSE);
  if (self->btn_load_more)
    gtk_widget_set_visible(self->btn_load_more, FALSE);
  if (self->posts_scroll)
    gtk_widget_set_visible(self->posts_scroll, TRUE);

  /* Cancel media loading */
  if (self->media_cancellable) {
    g_cancellable_cancel(self->media_cancellable);
    g_clear_object(&self->media_cancellable);
  }

  /* Clear media */
  if (self->media_model) {
    g_list_store_remove_all(self->media_model);
  }
  self->media_loaded = FALSE;
  self->media_oldest_timestamp = 0;

  /* Reset media UI state */
  if (self->media_loading_box)
    gtk_widget_set_visible(self->media_loading_box, FALSE);
  if (self->media_empty_box)
    gtk_widget_set_visible(self->media_empty_box, FALSE);
  if (self->btn_media_load_more)
    gtk_widget_set_visible(self->btn_media_load_more, FALSE);
  if (self->media_scroll)
    gtk_widget_set_visible(self->media_scroll, TRUE);

  /* Clear NIP-84 highlights */
  if (self->highlights_cancellable) {
    g_cancellable_cancel(self->highlights_cancellable);
    g_clear_object(&self->highlights_cancellable);
  }
  self->highlights_loaded = FALSE;
  /* Clear highlight widgets from list */
  if (GTK_IS_BOX(self->highlights_list)) {
    GtkWidget *child = gtk_widget_get_first_child(self->highlights_list);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(self->highlights_list), child);
      child = next;
    }
  }
  /* Reset highlights UI state */
  if (self->highlights_loading_box)
    gtk_widget_set_visible(self->highlights_loading_box, FALSE);
  if (self->highlights_empty_box)
    gtk_widget_set_visible(self->highlights_empty_box, FALSE);
  if (self->highlights_scroll)
    gtk_widget_set_visible(self->highlights_scroll, TRUE);

  /* Clear profile data for posts */
  g_clear_pointer(&self->current_display_name, g_free);
  g_clear_pointer(&self->current_handle, g_free);
  g_clear_pointer(&self->current_avatar_url, g_free);

  g_clear_pointer(&self->current_pubkey, g_free);

  /* Clear NIP-38 user status state */
  if (self->status_cancellable) {
    g_cancellable_cancel(self->status_cancellable);
    g_clear_object(&self->status_cancellable);
  }
  g_clear_pointer(&self->current_general_status, gnostr_user_status_free);
  g_clear_pointer(&self->current_music_status, gnostr_user_status_free);
  self->status_loaded = FALSE;

  /* Remove status widgets */
  if (self->status_box) {
    GtkWidget *parent = gtk_widget_get_parent(self->status_box);
    if (parent && GTK_IS_BOX(parent)) {
      gtk_box_remove(GTK_BOX(parent), self->status_box);
    }
    self->status_box = NULL;
    self->status_general_row = NULL;
    self->status_music_row = NULL;
  }

  /* Clear NIP-58 badge state */
  if (self->badges_cancellable) {
    g_cancellable_cancel(self->badges_cancellable);
    g_clear_object(&self->badges_cancellable);
  }
  g_clear_pointer(&self->profile_badges, g_ptr_array_unref);
  self->badges_loaded = FALSE;

  /* Remove badge widgets */
  if (self->badges_box) {
    GtkWidget *parent = gtk_widget_get_parent(self->badges_box);
    if (parent && GTK_IS_BOX(parent)) {
      gtk_box_remove(GTK_BOX(parent), self->badges_box);
    }
    self->badges_box = NULL;
  }

  /* Clear NIP-39 external identity state */
  g_clear_pointer(&self->current_event_json, g_free);
  g_clear_pointer(&self->external_identities, g_ptr_array_unref);
  if (self->identities_box) {
    GtkWidget *parent = gtk_widget_get_parent(self->identities_box);
    if (parent && GTK_IS_BOX(parent)) {
      gtk_box_remove(GTK_BOX(parent), self->identities_box);
    }
    self->identities_box = NULL;
  }

  /* Reset to About tab */
  if (self->content_stack) {
    gtk_stack_set_visible_child_name(GTK_STACK(self->content_stack), "about");
  }
}

/* Helper to add a metadata row */
static void add_metadata_row(GnostrProfilePane *self, const char *icon_name, const char *label, const char *value, gboolean is_link) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  /* Icon */
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(row), icon);

  /* Label */
  GtkWidget *lbl = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
  gtk_widget_add_css_class(lbl, "dim-label");
  gtk_box_append(GTK_BOX(row), lbl);

  /* Value */
  if (is_link) {
    char *markup = g_markup_printf_escaped("<a href=\"%s\">%s</a>", value, value);
    GtkWidget *link = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(link), markup);
    gtk_label_set_xalign(GTK_LABEL(link), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(link), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(link, TRUE);
    gtk_box_append(GTK_BOX(row), link);
    g_free(markup);
  } else {
    GtkWidget *val = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(val), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_widget_set_hexpand(val, TRUE);
    gtk_box_append(GTK_BOX(row), val);
  }

  gtk_box_append(GTK_BOX(self->metadata_box), row);
  gtk_widget_set_visible(self->metadata_box, TRUE);
}

/* ============== NIP-38 User Status Display ============== */

/* Create a status row widget */
static GtkWidget *create_status_row(const char *icon_name, const char *label,
                                     const char *content, const char *link_url) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  /* Icon */
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_widget_add_css_class(icon, "accent");
  gtk_box_append(GTK_BOX(row), icon);

  /* Label */
  GtkWidget *lbl = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
  gtk_widget_add_css_class(lbl, "dim-label");
  gtk_box_append(GTK_BOX(row), lbl);

  /* Content with optional link */
  if (link_url && *link_url) {
    char *markup = g_markup_printf_escaped("<a href=\"%s\">%s</a>", link_url, content);
    GtkWidget *val = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(val), markup);
    gtk_label_set_xalign(GTK_LABEL(val), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(val, TRUE);
    gtk_box_append(GTK_BOX(row), val);
    g_free(markup);
  } else {
    GtkWidget *val = gtk_label_new(content);
    gtk_label_set_xalign(GTK_LABEL(val), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_widget_set_hexpand(val, TRUE);
    gtk_box_append(GTK_BOX(row), val);
  }

  return row;
}

/* Ensure status_box exists and is added to the about_content */
static void ensure_status_box(GnostrProfilePane *self) {
  if (self->status_box) return;

  self->status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(self->status_box, "profile-status-box");
  gtk_widget_set_margin_top(self->status_box, 8);
  gtk_widget_set_margin_bottom(self->status_box, 8);

  /* Insert after bio (lbl_bio) but before metadata_box in about_content */
  if (self->about_content && GTK_IS_BOX(self->about_content)) {
    /* Find position after bio */
    GtkWidget *sibling = NULL;
    if (self->lbl_bio && gtk_widget_get_visible(self->lbl_bio)) {
      sibling = self->lbl_bio;
    }

    if (sibling) {
      gtk_box_insert_child_after(GTK_BOX(self->about_content), self->status_box, sibling);
    } else {
      /* Prepend if no bio */
      gtk_box_prepend(GTK_BOX(self->about_content), self->status_box);
    }
  }
}

/* Update the status display with current status data */
static void update_status_display(GnostrProfilePane *self) {
  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  /* Check if we have any status to display */
  gboolean has_general = self->current_general_status &&
                          self->current_general_status->content &&
                          *self->current_general_status->content;
  gboolean has_music = self->current_music_status &&
                        self->current_music_status->content &&
                        *self->current_music_status->content;

  if (!has_general && !has_music) {
    /* No status to show - hide/remove status box */
    if (self->status_box) {
      gtk_widget_set_visible(self->status_box, FALSE);
    }
    return;
  }

  /* Ensure status box exists */
  ensure_status_box(self);

  /* Clear existing rows */
  if (self->status_general_row) {
    gtk_box_remove(GTK_BOX(self->status_box), self->status_general_row);
    self->status_general_row = NULL;
  }
  if (self->status_music_row) {
    gtk_box_remove(GTK_BOX(self->status_box), self->status_music_row);
    self->status_music_row = NULL;
  }

  /* Add general status row */
  if (has_general) {
    self->status_general_row = create_status_row(
      "user-status-symbolic",  /* fallback icon */
      "Status",
      self->current_general_status->content,
      self->current_general_status->link_url);
    gtk_box_append(GTK_BOX(self->status_box), self->status_general_row);
  }

  /* Add music status row */
  if (has_music) {
    self->status_music_row = create_status_row(
      "audio-x-generic-symbolic",
      "Listening",
      self->current_music_status->content,
      self->current_music_status->link_url);
    gtk_box_append(GTK_BOX(self->status_box), self->status_music_row);
  }

  gtk_widget_set_visible(self->status_box, TRUE);
}

/* Callback when user status is fetched */
static void on_user_status_fetched(GPtrArray *statuses, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  if (!GNOSTR_IS_PROFILE_PANE(self)) {
    if (statuses) g_ptr_array_unref(statuses);
    return;
  }

  self->status_loaded = TRUE;

  if (!statuses || statuses->len == 0) {
    g_debug("profile_pane: no user status found for %s",
            self->current_pubkey ? self->current_pubkey : "(null)");
    if (statuses) g_ptr_array_unref(statuses);
    return;
  }

  g_debug("profile_pane: received %u user statuses for %s",
          statuses->len, self->current_pubkey ? self->current_pubkey : "(null)");

  /* Process statuses */
  for (guint i = 0; i < statuses->len; i++) {
    GnostrUserStatus *status = g_ptr_array_index(statuses, i);
    if (!status) continue;

    /* Skip expired statuses */
    if (gnostr_user_status_is_expired(status)) {
      g_debug("profile_pane: skipping expired %s status",
              gnostr_user_status_type_to_string(status->type));
      continue;
    }

    /* Store by type (keep newest) */
    if (status->type == GNOSTR_STATUS_GENERAL) {
      if (!self->current_general_status ||
          status->created_at > self->current_general_status->created_at) {
        g_clear_pointer(&self->current_general_status, gnostr_user_status_free);
        self->current_general_status = gnostr_user_status_copy(status);
        g_debug("profile_pane: updated general status: %s",
                status->content ? status->content : "(empty)");
      }
    } else if (status->type == GNOSTR_STATUS_MUSIC) {
      if (!self->current_music_status ||
          status->created_at > self->current_music_status->created_at) {
        g_clear_pointer(&self->current_music_status, gnostr_user_status_free);
        self->current_music_status = gnostr_user_status_copy(status);
        g_debug("profile_pane: updated music status: %s",
                status->content ? status->content : "(empty)");
      }
    }
  }

  g_ptr_array_unref(statuses);

  /* Update UI */
  update_status_display(self);
}

/* Fetch user status for the current profile */
static void fetch_user_status(GnostrProfilePane *self) {
  if (!self->current_pubkey || !*self->current_pubkey) {
    g_debug("profile_pane: no pubkey set, cannot fetch status");
    return;
  }

  /* Cancel any pending fetch */
  if (self->status_cancellable) {
    g_cancellable_cancel(self->status_cancellable);
    g_clear_object(&self->status_cancellable);
  }
  self->status_cancellable = g_cancellable_new();

  g_debug("profile_pane: fetching user status for %.8s...", self->current_pubkey);

  gnostr_user_status_fetch_async(
    self->current_pubkey,
    self->status_cancellable,
    on_user_status_fetched,
    self);
}

/* NIP-05 verification callback */
static void on_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  if (!GNOSTR_IS_PROFILE_PANE(self) || !result) {
    if (result) gnostr_nip05_result_free(result);
    return;
  }

  g_debug("profile_pane: NIP-05 verification result for %s: %s",
          result->identifier, gnostr_nip05_status_to_string(result->status));

  /* Show badge if verified */
  if (result->status == GNOSTR_NIP05_STATUS_VERIFIED && self->nip05_badge) {
    gtk_widget_set_visible(self->nip05_badge, TRUE);
    g_debug("profile_pane: showing NIP-05 verified badge for %s", result->identifier);
  }

  gnostr_nip05_result_free(result);
}

/* Helper to add NIP-05 row with verification badge */
static void add_nip05_row(GnostrProfilePane *self, const char *nip05, const char *pubkey_hex) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  /* Icon */
  GtkWidget *icon = gtk_image_new_from_icon_name("mail-unread-symbolic");
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(row), icon);

  /* Label */
  GtkWidget *lbl = gtk_label_new("NIP-05");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
  gtk_widget_add_css_class(lbl, "dim-label");
  gtk_box_append(GTK_BOX(row), lbl);

  /* NIP-05 value with display formatting */
  char *display = gnostr_nip05_get_display(nip05);
  GtkWidget *val = gtk_label_new(display ? display : nip05);
  g_free(display);
  gtk_label_set_xalign(GTK_LABEL(val), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable(GTK_LABEL(val), TRUE);
  gtk_widget_set_hexpand(val, TRUE);
  gtk_box_append(GTK_BOX(row), val);

  /* Verification badge (hidden initially) */
  GtkWidget *badge = gnostr_nip05_create_badge();
  gtk_widget_set_visible(badge, FALSE);
  gtk_box_append(GTK_BOX(row), badge);

  /* Store references */
  self->nip05_row = row;
  self->nip05_badge = badge;

  gtk_box_append(GTK_BOX(self->metadata_box), row);
  gtk_widget_set_visible(self->metadata_box, TRUE);

  /* Store NIP-05 identifier */
  g_free(self->current_nip05);
  self->current_nip05 = g_strdup(nip05);

  /* Start async verification if we have pubkey */
  if (pubkey_hex && strlen(pubkey_hex) == 64) {
    /* Cancel any previous verification */
    if (self->nip05_cancellable) {
      g_cancellable_cancel(self->nip05_cancellable);
      g_object_unref(self->nip05_cancellable);
    }
    self->nip05_cancellable = g_cancellable_new();

    /* Check cache first for immediate display */
    GnostrNip05Result *cached = gnostr_nip05_cache_get(nip05);
    if (cached) {
      if (cached->status == GNOSTR_NIP05_STATUS_VERIFIED &&
          cached->pubkey_hex &&
          g_ascii_strcasecmp(cached->pubkey_hex, pubkey_hex) == 0) {
        gtk_widget_set_visible(badge, TRUE);
        g_debug("profile_pane: NIP-05 verified from cache for %s", nip05);
      }
      gnostr_nip05_result_free(cached);
    } else {
      /* Verify async */
      gnostr_nip05_verify_async(nip05, pubkey_hex, on_nip05_verified, self, self->nip05_cancellable);
    }
  }
}

/* ============== NIP-39 External Identity Display ============== */

/* Display external identities from "i" tags */
static void display_external_identities(GnostrProfilePane *self) {
  if (!self->external_identities || self->external_identities->len == 0) {
    g_debug("profile_pane: no external identities to display");
    return;
  }

  /* Remove existing identities box if any */
  if (self->identities_box) {
    GtkWidget *parent = gtk_widget_get_parent(self->identities_box);
    if (parent && GTK_IS_BOX(parent)) {
      gtk_box_remove(GTK_BOX(parent), self->identities_box);
    }
    self->identities_box = NULL;
  }

  /* Create container for identities */
  self->identities_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(self->identities_box, "profile-identities");
  gtk_widget_set_margin_top(self->identities_box, 8);

  /* Add section header */
  GtkWidget *header = gtk_label_new("External Identities");
  gtk_label_set_xalign(GTK_LABEL(header), 0.0);
  gtk_widget_add_css_class(header, "heading");
  gtk_widget_add_css_class(header, "dim-label");
  gtk_widget_set_margin_bottom(header, 4);
  gtk_box_append(GTK_BOX(self->identities_box), header);

  /* Add identity rows */
  for (guint i = 0; i < self->external_identities->len; i++) {
    GnostrExternalIdentity *identity = g_ptr_array_index(self->external_identities, i);
    if (!identity) continue;

    GtkWidget *row = gnostr_nip39_create_identity_row(identity);
    if (row) {
      gtk_box_append(GTK_BOX(self->identities_box), row);
    }
  }

  /* Add identities box to metadata_box (which is in about_content) */
  if (self->metadata_box && GTK_IS_BOX(self->metadata_box)) {
    gtk_box_append(GTK_BOX(self->metadata_box), self->identities_box);
    gtk_widget_set_visible(self->metadata_box, TRUE);
  }

  g_debug("profile_pane: displayed %u external identities", self->external_identities->len);
}

/* Parse external identities from the current event JSON */
static void parse_external_identities(GnostrProfilePane *self) {
  /* Clear existing identities */
  g_clear_pointer(&self->external_identities, g_ptr_array_unref);

  if (!self->current_event_json || !*self->current_event_json) {
    g_debug("profile_pane: no event JSON available for identity parsing");
    return;
  }

  self->external_identities = gnostr_nip39_parse_identities_from_event(self->current_event_json);

  if (self->external_identities) {
    g_debug("profile_pane: parsed %u external identities from event",
            self->external_identities->len);
    display_external_identities(self);
  }
}

#ifdef HAVE_SOUP3
/* Helper: Insert into image_cache with LRU eviction */
static void image_cache_insert(GnostrProfilePane *self, const char *url, GdkTexture *texture) {
  if (!self || !url || !texture) return;
  if (!self->image_cache || !self->image_cache_lru) return;

  /* Check if already in cache - just update */
  if (g_hash_table_contains(self->image_cache, url)) {
    g_hash_table_replace(self->image_cache, g_strdup(url), g_object_ref(texture));
    return;
  }

  /* Evict oldest entries if over limit */
  while (g_hash_table_size(self->image_cache) >= IMAGE_CACHE_MAX &&
         !g_queue_is_empty(self->image_cache_lru)) {
    char *oldest_url = g_queue_pop_head(self->image_cache_lru);
    if (oldest_url) {
      g_hash_table_remove(self->image_cache, oldest_url);
      g_free(oldest_url);
    }
  }

  /* Insert new entry */
  g_hash_table_insert(self->image_cache, g_strdup(url), g_object_ref(texture));
  g_queue_push_tail(self->image_cache_lru, g_strdup(url));
}

/* Async image loading callback */
static void on_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  SoupSession *session = SOUP_SESSION(source);
  GTask *task = G_TASK(user_data);
  GnostrProfilePane *self = g_task_get_source_object(task);
  GtkPicture *picture = g_task_get_task_data(task);
  GError *error = NULL;
  
  GBytes *bytes = soup_session_send_and_read_finish(session, res, &error);
  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("Failed to load image: %s", error->message);
    }
    g_error_free(error);
    g_object_unref(task);
    return;
  }
  
  if (!bytes || g_bytes_get_size(bytes) == 0) {
    g_debug("Empty image response");
    if (bytes) g_bytes_unref(bytes);
    g_object_unref(task);
    return;
  }
  
  /* Create texture from bytes */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);
  
  if (error) {
    g_debug("Failed to create texture: %s", error->message);
    g_error_free(error);
    g_object_unref(task);
    return;
  }
  
  /* Update UI on main thread */
  if (GNOSTR_IS_PROFILE_PANE(self) && GTK_IS_PICTURE(picture)) {
    gtk_picture_set_paintable(picture, GDK_PAINTABLE(texture));
    gtk_widget_set_visible(GTK_WIDGET(picture), TRUE);
    
    /* Hide initials when avatar image is loaded */
    if (picture == GTK_PICTURE(self->avatar_image)) {
      gtk_widget_set_visible(self->avatar_initials, FALSE);
    }
    
    /* Cache the texture with LRU eviction */
    const char *url = g_task_get_task_data(task);
    if (url) {
      image_cache_insert(self, url, texture);
    }
  }
  
  g_object_unref(texture);
  g_object_unref(task);
}

/* Use centralized avatar cache API (avatar_cache.h) */

static void load_image_async(GnostrProfilePane *self, const char *url, GtkPicture *picture, GCancellable **cancellable_slot) {
  if (!url || !*url) return;
  
  /* OPTIMIZATION: Use global cache system (memory + disk) instead of local cache */
  GdkTexture *cached = gnostr_avatar_try_load_cached(url);
  if (cached) {
    /* Cache hit! Apply immediately without HTTP request */
    gtk_picture_set_paintable(picture, GDK_PAINTABLE(cached));
    gtk_widget_set_visible(GTK_WIDGET(picture), TRUE);
    
    /* Hide initials if this is the avatar */
    if (picture == GTK_PICTURE(self->avatar_image)) {
      gtk_widget_set_visible(self->avatar_initials, FALSE);
    }
    
    /* Also cache in local hash table for faster subsequent lookups (with LRU eviction) */
    image_cache_insert(self, url, cached);
    
    g_object_unref(cached);
    g_debug("profile_pane: avatar cache HIT for url=%s", url);
    return;
  }
  
  /* Cache miss - use global download system which handles caching automatically */
  g_debug("profile_pane: avatar cache MISS, downloading url=%s", url);
  
  /* Cancel previous load if any */
  if (*cancellable_slot) {
    g_cancellable_cancel(*cancellable_slot);
    g_clear_object(cancellable_slot);
  }
  
  /* Use global download function which validates and caches properly */
  GtkWidget *initials_widget = (picture == GTK_PICTURE(self->avatar_image)) ? self->avatar_initials : NULL;
  gnostr_avatar_download_async(url, GTK_WIDGET(picture), initials_widget);
}

/* Context for banner async loading */
typedef struct _BannerLoadCtx {
  GnostrProfilePane *self;  /* weak ref */
  char *url;                /* owned */
} BannerLoadCtx;

static void banner_load_ctx_free(BannerLoadCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->url);
  g_free(ctx);
}

/* Async banner loading callback - loads at full resolution for quality */
static void on_banner_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  BannerLoadCtx *ctx = (BannerLoadCtx*)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("Failed to load banner: %s", error->message);
    }
    g_clear_error(&error);
    banner_load_ctx_free(ctx);
    return;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    g_debug("Empty banner response");
    if (bytes) g_bytes_unref(bytes);
    banner_load_ctx_free(ctx);
    return;
  }

  /* Create texture from bytes at FULL RESOLUTION for banner quality */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (error) {
    g_debug("Failed to create banner texture: %s", error->message);
    g_clear_error(&error);
    banner_load_ctx_free(ctx);
    return;
  }

  /* Update UI - banner uses full resolution for crisp display */
  if (GNOSTR_IS_PROFILE_PANE(ctx->self) && GTK_IS_PICTURE(ctx->self->banner_image)) {
    gtk_picture_set_paintable(GTK_PICTURE(ctx->self->banner_image), GDK_PAINTABLE(texture));
    gtk_widget_set_visible(ctx->self->banner_image, TRUE);
    g_debug("profile_pane: banner loaded at full resolution for url=%s", ctx->url);

    /* Cache the banner texture locally with LRU eviction */
    image_cache_insert(ctx->self, ctx->url, texture);
  }

  g_object_unref(texture);
  banner_load_ctx_free(ctx);
}

/* Load banner image at full resolution (not using avatar cache which downscales to 96px) */
static void load_banner_async(GnostrProfilePane *self, const char *url) {
  if (!url || !*url) return;

  /* Check local cache first */
  if (self->image_cache) {
    GdkTexture *cached = g_hash_table_lookup(self->image_cache, url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(self->banner_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->banner_image, TRUE);
      g_debug("profile_pane: banner cache HIT for url=%s", url);
      return;
    }
  }

  /* Cancel previous banner load if any */
  if (self->banner_cancellable) {
    g_cancellable_cancel(self->banner_cancellable);
    g_clear_object(&self->banner_cancellable);
  }
  self->banner_cancellable = g_cancellable_new();

  /* Create soup session if needed */
  if (!self->soup_session) {
    self->soup_session = soup_session_new();
  }

  /* Setup context */
  BannerLoadCtx *ctx = g_new0(BannerLoadCtx, 1);
  ctx->self = self;
  ctx->url = g_strdup(url);

  /* Fetch banner at full resolution */
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_debug("profile_pane: invalid banner URL: %s", url);
    banner_load_ctx_free(ctx);
    return;
  }

  g_debug("profile_pane: loading banner at full resolution url=%s", url);
  soup_session_send_and_read_async(self->soup_session, msg, G_PRIORITY_DEFAULT,
                                    self->banner_cancellable, on_banner_loaded, ctx);
  g_object_unref(msg);
}
#endif

static void update_profile_ui(GnostrProfilePane *self, json_t *profile_json) {
  if (!profile_json || !json_is_object(profile_json)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_display_name), "Unknown");
    gtk_label_set_text(GTK_LABEL(self->lbl_handle), self->current_pubkey ? self->current_pubkey : "");
    return;
  }

  /* Clear existing metadata rows to prevent duplicates on re-update */
  if (self->metadata_box) {
    GtkWidget *child = gtk_widget_get_first_child(self->metadata_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(self->metadata_box), child);
      child = next;
    }
    gtk_widget_set_visible(self->metadata_box, FALSE);
  }
  /* Reset NIP-05 state */
  self->nip05_row = NULL;
  self->nip05_badge = NULL;
  
  /* Extract fields - NIP-01 standard plus NIP-24 extra metadata */
  const char *name = NULL;
  const char *display_name = NULL;
  const char *about = NULL;
  const char *picture = NULL;
  const char *banner = NULL;
  const char *nip05 = NULL;
  const char *website = NULL;
  const char *lud06 = NULL;
  const char *lud16 = NULL;
  gboolean is_bot = FALSE;  /* NIP-24: bot indicator */

  json_t *val;
  if ((val = json_object_get(profile_json, "name")) && json_is_string(val))
    name = json_string_value(val);
  if ((val = json_object_get(profile_json, "display_name")) && json_is_string(val))
    display_name = json_string_value(val);
  if ((val = json_object_get(profile_json, "about")) && json_is_string(val))
    about = json_string_value(val);
  if ((val = json_object_get(profile_json, "picture")) && json_is_string(val))
    picture = json_string_value(val);
  if ((val = json_object_get(profile_json, "banner")) && json_is_string(val))
    banner = json_string_value(val);
  if ((val = json_object_get(profile_json, "nip05")) && json_is_string(val))
    nip05 = json_string_value(val);
  if ((val = json_object_get(profile_json, "website")) && json_is_string(val))
    website = json_string_value(val);
  if ((val = json_object_get(profile_json, "lud06")) && json_is_string(val))
    lud06 = json_string_value(val);
  if ((val = json_object_get(profile_json, "lud16")) && json_is_string(val))
    lud16 = json_string_value(val);
  /* NIP-24: Parse bot field - can be boolean true or string "true" */
  if ((val = json_object_get(profile_json, "bot"))) {
    if (json_is_boolean(val)) {
      is_bot = json_boolean_value(val);
    } else if (json_is_string(val)) {
      const char *bot_str = json_string_value(val);
      is_bot = (bot_str && g_ascii_strcasecmp(bot_str, "true") == 0);
    }
  }
  
  /* Update display name - NIP-24: display_name > name > shortened hex key */
  char *shortened_key = NULL;
  const char *final_display = NULL;
  if (display_name && *display_name) {
    final_display = display_name;
  } else if (name && *name) {
    final_display = name;
  } else if (self->current_pubkey && strlen(self->current_pubkey) >= 8) {
    shortened_key = g_strdup_printf("%.8s...", self->current_pubkey);
    final_display = shortened_key;
  } else {
    final_display = "Unknown";
  }
  gtk_label_set_text(GTK_LABEL(self->lbl_display_name), final_display);

  /* NIP-24: Show/hide bot indicator badge */
  if (self->bot_badge) {
    gtk_widget_set_visible(self->bot_badge, is_bot);
    if (is_bot) {
      gtk_widget_set_tooltip_text(self->bot_badge, "This account is a bot");
    }
  }

  /* Store profile info for posts */
  g_free(self->current_display_name);
  self->current_display_name = g_strdup(final_display);
  g_free(shortened_key); /* Free after storing copy */

  /* Update handle - show nip-05 if available, otherwise @name or truncated npub */
  char *handle_text = NULL;
  if (nip05 && *nip05) {
    handle_text = g_strdup(nip05);
  } else if (name && *name) {
    handle_text = g_strdup_printf("@%s", name);
  } else if (self->current_pubkey) {
    /* Show truncated npub */
    handle_text = g_strdup_printf("npub1%.*s...", 8, self->current_pubkey);
  }
  if (handle_text) {
    gtk_label_set_text(GTK_LABEL(self->lbl_handle), handle_text);
    /* Store handle for posts (don't free, we need it) */
    g_free(self->current_handle);
    self->current_handle = g_strdup(handle_text);
    g_free(handle_text);
  }

  /* Store avatar URL for posts */
  g_free(self->current_avatar_url);
  self->current_avatar_url = picture ? g_strdup(picture) : NULL;
  
  /* Update bio */
  if (about && *about) {
    gtk_label_set_text(GTK_LABEL(self->lbl_bio), about);
    gtk_widget_set_visible(self->lbl_bio, TRUE);
  } else {
    gtk_widget_set_visible(self->lbl_bio, FALSE);
  }
  
  /* Update avatar initials */
  if (final_display && *final_display) {
    char initials[7] = {0}; /* UTF-8 char can be up to 6 bytes + null terminator */
    gunichar c = g_utf8_get_char(final_display);
    if (c) {
      g_unichar_to_utf8(g_unichar_toupper(c), initials);
      gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
    }
  }
  
  /* Load images */
#ifdef HAVE_SOUP3
  if (picture && *picture) {
    load_image_async(self, picture, GTK_PICTURE(self->avatar_image), &self->avatar_cancellable);
  }
  if (banner && *banner) {
    /* Use dedicated banner loader for full resolution (avatar cache downscales to 96px) */
    load_banner_async(self, banner);
  }
#endif
  
  /* Add metadata rows */
  if (website && *website) {
    add_metadata_row(self, "web-browser-symbolic", "Website", website, TRUE);
  }
  if (nip05 && *nip05) {
    /* Use special NIP-05 row with verification badge */
    add_nip05_row(self, nip05, self->current_pubkey);
  }
  if (lud16 && *lud16) {
    add_metadata_row(self, "network-wireless-symbolic", "Lightning", lud16, FALSE);
  } else if (lud06 && *lud06) {
    add_metadata_row(self, "network-wireless-symbolic", "Lightning", lud06, FALSE);
  }
  
  /* Show any additional fields (skip NIP-01/NIP-24 standard fields) */
  const char *key;
  json_t *value;
  json_object_foreach(profile_json, key, value) {
    if (json_is_string(value) &&
        strcmp(key, "name") != 0 &&
        strcmp(key, "display_name") != 0 &&
        strcmp(key, "about") != 0 &&
        strcmp(key, "picture") != 0 &&
        strcmp(key, "banner") != 0 &&
        strcmp(key, "nip05") != 0 &&
        strcmp(key, "website") != 0 &&
        strcmp(key, "lud06") != 0 &&
        strcmp(key, "lud16") != 0 &&
        strcmp(key, "bot") != 0) {  /* NIP-24: exclude bot from additional fields */
      const char *str_val = json_string_value(value);
      if (str_val && *str_val) {
        add_metadata_row(self, "text-x-generic-symbolic", key, str_val, FALSE);
      }
    }
  }
}

/* Helper: Check if a post with this ID already exists in the model */
static gboolean post_exists_in_model(GnostrProfilePane *self, const char *id_hex) {
  if (!self->posts_model || !id_hex) return FALSE;

  guint n_items = g_list_model_get_n_items(G_LIST_MODEL(self->posts_model));
  for (guint i = 0; i < n_items; i++) {
    ProfilePostItem *item = g_list_model_get_item(G_LIST_MODEL(self->posts_model), i);
    if (item) {
      gboolean match = (item->id_hex && g_strcmp0(item->id_hex, id_hex) == 0);
      g_object_unref(item);
      if (match) return TRUE;
    }
  }
  return FALSE;
}

/* Load posts from local nostrdb cache.
 * Returns number of posts loaded from cache. */
static guint load_posts_from_cache(GnostrProfilePane *self) {
  if (!self->current_pubkey || !*self->current_pubkey) {
    g_debug("profile_pane: no pubkey set, cannot load from cache");
    return 0;
  }

  /* Build NIP-01 filter JSON for author's kind:1 posts */
  GString *filter_json = g_string_new("[{");
  g_string_append(filter_json, "\"kinds\":[1],");
  g_string_append_printf(filter_json, "\"authors\":[\"%s\"],", self->current_pubkey);

  /* Apply pagination if we already have posts */
  if (self->posts_oldest_timestamp > 0) {
    g_string_append_printf(filter_json, "\"until\":%" G_GINT64_FORMAT ",",
                           self->posts_oldest_timestamp - 1);
  }

  g_string_append_printf(filter_json, "\"limit\":%d}]", POSTS_PAGE_SIZE);

  g_debug("profile_pane: querying nostrdb with filter: %s", filter_json->str);

  /* Begin query transaction */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) != 0 || !txn) {
    g_warning("profile_pane: failed to begin nostrdb query");
    g_string_free(filter_json, TRUE);
    return 0;
  }

  /* Execute query */
  char **json_results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json->str, &json_results, &count);
  g_string_free(filter_json, TRUE);

  if (rc != 0) {
    g_warning("profile_pane: nostrdb query failed with rc=%d", rc);
    storage_ndb_end_query(txn);
    return 0;
  }

  g_debug("profile_pane: nostrdb returned %d cached posts for %s", count, self->current_pubkey);

  guint added = 0;
  gint64 oldest_timestamp = self->posts_oldest_timestamp > 0 ? self->posts_oldest_timestamp : G_MAXINT64;

  for (int i = 0; i < count; i++) {
    const char *json_str = json_results[i];
    if (!json_str) continue;

    /* Parse event */
    NostrEvent *evt = nostr_event_new();
    if (nostr_event_deserialize(evt, json_str) != 0) {
      nostr_event_free(evt);
      continue;
    }

    const char *id_hex = nostr_event_get_id(evt);
    const char *pubkey_hex = nostr_event_get_pubkey(evt);
    const char *content = nostr_event_get_content(evt);
    gint64 created_at = (gint64)nostr_event_get_created_at(evt);

    /* Skip if already in model (dedup) */
    if (post_exists_in_model(self, id_hex)) {
      nostr_event_free(evt);
      continue;
    }

    /* Track oldest timestamp for pagination */
    if (created_at < oldest_timestamp) {
      oldest_timestamp = created_at;
    }

    /* Create post item */
    ProfilePostItem *item = profile_post_item_new(id_hex, pubkey_hex, content, created_at);

    /* Set author info from profile */
    item->display_name = g_strdup(self->current_display_name);
    item->handle = g_strdup(self->current_handle);
    item->avatar_url = g_strdup(self->current_avatar_url);

    /* Add to model */
    g_list_store_append(self->posts_model, item);
    g_object_unref(item);
    added++;

    nostr_event_free(evt);
  }

  storage_ndb_free_results(json_results, count);
  storage_ndb_end_query(txn);

  if (added > 0) {
    self->posts_oldest_timestamp = oldest_timestamp;
    self->posts_loaded = TRUE;
  }

  g_debug("profile_pane: loaded %u posts from cache (oldest_ts=%" G_GINT64_FORMAT ")",
          added, oldest_timestamp);
  return added;
}

/* Callback for posts query completion (network fetch) */
static void on_posts_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  GError *error = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  /* Hide loading indicator */
  if (self->posts_loading_box)
    gtk_widget_set_visible(self->posts_loading_box, FALSE);
  if (self->posts_scroll)
    gtk_widget_set_visible(self->posts_scroll, TRUE);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("Failed to load posts: %s", error->message);
      if (self->posts_empty_label)
        gtk_label_set_text(GTK_LABEL(self->posts_empty_label), "Failed to load posts");
      if (self->posts_empty_box)
        gtk_widget_set_visible(self->posts_empty_box, TRUE);
    }
    g_error_free(error);
    return;
  }

  if (!results || results->len == 0) {
    /* No posts found */
    if (!self->posts_loaded && self->posts_empty_box) {
      gtk_widget_set_visible(self->posts_empty_box, TRUE);
    }
    if (self->btn_load_more)
      gtk_widget_set_visible(self->btn_load_more, FALSE);
    if (results) g_ptr_array_unref(results);
    return;
  }

  self->posts_loaded = TRUE;

  /* Parse and add posts to model, deduplicating with cached results */
  gint64 oldest_timestamp = self->posts_oldest_timestamp > 0 ? self->posts_oldest_timestamp : G_MAXINT64;
  guint added = 0;

  for (guint i = 0; i < results->len; i++) {
    const char *json_str = g_ptr_array_index(results, i);
    if (!json_str) continue;

    /* Parse event */
    NostrEvent *evt = nostr_event_new();
    if (nostr_event_deserialize(evt, json_str) != 0) {
      nostr_event_free(evt);
      continue;
    }

    const char *id_hex = nostr_event_get_id(evt);
    const char *pubkey_hex = nostr_event_get_pubkey(evt);
    const char *content = nostr_event_get_content(evt);
    gint64 created_at = (gint64)nostr_event_get_created_at(evt);

    /* Skip if already in model (dedup with cache) */
    if (post_exists_in_model(self, id_hex)) {
      nostr_event_free(evt);
      continue;
    }

    /* Track oldest timestamp for pagination */
    if (created_at < oldest_timestamp) {
      oldest_timestamp = created_at;
    }

    /* Create post item */
    ProfilePostItem *item = profile_post_item_new(id_hex, pubkey_hex, content, created_at);

    /* Set author info from profile */
    item->display_name = g_strdup(self->current_display_name);
    item->handle = g_strdup(self->current_handle);
    item->avatar_url = g_strdup(self->current_avatar_url);

    /* Add to model */
    g_list_store_append(self->posts_model, item);
    g_object_unref(item);
    added++;

    nostr_event_free(evt);
  }

  self->posts_oldest_timestamp = oldest_timestamp;
  g_debug("profile_pane: network fetch added %u new posts", added);

  /* Show load more button if we got a full page */
  if (self->btn_load_more)
    gtk_widget_set_visible(self->btn_load_more, results->len >= POSTS_PAGE_SIZE);

  g_ptr_array_unref(results);
}

/* Actually load posts using the given relay URLs */
static void load_posts_with_relays(GnostrProfilePane *self, GPtrArray *relay_urls) {
  if (!self->current_pubkey || !*self->current_pubkey) {
    g_debug("profile_pane: no pubkey set, cannot load posts");
    return;
  }

  /* Cancel previous request */
  if (self->posts_cancellable) {
    g_cancellable_cancel(self->posts_cancellable);
    g_clear_object(&self->posts_cancellable);
  }
  self->posts_cancellable = g_cancellable_new();

  /* Build filter for kind 1 events by this author */
  NostrFilter *filter = nostr_filter_new();

  int kinds[1] = { 1 };
  nostr_filter_set_kinds(filter, kinds, 1);

  const char *authors[1] = { self->current_pubkey };
  nostr_filter_set_authors(filter, authors, 1);

  nostr_filter_set_limit(filter, POSTS_PAGE_SIZE);

  /* Set until for pagination (if not first page) */
  if (self->posts_oldest_timestamp > 0) {
    nostr_filter_set_until_i64(filter, self->posts_oldest_timestamp - 1);
  }

  /* Build URL array */
  const char **urls = g_new0(const char*, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Start query */
  gnostr_simple_pool_query_single_async(
    self->simple_pool,
    urls,
    relay_urls->len,
    filter,
    self->posts_cancellable,
    on_posts_query_done,
    self
  );

  g_free(urls);
  nostr_filter_free(filter);
}

/* Callback when NIP-65 relays are fetched */
static void on_nip65_relays_fetched(GPtrArray *relays, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  /* Mark as fetched */
  self->nip65_fetched = TRUE;

  /* Cache the relays */
  g_clear_pointer(&self->nip65_relays, g_ptr_array_unref);
  if (relays && relays->len > 0) {
    self->nip65_relays = g_ptr_array_ref(relays);
    g_debug("profile_pane: fetched %u NIP-65 relays for %s", relays->len, self->current_pubkey);
  } else {
    self->nip65_relays = NULL;
    g_debug("profile_pane: no NIP-65 relays found for %s", self->current_pubkey);
  }

  /* Now load posts using NIP-65 write relays or fallback */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

  /* Use write relays from NIP-65 (where the user publishes their posts) */
  if (self->nip65_relays && self->nip65_relays->len > 0) {
    GPtrArray *write_relays = gnostr_nip65_get_write_relays(self->nip65_relays);
    for (guint i = 0; i < write_relays->len; i++) {
      g_ptr_array_add(relay_urls, g_strdup(g_ptr_array_index(write_relays, i)));
    }
    g_ptr_array_unref(write_relays);
  }

  /* Fall back to configured relays if no NIP-65 write relays */
  if (relay_urls->len == 0) {
    gnostr_get_read_relay_urls_into(relay_urls);
  }

  /* Relays come from GSettings with defaults configured in schema */

  load_posts_with_relays(self, relay_urls);
  g_ptr_array_unref(relay_urls);
}

/* Load posts for the current profile.
 * nostrc-76x: First query nostrdb cache for existing posts, display immediately,
 * then optionally fetch newer posts from relays in background. */
static void load_posts(GnostrProfilePane *self) {
  if (!self->current_pubkey || !*self->current_pubkey) {
    g_debug("profile_pane: no pubkey set, cannot load posts");
    return;
  }

  /* Show loading indicator initially */
  if (self->posts_loading_box)
    gtk_widget_set_visible(self->posts_loading_box, TRUE);
  if (self->posts_empty_box)
    gtk_widget_set_visible(self->posts_empty_box, FALSE);
  if (self->btn_load_more)
    gtk_widget_set_visible(self->btn_load_more, FALSE);

  /* nostrc-76x: First load cached posts from nostrdb and display immediately */
  guint cached_count = load_posts_from_cache(self);

  if (cached_count > 0) {
    /* Hide loading indicator - we have cached data to show */
    if (self->posts_loading_box)
      gtk_widget_set_visible(self->posts_loading_box, FALSE);
    if (self->posts_scroll)
      gtk_widget_set_visible(self->posts_scroll, TRUE);

    /* Show load more button if we got a full page from cache */
    if (self->btn_load_more)
      gtk_widget_set_visible(self->btn_load_more, cached_count >= POSTS_PAGE_SIZE);

    g_debug("profile_pane: displayed %u cached posts, fetching updates from network", cached_count);
  }

  /* nostrc-76x: Now fetch from network in background to get newer posts
   * The callback will merge new posts with cached ones, deduplicating */

  /* If NIP-65 relays already fetched for this profile, use them directly */
  if (self->nip65_fetched) {
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

    /* Use write relays from NIP-65 (where the user publishes their posts) */
    if (self->nip65_relays && self->nip65_relays->len > 0) {
      GPtrArray *write_relays = gnostr_nip65_get_write_relays(self->nip65_relays);
      for (guint i = 0; i < write_relays->len; i++) {
        g_ptr_array_add(relay_urls, g_strdup(g_ptr_array_index(write_relays, i)));
      }
      g_ptr_array_unref(write_relays);
    }

    /* Fall back to configured relays if no NIP-65 write relays */
    if (relay_urls->len == 0) {
      gnostr_get_read_relay_urls_into(relay_urls);
    }

    /* Relays come from GSettings with defaults configured in schema */

    load_posts_with_relays(self, relay_urls);
    g_ptr_array_unref(relay_urls);
    return;
  }

  /* Cancel previous NIP-65 request */
  if (self->nip65_cancellable) {
    g_cancellable_cancel(self->nip65_cancellable);
    g_clear_object(&self->nip65_cancellable);
  }
  self->nip65_cancellable = g_cancellable_new();

  /* Fetch NIP-65 relay list for this profile, then load from network */
  g_debug("profile_pane: fetching NIP-65 relays for %s", self->current_pubkey);
  gnostr_nip65_fetch_relays_async(
    self->current_pubkey,
    self->nip65_cancellable,
    on_nip65_relays_fetched,
    self
  );
}

/* Check if URL looks like media (image/video) */
static gboolean is_media_url(const char *url) {
  if (!url || !*url) return FALSE;

  /* Common image extensions */
  const char *image_exts[] = { ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".bmp", ".svg", ".ico", ".tiff", ".tif", NULL };
  /* Common video extensions */
  const char *video_exts[] = { ".mp4", ".webm", ".mov", ".avi", ".mkv", NULL };

  /* Get lowercase extension */
  const char *last_dot = strrchr(url, '.');
  if (!last_dot) return FALSE;

  char *ext_lower = g_ascii_strdown(last_dot, -1);
  gboolean result = FALSE;

  /* Check image extensions */
  for (int i = 0; image_exts[i]; i++) {
    if (g_str_has_prefix(ext_lower, image_exts[i])) {
      result = TRUE;
      break;
    }
  }

  /* Check video extensions */
  if (!result) {
    for (int i = 0; video_exts[i]; i++) {
      if (g_str_has_prefix(ext_lower, video_exts[i])) {
        result = TRUE;
        break;
      }
    }
  }

  g_free(ext_lower);
  return result;
}

/* Extract media URLs from note content */
static GPtrArray *extract_media_urls_from_content(const char *content) {
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  if (!content || !*content) return urls;

  /* Simple regex-free URL extraction - look for http:// or https:// */
  const char *p = content;
  while (*p) {
    const char *start = NULL;

    /* Look for URL start */
    if (g_str_has_prefix(p, "https://")) {
      start = p;
    } else if (g_str_has_prefix(p, "http://")) {
      start = p;
    }

    if (start) {
      /* Find end of URL (whitespace or end of string) */
      const char *end = start;
      while (*end && !g_ascii_isspace(*end)) {
        end++;
      }

      /* Extract URL */
      gchar *url = g_strndup(start, end - start);

      /* Check if it's a media URL */
      if (is_media_url(url)) {
        g_ptr_array_add(urls, url);
      } else {
        g_free(url);
      }

      p = end;
    } else {
      p++;
    }
  }

  return urls;
}

/* Setup media grid item widget factory */
static void setup_media_item(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  /* Create a square frame with image inside */
  GtkWidget *frame = gtk_frame_new(NULL);
  gtk_widget_set_size_request(frame, 100, 100);
  gtk_widget_add_css_class(frame, "profile-media-item");

  GtkWidget *picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
  gtk_widget_set_size_request(picture, 100, 100);

  gtk_frame_set_child(GTK_FRAME(frame), picture);
  gtk_list_item_set_child(list_item, frame);
}

static void bind_media_item(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  GObject *item = gtk_list_item_get_item(list_item);
  ProfileMediaItem *media = (ProfileMediaItem *)item;

  GtkWidget *frame = gtk_list_item_get_child(list_item);
  if (!GTK_IS_FRAME(frame)) return;

  GtkWidget *picture = gtk_frame_get_child(GTK_FRAME(frame));
  if (!GTK_IS_PICTURE(picture)) return;

  const char *url = media->thumb_url ? media->thumb_url : media->url;
  if (url && *url) {
    /* Try to load from cache or download */
    GdkTexture *cached = gnostr_avatar_try_load_cached(url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(cached));
      g_object_unref(cached);
    } else {
      /* Download async - use avatar download (it works for any image) */
      gnostr_avatar_download_async(url, picture, NULL);
    }
  }
}

/* Callback for media query completion */
static void on_media_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  GError *err = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("profile_pane: media query error: %s", err->message);
    }
    g_error_free(err);

    /* Show empty state */
    if (self->media_loading_box)
      gtk_widget_set_visible(self->media_loading_box, FALSE);
    if (self->media_empty_box)
      gtk_widget_set_visible(self->media_empty_box, TRUE);
    return;
  }

  self->media_loaded = TRUE;

  /* Hide loading */
  if (self->media_loading_box)
    gtk_widget_set_visible(self->media_loading_box, FALSE);

  if (!results || results->len == 0) {
    /* Show empty state */
    if (self->media_empty_box)
      gtk_widget_set_visible(self->media_empty_box, TRUE);
    if (results) g_ptr_array_unref(results);
    return;
  }

  g_debug("profile_pane: received %u events for media extraction", results->len);

  /* Track oldest timestamp for pagination */
  gint64 oldest_timestamp = G_MAXINT64;

  /* Extract media from each event */
  for (guint i = 0; i < results->len; i++) {
    const char *json = g_ptr_array_index(results, i);
    if (!json) continue;

    NostrEvent *evt = nostr_event_new();
    if (!evt || nostr_event_deserialize(evt, json) != 0) {
      if (evt) nostr_event_free(evt);
      continue;
    }

    const char *id_hex = nostr_event_get_id(evt);
    const char *content = nostr_event_get_content(evt);
    gint64 created_at = (gint64)nostr_event_get_created_at(evt);

    if (created_at > 0 && created_at < oldest_timestamp) {
      oldest_timestamp = created_at;
    }

    /* Extract media URLs from content */
    GPtrArray *media_urls = extract_media_urls_from_content(content);

    for (guint j = 0; j < media_urls->len; j++) {
      const char *url = g_ptr_array_index(media_urls, j);

      ProfileMediaItem *item = profile_media_item_new(url, url, id_hex, NULL, created_at);
      g_list_store_append(self->media_model, item);
      g_object_unref(item);
    }

    g_ptr_array_unref(media_urls);
    nostr_event_free(evt);
  }

  self->media_oldest_timestamp = oldest_timestamp;

  /* Check if we have any media */
  guint media_count = g_list_model_get_n_items(G_LIST_MODEL(self->media_model));
  if (media_count == 0) {
    if (self->media_empty_box)
      gtk_widget_set_visible(self->media_empty_box, TRUE);
  } else {
    /* Show load more if we got a full page */
    if (self->btn_media_load_more)
      gtk_widget_set_visible(self->btn_media_load_more, results->len >= MEDIA_PAGE_SIZE);
  }

  g_ptr_array_unref(results);
}

/* Load media for the current profile */
static void load_media(GnostrProfilePane *self) {
  if (!self->current_pubkey || !*self->current_pubkey) {
    g_debug("profile_pane: no pubkey set, cannot load media");
    return;
  }

  /* Cancel previous request */
  if (self->media_cancellable) {
    g_cancellable_cancel(self->media_cancellable);
    g_clear_object(&self->media_cancellable);
  }
  self->media_cancellable = g_cancellable_new();

  /* Setup model and selection if not done */
  if (!self->media_model) {
    self->media_model = g_list_store_new(profile_media_item_get_type());
  }

  if (!self->media_selection && self->media_model) {
    self->media_selection = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(self->media_model)));

    if (GTK_IS_GRID_VIEW(self->media_grid)) {
      /* Setup factory */
      GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
      g_signal_connect(factory, "setup", G_CALLBACK(setup_media_item), self);
      g_signal_connect(factory, "bind", G_CALLBACK(bind_media_item), self);

      gtk_grid_view_set_model(GTK_GRID_VIEW(self->media_grid), self->media_selection);
      gtk_grid_view_set_factory(GTK_GRID_VIEW(self->media_grid), factory);
      g_object_unref(factory);
    }
  }

  /* Show loading indicator */
  if (self->media_loading_box)
    gtk_widget_set_visible(self->media_loading_box, TRUE);
  if (self->media_empty_box)
    gtk_widget_set_visible(self->media_empty_box, FALSE);
  if (self->btn_media_load_more)
    gtk_widget_set_visible(self->btn_media_load_more, FALSE);

  /* Build filter for kind 1 events by this author */
  NostrFilter *filter = nostr_filter_new();

  int kinds[1] = { 1 };
  nostr_filter_set_kinds(filter, kinds, 1);

  const char *authors[1] = { self->current_pubkey };
  nostr_filter_set_authors(filter, authors, 1);

  nostr_filter_set_limit(filter, MEDIA_PAGE_SIZE);

  /* Set until for pagination (if not first page) */
  if (self->media_oldest_timestamp > 0) {
    nostr_filter_set_until_i64(filter, self->media_oldest_timestamp - 1);
  }

  /* Get relay URLs - use NIP-65 if available or fall back to configured */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

  if (self->nip65_relays && self->nip65_relays->len > 0) {
    GPtrArray *write_relays = gnostr_nip65_get_write_relays(self->nip65_relays);
    for (guint i = 0; i < write_relays->len; i++) {
      g_ptr_array_add(relay_urls, g_strdup(g_ptr_array_index(write_relays, i)));
    }
    g_ptr_array_unref(write_relays);
  }

  if (relay_urls->len == 0) {
    gnostr_get_read_relay_urls_into(relay_urls);
  }

  /* Relays come from GSettings with defaults configured in schema */

  /* Build URL array */
  const char **urls = g_new0(const char*, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Start query */
  gnostr_simple_pool_query_single_async(
    self->simple_pool,
    urls,
    relay_urls->len,
    filter,
    self->media_cancellable,
    on_media_query_done,
    self
  );

  g_free(urls);
  g_ptr_array_unref(relay_urls);
  nostr_filter_free(filter);
}

/* ============== Profile Cache/Network Fetch ============== */

/* Helper: convert hex string to 32-byte binary */
static gboolean hex_to_bytes32(const char *hex, uint8_t *out32) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int b;
    if (sscanf(hex + i*2, "%2x", &b) != 1) return FALSE;
    out32[i] = (uint8_t)b;
  }
  return TRUE;
}

/* Callback when network profile fetch completes */
static void on_profile_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  if (!GNOSTR_IS_PROFILE_PANE(self)) return;

  GError *error = NULL;
  GPtrArray *results = gnostr_simple_pool_fetch_profiles_by_authors_finish(
    GNOSTR_SIMPLE_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("profile_pane: network profile fetch failed: %s", error->message);
    }
    g_error_free(error);
    return;
  }

  if (!results || results->len == 0) {
    g_debug("profile_pane: no profile found on network for %.8s",
            self->current_pubkey ? self->current_pubkey : "(null)");
    if (results) g_ptr_array_unref(results);
    return;
  }

  g_debug("profile_pane: received %u profile events from network", results->len);

  /* Find the most recent kind:0 event */
  char *best_content = NULL;
  char *best_event_json = NULL;
  gint64 best_created_at = 0;

  for (guint i = 0; i < results->len; i++) {
    const char *event_json = g_ptr_array_index(results, i);
    if (!event_json) continue;

    NostrEvent *evt = nostr_event_new();
    if (evt && nostr_event_deserialize(evt, event_json) == 0) {
      gint64 created_at = (gint64)nostr_event_get_created_at(evt);
      if (created_at > best_created_at) {
        best_created_at = created_at;
        /* Store content - make a copy since we need it after freeing the event */
        g_free(best_content);
        const char *content = nostr_event_get_content(evt);
        best_content = content ? g_strdup(content) : NULL;
        /* Store full event JSON for NIP-39 identity parsing */
        g_free(best_event_json);
        best_event_json = g_strdup(event_json);
      }
    }
    if (evt) nostr_event_free(evt);
  }

  if (best_content && *best_content) {
    g_debug("profile_pane: updating UI with network profile for %.8s (created_at=%" G_GINT64_FORMAT ")",
            self->current_pubkey ? self->current_pubkey : "(null)", best_created_at);

    /* Store full event JSON for NIP-39 identity parsing */
    g_free(self->current_event_json);
    self->current_event_json = best_event_json;
    best_event_json = NULL;  /* Ownership transferred */

    gnostr_profile_pane_update_from_json(self, best_content);

    /* Parse NIP-39 external identities from the event tags */
    parse_external_identities(self);
  }
  g_free(best_content);
  g_free(best_event_json);

  g_ptr_array_unref(results);
}

/* Fetch profile from nostrdb cache first, then from network */
static void fetch_profile_from_cache_or_network(GnostrProfilePane *self) {
  if (!self->current_pubkey || !*self->current_pubkey) {
    g_debug("profile_pane: no pubkey set, cannot fetch profile");
    return;
  }

  g_debug("profile_pane: fetching profile for %.8s", self->current_pubkey);

  /* Cancel any previous profile fetch */
  if (self->profile_cancellable) {
    g_cancellable_cancel(self->profile_cancellable);
    g_clear_object(&self->profile_cancellable);
  }
  self->profile_cancellable = g_cancellable_new();
  self->profile_loaded_from_cache = FALSE;

  /* Step 1: Try nostrdb cache first */
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn) == 0 && txn) {
    uint8_t pk32[32];
    if (hex_to_bytes32(self->current_pubkey, pk32)) {
      char *event_json = NULL;
      int event_len = 0;

      if (storage_ndb_get_profile_by_pubkey(txn, pk32, &event_json, &event_len) == 0 && event_json) {
        /* Parse the event to get the content field */
        NostrEvent *evt = nostr_event_new();
        if (evt && nostr_event_deserialize(evt, event_json) == 0) {
          const char *content = nostr_event_get_content(evt);
          if (content && *content) {
            g_debug("profile_pane: loaded profile from nostrdb cache for %.8s", self->current_pubkey);

            /* Store full event JSON for NIP-39 identity parsing */
            g_free(self->current_event_json);
            self->current_event_json = g_strdup(event_json);

            gnostr_profile_pane_update_from_json(self, content);
            self->profile_loaded_from_cache = TRUE;

            /* Parse NIP-39 external identities from the event tags */
            parse_external_identities(self);
          }
        }
        if (evt) nostr_event_free(evt);
        /* Note: event_json is owned by nostrdb, do not free */
      }
    }
    storage_ndb_end_query(txn);
  }

  /* Step 2: Always fetch from network for fresh data (even if cached) */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);

  /* Use read relays from GSettings */
  gnostr_get_read_relay_urls_into(relay_urls);

  if (relay_urls->len == 0) {
    g_debug("profile_pane: no relays configured for profile fetch");
    g_ptr_array_unref(relay_urls);
    return;
  }

  /* Build URL array for the API */
  const char **urls = g_new0(const char*, relay_urls->len);
  for (guint i = 0; i < relay_urls->len; i++) {
    urls[i] = g_ptr_array_index(relay_urls, i);
  }

  /* Build authors array */
  const char *authors[1] = { self->current_pubkey };

  g_debug("profile_pane: fetching profile from %u relays for %.8s",
          relay_urls->len, self->current_pubkey);

  /* Fetch profile from network */
  gnostr_simple_pool_fetch_profiles_by_authors_async(
    self->simple_pool,
    urls,
    relay_urls->len,
    authors,
    1,      /* author_count */
    1,      /* limit - we only need the latest */
    self->profile_cancellable,
    on_profile_fetch_done,
    self
  );

  g_free(urls);
  g_ptr_array_unref(relay_urls);

  /* Step 3: Also fetch NIP-38 user status in parallel */
  fetch_user_status(self);
}

/* NIP-84: Load highlights for the current user */
static void load_highlights(GnostrProfilePane *self) {
  if (!self || !self->current_pubkey || self->highlights_loaded) return;

  self->highlights_loaded = TRUE;

  /* Show loading state */
  if (GTK_IS_WIDGET(self->highlights_loading_box)) {
    gtk_widget_set_visible(self->highlights_loading_box, TRUE);
  }
  if (GTK_IS_SPINNER(self->highlights_spinner)) {
    gtk_spinner_start(GTK_SPINNER(self->highlights_spinner));
  }
  if (GTK_IS_WIDGET(self->highlights_empty_box)) {
    gtk_widget_set_visible(self->highlights_empty_box, FALSE);
  }

  /* For now, show empty state after brief delay - actual fetching would use relay pool */
  /* TODO: Implement actual NIP-84 highlight fetching via SimplePool */
  g_message("NIP-84: Highlights tab opened for pubkey %.8s...", self->current_pubkey);

  /* Hide loading, show empty state for now (until relay fetching is implemented) */
  if (GTK_IS_SPINNER(self->highlights_spinner)) {
    gtk_spinner_stop(GTK_SPINNER(self->highlights_spinner));
  }
  if (GTK_IS_WIDGET(self->highlights_loading_box)) {
    gtk_widget_set_visible(self->highlights_loading_box, FALSE);
  }
  if (GTK_IS_WIDGET(self->highlights_empty_box)) {
    gtk_widget_set_visible(self->highlights_empty_box, TRUE);
  }
}

/* Handle tab switch */
static void on_stack_visible_child_changed(GtkStack *stack, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  const char *visible = gtk_stack_get_visible_child_name(stack);
  if (!visible) return;

  if (g_strcmp0(visible, "posts") == 0 && !self->posts_loaded) {
    /* Lazy load posts on first tab switch */
    load_posts(self);
  } else if (g_strcmp0(visible, "media") == 0 && !self->media_loaded) {
    /* Lazy load media on first tab switch */
    load_media(self);
  } else if (g_strcmp0(visible, "highlights") == 0 && !self->highlights_loaded) {
    /* Lazy load highlights on first tab switch */
    load_highlights(self);
  }
}

const char* gnostr_profile_pane_get_current_pubkey(GnostrProfilePane *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE_PANE(self), NULL);
  return self->current_pubkey;
}

void gnostr_profile_pane_set_pubkey(GnostrProfilePane *self, const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_PROFILE_PANE(self));
  g_return_if_fail(pubkey_hex != NULL);

  /* Check if already showing this profile */
  if (self->current_pubkey && strcmp(self->current_pubkey, pubkey_hex) == 0) {
    return;
  }

  /* Clear previous profile */
  gnostr_profile_pane_clear(self);

  /* Store new pubkey */
  self->current_pubkey = g_strdup(pubkey_hex);

  g_debug("profile_pane: set_pubkey called for %.8s...", pubkey_hex);

  /* Show temporary handle while loading */
  char *temp_handle = g_strdup_printf("npub1%.8s...", pubkey_hex);
  gtk_label_set_text(GTK_LABEL(self->lbl_handle), temp_handle);
  g_free(temp_handle);

  /* Fetch profile from cache first, then network for updates */
  fetch_profile_from_cache_or_network(self);
}

/* Public API to update profile from JSON (called by main window) */
void gnostr_profile_pane_update_from_json(GnostrProfilePane *self, const char *profile_json_str) {
  g_return_if_fail(GNOSTR_IS_PROFILE_PANE(self));

  if (!profile_json_str || !*profile_json_str) {
    g_debug("ProfilePane: empty profile JSON");
    return;
  }

  /* Store raw JSON for edit dialog */
  g_free(self->current_profile_json);
  self->current_profile_json = g_strdup(profile_json_str);

  json_error_t error;
  json_t *root = json_loads(profile_json_str, 0, &error);
  if (!root) {
    g_warning("ProfilePane: failed to parse profile JSON: %s", error.text);
    return;
  }

  update_profile_ui(self, root);
  json_decref(root);

  /* Update button visibility after profile is loaded */
  update_action_buttons_visibility(self);

  /* Fetch NIP-58 badges if not already loaded */
  if (!self->badges_loaded && self->current_pubkey) {
    load_badges(self);
  }
}

void gnostr_profile_pane_set_own_pubkey(GnostrProfilePane *self, const char *own_pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_PROFILE_PANE(self));

  g_free(self->own_pubkey);
  self->own_pubkey = own_pubkey_hex ? g_strdup(own_pubkey_hex) : NULL;

  /* Update button visibility */
  update_action_buttons_visibility(self);
}

const char* gnostr_profile_pane_get_profile_json(GnostrProfilePane *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE_PANE(self), NULL);
  return self->current_profile_json;
}

void gnostr_profile_pane_refresh(GnostrProfilePane *self) {
  g_return_if_fail(GNOSTR_IS_PROFILE_PANE(self));

  if (!self->current_pubkey || !*self->current_pubkey) {
    g_debug("profile_pane: no pubkey set, cannot refresh");
    return;
  }

  g_debug("profile_pane: refreshing profile for %.8s", self->current_pubkey);

  /* Fetch profile from cache first, then network for updates */
  fetch_profile_from_cache_or_network(self);
}

gboolean gnostr_profile_pane_is_profile_cached(GnostrProfilePane *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE_PANE(self), FALSE);
  return self->profile_loaded_from_cache;
}

/* ============== NIP-58 Badge Display ============== */

/* Badge icon size in pixels */
#define BADGE_ICON_SIZE 32

/* Maximum badges to display */
#define MAX_VISIBLE_BADGES 8

/* Create a badge icon widget with click handler for popover */
static GtkWidget *
create_badge_icon(GnostrProfilePane *self, GnostrProfileBadge *badge)
{
  (void)self; /* May be used for popover parent in future */

  if (!badge || !badge->definition) return NULL;

  GnostrBadgeDefinition *def = badge->definition;

  /* Create a clickable frame for the badge */
  GtkWidget *button = gtk_button_new();
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "profile-badge-icon");
  gtk_widget_set_size_request(button, BADGE_ICON_SIZE, BADGE_ICON_SIZE);

  /* Create the badge image */
  GtkWidget *picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
  gtk_widget_set_size_request(picture, BADGE_ICON_SIZE, BADGE_ICON_SIZE);

  /* Use thumbnail if available, otherwise image */
  const char *image_url = def->thumb_url ? def->thumb_url : def->image_url;
  if (image_url && *image_url) {
    /* Try to load from cache */
    GdkTexture *cached = gnostr_badge_get_cached_image(image_url);
    if (cached) {
      gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(cached));
      g_object_unref(cached);
    } else {
      /* Download async using avatar cache infrastructure */
      gnostr_avatar_download_async(image_url, picture, NULL);
    }
  } else {
    /* No image - show a placeholder icon */
    GtkWidget *icon = gtk_image_new_from_icon_name("starred-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), BADGE_ICON_SIZE - 8);
    gtk_button_set_child(GTK_BUTTON(button), icon);

    /* Set tooltip with badge name */
    if (def->name && *def->name) {
      gtk_widget_set_tooltip_text(button, def->name);
    }
    return button;
  }

  gtk_button_set_child(GTK_BUTTON(button), picture);

  /* Set tooltip with badge name and description */
  if (def->name && *def->name) {
    GString *tooltip = g_string_new(def->name);
    if (def->description && *def->description) {
      g_string_append_printf(tooltip, "\n%s", def->description);
    }
    gtk_widget_set_tooltip_text(button, tooltip->str);
    g_string_free(tooltip, TRUE);
  }

  return button;
}

/* Build the badges display box */
static void
build_badges_display(GnostrProfilePane *self)
{
  if (!self->profile_badges || self->profile_badges->len == 0) {
    g_debug("profile_pane: no badges to display");
    return;
  }

  /* Remove existing badges box if any */
  if (self->badges_box) {
    GtkWidget *parent = gtk_widget_get_parent(self->badges_box);
    if (parent && GTK_IS_BOX(parent)) {
      gtk_box_remove(GTK_BOX(parent), self->badges_box);
    }
    self->badges_box = NULL;
  }

  /* Create container for badges */
  self->badges_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(self->badges_box, "profile-badges");
  gtk_widget_set_margin_top(self->badges_box, 8);
  gtk_widget_set_margin_bottom(self->badges_box, 4);
  gtk_widget_set_halign(self->badges_box, GTK_ALIGN_START);

  /* Add badge label */
  GtkWidget *label = gtk_label_new("Badges:");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_set_margin_end(label, 4);
  gtk_box_append(GTK_BOX(self->badges_box), label);

  /* Add badge icons (up to max visible) */
  guint count = MIN(self->profile_badges->len, MAX_VISIBLE_BADGES);
  for (guint i = 0; i < count; i++) {
    GnostrProfileBadge *badge = g_ptr_array_index(self->profile_badges, i);
    GtkWidget *icon = create_badge_icon(self, badge);
    if (icon) {
      gtk_box_append(GTK_BOX(self->badges_box), icon);
    }
  }

  /* If there are more badges, show a "more" indicator */
  if (self->profile_badges->len > MAX_VISIBLE_BADGES) {
    gchar *more_text = g_strdup_printf("+%u", self->profile_badges->len - MAX_VISIBLE_BADGES);
    GtkWidget *more_label = gtk_label_new(more_text);
    gtk_widget_add_css_class(more_label, "dim-label");
    gtk_widget_set_margin_start(more_label, 4);
    gtk_box_append(GTK_BOX(self->badges_box), more_label);
    g_free(more_text);
  }

  /* Insert badges box into about_content after bio but before metadata */
  if (self->about_content && GTK_IS_BOX(self->about_content)) {
    /* Find the bio label and insert after it */
    GtkWidget *child = gtk_widget_get_first_child(self->about_content);
    GtkWidget *insert_after = NULL;

    while (child) {
      if (child == self->lbl_bio) {
        insert_after = child;
        break;
      }
      child = gtk_widget_get_next_sibling(child);
    }

    if (insert_after) {
      /* Insert after bio label */
      GtkWidget *next = gtk_widget_get_next_sibling(insert_after);
      if (next) {
        gtk_box_insert_child_after(GTK_BOX(self->about_content), self->badges_box, insert_after);
      } else {
        gtk_box_append(GTK_BOX(self->about_content), self->badges_box);
      }
    } else {
      /* Fallback: append at the end */
      gtk_box_append(GTK_BOX(self->about_content), self->badges_box);
    }
  }

  g_debug("profile_pane: displaying %u badges", count);
}

/* Callback when badges are fetched */
static void
on_badges_fetched(GPtrArray *badges, gpointer user_data)
{
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);

  if (!GNOSTR_IS_PROFILE_PANE(self)) {
    if (badges) g_ptr_array_unref(badges);
    return;
  }

  self->badges_loaded = TRUE;

  /* Clear previous badges */
  g_clear_pointer(&self->profile_badges, g_ptr_array_unref);

  if (!badges || badges->len == 0) {
    g_debug("profile_pane: no badges found for user");
    if (badges) g_ptr_array_unref(badges);
    return;
  }

  /* Store the badges (transfer ownership) */
  self->profile_badges = badges;

  g_debug("profile_pane: received %u badges", badges->len);

  /* Build the display */
  build_badges_display(self);
}

/* Fetch badges for the current profile */
static void
load_badges(GnostrProfilePane *self)
{
  if (!self->current_pubkey || strlen(self->current_pubkey) != 64) {
    g_debug("profile_pane: no valid pubkey for badge fetch");
    return;
  }

  if (self->badges_loaded) {
    g_debug("profile_pane: badges already loaded");
    return;
  }

  /* Cancel previous fetch if any */
  if (self->badges_cancellable) {
    g_cancellable_cancel(self->badges_cancellable);
    g_clear_object(&self->badges_cancellable);
  }
  self->badges_cancellable = g_cancellable_new();

  g_debug("profile_pane: fetching badges for %.8s", self->current_pubkey);

  gnostr_fetch_profile_badges_async(
    self->current_pubkey,
    self->badges_cancellable,
    on_badges_fetched,
    self
  );
}
