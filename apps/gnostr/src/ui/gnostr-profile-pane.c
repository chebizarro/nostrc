#include "gnostr-profile-pane.h"
#include "gnostr-profile-edit.h"
#include "note_card_row.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "gnostr-avatar-cache.h"
#include "../util/nip05.h"
#include "../util/relays.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr_simple_pool.h"
#include <jansson.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-profile-pane.ui"

/* Maximum posts to fetch per page */
#define POSTS_PAGE_SIZE 20

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

struct _GnostrProfilePane {
  GtkWidget parent_instance;

  /* Template children */
  GtkWidget *root;
  GtkWidget *btn_close;
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
  GCancellable *nip05_cancellable;
  GCancellable *posts_cancellable;
  gboolean posts_loaded;
  gint64 posts_oldest_timestamp;  /* For pagination */
#ifdef HAVE_SOUP3
  SoupSession *soup_session;
  GCancellable *banner_cancellable;
  GCancellable *avatar_cancellable;
  GHashTable *image_cache; /* URL -> GdkTexture */
#endif

  /* SimplePool for fetching posts */
  GnostrSimplePool *simple_pool;
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
#endif
static void update_profile_ui(GnostrProfilePane *self, json_t *profile_json);
static void load_posts(GnostrProfilePane *self);
static void on_stack_visible_child_changed(GtkStack *stack, GParamSpec *pspec, gpointer user_data);

static void gnostr_profile_pane_dispose(GObject *obj) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(obj);
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
#endif
  /* Clear posts model */
  if (self->posts_list && GTK_IS_LIST_VIEW(self->posts_list)) {
    gtk_list_view_set_model(GTK_LIST_VIEW(self->posts_list), NULL);
  }
  g_clear_object(&self->posts_selection);
  g_clear_object(&self->posts_model);
  g_clear_object(&self->simple_pool);
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_PROFILE_PANE);
  G_OBJECT_CLASS(gnostr_profile_pane_parent_class)->dispose(obj);
}

static void gnostr_profile_pane_finalize(GObject *obj) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(obj);
  g_clear_pointer(&self->current_pubkey, g_free);
  g_clear_pointer(&self->own_pubkey, g_free);
  g_clear_pointer(&self->current_profile_json, g_free);
  g_clear_pointer(&self->current_nip05, g_free);
  g_clear_pointer(&self->current_display_name, g_free);
  g_clear_pointer(&self->current_handle, g_free);
  g_clear_pointer(&self->current_avatar_url, g_free);
  G_OBJECT_CLASS(gnostr_profile_pane_parent_class)->finalize(obj);
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)btn;
  g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
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

  /* Connect to profile-saved signal */
  g_signal_connect(edit_dialog, "profile-saved", G_CALLBACK(on_profile_saved), self);

  /* Show the dialog */
  gtk_window_present(GTK_WINDOW(edit_dialog));
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
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, banner_image);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, avatar_box);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, avatar_image);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, avatar_initials);
  gtk_widget_class_bind_template_child(wclass, GnostrProfilePane, lbl_display_name);
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

  gtk_widget_class_bind_template_callback(wclass, on_close_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_edit_profile_clicked);

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

  /* Connect edit profile button */
  if (self->btn_edit_profile) {
    g_signal_connect(self->btn_edit_profile, "clicked", G_CALLBACK(on_edit_profile_clicked), self);
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
#endif

  /* Create SimplePool for fetching posts */
  self->simple_pool = gnostr_simple_pool_new();
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
  
  /* Clear images and show initials */
  gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
  gtk_widget_set_visible(self->banner_image, FALSE);
  
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
  gtk_picture_set_paintable(GTK_PICTURE(self->banner_image), NULL);
  
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
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

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

  /* Reset posts UI state */
  if (self->posts_loading_box)
    gtk_widget_set_visible(self->posts_loading_box, FALSE);
  if (self->posts_empty_box)
    gtk_widget_set_visible(self->posts_empty_box, FALSE);
  if (self->btn_load_more)
    gtk_widget_set_visible(self->btn_load_more, FALSE);
  if (self->posts_scroll)
    gtk_widget_set_visible(self->posts_scroll, TRUE);

  /* Clear profile data for posts */
  g_clear_pointer(&self->current_display_name, g_free);
  g_clear_pointer(&self->current_handle, g_free);
  g_clear_pointer(&self->current_avatar_url, g_free);

  g_clear_pointer(&self->current_pubkey, g_free);

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

#ifdef HAVE_SOUP3
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
    
    /* Cache the texture */
    const char *url = g_task_get_task_data(task);
    if (url && self->image_cache) {
      g_hash_table_insert(self->image_cache, g_strdup(url), g_object_ref(texture));
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
    
    /* Also cache in local hash table for faster subsequent lookups */
    if (self->image_cache) {
      g_hash_table_insert(self->image_cache, g_strdup(url), g_object_ref(cached));
    }
    
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
#endif

static void update_profile_ui(GnostrProfilePane *self, json_t *profile_json) {
  if (!profile_json || !json_is_object(profile_json)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_display_name), "Unknown");
    gtk_label_set_text(GTK_LABEL(self->lbl_handle), self->current_pubkey ? self->current_pubkey : "");
    return;
  }
  
  /* Extract fields */
  const char *name = NULL;
  const char *display_name = NULL;
  const char *about = NULL;
  const char *picture = NULL;
  const char *banner = NULL;
  const char *nip05 = NULL;
  const char *website = NULL;
  const char *lud06 = NULL;
  const char *lud16 = NULL;
  
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
  
  /* Update display name */
  const char *final_display = display_name ? display_name : (name ? name : "Anonymous");
  gtk_label_set_text(GTK_LABEL(self->lbl_display_name), final_display);

  /* Store profile info for posts */
  g_free(self->current_display_name);
  self->current_display_name = g_strdup(final_display);

  /* Update handle */
  char *handle_text = NULL;
  if (name && *name) {
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
    load_image_async(self, banner, GTK_PICTURE(self->banner_image), &self->banner_cancellable);
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
  
  /* Show any additional fields */
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
        strcmp(key, "lud16") != 0) {
      const char *str_val = json_string_value(value);
      if (str_val && *str_val) {
        add_metadata_row(self, "text-x-generic-symbolic", key, str_val, FALSE);
      }
    }
  }
}

/* Callback for posts query completion */
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

  /* Parse and add posts to model */
  gint64 oldest_timestamp = G_MAXINT64;

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

    nostr_event_free(evt);
  }

  self->posts_oldest_timestamp = oldest_timestamp;

  /* Show load more button if we got a full page */
  if (self->btn_load_more)
    gtk_widget_set_visible(self->btn_load_more, results->len >= POSTS_PAGE_SIZE);

  g_ptr_array_unref(results);
}

/* Load posts for the current profile */
static void load_posts(GnostrProfilePane *self) {
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

  /* Show loading indicator */
  if (self->posts_loading_box)
    gtk_widget_set_visible(self->posts_loading_box, TRUE);
  if (self->posts_empty_box)
    gtk_widget_set_visible(self->posts_empty_box, FALSE);
  if (self->btn_load_more)
    gtk_widget_set_visible(self->btn_load_more, FALSE);

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

  /* Get relay URLs */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_arr);

  if (relay_arr->len == 0) {
    /* Add default relays if none configured */
    g_ptr_array_add(relay_arr, g_strdup("wss://relay.damus.io"));
    g_ptr_array_add(relay_arr, g_strdup("wss://nos.lol"));
    g_ptr_array_add(relay_arr, g_strdup("wss://relay.nostr.band"));
  }

  /* Build URL array */
  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Start query */
  gnostr_simple_pool_query_single_async(
    self->simple_pool,
    urls,
    relay_arr->len,
    filter,
    self->posts_cancellable,
    on_posts_query_done,
    self
  );

  g_free(urls);
  g_ptr_array_unref(relay_arr);
  nostr_filter_free(filter);
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
  
  /* TODO: Fetch profile from cache or network
   * For now, we'll emit a signal that the main window can handle
   * The main window already has profile caching logic we can reuse
   */
  g_message("ProfilePane: set_pubkey called for %.*s...", 8, pubkey_hex);
  
  /* Temporary: show pubkey as handle */
  char *temp_handle = g_strdup_printf("npub1%.*s...", 8, pubkey_hex);
  gtk_label_set_text(GTK_LABEL(self->lbl_handle), temp_handle);
  g_free(temp_handle);
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
