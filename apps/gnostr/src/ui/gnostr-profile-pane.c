#include "gnostr-profile-pane.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "gnostr-avatar-cache.h"
#include "../util/nip05.h"
#include <jansson.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-profile-pane.ui"

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
  
  /* State */
  char *current_pubkey;
  char *current_nip05;           /* NIP-05 identifier from profile */
  GtkWidget *nip05_badge;        /* Verification badge widget */
  GtkWidget *nip05_row;          /* NIP-05 metadata row */
  GCancellable *nip05_cancellable;
#ifdef HAVE_SOUP3
  SoupSession *soup_session;
  GCancellable *banner_cancellable;
  GCancellable *avatar_cancellable;
  GHashTable *image_cache; /* URL -> GdkTexture */
#endif
};

G_DEFINE_TYPE(GnostrProfilePane, gnostr_profile_pane, GTK_TYPE_WIDGET)

enum {
  SIGNAL_CLOSE_REQUESTED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
#ifdef HAVE_SOUP3
static void load_image_async(GnostrProfilePane *self, const char *url, GtkPicture *picture, GCancellable **cancellable_slot);
#endif
static void update_profile_ui(GnostrProfilePane *self, json_t *profile_json);

static void gnostr_profile_pane_dispose(GObject *obj) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(obj);
  /* Cancel NIP-05 verification if in progress */
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
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
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_PROFILE_PANE);
  G_OBJECT_CLASS(gnostr_profile_pane_parent_class)->dispose(obj);
}

static void gnostr_profile_pane_finalize(GObject *obj) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(obj);
  g_clear_pointer(&self->current_pubkey, g_free);
  g_clear_pointer(&self->current_nip05, g_free);
  G_OBJECT_CLASS(gnostr_profile_pane_parent_class)->finalize(obj);
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  GnostrProfilePane *self = GNOSTR_PROFILE_PANE(user_data);
  (void)btn;
  g_signal_emit(self, signals[SIGNAL_CLOSE_REQUESTED], 0);
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
  
  gtk_widget_class_bind_template_callback(wclass, on_close_clicked);
  
  signals[SIGNAL_CLOSE_REQUESTED] = g_signal_new(
    "close-requested",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void gnostr_profile_pane_init(GnostrProfilePane *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_close),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Close Profile", -1);
  g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);
  
#ifdef HAVE_SOUP3
  /* Create session with connection limits to avoid overwhelming the TLS stack */
  self->soup_session = soup_session_new_with_options(
    "max-conns", 2,
    "max-conns-per-host", 1,
    NULL);
  self->image_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
#endif
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

  g_clear_pointer(&self->current_pubkey, g_free);
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
    g_free(handle_text);
  }
  
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
  
  json_error_t error;
  json_t *root = json_loads(profile_json_str, 0, &error);
  if (!root) {
    g_warning("ProfilePane: failed to parse profile JSON: %s", error.text);
    return;
  }
  
  update_profile_ui(self, root);
  json_decref(root);
}
