/**
 * gnostr NIP-51 Settings Sync Implementation
 *
 * Synchronizes application settings via kind 30078 (NIP-78 application-specific data).
 * Settings are stored as JSON in the event content with d-tag "gnostr/settings".
 */

#include "nip51_settings.h"
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "blossom_settings.h"
#include "../ipc/gnostr-signer-service.h"
#include <json-glib/json-glib.h>
#include <time.h>

#ifndef GNOSTR_NIP51_TEST_ONLY
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#endif

/* GSettings schema IDs */
#define CLIENT_SCHEMA_ID "org.gnostr.Client"
#define DISPLAY_SCHEMA_ID "org.gnostr.Display"

/* Settings version for migration support */
#define SETTINGS_VERSION 1

/* Cached GSettings instances */
static GSettings *s_client_settings = NULL;
static GSettings *s_display_settings = NULL;

static void ensure_settings(void) {
  if (!s_client_settings) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (src) {
      GSettingsSchema *schema = g_settings_schema_source_lookup(src, CLIENT_SCHEMA_ID, TRUE);
      if (schema) {
        s_client_settings = g_settings_new(CLIENT_SCHEMA_ID);
        g_settings_schema_unref(schema);
      }
    }
  }
  if (!s_display_settings) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    if (src) {
      GSettingsSchema *schema = g_settings_schema_source_lookup(src, DISPLAY_SCHEMA_ID, TRUE);
      if (schema) {
        s_display_settings = g_settings_new(DISPLAY_SCHEMA_ID);
        g_settings_schema_unref(schema);
      }
    }
  }
}

gboolean gnostr_nip51_settings_sync_enabled(void) {
  ensure_settings();
  if (!s_client_settings) return FALSE;
  return g_settings_get_boolean(s_client_settings, "nip51-sync-enabled");
}

void gnostr_nip51_settings_set_sync_enabled(gboolean enabled) {
  ensure_settings();
  if (s_client_settings) {
    g_settings_set_boolean(s_client_settings, "nip51-sync-enabled", enabled);
  }
}

gint64 gnostr_nip51_settings_last_sync(void) {
  ensure_settings();
  if (!s_client_settings) return 0;
  return g_settings_get_int64(s_client_settings, "nip51-last-sync");
}

static void set_last_sync(gint64 timestamp) {
  ensure_settings();
  if (s_client_settings) {
    g_settings_set_int64(s_client_settings, "nip51-last-sync", timestamp);
  }
}

gchar *gnostr_nip51_settings_build_event_json(void) {
  ensure_settings();

  g_autoptr(JsonBuilder) builder = json_builder_new();

  /* Build the content object containing all settings */
  json_builder_begin_object(builder);

  /* Version for future migration */
  json_builder_set_member_name(builder, "version");
  json_builder_add_int_value(builder, SETTINGS_VERSION);

  /* Client settings */
  json_builder_set_member_name(builder, "client");
  json_builder_begin_object(builder);

  if (s_client_settings) {
    /* Blossom server */
    g_autofree gchar *blossom_server = g_settings_get_string(s_client_settings, "blossom-server");
    json_builder_set_member_name(builder, "blossom-server");
    json_builder_add_string_value(builder, blossom_server ? blossom_server : "");

    /* Video settings */
    json_builder_set_member_name(builder, "video-autoplay");
    json_builder_add_boolean_value(builder, g_settings_get_boolean(s_client_settings, "video-autoplay"));

    json_builder_set_member_name(builder, "video-loop");
    json_builder_add_boolean_value(builder, g_settings_get_boolean(s_client_settings, "video-loop"));

    /* Image quality */
    g_autofree gchar *image_quality = g_settings_get_string(s_client_settings, "image-quality");
    json_builder_set_member_name(builder, "image-quality");
    json_builder_add_string_value(builder, image_quality ? image_quality : "auto");
  }

  json_builder_end_object(builder); /* client */

  /* Display settings */
  json_builder_set_member_name(builder, "display");
  json_builder_begin_object(builder);

  if (s_display_settings) {
    /* Color scheme */
    g_autofree gchar *color_scheme = g_settings_get_string(s_display_settings, "color-scheme");
    json_builder_set_member_name(builder, "color-scheme");
    json_builder_add_string_value(builder, color_scheme ? color_scheme : "system");

    /* Font scale */
    json_builder_set_member_name(builder, "font-scale");
    json_builder_add_double_value(builder, g_settings_get_double(s_display_settings, "font-scale"));

    /* Timeline density */
    g_autofree gchar *density = g_settings_get_string(s_display_settings, "timeline-density");
    json_builder_set_member_name(builder, "timeline-density");
    json_builder_add_string_value(builder, density ? density : "normal");

    /* Animation and avatar preferences */
    json_builder_set_member_name(builder, "enable-animations");
    json_builder_add_boolean_value(builder, g_settings_get_boolean(s_display_settings, "enable-animations"));

    json_builder_set_member_name(builder, "show-avatars");
    json_builder_add_boolean_value(builder, g_settings_get_boolean(s_display_settings, "show-avatars"));

    json_builder_set_member_name(builder, "show-media-previews");
    json_builder_add_boolean_value(builder, g_settings_get_boolean(s_display_settings, "show-media-previews"));
  }

  json_builder_end_object(builder); /* display */

  json_builder_end_object(builder); /* root content */

  /* Generate content JSON string */
  JsonNode *content_node = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) content_gen = json_generator_new();
  json_generator_set_root(content_gen, content_node);
  gchar *content_str = json_generator_to_data(content_gen, NULL);
  json_node_unref(content_node);

  /* Build the event */
  g_clear_object(&builder);
  builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Kind 30078 = NIP-78 application-specific data */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, GNOSTR_KIND_APP_SPECIFIC_DATA);

  /* Created at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* Content is the settings JSON */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, content_str);

  /* Tags - d-tag for addressable event */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* d-tag for addressable event */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "d");
  json_builder_add_string_value(builder, GNOSTR_NIP51_SETTINGS_D_TAG);
  json_builder_end_array(builder);

  json_builder_end_array(builder);

  json_builder_end_object(builder);

  /* Generate event JSON */
  JsonNode *event_node = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) event_gen = json_generator_new();
  json_generator_set_root(event_gen, event_node);
  gchar *event_str = json_generator_to_data(event_gen, NULL);

  json_node_unref(event_node);
  g_free(content_str);

  return event_str;
}

gboolean gnostr_nip51_settings_from_event(const gchar *event_json) {
  if (!event_json) return FALSE;

  ensure_settings();

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("nip51_settings: failed to parse event: %s", error->message);
    g_error_free(error);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return FALSE;
  }

  JsonObject *event = json_node_get_object(root);

  /* Verify kind */
  if (json_object_has_member(event, "kind")) {
    gint64 kind = json_object_get_int_member(event, "kind");
    if (kind != GNOSTR_KIND_APP_SPECIFIC_DATA) {
      g_warning("nip51_settings: wrong kind %" G_GINT64_FORMAT ", expected %d", kind, GNOSTR_KIND_APP_SPECIFIC_DATA);
      return FALSE;
    }
  }

  /* Get content */
  if (!json_object_has_member(event, "content")) {
    return FALSE;
  }

  const gchar *content_str = json_object_get_string_member(event, "content");
  if (!content_str || !*content_str) {
    return FALSE;
  }

  /* Parse content JSON */
  g_autoptr(JsonParser) content_parser = json_parser_new();
  if (!json_parser_load_from_data(content_parser, content_str, -1, NULL)) {
    return FALSE;
  }

  JsonNode *content_root = json_parser_get_root(content_parser);
  if (!content_root || !JSON_NODE_HOLDS_OBJECT(content_root)) {
    return FALSE;
  }

  JsonObject *content = json_node_get_object(content_root);

  /* Apply client settings */
  if (json_object_has_member(content, "client") && s_client_settings) {
    JsonObject *client = json_object_get_object_member(content, "client");
    if (client) {
      if (json_object_has_member(client, "blossom-server")) {
        const gchar *val = json_object_get_string_member(client, "blossom-server");
        if (val) g_settings_set_string(s_client_settings, "blossom-server", val);
      }
      if (json_object_has_member(client, "video-autoplay")) {
        g_settings_set_boolean(s_client_settings, "video-autoplay",
                               json_object_get_boolean_member(client, "video-autoplay"));
      }
      if (json_object_has_member(client, "video-loop")) {
        g_settings_set_boolean(s_client_settings, "video-loop",
                               json_object_get_boolean_member(client, "video-loop"));
      }
      if (json_object_has_member(client, "image-quality")) {
        const gchar *val = json_object_get_string_member(client, "image-quality");
        if (val) g_settings_set_string(s_client_settings, "image-quality", val);
      }
    }
  }

  /* Apply display settings */
  if (json_object_has_member(content, "display") && s_display_settings) {
    JsonObject *display = json_object_get_object_member(content, "display");
    if (display) {
      if (json_object_has_member(display, "color-scheme")) {
        const gchar *val = json_object_get_string_member(display, "color-scheme");
        if (val) g_settings_set_string(s_display_settings, "color-scheme", val);
      }
      if (json_object_has_member(display, "font-scale")) {
        g_settings_set_double(s_display_settings, "font-scale",
                              json_object_get_double_member(display, "font-scale"));
      }
      if (json_object_has_member(display, "timeline-density")) {
        const gchar *val = json_object_get_string_member(display, "timeline-density");
        if (val) g_settings_set_string(s_display_settings, "timeline-density", val);
      }
      if (json_object_has_member(display, "enable-animations")) {
        g_settings_set_boolean(s_display_settings, "enable-animations",
                               json_object_get_boolean_member(display, "enable-animations"));
      }
      if (json_object_has_member(display, "show-avatars")) {
        g_settings_set_boolean(s_display_settings, "show-avatars",
                               json_object_get_boolean_member(display, "show-avatars"));
      }
      if (json_object_has_member(display, "show-media-previews")) {
        g_settings_set_boolean(s_display_settings, "show-media-previews",
                               json_object_get_boolean_member(display, "show-media-previews"));
      }
    }
  }


  /* Update last sync timestamp */
  set_last_sync((gint64)time(NULL));

  g_message("nip51_settings: applied settings from event");
  return TRUE;
}

#ifndef GNOSTR_NIP51_TEST_ONLY

/* Async load context */
typedef struct {
  gchar *pubkey_hex;
  GnostrNip51SettingsLoadCallback callback;
  gpointer user_data;
} Nip51LoadCtx;

static void nip51_load_ctx_free(Nip51LoadCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  g_free(ctx);
}

static void on_nip51_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip51LoadCtx *ctx = (Nip51LoadCtx*)user_data;
  if (!ctx) return;

  GError *err = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("nip51_settings: query failed: %s", err->message);
    }
    if (ctx->callback) {
      ctx->callback(FALSE, err->message, ctx->user_data);
    }
    g_error_free(err);
    nip51_load_ctx_free(ctx);
    return;
  }

  gboolean success = FALSE;
  gint64 newest_created_at = 0;

  /* Find the newest event with our d-tag */
  if (results && results->len > 0) {
    for (guint i = 0; i < results->len; i++) {
      const gchar *json = g_ptr_array_index(results, i);

      /* Parse to check created_at and d-tag */
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
          JsonObject *obj = json_node_get_object(root);
          gint64 created_at = json_object_has_member(obj, "created_at")
                               ? json_object_get_int_member(obj, "created_at")
                               : 0;

          /* Check d-tag */
          gboolean has_dtag = FALSE;
          if (json_object_has_member(obj, "tags")) {
            JsonArray *tags = json_object_get_array_member(obj, "tags");
            for (guint j = 0; j < json_array_get_length(tags); j++) {
              JsonArray *tag = json_array_get_array_element(tags, j);
              if (tag && json_array_get_length(tag) >= 2) {
                const gchar *tag_name = json_array_get_string_element(tag, 0);
                const gchar *tag_val = json_array_get_string_element(tag, 1);
                if (g_strcmp0(tag_name, "d") == 0 &&
                    g_strcmp0(tag_val, GNOSTR_NIP51_SETTINGS_D_TAG) == 0) {
                  has_dtag = TRUE;
                  break;
                }
              }
            }
          }

          if (has_dtag && created_at > newest_created_at) {
            if (gnostr_nip51_settings_from_event(json)) {
              newest_created_at = created_at;
              success = TRUE;
            }
          }
        }
      }
    }
  }

  if (results) g_ptr_array_unref(results);

  if (ctx->callback) {
    ctx->callback(success, success ? NULL : "No settings found", ctx->user_data);
  }

  nip51_load_ctx_free(ctx);
}

void gnostr_nip51_settings_load_async(const gchar *pubkey_hex,
                                       GnostrNip51SettingsLoadCallback callback,
                                       gpointer user_data) {
  if (!pubkey_hex || !*pubkey_hex) {
    if (callback) callback(FALSE, "No pubkey provided", user_data);
    return;
  }

  Nip51LoadCtx *ctx = g_new0(Nip51LoadCtx, 1);
  ctx->pubkey_hex = g_strdup(pubkey_hex);
  ctx->callback = callback;
  ctx->user_data = user_data;

  /* Build filter for kind 30078 with our d-tag */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { GNOSTR_KIND_APP_SPECIFIC_DATA };
  nostr_filter_set_kinds(filter, kinds, 1);
  const char *authors[1] = { pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_set_limit(filter, 5); /* Get a few to find the right d-tag */

  /* Get relay URLs */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_arr);

  const char **urls = g_new0(const char*, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Use static pool */
  static GNostrPool *nip51_pool = NULL;
  if (!nip51_pool) nip51_pool = gnostr_pool_new();

    gnostr_pool_sync_relays(nip51_pool, (const gchar **)urls, relay_arr->len);
  {
    NostrFilters *_qf = nostr_filters_new();
    nostr_filters_add(_qf, filter);
    gnostr_pool_query_async(nip51_pool, _qf, NULL, /* cancellable */
    on_nip51_query_done, ctx);
  }

  g_free(urls);
  g_ptr_array_unref(relay_arr);
  nostr_filter_free(filter);
}

/* Async backup context */
typedef struct {
  GnostrNip51SettingsBackupCallback callback;
  gpointer user_data;
  gchar *event_json;
} Nip51BackupCtx;

static void nip51_backup_ctx_free(Nip51BackupCtx *ctx) {
  if (!ctx) return;
  g_free(ctx->event_json);
  g_free(ctx);
}

/* hq-0df86: Worker thread data for async relay publishing */
typedef struct {
  NostrEvent *event;
  GPtrArray  *relay_urls;
  guint       success_count;
  guint       fail_count;
} Nip51RelayPublishData;

static void nip51_relay_publish_data_free(Nip51RelayPublishData *d) {
  if (!d) return;
  if (d->event) nostr_event_free(d->event);
  if (d->relay_urls) g_ptr_array_free(d->relay_urls, TRUE);
  g_free(d);
}

/* hq-0df86: Worker thread — connect+publish loop runs off main thread */
static void
nip51_publish_thread(GTask *task, gpointer source_object,
                     gpointer task_data, GCancellable *cancellable)
{
  (void)source_object; (void)cancellable;
  Nip51RelayPublishData *d = (Nip51RelayPublishData *)task_data;

  for (guint i = 0; i < d->relay_urls->len; i++) {
    const gchar *url = (const gchar *)g_ptr_array_index(d->relay_urls, i);
    g_autoptr(GNostrRelay) relay = gnostr_relay_new(url);
    if (!relay) { d->fail_count++; continue; }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_debug("nip51_settings: failed to connect to %s: %s", url,
              conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      d->fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, d->event, &pub_err)) {
      g_message("nip51_settings: published to %s", url);
      d->success_count++;
    } else {
      g_debug("nip51_settings: publish failed to %s: %s", url,
              pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
      d->fail_count++;
    }
  }

  g_task_return_boolean(task, d->success_count > 0);
}

/* hq-0df86: Completion callback — runs on main thread */
static void
nip51_publish_task_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  (void)source_object;
  Nip51BackupCtx *ctx = (Nip51BackupCtx *)user_data;

  GTask *task = G_TASK(res);
  Nip51RelayPublishData *d = g_task_get_task_data(task);
  GError *error = NULL;
  g_task_propagate_boolean(task, &error);

  /* Update last sync timestamp on success */
  if (d->success_count > 0) {
    set_last_sync((gint64)time(NULL));
  }

  if (ctx->callback) {
    if (d->success_count > 0) {
      ctx->callback(TRUE, NULL, ctx->user_data);
    } else {
      ctx->callback(FALSE, "Failed to publish to any relay", ctx->user_data);
    }
  }

  g_message("nip51_settings: published to %u relays, failed %u",
            d->success_count, d->fail_count);
  g_clear_error(&error);
  nip51_backup_ctx_free(ctx);
}

static void on_nip51_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip51BackupCtx *ctx = (Nip51BackupCtx*)user_data;
  (void)source;
  if (!ctx) return;

  GError *error = NULL;
  gchar *signed_event_json = NULL;

  gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

  if (!ok || !signed_event_json) {
    g_warning("nip51_settings: signing failed: %s", error ? error->message : "unknown");
    if (ctx->callback) {
      ctx->callback(FALSE, error ? error->message : "Signing failed", ctx->user_data);
    }
    g_clear_error(&error);
    nip51_backup_ctx_free(ctx);
    return;
  }

  /* Parse the signed event */
  NostrEvent *event = nostr_event_new();
  int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
  if (parse_rc != 1) {
    g_warning("nip51_settings: failed to parse signed event");
    if (ctx->callback) {
      ctx->callback(FALSE, "Failed to parse signed event", ctx->user_data);
    }
    nostr_event_free(event);
    g_free(signed_event_json);
    nip51_backup_ctx_free(ctx);
    return;
  }

  /* Get relay URLs */
  GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_urls);

  g_free(signed_event_json);

  /* hq-0df86: Move connect+publish loop to worker thread to avoid blocking UI */
  Nip51RelayPublishData *wd = g_new0(Nip51RelayPublishData, 1);
  wd->event = event;          /* transfer ownership */
  wd->relay_urls = relay_urls; /* transfer ownership */

  GTask *task = g_task_new(NULL, NULL, nip51_publish_task_done, ctx);
  g_task_set_task_data(task, wd, (GDestroyNotify)nip51_relay_publish_data_free);
  g_task_run_in_thread(task, nip51_publish_thread);
  g_object_unref(task);
}

void gnostr_nip51_settings_backup_async(GnostrNip51SettingsBackupCallback callback,
                                         gpointer user_data) {
  /* Check if signer service is available */
  GnostrSignerService *signer = gnostr_signer_service_get_default();
  if (!gnostr_signer_service_is_available(signer)) {
    if (callback) callback(FALSE, "Signer not available", user_data);
    return;
  }

  /* Build unsigned event JSON */
  gchar *event_json = gnostr_nip51_settings_build_event_json();
  if (!event_json) {
    if (callback) callback(FALSE, "Failed to build event JSON", user_data);
    return;
  }

  g_message("nip51_settings: requesting signature for settings backup");

  /* Create backup context */
  Nip51BackupCtx *ctx = g_new0(Nip51BackupCtx, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->event_json = event_json;

  /* Call unified signer service (uses NIP-46 or NIP-55L based on login method) */
  gnostr_sign_event_async(
    event_json,
    "",        /* current_user: ignored */
    "gnostr",  /* app_id: ignored */
    NULL,      /* cancellable */
    on_nip51_sign_complete,
    ctx
  );
}

void gnostr_nip51_settings_auto_sync_on_login(const gchar *pubkey_hex) {
  if (!pubkey_hex || !*pubkey_hex) return;

  if (!gnostr_nip51_settings_sync_enabled()) {
    g_debug("nip51_settings: auto-sync disabled, skipping");
    return;
  }

  g_message("nip51_settings: auto-syncing settings for user %.*s...", 8, pubkey_hex);
  gnostr_nip51_settings_load_async(pubkey_hex, NULL, NULL);
}

#endif /* GNOSTR_NIP51_TEST_ONLY */
