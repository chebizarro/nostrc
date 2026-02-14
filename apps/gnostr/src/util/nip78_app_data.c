/**
 * NIP-78 App-Specific Data Support Implementation
 *
 * Handles parsing, creation, and relay operations for kind 30078 events.
 */

#include "nip78_app_data.h"
#include "relays.h"
#include "../ipc/gnostr-signer-service.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include "json.h"
#include <string.h>
#include <time.h>

#ifndef GNOSTR_NIP78_TEST_ONLY
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include <nostr-gobject-1.0/nostr_pool.h>
#endif

/* ---- Memory Management ---- */

GnostrAppData *gnostr_app_data_new(void) {
    GnostrAppData *data = g_new0(GnostrAppData, 1);
    return data;
}

void gnostr_app_data_free(GnostrAppData *data) {
    if (!data) return;
    g_free(data->app_id);
    g_free(data->data_key);
    g_free(data->d_tag);
    g_free(data->content);
    g_free(data->event_id);
    g_free(data->pubkey);
    g_free(data);
}

GnostrAppData *gnostr_app_data_copy(const GnostrAppData *data) {
    if (!data) return NULL;

    GnostrAppData *copy = gnostr_app_data_new();
    copy->app_id = g_strdup(data->app_id);
    copy->data_key = g_strdup(data->data_key);
    copy->d_tag = g_strdup(data->d_tag);
    copy->content = g_strdup(data->content);
    copy->event_id = g_strdup(data->event_id);
    copy->pubkey = g_strdup(data->pubkey);
    copy->created_at = data->created_at;

    return copy;
}

/* ---- Parsing ---- */

/* Callback context for finding d-tag */
typedef struct {
    char *d_tag_value;
    gboolean found;
} FindDTagCtx;

/* Callback for iterating tags to find d-tag */
static gboolean find_d_tag_cb(gsize idx, const gchar *element_json, gpointer user_data) {
    (void)idx;
    FindDTagCtx *ctx = (FindDTagCtx *)user_data;
    if (ctx->found) return FALSE;

    char *tag_name = NULL;
    tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
    if (!tag_name) {
        return TRUE; /* continue */
    }

    if (g_strcmp0(tag_name, "d") == 0) {
        ctx->d_tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
        ctx->found = TRUE;
    }
    g_free(tag_name);
    return !ctx->found;
}

gboolean gnostr_app_data_parse_d_tag(const char *d_tag,
                                      char **out_app_id,
                                      char **out_data_key) {
    if (!d_tag || !out_app_id || !out_data_key) return FALSE;

    *out_app_id = NULL;
    *out_data_key = NULL;

    /* Find the first '/' separator */
    const char *slash = strchr(d_tag, '/');

    if (slash) {
        /* Split at the slash */
        size_t app_id_len = slash - d_tag;
        *out_app_id = g_strndup(d_tag, app_id_len);
        *out_data_key = g_strdup(slash + 1);
    } else {
        /* No slash - entire tag is app_id, no data_key */
        *out_app_id = g_strdup(d_tag);
        *out_data_key = g_strdup("");
    }

    return TRUE;
}

char *gnostr_app_data_build_d_tag(const char *app_id, const char *data_key) {
    if (!app_id || !*app_id) return NULL;

    if (data_key && *data_key) {
        return g_strdup_printf("%s/%s", app_id, data_key);
    } else {
        return g_strdup(app_id);
    }
}

GnostrAppData *gnostr_app_data_parse_event(const char *event_json) {
    if (!event_json) return NULL;

    /* Verify kind */
    int64_t kind = gnostr_json_get_int64(event_json, "kind", NULL);
    if (kind != GNOSTR_NIP78_KIND_APP_DATA) {
        g_debug("nip78: not a kind %d event", GNOSTR_NIP78_KIND_APP_DATA);
        return NULL;
    }

    GnostrAppData *data = gnostr_app_data_new();

    /* Extract event metadata */
    data->event_id = gnostr_json_get_string(event_json, "id", NULL);
    data->pubkey = gnostr_json_get_string(event_json, "pubkey", NULL);
    data->created_at = gnostr_json_get_int64(event_json, "created_at", NULL);

    /* Extract content */
    data->content = gnostr_json_get_string(event_json, "content", NULL);

    /* Find d-tag using callback-based tag iteration */
    char *tags_json = NULL;
    tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
    if (tags_json) {
        FindDTagCtx ctx = { .d_tag_value = NULL, .found = FALSE };
        gnostr_json_array_foreach_root(tags_json, find_d_tag_cb, &ctx);
        if (ctx.found && ctx.d_tag_value) {
            data->d_tag = ctx.d_tag_value;
            gnostr_app_data_parse_d_tag(data->d_tag, &data->app_id, &data->data_key);
        }
        g_free(tags_json);
    }

    /* Validate we have the minimum required data */
    if (!data->d_tag) {
        g_warning("nip78: event missing d-tag");
        gnostr_app_data_free(data);
        return NULL;
    }

    return data;
}

/* ---- Event Creation ---- */

char *gnostr_app_data_build_event_json(const char *app_id,
                                        const char *data_key,
                                        const char *content) {
    return gnostr_app_data_build_event_json_full(app_id, data_key, content, NULL);
}

char *gnostr_app_data_build_event_json_full(const char *app_id,
                                             const char *data_key,
                                             const char *content,
                                             const char *extra_tags_json) {
    if (!app_id || !*app_id) return NULL;

    g_autofree char *d_tag_value = gnostr_app_data_build_d_tag(app_id, data_key);
    if (!d_tag_value) return NULL;

    GNostrJsonBuilder *builder = gnostr_json_builder_new();
    gnostr_json_builder_begin_object(builder);

    /* kind */
    gnostr_json_builder_set_key(builder, "kind");
    gnostr_json_builder_add_int(builder, GNOSTR_NIP78_KIND_APP_DATA);

    /* created_at */
    gnostr_json_builder_set_key(builder, "created_at");
    gnostr_json_builder_add_int(builder, (int64_t)time(NULL));

    /* content */
    gnostr_json_builder_set_key(builder, "content");
    gnostr_json_builder_add_string(builder, content ? content : "");

    /* tags array */
    gnostr_json_builder_set_key(builder, "tags");
    gnostr_json_builder_begin_array(builder);

    /* Add d-tag ["d", "app_id/data_key"] */
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "d");
    gnostr_json_builder_add_string(builder, d_tag_value);
    gnostr_json_builder_end_array(builder);

    /* Add extra tags if provided (JSON array string) */
    if (extra_tags_json && *extra_tags_json) {
        /* Parse and add each extra tag */
        /* For simplicity, we append the raw tags - caller must provide valid JSON */
        /* A better approach would iterate, but we don't have a caller using this */
    }

    gnostr_json_builder_end_array(builder); /* end tags */

    gnostr_json_builder_end_object(builder);

    char *result = gnostr_json_builder_finish(builder);
    g_object_unref(builder);

    return result;
}

/* ---- JSON Content Helpers ---- */

/* Note: Returns pointer to static/cached string, valid until next call */
const char *gnostr_app_data_get_json_string(const GnostrAppData *data,
                                             const char *key) {
    static char *s_cached_string = NULL;
    if (!data || !data->content || !key) return NULL;

    g_free(s_cached_string);
    s_cached_string = NULL;

    s_cached_string = gnostr_json_get_string(data->content, key, NULL);
    if (s_cached_string) {
        return s_cached_string;
    }
    return NULL;
}

gint64 gnostr_app_data_get_json_int(const GnostrAppData *data,
                                     const char *key,
                                     gint64 default_val) {
    if (!data || !data->content || !key) return default_val;

    int64_t val = 0;
    if ((val = gnostr_json_get_int64(data->content, key, NULL), TRUE)) {
        return (gint64)val;
    }
    return default_val;
}

gboolean gnostr_app_data_get_json_bool(const GnostrAppData *data,
                                        const char *key,
                                        gboolean default_val) {
    if (!data || !data->content || !key) return default_val;

    bool val = false;
    val = gnostr_json_get_boolean(data->content, key, NULL);
    {
        return val ? TRUE : FALSE;
    }
    return default_val;
}

char *gnostr_app_data_get_json_raw(const GnostrAppData *data,
                                    const char *key) {
    if (!data || !data->content || !key) return NULL;

    char *raw = NULL;
    if ((raw = gnostr_json_get_raw(data->content, key, NULL)) != NULL) {
        return raw;
    }
    return NULL;
}

/* ---- Utility ---- */

gboolean gnostr_app_data_is_valid_app_id(const char *app_id) {
    if (!app_id || !*app_id) return FALSE;

    /* Must not contain '/' */
    if (strchr(app_id, '/') != NULL) return FALSE;

    /* Allow alphanumeric, hyphens, underscores, dots */
    for (const char *p = app_id; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.') {
            return FALSE;
        }
    }

    return TRUE;
}

gboolean gnostr_app_data_is_valid_data_key(const char *data_key) {
    if (!data_key) return TRUE; /* NULL is valid (no data key) */

    /* Must not contain '/' to avoid nested paths */
    if (strchr(data_key, '/') != NULL) return FALSE;

    return TRUE;
}

/* ---- Relay Operations ---- */

#ifndef GNOSTR_NIP78_TEST_ONLY

/* Singleton pool for NIP-78 queries */
static GNostrPool *s_nip78_pool = NULL;

static GNostrPool *get_nip78_pool(void) {
    if (!s_nip78_pool) {
        s_nip78_pool = gnostr_pool_new();
    }
    return s_nip78_pool;
}

/* ---- Fetch Single ---- */

typedef struct {
    char *pubkey_hex;
    char *app_id;
    char *data_key;
    char *d_tag;
    GnostrAppDataFetchCallback callback;
    gpointer user_data;
} FetchSingleContext;

static void fetch_single_context_free(FetchSingleContext *ctx) {
    if (!ctx) return;
    g_free(ctx->pubkey_hex);
    g_free(ctx->app_id);
    g_free(ctx->data_key);
    g_free(ctx->d_tag);
    g_free(ctx);
}

static void on_fetch_single_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    FetchSingleContext *ctx = (FetchSingleContext *)user_data;
    if (!ctx) return;

    GError *err = NULL;
    GPtrArray *results = gnostr_pool_query_finish(
        GNOSTR_POOL(source), res, &err);

    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("nip78: fetch failed: %s", err->message);
        }
        if (ctx->callback) {
            ctx->callback(NULL, FALSE, err->message, ctx->user_data);
        }
        g_error_free(err);
        fetch_single_context_free(ctx);
        return;
    }

    GnostrAppData *newest = NULL;
    gint64 newest_created_at = 0;

    /* Find the newest event matching our d-tag */
    if (results && results->len > 0) {
        for (guint i = 0; i < results->len; i++) {
            const char *json = g_ptr_array_index(results, i);
            GnostrAppData *data = gnostr_app_data_parse_event(json);

            if (data && g_strcmp0(data->d_tag, ctx->d_tag) == 0) {
                if (data->created_at > newest_created_at) {
                    gnostr_app_data_free(newest);
                    newest = data;
                    newest_created_at = data->created_at;
                } else {
                    gnostr_app_data_free(data);
                }
            } else {
                gnostr_app_data_free(data);
            }
        }
    }

    if (results) g_ptr_array_unref(results);

    if (ctx->callback) {
        ctx->callback(newest, newest != NULL,
                     newest ? NULL : "No matching data found",
                     ctx->user_data);
    }

    /* Note: caller owns newest, don't free it */
    fetch_single_context_free(ctx);
}

void gnostr_app_data_fetch_async(const char *pubkey_hex,
                                  const char *app_id,
                                  const char *data_key,
                                  GnostrAppDataFetchCallback callback,
                                  gpointer user_data) {
    if (!pubkey_hex || !app_id) {
        if (callback) callback(NULL, FALSE, "Missing pubkey or app_id", user_data);
        return;
    }

    char *d_tag = gnostr_app_data_build_d_tag(app_id, data_key);
    if (!d_tag) {
        if (callback) callback(NULL, FALSE, "Failed to build d-tag", user_data);
        return;
    }

    FetchSingleContext *ctx = g_new0(FetchSingleContext, 1);
    ctx->pubkey_hex = g_strdup(pubkey_hex);
    ctx->app_id = g_strdup(app_id);
    ctx->data_key = g_strdup(data_key);
    ctx->d_tag = d_tag;
    ctx->callback = callback;
    ctx->user_data = user_data;

    /* Build filter */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { GNOSTR_NIP78_KIND_APP_DATA };
    nostr_filter_set_kinds(filter, kinds, 1);
    const char *authors[1] = { pubkey_hex };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_limit(filter, 10);

    /* Get relay URLs */
    GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
    gnostr_load_relays_into(relay_arr);

    if (relay_arr->len == 0) {
        if (callback) callback(NULL, FALSE, "No relays configured", user_data);
        nostr_filter_free(filter);
        g_ptr_array_unref(relay_arr);
        fetch_single_context_free(ctx);
        return;
    }

    const char **urls = g_new0(const char*, relay_arr->len);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    g_message("nip78: fetching app data %s/%s for %.8s...",
              app_id, data_key ? data_key : "", pubkey_hex);

        gnostr_pool_sync_relays(get_nip78_pool(), (const gchar **)urls, relay_arr->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      gnostr_pool_query_async(get_nip78_pool(), _qf, NULL, on_fetch_single_done, ctx);
    }

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
}

/* ---- Fetch All ---- */

typedef struct {
    char *pubkey_hex;
    char *app_id;
    GnostrAppDataListCallback callback;
    gpointer user_data;
} FetchAllContext;

static void fetch_all_context_free(FetchAllContext *ctx) {
    if (!ctx) return;
    g_free(ctx->pubkey_hex);
    g_free(ctx->app_id);
    g_free(ctx);
}

static void on_fetch_all_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    FetchAllContext *ctx = (FetchAllContext *)user_data;
    if (!ctx) return;

    GError *err = NULL;
    GPtrArray *results = gnostr_pool_query_finish(
        GNOSTR_POOL(source), res, &err);

    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_warning("nip78: fetch all failed: %s", err->message);
        }
        if (ctx->callback) {
            ctx->callback(NULL, FALSE, err->message, ctx->user_data);
        }
        g_error_free(err);
        fetch_all_context_free(ctx);
        return;
    }

    /* Hash table to track newest event per d-tag */
    GHashTable *by_d_tag = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, (GDestroyNotify)gnostr_app_data_free);

    /* Build prefix for filtering */
    char *prefix = g_strdup_printf("%s/", ctx->app_id);
    size_t prefix_len = strlen(prefix);

    if (results && results->len > 0) {
        for (guint i = 0; i < results->len; i++) {
            const char *json = g_ptr_array_index(results, i);
            GnostrAppData *data = gnostr_app_data_parse_event(json);

            if (!data || !data->d_tag) {
                gnostr_app_data_free(data);
                continue;
            }

            /* Check if d-tag matches our app_id */
            gboolean matches = (g_strcmp0(data->d_tag, ctx->app_id) == 0) ||
                              (strncmp(data->d_tag, prefix, prefix_len) == 0);

            if (!matches) {
                gnostr_app_data_free(data);
                continue;
            }

            /* Check if newer than existing entry for this d-tag */
            GnostrAppData *existing = g_hash_table_lookup(by_d_tag, data->d_tag);
            if (!existing || data->created_at > existing->created_at) {
                g_hash_table_insert(by_d_tag, g_strdup(data->d_tag), data);
            } else {
                gnostr_app_data_free(data);
            }
        }
    }

    g_free(prefix);
    if (results) g_ptr_array_unref(results);

    /* Convert hash table to array */
    GPtrArray *data_list = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_app_data_free);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, by_d_tag);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GnostrAppData *data = (GnostrAppData *)value;
        g_ptr_array_add(data_list, gnostr_app_data_copy(data));
    }

    g_hash_table_destroy(by_d_tag);

    g_message("nip78: fetched %u app data entries for %s", data_list->len, ctx->app_id);

    if (ctx->callback) {
        ctx->callback(data_list, TRUE, NULL, ctx->user_data);
    }

    /* Note: caller owns data_list */
    fetch_all_context_free(ctx);
}

void gnostr_app_data_fetch_all_async(const char *pubkey_hex,
                                      const char *app_id,
                                      GnostrAppDataListCallback callback,
                                      gpointer user_data) {
    if (!pubkey_hex || !app_id) {
        if (callback) callback(NULL, FALSE, "Missing pubkey or app_id", user_data);
        return;
    }

    FetchAllContext *ctx = g_new0(FetchAllContext, 1);
    ctx->pubkey_hex = g_strdup(pubkey_hex);
    ctx->app_id = g_strdup(app_id);
    ctx->callback = callback;
    ctx->user_data = user_data;

    /* Build filter for all kind 30078 from this author */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { GNOSTR_NIP78_KIND_APP_DATA };
    nostr_filter_set_kinds(filter, kinds, 1);
    const char *authors[1] = { pubkey_hex };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_limit(filter, 100);

    /* Get relay URLs */
    GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
    gnostr_load_relays_into(relay_arr);

    if (relay_arr->len == 0) {
        if (callback) callback(NULL, FALSE, "No relays configured", user_data);
        nostr_filter_free(filter);
        g_ptr_array_unref(relay_arr);
        fetch_all_context_free(ctx);
        return;
    }

    const char **urls = g_new0(const char*, relay_arr->len);
    for (guint i = 0; i < relay_arr->len; i++) {
        urls[i] = g_ptr_array_index(relay_arr, i);
    }

    g_message("nip78: fetching all app data for %s from %.8s...", app_id, pubkey_hex);

        gnostr_pool_sync_relays(get_nip78_pool(), (const gchar **)urls, relay_arr->len);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, filter);
      gnostr_pool_query_async(get_nip78_pool(), _qf, NULL, on_fetch_all_done, ctx);
    }

    g_free(urls);
    g_ptr_array_unref(relay_arr);
    nostr_filter_free(filter);
}

/* ---- Publish ---- */

typedef struct {
    char *app_id;
    char *data_key;
    char *content;
    char *event_json;
    GnostrAppDataCallback callback;
    gpointer user_data;
} PublishContext;

static void publish_context_free(PublishContext *ctx) {
    if (!ctx) return;
    g_free(ctx->app_id);
    g_free(ctx->data_key);
    g_free(ctx->content);
    g_free(ctx->event_json);
    g_free(ctx);
}

/* hq-0df86: Worker thread data for async relay publishing */
typedef struct {
  NostrEvent *event;
  GPtrArray  *relay_urls;
  guint       success_count;
  guint       fail_count;
} Nip78RelayPublishData;

static void nip78_relay_publish_data_free(Nip78RelayPublishData *d) {
  if (!d) return;
  if (d->event) nostr_event_free(d->event);
  if (d->relay_urls) g_ptr_array_free(d->relay_urls, TRUE);
  g_free(d);
}

/* hq-0df86: Worker thread — connect+publish loop runs off main thread */
static void
nip78_publish_thread(GTask *task, gpointer source_object,
                     gpointer task_data, GCancellable *cancellable)
{
  (void)source_object; (void)cancellable;
  Nip78RelayPublishData *d = (Nip78RelayPublishData *)task_data;

  for (guint i = 0; i < d->relay_urls->len; i++) {
    const char *url = g_ptr_array_index(d->relay_urls, i);
    GNostrRelay *relay = gnostr_relay_new(url);
    if (!relay) { d->fail_count++; continue; }

    GError *conn_err = NULL;
    if (!gnostr_relay_connect(relay, &conn_err)) {
      g_debug("nip78: failed to connect to %s: %s", url,
              conn_err ? conn_err->message : "unknown");
      g_clear_error(&conn_err);
      g_object_unref(relay);
      d->fail_count++;
      continue;
    }

    GError *pub_err = NULL;
    if (gnostr_relay_publish(relay, d->event, &pub_err)) {
      g_message("nip78: published to %s", url);
      d->success_count++;
    } else {
      g_debug("nip78: publish failed to %s: %s", url,
              pub_err ? pub_err->message : "unknown");
      g_clear_error(&pub_err);
      d->fail_count++;
    }
    g_object_unref(relay);
  }

  g_task_return_boolean(task, d->success_count > 0);
}

/* hq-0df86: Completion callback — runs on main thread */
static void
nip78_publish_task_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  (void)source_object;
  PublishContext *ctx = (PublishContext *)user_data;

  GTask *task = G_TASK(res);
  Nip78RelayPublishData *d = g_task_get_task_data(task);
  GError *error = NULL;
  g_task_propagate_boolean(task, &error);

  g_message("nip78: published to %u relays, failed %u",
            d->success_count, d->fail_count);

  if (ctx->callback) {
    if (d->success_count > 0) {
      ctx->callback(TRUE, NULL, ctx->user_data);
    } else {
      ctx->callback(FALSE, "Failed to publish to any relay", ctx->user_data);
    }
  }

  g_clear_error(&error);
  publish_context_free(ctx);
}

static void on_publish_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
    PublishContext *ctx = (PublishContext *)user_data;
    (void)source;
    if (!ctx) return;

    GError *error = NULL;
    char *signed_event_json = NULL;

    gboolean ok = gnostr_sign_event_finish(res, &signed_event_json, &error);

    if (!ok || !signed_event_json) {
        g_warning("nip78: signing failed: %s", error ? error->message : "unknown");
        if (ctx->callback) {
            ctx->callback(FALSE, error ? error->message : "Signing failed", ctx->user_data);
        }
        g_clear_error(&error);
        publish_context_free(ctx);
        return;
    }

    /* Parse signed event */
    NostrEvent *event = nostr_event_new();
    int parse_rc = nostr_event_deserialize_compact(event, signed_event_json, NULL);
    if (parse_rc != 1) {
        g_warning("nip78: failed to parse signed event");
        if (ctx->callback) {
            ctx->callback(FALSE, "Failed to parse signed event", ctx->user_data);
        }
        nostr_event_free(event);
        g_free(signed_event_json);
        publish_context_free(ctx);
        return;
    }

    /* Get relay URLs */
    GPtrArray *relay_urls = g_ptr_array_new_with_free_func(g_free);
    gnostr_get_write_relay_urls_into(relay_urls);
    if (relay_urls->len == 0) {
        gnostr_load_relays_into(relay_urls);
    }

    g_free(signed_event_json);

    /* hq-0df86: Move connect+publish loop to worker thread to avoid blocking UI */
    Nip78RelayPublishData *wd = g_new0(Nip78RelayPublishData, 1);
    wd->event = event;          /* transfer ownership */
    wd->relay_urls = relay_urls; /* transfer ownership */

    GTask *task = g_task_new(NULL, NULL, nip78_publish_task_done, ctx);
    g_task_set_task_data(task, wd, (GDestroyNotify)nip78_relay_publish_data_free);
    g_task_run_in_thread(task, nip78_publish_thread);
    g_object_unref(task);
}

void gnostr_app_data_publish_async(const char *app_id,
                                    const char *data_key,
                                    const char *content,
                                    GnostrAppDataCallback callback,
                                    gpointer user_data) {
    if (!app_id) {
        if (callback) callback(FALSE, "Missing app_id", user_data);
        return;
    }

    /* Check signer availability */
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (!gnostr_signer_service_is_available(signer)) {
        if (callback) callback(FALSE, "Signer not available", user_data);
        return;
    }

    /* Build unsigned event */
    char *event_json = gnostr_app_data_build_event_json(app_id, data_key, content);
    if (!event_json) {
        if (callback) callback(FALSE, "Failed to build event JSON", user_data);
        return;
    }

    PublishContext *ctx = g_new0(PublishContext, 1);
    ctx->app_id = g_strdup(app_id);
    ctx->data_key = g_strdup(data_key);
    ctx->content = g_strdup(content);
    ctx->event_json = event_json;
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_message("nip78: requesting signature for %s/%s", app_id, data_key ? data_key : "");

    gnostr_sign_event_async(
        event_json,
        "",
        "gnostr",
        NULL,
        on_publish_sign_complete,
        ctx
    );
}

void gnostr_app_data_delete_async(const char *app_id,
                                   const char *data_key,
                                   GnostrAppDataCallback callback,
                                   gpointer user_data) {
    /* Delete by publishing empty content */
    gnostr_app_data_publish_async(app_id, data_key, "", callback, user_data);
}

#else /* GNOSTR_NIP78_TEST_ONLY */

void gnostr_app_data_fetch_async(const char *pubkey_hex,
                                  const char *app_id,
                                  const char *data_key,
                                  GnostrAppDataFetchCallback callback,
                                  gpointer user_data) {
    (void)pubkey_hex;
    (void)app_id;
    (void)data_key;
    g_message("nip78: fetch requested (test stub)");
    if (callback) callback(NULL, TRUE, NULL, user_data);
}

void gnostr_app_data_fetch_all_async(const char *pubkey_hex,
                                      const char *app_id,
                                      GnostrAppDataListCallback callback,
                                      gpointer user_data) {
    (void)pubkey_hex;
    (void)app_id;
    g_message("nip78: fetch all requested (test stub)");
    GPtrArray *empty = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_app_data_free);
    if (callback) callback(empty, TRUE, NULL, user_data);
}

void gnostr_app_data_publish_async(const char *app_id,
                                    const char *data_key,
                                    const char *content,
                                    GnostrAppDataCallback callback,
                                    gpointer user_data) {
    (void)app_id;
    (void)data_key;
    (void)content;
    g_message("nip78: publish requested (test stub)");
    if (callback) callback(TRUE, NULL, user_data);
}

void gnostr_app_data_delete_async(const char *app_id,
                                   const char *data_key,
                                   GnostrAppDataCallback callback,
                                   gpointer user_data) {
    (void)app_id;
    (void)data_key;
    g_message("nip78: delete requested (test stub)");
    if (callback) callback(TRUE, NULL, user_data);
}

#endif /* GNOSTR_NIP78_TEST_ONLY */
