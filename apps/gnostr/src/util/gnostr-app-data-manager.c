/**
 * GnostrAppDataManager Implementation
 *
 * High-level manager for NIP-78 app-specific data sync.
 * Coordinates preferences, mutes, bookmarks, and drafts sync.
 */

#include "gnostr-app-data-manager.h"
#include "nip78_app_data.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/gnostr-mute-list.h>
#include "bookmarks.h"
#include "pin_list.h"
#include "gnostr-drafts.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* GSettings schema IDs */
#define CLIENT_SCHEMA_ID "org.gnostr.Client"
#define DISPLAY_SCHEMA_ID "org.gnostr.Display"

/* Settings version for migration support */
#define PREFERENCES_VERSION 1

/* ---- Private Structure ---- */

struct _GnostrAppDataManager {
    GObject parent_instance;

    /* Configuration */
    char *user_pubkey;
    gboolean sync_enabled;

    /* State */
    GnostrAppDataSyncStatus sync_status;
    gint pending_sync_ops;

    /* Timestamps for last sync */
    gint64 last_sync_preferences;
    gint64 last_sync_mutes;
    gint64 last_sync_bookmarks;
    gint64 last_sync_drafts;

    /* GSettings instances */
    GSettings *client_settings;
    GSettings *display_settings;

    /* Thread safety */
    GMutex lock;
};

G_DEFINE_TYPE(GnostrAppDataManager, gnostr_app_data_manager, G_TYPE_OBJECT)

/* Signals */
enum {
    SIGNAL_SYNC_STARTED,
    SIGNAL_SYNC_COMPLETED,
    SIGNAL_PREFERENCES_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* Singleton */
static GnostrAppDataManager *s_default_instance = NULL;
static GMutex s_singleton_lock;

/* ---- Private Helpers ---- */

static void ensure_settings(GnostrAppDataManager *self) {
    if (!self->client_settings) {
        GSettingsSchemaSource *src = g_settings_schema_source_get_default();
        if (src) {
            GSettingsSchema *schema = g_settings_schema_source_lookup(src, CLIENT_SCHEMA_ID, TRUE);
            if (schema) {
                self->client_settings = g_settings_new(CLIENT_SCHEMA_ID);
                g_settings_schema_unref(schema);
            }
        }
    }
    if (!self->display_settings) {
        GSettingsSchemaSource *src = g_settings_schema_source_get_default();
        if (src) {
            GSettingsSchema *schema = g_settings_schema_source_lookup(src, DISPLAY_SCHEMA_ID, TRUE);
            if (schema) {
                self->display_settings = g_settings_new(DISPLAY_SCHEMA_ID);
                g_settings_schema_unref(schema);
            }
        }
    }
}

/* ---- GObject Lifecycle ---- */

static void gnostr_app_data_manager_finalize(GObject *object) {
    GnostrAppDataManager *self = GNOSTR_APP_DATA_MANAGER(object);

    g_mutex_lock(&self->lock);
    g_clear_pointer(&self->user_pubkey, g_free);
    g_clear_object(&self->client_settings);
    g_clear_object(&self->display_settings);
    g_mutex_unlock(&self->lock);
    g_mutex_clear(&self->lock);

    G_OBJECT_CLASS(gnostr_app_data_manager_parent_class)->finalize(object);
}

static void gnostr_app_data_manager_class_init(GnostrAppDataManagerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_app_data_manager_finalize;

    /* Define signals */
    signals[SIGNAL_SYNC_STARTED] = g_signal_new(
        "sync-started",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING
    );

    signals[SIGNAL_SYNC_COMPLETED] = g_signal_new(
        "sync-completed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN
    );

    signals[SIGNAL_PREFERENCES_CHANGED] = g_signal_new(
        "preferences-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0
    );
}

static void gnostr_app_data_manager_init(GnostrAppDataManager *self) {
    g_mutex_init(&self->lock);
    self->user_pubkey = NULL;
    self->sync_enabled = TRUE;
    self->sync_status = GNOSTR_APP_DATA_SYNC_IDLE;
    self->pending_sync_ops = 0;
    self->last_sync_preferences = 0;
    self->last_sync_mutes = 0;
    self->last_sync_bookmarks = 0;
    self->last_sync_drafts = 0;
    self->client_settings = NULL;
    self->display_settings = NULL;
}

/* ---- Singleton Access ---- */

GnostrAppDataManager *gnostr_app_data_manager_get_default(void) {
    g_mutex_lock(&s_singleton_lock);
    if (!s_default_instance) {
        s_default_instance = g_object_new(GNOSTR_TYPE_APP_DATA_MANAGER, NULL);
    }
    g_mutex_unlock(&s_singleton_lock);
    return s_default_instance;
}

void gnostr_app_data_manager_shutdown(void) {
    g_mutex_lock(&s_singleton_lock);
    if (s_default_instance) {
        g_object_unref(s_default_instance);
        s_default_instance = NULL;
    }
    g_mutex_unlock(&s_singleton_lock);
}

/* ---- Configuration ---- */

void gnostr_app_data_manager_set_user_pubkey(GnostrAppDataManager *self,
                                              const char *pubkey_hex) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    g_mutex_lock(&self->lock);
    g_free(self->user_pubkey);
    self->user_pubkey = g_strdup(pubkey_hex);
    g_mutex_unlock(&self->lock);
}

const char *gnostr_app_data_manager_get_user_pubkey(GnostrAppDataManager *self) {
    g_return_val_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self), NULL);
    return self->user_pubkey;
}

void gnostr_app_data_manager_set_sync_enabled(GnostrAppDataManager *self,
                                               gboolean enabled) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    g_mutex_lock(&self->lock);
    self->sync_enabled = enabled;
    g_mutex_unlock(&self->lock);
}

gboolean gnostr_app_data_manager_is_sync_enabled(GnostrAppDataManager *self) {
    g_return_val_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self), FALSE);
    return self->sync_enabled;
}

/* ---- Sync Status ---- */

GnostrAppDataSyncStatus gnostr_app_data_manager_get_sync_status(GnostrAppDataManager *self) {
    g_return_val_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self), GNOSTR_APP_DATA_SYNC_IDLE);
    return self->sync_status;
}

gint64 gnostr_app_data_manager_get_last_sync_time(GnostrAppDataManager *self,
                                                   const char *data_key) {
    g_return_val_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self), 0);
    if (!data_key) return 0;

    g_mutex_lock(&self->lock);
    gint64 result = 0;
    if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_PREFERENCES) == 0) {
        result = self->last_sync_preferences;
    } else if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_MUTES) == 0) {
        result = self->last_sync_mutes;
    } else if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_BOOKMARKS) == 0) {
        result = self->last_sync_bookmarks;
    } else if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_DRAFTS) == 0) {
        result = self->last_sync_drafts;
    }
    g_mutex_unlock(&self->lock);
    return result;
}

gboolean gnostr_app_data_manager_is_syncing(GnostrAppDataManager *self) {
    g_return_val_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self), FALSE);
    return self->sync_status == GNOSTR_APP_DATA_SYNC_LOADING ||
           self->sync_status == GNOSTR_APP_DATA_SYNC_SAVING;
}

/* ---- Preferences JSON Building ---- */

char *gnostr_app_data_manager_build_preferences_json(GnostrAppDataManager *self) {
    g_return_val_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self), NULL);

    ensure_settings(self);

    g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
    gnostr_json_builder_begin_object(builder);

    /* Version for migration */
    gnostr_json_builder_set_key(builder, "version");
    gnostr_json_builder_add_int(builder, PREFERENCES_VERSION);

    /* Client settings */
    gnostr_json_builder_set_key(builder, "client");
    gnostr_json_builder_begin_object(builder);
    if (self->client_settings) {
        g_autofree char *blossom_server = g_settings_get_string(self->client_settings, "blossom-server");
        if (blossom_server) {
            gnostr_json_builder_set_key(builder, "blossom-server");
            gnostr_json_builder_add_string(builder, blossom_server);
        }

        gnostr_json_builder_set_key(builder, "video-autoplay");
        gnostr_json_builder_add_boolean(builder, g_settings_get_boolean(self->client_settings, "video-autoplay"));

        gnostr_json_builder_set_key(builder, "video-loop");
        gnostr_json_builder_add_boolean(builder, g_settings_get_boolean(self->client_settings, "video-loop"));

        g_autofree char *image_quality = g_settings_get_string(self->client_settings, "image-quality");
        if (image_quality) {
            gnostr_json_builder_set_key(builder, "image-quality");
            gnostr_json_builder_add_string(builder, image_quality);
        }
    }
    gnostr_json_builder_end_object(builder);

    /* Display settings */
    gnostr_json_builder_set_key(builder, "display");
    gnostr_json_builder_begin_object(builder);
    if (self->display_settings) {
        g_autofree char *color_scheme = g_settings_get_string(self->display_settings, "color-scheme");
        if (color_scheme) {
            gnostr_json_builder_set_key(builder, "color-scheme");
            gnostr_json_builder_add_string(builder, color_scheme);
        }

        gnostr_json_builder_set_key(builder, "font-scale");
        gnostr_json_builder_add_double(builder, g_settings_get_double(self->display_settings, "font-scale"));

        g_autofree char *density = g_settings_get_string(self->display_settings, "timeline-density");
        if (density) {
            gnostr_json_builder_set_key(builder, "timeline-density");
            gnostr_json_builder_add_string(builder, density);
        }

        gnostr_json_builder_set_key(builder, "enable-animations");
        gnostr_json_builder_add_boolean(builder, g_settings_get_boolean(self->display_settings, "enable-animations"));

        gnostr_json_builder_set_key(builder, "show-avatars");
        gnostr_json_builder_add_boolean(builder, g_settings_get_boolean(self->display_settings, "show-avatars"));

        gnostr_json_builder_set_key(builder, "show-media-previews");
        gnostr_json_builder_add_boolean(builder, g_settings_get_boolean(self->display_settings, "show-media-previews"));
    }
    gnostr_json_builder_end_object(builder);

    /* Timestamp */
    gnostr_json_builder_set_key(builder, "updated_at");
    gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

    gnostr_json_builder_end_object(builder);

    char *result = gnostr_json_builder_finish(builder);

    return result;
}

gboolean gnostr_app_data_manager_apply_preferences_json(GnostrAppDataManager *self,
                                                         const char *json_str) {
    g_return_val_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self), FALSE);
    if (!json_str || !*json_str) return FALSE;

    ensure_settings(self);

    if (!gnostr_json_is_valid(json_str)) {
        g_warning("app-data-manager: failed to parse preferences JSON");
        return FALSE;
    }

    /* Get raw client and display objects for nested lookups */
    char *client_json = NULL;
    char *display_json = NULL;
    client_json = gnostr_json_get_raw(json_str, "client", NULL);
    display_json = gnostr_json_get_raw(json_str, "display", NULL);

    /* Apply client settings */
    if (client_json && self->client_settings) {
        char *str_val = NULL;
        bool bool_val;

        str_val = gnostr_json_get_string(client_json, "blossom-server", NULL);
        if (str_val) {
            g_settings_set_string(self->client_settings, "blossom-server", str_val);
            free(str_val);
        }

        bool_val = gnostr_json_get_boolean(client_json, "video-autoplay", NULL);
        {
            g_settings_set_boolean(self->client_settings, "video-autoplay", bool_val);
        }

        bool_val = gnostr_json_get_boolean(client_json, "video-loop", NULL);
        {
            g_settings_set_boolean(self->client_settings, "video-loop", bool_val);
        }

        str_val = gnostr_json_get_string(client_json, "image-quality", NULL);
        if (str_val) {
            g_settings_set_string(self->client_settings, "image-quality", str_val);
            free(str_val);
        }
    }

    /* Apply display settings */
    if (display_json && self->display_settings) {
        char *str_val = NULL;
        bool bool_val;
        int int_val;

        str_val = gnostr_json_get_string(display_json, "color-scheme", NULL);
        if (str_val) {
            g_settings_set_string(self->display_settings, "color-scheme", str_val);
            free(str_val);
        }

        /* font-scale is a double - use int getter and convert, or get raw */
        char *font_scale_raw = NULL;
        if ((font_scale_raw = gnostr_json_get_raw(display_json, "font-scale", NULL)) != NULL) {
            double font_scale = strtod(font_scale_raw, NULL);
            g_settings_set_double(self->display_settings, "font-scale", font_scale);
            free(font_scale_raw);
        }

        str_val = gnostr_json_get_string(display_json, "timeline-density", NULL);
        if (str_val) {
            g_settings_set_string(self->display_settings, "timeline-density", str_val);
            free(str_val);
        }

        bool_val = gnostr_json_get_boolean(display_json, "enable-animations", NULL);
        {
            g_settings_set_boolean(self->display_settings, "enable-animations", bool_val);
        }

        bool_val = gnostr_json_get_boolean(display_json, "show-avatars", NULL);
        {
            g_settings_set_boolean(self->display_settings, "show-avatars", bool_val);
        }

        bool_val = gnostr_json_get_boolean(display_json, "show-media-previews", NULL);
        {
            g_settings_set_boolean(self->display_settings, "show-media-previews", bool_val);
        }
    }

    free(client_json);
    free(display_json);

    g_message("app-data-manager: applied preferences from JSON");
    g_signal_emit(self, signals[SIGNAL_PREFERENCES_CHANGED], 0);

    return TRUE;
}

/* ---- Preferences Sync ---- */

typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataPreferencesCallback callback;
    gpointer user_data;
} PreferencesLoadContext;

static void on_preferences_fetch_done(GnostrAppData *data,
                                       gboolean success,
                                       const char *error_msg,
                                       gpointer user_data) {
    PreferencesLoadContext *ctx = (PreferencesLoadContext *)user_data;
    if (!ctx) return;

    GnostrAppDataManager *self = ctx->manager;

    if (success && data && data->content) {
        /* Check timestamp - only apply if remote is newer */
        gint64 local_time = self->last_sync_preferences;

        if (data->created_at > local_time) {
            if (gnostr_app_data_manager_apply_preferences_json(self, data->content)) {
                g_mutex_lock(&self->lock);
                self->last_sync_preferences = data->created_at;
                g_mutex_unlock(&self->lock);
                g_message("app-data-manager: loaded preferences from relay (timestamp=%lld)",
                          (long long)data->created_at);
            }
        } else {
            g_debug("app-data-manager: remote preferences older than local, skipping");
        }
    }

    self->sync_status = success ? GNOSTR_APP_DATA_SYNC_COMPLETE : GNOSTR_APP_DATA_SYNC_ERROR;
    g_signal_emit(self, signals[SIGNAL_SYNC_COMPLETED], 0,
                  GNOSTR_APP_DATA_KEY_PREFERENCES, success);

    if (ctx->callback) {
        ctx->callback(self, success, error_msg, ctx->user_data);
    }

    gnostr_app_data_free(data);
    g_free(ctx);
}

void gnostr_app_data_manager_load_preferences_async(GnostrAppDataManager *self,
                                                     GnostrAppDataPreferencesCallback callback,
                                                     gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    if (!self->user_pubkey || !*self->user_pubkey) {
        if (callback) callback(self, FALSE, "User pubkey not set", user_data);
        return;
    }

    PreferencesLoadContext *ctx = g_new0(PreferencesLoadContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    self->sync_status = GNOSTR_APP_DATA_SYNC_LOADING;
    g_signal_emit(self, signals[SIGNAL_SYNC_STARTED], 0, GNOSTR_APP_DATA_KEY_PREFERENCES);

    gnostr_app_data_fetch_async(
        self->user_pubkey,
        GNOSTR_APP_DATA_APP_ID,
        GNOSTR_APP_DATA_KEY_PREFERENCES,
        on_preferences_fetch_done,
        ctx
    );
}

typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataPreferencesCallback callback;
    gpointer user_data;
} PreferencesSaveContext;

static void on_preferences_publish_done(gboolean success,
                                         const char *error_msg,
                                         gpointer user_data) {
    PreferencesSaveContext *ctx = (PreferencesSaveContext *)user_data;
    if (!ctx) return;

    GnostrAppDataManager *self = ctx->manager;

    if (success) {
        g_mutex_lock(&self->lock);
        self->last_sync_preferences = (gint64)time(NULL);
        g_mutex_unlock(&self->lock);
        g_message("app-data-manager: saved preferences to relays");
    }

    self->sync_status = success ? GNOSTR_APP_DATA_SYNC_COMPLETE : GNOSTR_APP_DATA_SYNC_ERROR;
    g_signal_emit(self, signals[SIGNAL_SYNC_COMPLETED], 0,
                  GNOSTR_APP_DATA_KEY_PREFERENCES, success);

    if (ctx->callback) {
        ctx->callback(self, success, error_msg, ctx->user_data);
    }

    g_free(ctx);
}

void gnostr_app_data_manager_save_preferences_async(GnostrAppDataManager *self,
                                                     GnostrAppDataPreferencesCallback callback,
                                                     gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    char *content = gnostr_app_data_manager_build_preferences_json(self);
    if (!content) {
        if (callback) callback(self, FALSE, "Failed to build preferences JSON", user_data);
        return;
    }

    PreferencesSaveContext *ctx = g_new0(PreferencesSaveContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    self->sync_status = GNOSTR_APP_DATA_SYNC_SAVING;
    g_signal_emit(self, signals[SIGNAL_SYNC_STARTED], 0, GNOSTR_APP_DATA_KEY_PREFERENCES);

    gnostr_app_data_publish_async(
        GNOSTR_APP_DATA_APP_ID,
        GNOSTR_APP_DATA_KEY_PREFERENCES,
        content,
        on_preferences_publish_done,
        ctx
    );

    g_free(content);
}

/* ---- Full Sync ---- */

typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataManagerCallback callback;
    gpointer user_data;
    gint pending;
    gboolean any_failed;
} SyncAllContext;

static void on_sync_all_item_done(GnostrAppDataManager *manager,
                                   gboolean success,
                                   const char *error_msg,
                                   gpointer user_data) {
    SyncAllContext *ctx = (SyncAllContext *)user_data;
    (void)manager;
    (void)error_msg;
    if (!ctx) return;

    if (!success) ctx->any_failed = TRUE;

    ctx->pending--;
    if (ctx->pending <= 0) {
        GnostrAppDataManager *self = ctx->manager;
        self->sync_status = ctx->any_failed ?
            GNOSTR_APP_DATA_SYNC_ERROR : GNOSTR_APP_DATA_SYNC_COMPLETE;

        g_signal_emit(self, signals[SIGNAL_SYNC_COMPLETED], 0, NULL, !ctx->any_failed);

        if (ctx->callback) {
            ctx->callback(self, !ctx->any_failed,
                         ctx->any_failed ? "Some sync operations failed" : NULL,
                         ctx->user_data);
        }

        g_free(ctx);
    }
}

void gnostr_app_data_manager_sync_all_async(GnostrAppDataManager *self,
                                             GnostrAppDataManagerCallback callback,
                                             gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    if (!self->user_pubkey || !*self->user_pubkey) {
        if (callback) callback(self, FALSE, "User pubkey not set", user_data);
        return;
    }

    if (!self->sync_enabled) {
        if (callback) callback(self, FALSE, "Sync disabled", user_data);
        return;
    }

    SyncAllContext *ctx = g_new0(SyncAllContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->pending = 1; /* Start with preferences */
    ctx->any_failed = FALSE;

    self->sync_status = GNOSTR_APP_DATA_SYNC_LOADING;
    g_signal_emit(self, signals[SIGNAL_SYNC_STARTED], 0, NULL);

    /* Load preferences - other data types are synced via their own modules */
    gnostr_app_data_manager_load_preferences_async(self,
        (GnostrAppDataPreferencesCallback)on_sync_all_item_done, ctx);

    /* Also trigger sync on mutes, bookmarks, pins via their own APIs */
    gnostr_bookmarks_sync_on_login(self->user_pubkey);
    gnostr_pin_list_sync_on_login(self->user_pubkey);
}

void gnostr_app_data_manager_sync_on_login(const char *pubkey_hex) {
    if (!pubkey_hex || !*pubkey_hex) return;

    GnostrAppDataManager *manager = gnostr_app_data_manager_get_default();
    gnostr_app_data_manager_set_user_pubkey(manager, pubkey_hex);

    if (!gnostr_app_data_manager_is_sync_enabled(manager)) {
        g_debug("app-data-manager: sync disabled, skipping auto-sync");
        return;
    }

    g_message("app-data-manager: starting sync on login for %.8s...", pubkey_hex);
    gnostr_app_data_manager_sync_all_async(manager, NULL, NULL);
}

/* ---- Individual Data Type Sync ---- */

/* Helper: convert app-data merge strategy to mute-list merge strategy */
static GNostrMuteListMergeStrategy
app_strategy_to_mute_strategy(GnostrAppDataMergeStrategy strategy) {
    switch (strategy) {
    case GNOSTR_APP_DATA_MERGE_LOCAL_WINS:
        return GNOSTR_MUTE_LIST_MERGE_LOCAL_WINS;
    case GNOSTR_APP_DATA_MERGE_UNION:
        return GNOSTR_MUTE_LIST_MERGE_UNION;
    case GNOSTR_APP_DATA_MERGE_LATEST:
        return GNOSTR_MUTE_LIST_MERGE_LATEST;
    case GNOSTR_APP_DATA_MERGE_REMOTE_WINS:
    default:
        return GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS;
    }
}

/* Context for mute sync callback */
typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataManagerCallback callback;
    gpointer user_data;
} MuteSyncContext;

static void on_mute_sync_done(GNostrMuteList *mute_list,
                               gboolean success,
                               gpointer user_data) {
    (void)mute_list;
    MuteSyncContext *ctx = (MuteSyncContext *)user_data;
    if (!ctx) return;

    if (ctx->callback) {
        ctx->callback(ctx->manager, success,
                      success ? NULL : "Mute list sync failed",
                      ctx->user_data);
    }

    g_free(ctx);
}

void gnostr_app_data_manager_sync_mutes_async(GnostrAppDataManager *self,
                                               GnostrAppDataMergeStrategy strategy,
                                               GnostrAppDataManagerCallback callback,
                                               gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    if (!self->user_pubkey || !*self->user_pubkey) {
        g_warning("app-data-manager: cannot sync mutes - user pubkey not set");
        if (callback) callback(self, FALSE, "User pubkey not set", user_data);
        return;
    }

    g_message("app-data-manager: mutes sync via NIP-51 mute list (strategy=%d)", strategy);

    MuteSyncContext *ctx = g_new0(MuteSyncContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    gnostr_mute_list_fetch_with_strategy_async(
        gnostr_mute_list_get_default(),
        self->user_pubkey,
        NULL,  /* use default relays */
        app_strategy_to_mute_strategy(strategy),
        on_mute_sync_done,
        ctx
    );
}

/* Helper: convert app-data merge strategy to bookmarks merge strategy */
static GnostrBookmarksMergeStrategy
app_strategy_to_bookmarks_strategy(GnostrAppDataMergeStrategy strategy) {
    switch (strategy) {
    case GNOSTR_APP_DATA_MERGE_LOCAL_WINS:
        return GNOSTR_BOOKMARKS_MERGE_LOCAL_WINS;
    case GNOSTR_APP_DATA_MERGE_UNION:
        return GNOSTR_BOOKMARKS_MERGE_UNION;
    case GNOSTR_APP_DATA_MERGE_LATEST:
        return GNOSTR_BOOKMARKS_MERGE_LATEST;
    case GNOSTR_APP_DATA_MERGE_REMOTE_WINS:
    default:
        return GNOSTR_BOOKMARKS_MERGE_REMOTE_WINS;
    }
}

/* Helper: convert app-data merge strategy to drafts merge strategy */
static GnostrDraftsMergeStrategy
app_strategy_to_drafts_strategy(GnostrAppDataMergeStrategy strategy) {
    switch (strategy) {
    case GNOSTR_APP_DATA_MERGE_LOCAL_WINS:
        return GNOSTR_DRAFTS_MERGE_LOCAL_WINS;
    case GNOSTR_APP_DATA_MERGE_UNION:
        return GNOSTR_DRAFTS_MERGE_UNION;
    case GNOSTR_APP_DATA_MERGE_LATEST:
        return GNOSTR_DRAFTS_MERGE_LATEST;
    case GNOSTR_APP_DATA_MERGE_REMOTE_WINS:
    default:
        return GNOSTR_DRAFTS_MERGE_REMOTE_WINS;
    }
}

/* Context for bookmarks sync callback */
typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataManagerCallback callback;
    gpointer user_data;
} BookmarksSyncContext;

static void on_bookmarks_sync_done(GnostrBookmarks *bookmarks,
                                    gboolean success,
                                    gpointer user_data) {
    (void)bookmarks;
    BookmarksSyncContext *ctx = (BookmarksSyncContext *)user_data;
    if (!ctx) return;

    if (ctx->callback) {
        ctx->callback(ctx->manager, success,
                      success ? NULL : "Bookmarks sync failed",
                      ctx->user_data);
    }

    g_free(ctx);
}

void gnostr_app_data_manager_sync_bookmarks_async(GnostrAppDataManager *self,
                                                   GnostrAppDataMergeStrategy strategy,
                                                   GnostrAppDataManagerCallback callback,
                                                   gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    if (!self->user_pubkey || !*self->user_pubkey) {
        g_warning("app-data-manager: cannot sync bookmarks - user pubkey not set");
        if (callback) callback(self, FALSE, "User pubkey not set", user_data);
        return;
    }

    g_message("app-data-manager: bookmarks sync via NIP-51 (strategy=%d)", strategy);

    BookmarksSyncContext *ctx = g_new0(BookmarksSyncContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    gnostr_bookmarks_fetch_with_strategy_async(
        gnostr_bookmarks_get_default(),
        self->user_pubkey,
        NULL,  /* use default relays */
        app_strategy_to_bookmarks_strategy(strategy),
        on_bookmarks_sync_done,
        ctx
    );
}

/* Context for drafts sync callback */
typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataManagerCallback callback;
    gpointer user_data;
} DraftsSyncContext;

static void on_drafts_sync_done(GnostrDrafts *drafts,
                                 GPtrArray *draft_list,
                                 GError *error,
                                 gpointer user_data) {
    (void)drafts;
    DraftsSyncContext *ctx = (DraftsSyncContext *)user_data;
    if (!ctx) return;

    gboolean success = (error == NULL);

    if (ctx->callback) {
        ctx->callback(ctx->manager, success,
                      error ? error->message : NULL,
                      ctx->user_data);
    }

    if (draft_list) g_ptr_array_unref(draft_list);
    g_free(ctx);
}

void gnostr_app_data_manager_sync_drafts_async(GnostrAppDataManager *self,
                                                GnostrAppDataMergeStrategy strategy,
                                                GnostrAppDataManagerCallback callback,
                                                gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    if (!self->user_pubkey || !*self->user_pubkey) {
        g_warning("app-data-manager: cannot sync drafts - user pubkey not set");
        if (callback) callback(self, FALSE, "User pubkey not set", user_data);
        return;
    }

    g_message("app-data-manager: drafts sync via NIP-37 (strategy=%d)", strategy);

    /* Set user pubkey on drafts manager */
    GnostrDrafts *drafts = gnostr_drafts_get_default();
    gnostr_drafts_set_user_pubkey(drafts, self->user_pubkey);

    DraftsSyncContext *ctx = g_new0(DraftsSyncContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    gnostr_drafts_load_with_strategy_async(
        drafts,
        app_strategy_to_drafts_strategy(strategy),
        on_drafts_sync_done,
        ctx
    );
}

/* ---- Custom App Data ---- */

typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataGetCallback callback;
    gpointer user_data;
} GetCustomDataContext;

static void on_custom_data_fetch_done(GnostrAppData *data,
                                       gboolean success,
                                       const char *error_msg,
                                       gpointer user_data) {
    GetCustomDataContext *ctx = (GetCustomDataContext *)user_data;
    if (!ctx) return;

    if (ctx->callback) {
        ctx->callback(ctx->manager,
                     data ? data->content : NULL,
                     data ? data->created_at : 0,
                     success, error_msg, ctx->user_data);
    }

    gnostr_app_data_free(data);
    g_free(ctx);
}

void gnostr_app_data_manager_get_custom_data_async(GnostrAppDataManager *self,
                                                    const char *data_key,
                                                    GnostrAppDataGetCallback callback,
                                                    gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    if (!self->user_pubkey || !*self->user_pubkey) {
        if (callback) callback(self, NULL, 0, FALSE, "User pubkey not set", user_data);
        return;
    }

    GetCustomDataContext *ctx = g_new0(GetCustomDataContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    gnostr_app_data_fetch_async(
        self->user_pubkey,
        GNOSTR_APP_DATA_APP_ID,
        data_key,
        on_custom_data_fetch_done,
        ctx
    );
}

typedef struct {
    GnostrAppDataManager *manager;
    GnostrAppDataManagerCallback callback;
    gpointer user_data;
} SetCustomDataContext;

static void on_custom_data_publish_done(gboolean success,
                                         const char *error_msg,
                                         gpointer user_data) {
    SetCustomDataContext *ctx = (SetCustomDataContext *)user_data;
    if (!ctx) return;

    if (ctx->callback) {
        ctx->callback(ctx->manager, success, error_msg, ctx->user_data);
    }

    g_free(ctx);
}

void gnostr_app_data_manager_set_custom_data_async(GnostrAppDataManager *self,
                                                    const char *data_key,
                                                    const char *content,
                                                    GnostrAppDataManagerCallback callback,
                                                    gpointer user_data) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    SetCustomDataContext *ctx = g_new0(SetCustomDataContext, 1);
    ctx->manager = self;
    ctx->callback = callback;
    ctx->user_data = user_data;

    gnostr_app_data_publish_async(
        GNOSTR_APP_DATA_APP_ID,
        data_key,
        content,
        on_custom_data_publish_done,
        ctx
    );
}

/* ---- Utility ---- */

void gnostr_app_data_manager_clear_local_cache(GnostrAppDataManager *self,
                                                const char *data_key) {
    g_return_if_fail(GNOSTR_IS_APP_DATA_MANAGER(self));

    g_mutex_lock(&self->lock);

    if (!data_key) {
        /* Clear all */
        self->last_sync_preferences = 0;
        self->last_sync_mutes = 0;
        self->last_sync_bookmarks = 0;
        self->last_sync_drafts = 0;
    } else if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_PREFERENCES) == 0) {
        self->last_sync_preferences = 0;
    } else if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_MUTES) == 0) {
        self->last_sync_mutes = 0;
    } else if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_BOOKMARKS) == 0) {
        self->last_sync_bookmarks = 0;
    } else if (g_strcmp0(data_key, GNOSTR_APP_DATA_KEY_DRAFTS) == 0) {
        self->last_sync_drafts = 0;
    }

    g_mutex_unlock(&self->lock);

    g_message("app-data-manager: cleared cache for %s", data_key ? data_key : "all");
}
