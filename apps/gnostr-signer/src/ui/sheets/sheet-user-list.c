/* sheet-user-list.c - User list management dialog implementation */
#include "sheet-user-list.h"
#include "../app-resources.h"
#include "../../profile_store.h"
#include "../../cache-manager.h"
#include "../../relay_store.h"

#include <gio/gio.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* Avatar cache - shared across all user list instances */
static GnCache *avatar_cache = NULL;
static GMutex avatar_cache_mutex;

/* Context for async avatar loading */
typedef struct {
  GtkWidget *avatar_widget;  /* The GtkImage to update */
  gchar *url;                /* URL being loaded */
  GCancellable *cancellable; /* For cancellation on widget destroy */
} AvatarLoadContext;

static void avatar_load_context_free(AvatarLoadContext *ctx) {
  if (!ctx) return;
  g_free(ctx->url);
  g_clear_object(&ctx->cancellable);
  g_free(ctx);
}

/* Initialize avatar cache (thread-safe) */
static GnCache *get_avatar_cache(void) {
  g_mutex_lock(&avatar_cache_mutex);
  if (!avatar_cache) {
    /* Cache up to 100 avatars, 10MB max, 1 hour TTL */
    avatar_cache = gn_cache_new("avatars", 100, 10 * 1024 * 1024, 3600,
                                 (GnCacheValueFree)g_object_unref);
  }
  g_mutex_unlock(&avatar_cache_mutex);
  return avatar_cache;
}

/* Callback when avatar image download completes */
static void on_avatar_stream_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
  AvatarLoadContext *ctx = user_data;
  GInputStream *stream = G_INPUT_STREAM(source);
  GError *error = NULL;

  /* Check if the widget was destroyed while loading */
  if (!GTK_IS_IMAGE(ctx->avatar_widget)) {
    avatar_load_context_free(ctx);
    return;
  }

  /* Check if cancelled */
  if (g_cancellable_is_cancelled(ctx->cancellable)) {
    avatar_load_context_free(ctx);
    return;
  }

  /* Load the pixbuf from the stream */
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_finish(result, &error);

  if (error) {
    g_debug("Failed to load avatar from %s: %s", ctx->url, error->message);
    g_error_free(error);
    avatar_load_context_free(ctx);
    return;
  }

  if (pixbuf) {
    /* Scale to appropriate size (40x40) */
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, 40, 40, GDK_INTERP_BILINEAR);
    g_object_unref(pixbuf);

    if (scaled) {
      /* Create texture and set on the image.
       * Note: gdk_texture_new_for_pixbuf is deprecated in GTK 4.20+
       * but we use it for broader compatibility. */
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      GdkTexture *texture = gdk_texture_new_for_pixbuf(scaled);
      G_GNUC_END_IGNORE_DEPRECATIONS

      /* Cache the texture for later use */
      GnCache *cache = get_avatar_cache();
      if (cache) {
        /* Add ref because cache will unref on eviction */
        g_object_ref(texture);
        gn_cache_put(cache, ctx->url, texture);
      }

      /* Update the image widget if still valid */
      if (GTK_IS_IMAGE(ctx->avatar_widget)) {
        gtk_image_set_from_paintable(GTK_IMAGE(ctx->avatar_widget), GDK_PAINTABLE(texture));
      }

      g_object_unref(texture);
      g_object_unref(scaled);
    }
  }

  avatar_load_context_free(ctx);
}

/* Callback when file input stream is opened */
static void on_avatar_file_opened(GObject *source, GAsyncResult *result, gpointer user_data) {
  AvatarLoadContext *ctx = user_data;
  GFile *file = G_FILE(source);
  GError *error = NULL;

  /* Check if the widget was destroyed while opening */
  if (!GTK_IS_IMAGE(ctx->avatar_widget)) {
    avatar_load_context_free(ctx);
    return;
  }

  GFileInputStream *stream = g_file_read_finish(file, result, &error);

  if (error) {
    g_debug("Failed to open avatar URL %s: %s", ctx->url, error->message);
    g_error_free(error);
    avatar_load_context_free(ctx);
    return;
  }

  /* Read the pixbuf asynchronously */
  gdk_pixbuf_new_from_stream_async(G_INPUT_STREAM(stream),
                                    ctx->cancellable,
                                    on_avatar_stream_ready,
                                    ctx);

  g_object_unref(stream);
}

/* Handle widget destruction - cancel any pending load */
static void on_avatar_widget_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  GCancellable *cancellable = G_CANCELLABLE(user_data);
  if (cancellable) {
    g_cancellable_cancel(cancellable);
  }
}

/* Load avatar image asynchronously from URL */
static void load_avatar_async(GtkImage *avatar, const gchar *url) {
  if (!avatar || !url || !*url) return;

  /* Check cache first */
  GnCache *cache = get_avatar_cache();
  if (cache) {
    GdkTexture *cached = gn_cache_get(cache, url);
    if (cached) {
      gtk_image_set_from_paintable(avatar, GDK_PAINTABLE(cached));
      return;
    }
  }

  /* Validate URL - only load http(s) URLs */
  if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
    return;
  }

  /* Create context for async operation */
  AvatarLoadContext *ctx = g_new0(AvatarLoadContext, 1);
  ctx->avatar_widget = GTK_WIDGET(avatar);
  ctx->url = g_strdup(url);
  ctx->cancellable = g_cancellable_new();

  /* Connect to widget destroy signal to cancel load */
  g_signal_connect(avatar, "destroy", G_CALLBACK(on_avatar_widget_destroy), ctx->cancellable);

  /* Open the URL as a GFile (GIO supports http/https) */
  GFile *file = g_file_new_for_uri(url);
  g_file_read_async(file, G_PRIORITY_LOW, ctx->cancellable,
                    on_avatar_file_opened, ctx);
  g_object_unref(file);
}

struct _SheetUserList {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_publish;
  GtkButton *btn_add;
  GtkButton *btn_sync;
  GtkEntry *entry_pubkey;
  GtkEntry *entry_petname;
  GtkSearchEntry *search_entry;
  GtkListBox *list_users;
  GtkLabel *lbl_title;
  GtkLabel *lbl_count;

  /* State */
  UserListType type;
  UserListStore *store;
  ProfileStore *profile_store;
  SheetUserListSaveCb on_publish;
  gpointer on_publish_ud;
};

G_DEFINE_TYPE(SheetUserList, sheet_user_list, ADW_TYPE_DIALOG)

/* Row data */
typedef struct {
  gchar *pubkey;
  gchar *petname;
  gchar *display_name;
  gchar *avatar_url;
  gchar *nip05;
} UserRowData;

static void user_row_data_free(UserRowData *data) {
  if (!data) return;
  g_free(data->pubkey);
  g_free(data->petname);
  g_free(data->display_name);
  g_free(data->avatar_url);
  g_free(data->nip05);
  g_free(data);
}

static void update_count_label(SheetUserList *self) {
  guint count = user_list_store_count(self->store);
  gchar *text = g_strdup_printf("%u %s",
    count,
    self->type == USER_LIST_FOLLOWS ? "following" : "muted");
  gtk_label_set_text(self->lbl_count, text);
  g_free(text);
}

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetUserList *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
}

static void save_list_to_store(SheetUserList *self) {
  /* Rebuild store from UI list */
  user_list_store_clear(self->store);

  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_users));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (!ADW_IS_ACTION_ROW(child)) continue;

    UserRowData *data = g_object_get_data(G_OBJECT(child), "user-data");
    if (!data) continue;

    user_list_store_add(self->store, data->pubkey, NULL, data->petname);
  }

  user_list_store_save(self->store);
}

static void on_publish(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetUserList *self = user_data;

  save_list_to_store(self);

  if (self->on_publish) {
    gchar *event_json = user_list_store_build_event_json(self->store);
    self->on_publish(self->type, event_json, self->on_publish_ud);
    g_free(event_json);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void on_row_remove(GtkButton *btn, gpointer user_data) {
  GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
  SheetUserList *self = g_object_get_data(G_OBJECT(row), "sheet-self");
  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));

  /* Get pubkey and remove from store */
  UserRowData *data = g_object_get_data(G_OBJECT(row), "user-data");
  if (data) {
    user_list_store_remove(self->store, data->pubkey);
  }

  gtk_list_box_remove(list, GTK_WIDGET(row));
  update_count_label(self);
}

static GtkWidget *create_user_row(SheetUserList *self, const gchar *pubkey,
                                  const gchar *petname, const gchar *display_name,
                                  const gchar *avatar_url, const gchar *nip05) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Determine title: petname > display_name > truncated pubkey */
  const gchar *title = NULL;
  gchar *title_owned = NULL;
  if (petname && *petname) {
    title = petname;
  } else if (display_name && *display_name) {
    title = display_name;
  } else {
    title_owned = g_strdup_printf("%.16s...", pubkey);
    title = title_owned;
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

  /* Build subtitle: nip05 if available, else truncated pubkey */
  gchar *subtitle = NULL;
  if (nip05 && *nip05) {
    subtitle = g_strdup(nip05);
  } else {
    subtitle = g_strdup_printf("%.12s...", pubkey);
  }
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);
  g_free(title_owned);

  /* Avatar - start with default, load async if URL available */
  GtkImage *avatar = GTK_IMAGE(gtk_image_new_from_icon_name("avatar-default-symbolic"));
  gtk_widget_set_size_request(GTK_WIDGET(avatar), 40, 40);
  gtk_widget_add_css_class(GTK_WIDGET(avatar), "avatar");
  adw_action_row_add_prefix(row, GTK_WIDGET(avatar));

  /* Load avatar image asynchronously from URL if available */
  if (avatar_url && *avatar_url) {
    load_avatar_async(avatar, avatar_url);
  }

  /* Remove button */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("user-trash-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "destructive-action");
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_remove));

  /* Store data */
  UserRowData *data = g_new0(UserRowData, 1);
  data->pubkey = g_strdup(pubkey);
  data->petname = g_strdup(petname);
  data->display_name = g_strdup(display_name);
  data->avatar_url = g_strdup(avatar_url);
  data->nip05 = g_strdup(nip05);
  g_object_set_data_full(G_OBJECT(row), "user-data", data, (GDestroyNotify)user_row_data_free);
  g_object_set_data(G_OBJECT(row), "sheet-self", self);

  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_row_remove), row);

  return GTK_WIDGET(row);
}

static void populate_list(SheetUserList *self, const gchar *filter) {
  /* Clear existing */
  while (TRUE) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_users));
    if (!child) break;
    gtk_list_box_remove(self->list_users, child);
  }

  /* Get filtered entries */
  GPtrArray *entries = user_list_store_search(self->store, filter);
  if (!entries) return;

  for (guint i = 0; i < entries->len; i++) {
    UserListEntry *entry = g_ptr_array_index(entries, i);

    /* Try to get profile info from profile store */
    const gchar *display_name = entry->display_name;
    const gchar *avatar_url = entry->avatar_url;
    const gchar *nip05 = entry->nip05;

    /* If we have a profile store, try to look up cached profile */
    if (self->profile_store && (!display_name || !*display_name)) {
      NostrProfile *profile = profile_store_get(self->profile_store, entry->pubkey);
      if (profile) {
        display_name = profile->name;
        avatar_url = profile->picture;
        nip05 = profile->nip05;
        /* Update the user list entry with profile info */
        user_list_store_update_profile(self->store, entry->pubkey,
                                       display_name, avatar_url, nip05);
        nostr_profile_free(profile);
      }
    }

    GtkWidget *row = create_user_row(self, entry->pubkey, entry->petname,
                                     display_name, avatar_url, nip05);
    gtk_list_box_append(self->list_users, row);
  }

  g_ptr_array_unref(entries);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
  SheetUserList *self = user_data;
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  populate_list(self, text && *text ? text : NULL);
}

static gboolean is_valid_hex_pubkey(const gchar *s) {
  if (!s) return FALSE;
  gsize len = strlen(s);
  if (len != 64) return FALSE;
  for (gsize i = 0; i < len; i++) {
    gchar c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

static void on_add_user(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetUserList *self = user_data;

  const gchar *pubkey = gtk_editable_get_text(GTK_EDITABLE(self->entry_pubkey));
  if (!pubkey || !*pubkey) return;

  /* Validate pubkey format */
  if (!is_valid_hex_pubkey(pubkey) && !g_str_has_prefix(pubkey, "npub1")) {
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkAlertDialog *dlg = gtk_alert_dialog_new("Invalid public key. Enter 64-char hex or npub1...");
    gtk_alert_dialog_show(dlg, GTK_WINDOW(root));
    g_object_unref(dlg);
    return;
  }

  /* Check for duplicate */
  if (user_list_store_contains(self->store, pubkey)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_pubkey), "");
    return;
  }

  const gchar *petname = NULL;
  if (self->type == USER_LIST_FOLLOWS && self->entry_petname) {
    petname = gtk_editable_get_text(GTK_EDITABLE(self->entry_petname));
    if (petname && !*petname) petname = NULL;
  }

  /* Add to store and UI */
  user_list_store_add(self->store, pubkey, NULL, petname);

  /* For newly added users, we don't have profile info yet */
  GtkWidget *row = create_user_row(self, pubkey, petname, NULL, NULL, NULL);
  gtk_list_box_append(self->list_users, row);

  /* Clear entries */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_pubkey), "");
  if (self->entry_petname) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_petname), "");
  }

  update_count_label(self);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)entry;
  SheetUserList *self = user_data;
  on_add_user(NULL, self);
}

/* Sync context for async operations */
typedef struct {
  SheetUserList *self;
  GtkWidget *spinner;
  guint timeout_id;
} SyncContext;

static void sync_context_free(SyncContext *ctx) {
  if (!ctx) return;
  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
  }
  g_free(ctx);
}

static void on_sync_complete(gpointer user_data) {
  SyncContext *ctx = user_data;
  if (!ctx || !ctx->self) return;

  SheetUserList *self = ctx->self;

  /* Re-enable sync button and stop spinner */
  if (GTK_IS_BUTTON(self->btn_sync)) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_sync), TRUE);
  }
  if (ctx->spinner && GTK_IS_SPINNER(ctx->spinner)) {
    gtk_spinner_stop(GTK_SPINNER(ctx->spinner));
    gtk_widget_set_visible(ctx->spinner, FALSE);
  }

  /* Mark as synced and refresh the list */
  user_list_store_mark_synced(self->store);
  populate_list(self, NULL);
  update_count_label(self);

  /* Show success feedback */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  if (root) {
    const gchar *list_name = (self->type == USER_LIST_FOLLOWS) ? "follows" : "mutes";
    gchar *msg = g_strdup_printf("Your %s list has been synced.", list_name);
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_show(dlg, GTK_WINDOW(root));
    g_object_unref(dlg);
    g_free(msg);
  }

  sync_context_free(ctx);
}

/* Simulate sync timeout (will be replaced with actual relay response handling) */
static gboolean on_sync_timeout(gpointer user_data) {
  SyncContext *ctx = user_data;
  if (ctx) {
    ctx->timeout_id = 0; /* Mark as handled */
    on_sync_complete(ctx);
  }
  return G_SOURCE_REMOVE;
}

static void on_sync(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetUserList *self = user_data;

  const gchar *list_name = (self->type == USER_LIST_FOLLOWS) ? "follows" : "mutes";
  g_message("Sync requested for %s list", list_name);

  /* Build the fetch filter for logging/debug purposes */
  const gchar *owner = user_list_store_get_owner(self->store);
  gchar *filter = NULL;
  if (owner) {
    filter = user_list_store_build_fetch_filter(self->store, owner);
    g_message("Fetch filter: %s", filter);
  }

  /* Check if we have relays configured */
  RelayStore *relay_store = relay_store_new();
  relay_store_load(relay_store);
  guint relay_count = relay_store_count(relay_store);

  if (relay_count == 0) {
    /* No relays configured - prompt user to add some */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkAlertDialog *dlg = gtk_alert_dialog_new(
      "No relays configured. Please add relays in Settings to sync your %s list.",
      list_name);
    gtk_alert_dialog_show(dlg, GTK_WINDOW(root));
    g_object_unref(dlg);
    relay_store_free(relay_store);
    g_free(filter);
    return;
  }

  /* Create sync context */
  SyncContext *ctx = g_new0(SyncContext, 1);
  ctx->self = self;

  /* Create and show spinner next to button */
  ctx->spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(ctx->spinner));

  /* Disable sync button during sync */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_sync), FALSE);

  g_message("Syncing %s list with %u relays...", list_name, relay_count);

  /* Get write relays for publishing */
  GPtrArray *write_relays = relay_store_get_write_relays(relay_store);

  /* Build the unsigned event JSON for the user list */
  gchar *event_json = user_list_store_build_event_json(self->store);
  g_message("Event to sync: %s", event_json);

  /* In a full implementation, we would:
   * 1. Connect to relays via WebSocket
   * 2. Subscribe with the filter to fetch the current list
   * 3. Merge any new entries from the fetched event
   * 4. Sign and publish our updated list
   *
   * For now, simulate the sync with a timeout to show the UI flow works.
   * The actual WebSocket relay connection will be added when the relay
   * connection infrastructure is complete.
   */

    /* Timeout-audit: Replaced 1.5s simulated delay with idle callback.
     * TODO: Replace with actual relay sync when infrastructure is ready. */
    ctx->timeout_id = g_idle_add(on_sync_timeout, ctx);

  /* Cleanup */
  g_ptr_array_unref(write_relays);
  relay_store_free(relay_store);
  g_free(event_json);
  g_free(filter);
}

static void sheet_user_list_finalize(GObject *obj) {
  SheetUserList *self = SHEET_USER_LIST(obj);
  user_list_store_free(self->store);
  if (self->profile_store) {
    profile_store_free(self->profile_store);
  }
  G_OBJECT_CLASS(sheet_user_list_parent_class)->finalize(obj);
}

static void sheet_user_list_class_init(SheetUserListClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_user_list_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-user-list.ui");
  gtk_widget_class_bind_template_child(wc, SheetUserList, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetUserList, btn_publish);
  gtk_widget_class_bind_template_child(wc, SheetUserList, btn_add);
  gtk_widget_class_bind_template_child(wc, SheetUserList, btn_sync);
  gtk_widget_class_bind_template_child(wc, SheetUserList, entry_pubkey);
  gtk_widget_class_bind_template_child(wc, SheetUserList, entry_petname);
  gtk_widget_class_bind_template_child(wc, SheetUserList, search_entry);
  gtk_widget_class_bind_template_child(wc, SheetUserList, list_users);
  gtk_widget_class_bind_template_child(wc, SheetUserList, lbl_title);
  gtk_widget_class_bind_template_child(wc, SheetUserList, lbl_count);
}

static void sheet_user_list_init(SheetUserList *self) {
  self->type = USER_LIST_FOLLOWS; /* Default, will be set by _new() */
  gtk_widget_init_template(GTK_WIDGET(self));

  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_publish, "clicked", G_CALLBACK(on_publish), self);
  g_signal_connect(self->btn_add, "clicked", G_CALLBACK(on_add_user), self);
  g_signal_connect(self->btn_sync, "clicked", G_CALLBACK(on_sync), self);
  g_signal_connect(self->entry_pubkey, "activate", G_CALLBACK(on_entry_activate), self);
  g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
}

SheetUserList *sheet_user_list_new(UserListType type) {
  SheetUserList *self = g_object_new(TYPE_SHEET_USER_LIST, NULL);
  self->type = type;

  /* Create profile store for caching user info */
  self->profile_store = profile_store_new();

  /* Load store */
  self->store = user_list_store_new(type);
  user_list_store_load(self->store);

  /* Update UI based on type */
  if (self->lbl_title) {
    gtk_label_set_text(self->lbl_title,
      type == USER_LIST_FOLLOWS ? "Manage Follows" : "Manage Mutes");
  }

  /* Hide petname entry for mutes */
  if (type == USER_LIST_MUTES && self->entry_petname) {
    gtk_widget_set_visible(GTK_WIDGET(self->entry_petname), FALSE);
  }

  update_count_label(self);
  populate_list(self, NULL);

  return self;
}

void sheet_user_list_set_on_publish(SheetUserList *self,
                                    SheetUserListSaveCb cb,
                                    gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->on_publish = cb;
  self->on_publish_ud = user_data;
}

UserListStore *sheet_user_list_get_store(SheetUserList *self) {
  g_return_val_if_fail(self != NULL, NULL);
  return self->store;
}

void sheet_user_list_refresh(SheetUserList *self) {
  g_return_if_fail(self != NULL);
  update_count_label(self);
  /* Re-populate with current search filter */
  const gchar *filter = NULL;
  if (self->search_entry) {
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    if (text && *text) filter = text;
  }
  populate_list(self, filter);
}

void sheet_user_list_update_user_profile(SheetUserList *self,
                                         const gchar *pubkey,
                                         const gchar *display_name,
                                         const gchar *avatar_url,
                                         const gchar *nip05) {
  g_return_if_fail(self != NULL);
  g_return_if_fail(pubkey != NULL);

  /* Update the underlying store */
  user_list_store_update_profile(self->store, pubkey, display_name, avatar_url, nip05);

  /* Find and update the row in the UI */
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self->list_users));
       child != NULL;
       child = gtk_widget_get_next_sibling(child)) {
    if (!ADW_IS_ACTION_ROW(child)) continue;

    UserRowData *data = g_object_get_data(G_OBJECT(child), "user-data");
    if (!data || g_strcmp0(data->pubkey, pubkey) != 0) continue;

    /* Update the row data */
    g_free(data->display_name);
    data->display_name = g_strdup(display_name);
    g_free(data->avatar_url);
    data->avatar_url = g_strdup(avatar_url);
    g_free(data->nip05);
    data->nip05 = g_strdup(nip05);

    /* Update row display */
    AdwActionRow *row = ADW_ACTION_ROW(child);

    /* Determine new title */
    const gchar *title = data->petname;
    if (!title || !*title) title = display_name;
    gchar *title_owned = NULL;
    if (!title || !*title) {
      title_owned = g_strdup_printf("%.16s...", pubkey);
      title = title_owned;
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_free(title_owned);

    /* Update subtitle */
    gchar *subtitle = NULL;
    if (nip05 && *nip05) {
      subtitle = g_strdup(nip05);
    } else {
      subtitle = g_strdup_printf("%.12s...", pubkey);
    }
    adw_action_row_set_subtitle(row, subtitle);
    g_free(subtitle);

    break;
  }
}
