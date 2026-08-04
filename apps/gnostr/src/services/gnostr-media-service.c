#include "gnostr-media-service.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>

/* Implemented by apps/gnostr/src/util/utils.c.  Keep this service header-light
 * so its unit tests do not need the app's relay/storage dependency graph. */
SoupSession *gnostr_get_shared_soup_session(void);
gboolean gnostr_is_remote_media_allowed(void);
#endif

#define MIB ((guint64)1024 * 1024)
#define DEFAULT_INLINE_BUDGET (500 * MIB)
#define DEFAULT_OG_IMAGE_BUDGET (64 * MIB)
#define DEFAULT_VIDEO_POSTER_BUDGET (64 * MIB)
#define DEFAULT_INLINE_BODY_CAP ((gsize)24 * 1024 * 1024)
#define DEFAULT_OG_IMAGE_BODY_CAP ((gsize)12 * 1024 * 1024)
#define DEFAULT_VIDEO_POSTER_BODY_CAP ((gsize)12 * 1024 * 1024)
#define DEFAULT_OG_METADATA_BODY_CAP ((gsize)2 * 1024 * 1024)
#define READ_CHUNK_SIZE ((gsize)64 * 1024)
#define MAX_TARGET_DIMENSION 4096
#define SETTINGS_SCHEMA "org.gnostr.Client"
#define INLINE_BUDGET_KEY "image-cache-max-mb"
#define OG_IMAGE_BUDGET_KEY "og-image-cache-max-mb"
#define VIDEO_POSTER_BUDGET_KEY "video-poster-cache-max-mb"

typedef enum {
  PENDING_TEXTURE,
  PENDING_OG_METADATA
} PendingKind;

typedef struct _PendingRequest PendingRequest;
typedef struct _Subscriber Subscriber;

typedef struct {
  char *key;
  char *url;
  GdkTexture *texture;
  guint64 decoded_bytes;
  GList *lru_link;
} TextureCacheEntry;

typedef struct {
  GHashTable *entries;
  GQueue lru;
  GnostrMediaClassStats stats;
} TextureClassCache;

typedef struct {
  char *key;
  gint64 expires_at;
  GList *lru_link;
} NegativeEntry;

struct _GnostrOgMetadata {
  gatomicrefcount ref_count;
  char *title;
  char *description;
  char *image_url;
  char *source_url;
};

typedef struct {
  char *url;
  GnostrOgMetadata *metadata;
  gint64 expires_at;
  GList *lru_link;
} OgCacheEntry;

struct _Subscriber {
  gatomicrefcount ref_count;
  PendingRequest *pending; /* main-context-only, cleared before pending free */
  GnostrMediaService *service; /* borrowed; async owner keeps it alive */
  char *url;
  PendingKind kind;
  GnostrMediaResourceClass resource_class;
  int target_width;
  int target_height;
  gsize body_cap;
  union {
    GnostrMediaTextureCallback texture;
    GnostrMediaOgCallback og;
  } callback;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
  GCancellable *cancellable;
  gulong cancel_handler;
  gint cancel_scheduled;
  gboolean completed;
};

typedef struct {
  PendingRequest *request;
  char *variant_key;
  GnostrMediaResourceClass resource_class;
  int target_width;
  int target_height;
} DecodeJob;

struct _PendingRequest {
  GnostrMediaService *service; /* strong: keeps service alive through callbacks */
  char *table_key;
  char *url;
  PendingKind kind;
  GPtrArray *subscribers; /* Subscriber* */
  GCancellable *network_cancellable;
  gsize download_cap;
  gboolean queued;
  gboolean network_active;
  gboolean network_done;
  guint outstanding_workers;
  gboolean any_worker_success;
  gboolean any_worker_failure;
  GBytes *body;
  GHashTable *decode_variants; /* borrowed DecodeJob.variant_key -> DecodeJob */
#ifdef HAVE_SOUP3
  SoupMessage *message;
  GInputStream *stream;
  GByteArray *download;
#endif
};

struct _GnostrMediaService {
  GObject parent_instance;
  GMainContext *context;
  GnostrMediaServiceConfig config;
  TextureClassCache texture_caches[GNOSTR_MEDIA_RESOURCE_N_CLASSES];

  GHashTable *pending; /* PendingRequest.table_key -> PendingRequest */
  GQueue download_queue;
  guint active_downloads;

  GHashTable *negative; /* NegativeEntry.key -> NegativeEntry */
  GQueue negative_lru;
  guint64 negative_hits;
  guint64 overflow_rejections;

  GHashTable *og_cache; /* OgCacheEntry.url -> OgCacheEntry */
  GQueue og_lru;
  guint64 og_hits;
  guint64 og_misses;
  guint64 og_evictions;

  GSettings *settings;
};

G_DEFINE_TYPE(GnostrMediaService, gnostr_media_service, G_TYPE_OBJECT)
G_LOCK_DEFINE_STATIC(default_service);
static GnostrMediaService *default_service;

static void pending_maybe_finish(PendingRequest *request);
static void start_queued_downloads(GnostrMediaService *self);
static void subscriber_complete_texture(Subscriber *subscriber,
                                        GdkTexture *texture,
                                        const GError *error);
static void subscriber_complete_og(Subscriber *subscriber,
                                   GnostrOgMetadata *metadata,
                                   const GError *error);

GQuark
gnostr_media_error_quark(void)
{
  return g_quark_from_static_string("gnostr-media-error-quark");
}

void
gnostr_media_service_config_init(GnostrMediaServiceConfig *config)
{
  g_return_if_fail(config != NULL);
  memset(config, 0, sizeof(*config));
  config->memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = DEFAULT_INLINE_BUDGET;
  config->memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] = DEFAULT_OG_IMAGE_BUDGET;
  config->memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER] = DEFAULT_VIDEO_POSTER_BUDGET;
  config->body_size_caps[GNOSTR_MEDIA_RESOURCE_INLINE] = DEFAULT_INLINE_BODY_CAP;
  config->body_size_caps[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] = DEFAULT_OG_IMAGE_BODY_CAP;
  config->body_size_caps[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER] = DEFAULT_VIDEO_POSTER_BODY_CAP;
  config->og_metadata_body_size_cap = DEFAULT_OG_METADATA_BODY_CAP;
  config->max_in_flight = 128;
  config->max_concurrent_downloads = 6;
  config->negative_cache_max_entries = 256;
  config->negative_cache_ttl_usec = 60 * G_USEC_PER_SEC;
  config->og_metadata_max_entries = 256;
  config->og_metadata_ttl_usec = 30 * 60 * G_USEC_PER_SEC;
}

GnostrOgMetadata *
gnostr_og_metadata_ref(GnostrOgMetadata *metadata)
{
  if (metadata)
    g_atomic_ref_count_inc(&metadata->ref_count);
  return metadata;
}

void
gnostr_og_metadata_unref(GnostrOgMetadata *metadata)
{
  if (!metadata)
    return;
  if (g_atomic_ref_count_dec(&metadata->ref_count)) {
    g_free(metadata->title);
    g_free(metadata->description);
    g_free(metadata->image_url);
    g_free(metadata->source_url);
    g_free(metadata);
  }
}

const char *
gnostr_og_metadata_get_title(const GnostrOgMetadata *metadata)
{
  return metadata ? metadata->title : NULL;
}

const char *
gnostr_og_metadata_get_description(const GnostrOgMetadata *metadata)
{
  return metadata ? metadata->description : NULL;
}

const char *
gnostr_og_metadata_get_image_url(const GnostrOgMetadata *metadata)
{
  return metadata ? metadata->image_url : NULL;
}

const char *
gnostr_og_metadata_get_source_url(const GnostrOgMetadata *metadata)
{
  return metadata ? metadata->source_url : NULL;
}

static char *
texture_key(const char *url, int width, int height)
{
  return g_strdup_printf("%dx%d\n%s", width, height, url);
}

static guint64
texture_decoded_bytes(GdkTexture *texture)
{
  guint64 width = (guint64)gdk_texture_get_width(texture);
  guint64 height = (guint64)gdk_texture_get_height(texture);
  if (width > G_MAXUINT64 / 4 || height > G_MAXUINT64 / (width * 4))
    return G_MAXUINT64;
  return width * height * 4;
}

static void
texture_entry_free(TextureCacheEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->key);
  g_free(entry->url);
  g_clear_object(&entry->texture);
  g_free(entry);
}

static void
texture_cache_remove_entry(TextureClassCache *cache,
                           TextureCacheEntry *entry,
                           gboolean count_eviction)
{
  g_hash_table_remove(cache->entries, entry->key);
  if (entry->lru_link)
    g_queue_delete_link(&cache->lru, entry->lru_link);
  cache->stats.resident_bytes -= entry->decoded_bytes;
  if (count_eviction)
    cache->stats.evictions++;
  texture_entry_free(entry);
  cache->stats.entries = g_hash_table_size(cache->entries);
}

static void
texture_cache_trim(TextureClassCache *cache)
{
  while (cache->stats.resident_bytes > cache->stats.budget_bytes &&
         !g_queue_is_empty(&cache->lru)) {
    TextureCacheEntry *oldest = g_queue_peek_head(&cache->lru);
    texture_cache_remove_entry(cache, oldest, TRUE);
  }
}

static GdkTexture *
texture_cache_lookup(GnostrMediaService *self,
                     GnostrMediaResourceClass resource_class,
                     const char *url,
                     int width,
                     int height)
{
  TextureClassCache *cache = &self->texture_caches[resource_class];
  g_autofree char *key = texture_key(url, width, height);
  TextureCacheEntry *entry = g_hash_table_lookup(cache->entries, key);
  if (!entry) {
    cache->stats.misses++;
    return NULL;
  }

  cache->stats.hits++;
  g_queue_unlink(&cache->lru, entry->lru_link);
  g_queue_push_tail_link(&cache->lru, entry->lru_link);
  return g_object_ref(entry->texture);
}

static void
texture_cache_store(GnostrMediaService *self,
                    GnostrMediaResourceClass resource_class,
                    const char *url,
                    int width,
                    int height,
                    GdkTexture *texture)
{
  TextureClassCache *cache = &self->texture_caches[resource_class];
  guint64 bytes = texture_decoded_bytes(texture);
  g_autofree char *lookup_key = texture_key(url, width, height);
  TextureCacheEntry *old = g_hash_table_lookup(cache->entries, lookup_key);

  if (old)
    texture_cache_remove_entry(cache, old, FALSE);

  /* Zero disables this memory tier.  Oversized single entries are delivered
   * but never allowed to break the class budget. */
  if (cache->stats.budget_bytes == 0 || bytes > cache->stats.budget_bytes)
    return;

  /* Evict before taking the cache's reference so resident accounting never
   * crosses the configured bound, even transiently. */
  while (cache->stats.resident_bytes > cache->stats.budget_bytes - bytes &&
         !g_queue_is_empty(&cache->lru)) {
    TextureCacheEntry *oldest = g_queue_peek_head(&cache->lru);
    texture_cache_remove_entry(cache, oldest, TRUE);
  }

  TextureCacheEntry *entry = g_new0(TextureCacheEntry, 1);
  entry->key = g_steal_pointer(&lookup_key);
  entry->url = g_strdup(url);
  entry->texture = g_object_ref(texture);
  entry->decoded_bytes = bytes;
  g_queue_push_tail(&cache->lru, entry);
  entry->lru_link = g_queue_peek_tail_link(&cache->lru);
  g_hash_table_insert(cache->entries, entry->key, entry);
  cache->stats.resident_bytes += bytes;
  cache->stats.peak_resident_bytes =
      MAX(cache->stats.peak_resident_bytes, cache->stats.resident_bytes);
  cache->stats.entries = g_hash_table_size(cache->entries);
  texture_cache_trim(cache);
}

static char *
operation_key(const char *url, PendingKind kind)
{
  return g_strdup_printf("%c\n%s", kind == PENDING_TEXTURE ? 'T' : 'M', url);
}

static void
negative_entry_free(NegativeEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->key);
  g_free(entry);
}

static void
negative_remove_entry(GnostrMediaService *self, NegativeEntry *entry)
{
  g_hash_table_remove(self->negative, entry->key);
  if (entry->lru_link)
    g_queue_delete_link(&self->negative_lru, entry->lru_link);
  negative_entry_free(entry);
}

static gboolean
negative_lookup(GnostrMediaService *self, const char *url, PendingKind kind)
{
  g_autofree char *key = operation_key(url, kind);
  NegativeEntry *entry = g_hash_table_lookup(self->negative, key);
  if (!entry)
    return FALSE;
  if (entry->expires_at <= g_get_monotonic_time()) {
    negative_remove_entry(self, entry);
    return FALSE;
  }

  self->negative_hits++;
  g_queue_unlink(&self->negative_lru, entry->lru_link);
  g_queue_push_tail_link(&self->negative_lru, entry->lru_link);
  return TRUE;
}

static void
negative_store(GnostrMediaService *self, const char *url, PendingKind kind)
{
  if (self->config.negative_cache_max_entries == 0 ||
      self->config.negative_cache_ttl_usec <= 0)
    return;

  g_autofree char *key = operation_key(url, kind);
  NegativeEntry *entry = g_hash_table_lookup(self->negative, key);
  if (entry) {
    entry->expires_at = g_get_monotonic_time() +
                        self->config.negative_cache_ttl_usec;
    g_queue_unlink(&self->negative_lru, entry->lru_link);
    g_queue_push_tail_link(&self->negative_lru, entry->lru_link);
    return;
  }

  entry = g_new0(NegativeEntry, 1);
  entry->key = g_steal_pointer(&key);
  entry->expires_at = g_get_monotonic_time() +
                      self->config.negative_cache_ttl_usec;
  g_queue_push_tail(&self->negative_lru, entry);
  entry->lru_link = g_queue_peek_tail_link(&self->negative_lru);
  g_hash_table_insert(self->negative, entry->key, entry);

  while (g_hash_table_size(self->negative) >
         self->config.negative_cache_max_entries) {
    NegativeEntry *oldest = g_queue_peek_head(&self->negative_lru);
    negative_remove_entry(self, oldest);
  }
}

static void
og_cache_entry_free(OgCacheEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->url);
  gnostr_og_metadata_unref(entry->metadata);
  g_free(entry);
}

static void
og_cache_remove_entry(GnostrMediaService *self,
                      OgCacheEntry *entry,
                      gboolean count_eviction)
{
  g_hash_table_remove(self->og_cache, entry->url);
  if (entry->lru_link)
    g_queue_delete_link(&self->og_lru, entry->lru_link);
  if (count_eviction)
    self->og_evictions++;
  og_cache_entry_free(entry);
}

static GnostrOgMetadata *
og_cache_lookup(GnostrMediaService *self, const char *url)
{
  OgCacheEntry *entry = g_hash_table_lookup(self->og_cache, url);
  if (!entry) {
    self->og_misses++;
    return NULL;
  }
  if (entry->expires_at <= g_get_monotonic_time()) {
    og_cache_remove_entry(self, entry, FALSE);
    self->og_misses++;
    return NULL;
  }

  self->og_hits++;
  g_queue_unlink(&self->og_lru, entry->lru_link);
  g_queue_push_tail_link(&self->og_lru, entry->lru_link);
  return gnostr_og_metadata_ref(entry->metadata);
}

static void
og_cache_store(GnostrMediaService *self,
               const char *url,
               GnostrOgMetadata *metadata)
{
  if (self->config.og_metadata_max_entries == 0 ||
      self->config.og_metadata_ttl_usec <= 0)
    return;

  OgCacheEntry *old = g_hash_table_lookup(self->og_cache, url);
  if (old)
    og_cache_remove_entry(self, old, FALSE);

  OgCacheEntry *entry = g_new0(OgCacheEntry, 1);
  entry->url = g_strdup(url);
  entry->metadata = gnostr_og_metadata_ref(metadata);
  entry->expires_at = g_get_monotonic_time() +
                      self->config.og_metadata_ttl_usec;
  g_queue_push_tail(&self->og_lru, entry);
  entry->lru_link = g_queue_peek_tail_link(&self->og_lru);
  g_hash_table_insert(self->og_cache, entry->url, entry);

  while (g_hash_table_size(self->og_cache) >
         self->config.og_metadata_max_entries) {
    OgCacheEntry *oldest = g_queue_peek_head(&self->og_lru);
    og_cache_remove_entry(self, oldest, TRUE);
  }
}

static Subscriber *
subscriber_ref(Subscriber *subscriber)
{
  g_atomic_ref_count_inc(&subscriber->ref_count);
  return subscriber;
}

static void
subscriber_unref(Subscriber *subscriber)
{
  if (!subscriber)
    return;
  if (!g_atomic_ref_count_dec(&subscriber->ref_count))
    return;

  if (subscriber->cancel_handler && subscriber->cancellable)
    g_cancellable_disconnect(subscriber->cancellable,
                             subscriber->cancel_handler);
  g_clear_object(&subscriber->cancellable);
  g_free(subscriber->url);
  if (subscriber->user_data_destroy)
    subscriber->user_data_destroy(subscriber->user_data);
  g_free(subscriber);
}

typedef struct {
  GnostrMediaService *service;
  Subscriber *subscriber;
} CancelDispatch;

static void
cancel_dispatch_free(CancelDispatch *dispatch)
{
  subscriber_unref(dispatch->subscriber);
  g_object_unref(dispatch->service);
  g_free(dispatch);
}

static gboolean
cancel_subscriber_on_context(gpointer data)
{
  CancelDispatch *dispatch = data;
  Subscriber *subscriber = dispatch->subscriber;
  g_autoptr(GError) error =
      g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                          "Media request cancelled");

  if (!subscriber->completed) {
    if (subscriber->kind == PENDING_TEXTURE)
      subscriber_complete_texture(subscriber, NULL, error);
    else
      subscriber_complete_og(subscriber, NULL, error);

    PendingRequest *request = subscriber->pending;
    if (request) {
      gboolean all_done = TRUE;
      for (guint i = 0; i < request->subscribers->len; i++) {
        Subscriber *other = g_ptr_array_index(request->subscribers, i);
        if (!other->completed) {
          all_done = FALSE;
          break;
        }
      }
      if (all_done) {
        if (request->queued) {
          g_queue_remove(&request->service->download_queue, request);
          request->queued = FALSE;
          request->network_done = TRUE;
          pending_maybe_finish(request);
          start_queued_downloads(dispatch->service);
        } else if (request->network_active) {
          g_cancellable_cancel(request->network_cancellable);
        }
      }
    }
  }

  return G_SOURCE_REMOVE;
}

static void
subscriber_cancelled(GCancellable *cancellable, gpointer user_data)
{
  (void)cancellable;
  Subscriber *subscriber = user_data;
  if (!g_atomic_int_compare_and_exchange(&subscriber->cancel_scheduled, 0, 1))
    return;

  GnostrMediaService *service = subscriber->service;
  if (!service)
    return;

  CancelDispatch *dispatch = g_new0(CancelDispatch, 1);
  dispatch->service = g_object_ref(service);
  dispatch->subscriber = subscriber_ref(subscriber);
  /* Always defer: g_main_context_invoke_full() is allowed to invoke inline
   * when the caller can acquire the context.  Inline completion would call
   * g_cancellable_disconnect() from inside this cancellable's own handler. */
  GSource *source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_callback(source, cancel_subscriber_on_context, dispatch,
                        (GDestroyNotify)cancel_dispatch_free);
  g_source_attach(source, service->context);
  g_source_unref(source);
}

static Subscriber *
subscriber_new(GnostrMediaService *service,
               const char *url,
               PendingKind kind,
               GCancellable *cancellable,
               gpointer user_data,
               GDestroyNotify user_data_destroy)
{
  Subscriber *subscriber = g_new0(Subscriber, 1);
  g_atomic_ref_count_init(&subscriber->ref_count);
  subscriber->service = service;
  subscriber->url = g_strdup(url);
  subscriber->kind = kind;
  subscriber->user_data = user_data;
  subscriber->user_data_destroy = user_data_destroy;
  if (cancellable) {
    subscriber->cancellable = g_object_ref(cancellable);
    subscriber->cancel_handler =
        g_cancellable_connect(cancellable, G_CALLBACK(subscriber_cancelled),
                              subscriber, NULL);
  }
  return subscriber;
}

static void
subscriber_disconnect_cancel(Subscriber *subscriber)
{
  if (subscriber->cancel_handler && subscriber->cancellable) {
    g_cancellable_disconnect(subscriber->cancellable,
                             subscriber->cancel_handler);
    subscriber->cancel_handler = 0;
  }
}

static void
subscriber_complete_texture(Subscriber *subscriber,
                            GdkTexture *texture,
                            const GError *error)
{
  if (subscriber->completed)
    return;
  subscriber->completed = TRUE;
  subscriber_disconnect_cancel(subscriber);
  if (subscriber->callback.texture) {
    subscriber->callback.texture(subscriber->service, subscriber->url,
                                 texture, error,
                                 subscriber->user_data);
  }
}

static void
subscriber_complete_og(Subscriber *subscriber,
                       GnostrOgMetadata *metadata,
                       const GError *error)
{
  if (subscriber->completed)
    return;
  subscriber->completed = TRUE;
  subscriber_disconnect_cancel(subscriber);
  if (subscriber->callback.og) {
    subscriber->callback.og(subscriber->service, subscriber->url,
                            metadata, error,
                            subscriber->user_data);
  }
}

typedef struct {
  GnostrMediaService *service;
  Subscriber *subscriber;
  char *url;
  GdkTexture *texture;
  GnostrOgMetadata *metadata;
  GError *error;
} DeferredDelivery;

static void
deferred_delivery_free(DeferredDelivery *delivery)
{
  subscriber_unref(delivery->subscriber);
  g_clear_object(&delivery->texture);
  gnostr_og_metadata_unref(delivery->metadata);
  g_clear_error(&delivery->error);
  g_free(delivery->url);
  g_object_unref(delivery->service);
  g_free(delivery);
}

static gboolean
deliver_deferred(gpointer data)
{
  DeferredDelivery *delivery = data;
  /* Cached/validation deliveries are not attached to a PendingRequest. */
  if (!delivery->subscriber->completed) {
    if (delivery->subscriber->kind == PENDING_TEXTURE) {
      delivery->subscriber->completed = TRUE;
      subscriber_disconnect_cancel(delivery->subscriber);
      delivery->subscriber->callback.texture(delivery->service, delivery->url,
                                             delivery->texture,
                                             delivery->error,
                                             delivery->subscriber->user_data);
    } else {
      delivery->subscriber->completed = TRUE;
      subscriber_disconnect_cancel(delivery->subscriber);
      delivery->subscriber->callback.og(delivery->service, delivery->url,
                                        delivery->metadata, delivery->error,
                                        delivery->subscriber->user_data);
    }
  }
  return G_SOURCE_REMOVE;
}

static void
schedule_delivery(GnostrMediaService *self,
                  Subscriber *subscriber,
                  const char *url,
                  GdkTexture *texture,
                  GnostrOgMetadata *metadata,
                  GError *error)
{
  DeferredDelivery *delivery = g_new0(DeferredDelivery, 1);
  delivery->service = g_object_ref(self);
  delivery->subscriber = subscriber_ref(subscriber);
  delivery->url = g_strdup(url);
  delivery->texture = texture ? g_object_ref(texture) : NULL;
  delivery->metadata = metadata ? gnostr_og_metadata_ref(metadata) : NULL;
  delivery->error = error;

  GSource *source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_callback(source, deliver_deferred, delivery,
                        (GDestroyNotify)deferred_delivery_free);
  g_source_attach(source, self->context);
  g_source_unref(source);
}

static gboolean
pending_all_completed(PendingRequest *request)
{
  for (guint i = 0; i < request->subscribers->len; i++) {
    Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
    if (!subscriber->completed)
      return FALSE;
  }
  return TRUE;
}

static void
decode_job_free(DecodeJob *job)
{
  if (!job)
    return;
  g_free(job->variant_key);
  g_free(job);
}

static void
pending_free(PendingRequest *request)
{
  if (!request)
    return;
  for (guint i = 0; i < request->subscribers->len; i++) {
    Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
    subscriber->pending = NULL;
  }
  g_ptr_array_unref(request->subscribers);
  g_clear_object(&request->network_cancellable);
  g_clear_pointer(&request->body, g_bytes_unref);
  g_clear_pointer(&request->decode_variants, g_hash_table_unref);
#ifdef HAVE_SOUP3
  g_clear_object(&request->message);
  g_clear_object(&request->stream);
  if (request->download)
    g_byte_array_unref(request->download);
#endif
  g_free(request->table_key);
  g_free(request->url);
  g_object_unref(request->service);
  g_free(request);
}

static void
pending_maybe_finish(PendingRequest *request)
{
  if (!request->network_done || request->outstanding_workers != 0 ||
      !pending_all_completed(request))
    return;

  GnostrMediaService *self = request->service;
  g_hash_table_remove(self->pending, request->table_key);
  pending_free(request);
}

static void
pending_network_done(PendingRequest *request)
{
  if (request->network_done)
    return;
  request->network_done = TRUE;
  if (request->network_active) {
    request->network_active = FALSE;
    g_assert(request->service->active_downloads > 0);
    request->service->active_downloads--;
    start_queued_downloads(request->service);
  }
}

static void
pending_fail(PendingRequest *request,
             GError *error,
             gboolean negative)
{
  pending_network_done(request);
  if (negative)
    negative_store(request->service, request->url, request->kind);

  for (guint i = 0; i < request->subscribers->len; i++) {
    Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
    if (subscriber->kind == PENDING_TEXTURE)
      subscriber_complete_texture(subscriber, NULL, error);
    else
      subscriber_complete_og(subscriber, NULL, error);
  }
  g_error_free(error);
  pending_maybe_finish(request);
}

typedef struct {
  GBytes *bytes;
  int target_width;
  int target_height;
} TextureTaskData;

static void
texture_task_data_free(TextureTaskData *data)
{
  g_bytes_unref(data->bytes);
  g_free(data);
}

static void
decode_texture_worker(GTask *task,
                      gpointer source_object,
                      gpointer task_data,
                      GCancellable *cancellable)
{
  (void)source_object;
  TextureTaskData *data = task_data;
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Texture decode cancelled");
    return;
  }

  g_autoptr(GInputStream) stream =
      g_memory_input_stream_new_from_bytes(data->bytes);
  g_autoptr(GError) error = NULL;
  g_autoptr(GdkPixbuf) pixbuf =
      gdk_pixbuf_new_from_stream_at_scale(stream,
                                          data->target_width,
                                          data->target_height,
                                          TRUE,
                                          cancellable,
                                          &error);
  if (!pixbuf) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  int width = gdk_pixbuf_get_width(pixbuf);
  int height = gdk_pixbuf_get_height(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
  gsize pixel_bytes = (gsize)rowstride * (gsize)height;
  g_autoptr(GBytes) pixels =
      g_bytes_new(gdk_pixbuf_get_pixels(pixbuf), pixel_bytes);
  GdkTexture *texture = GDK_TEXTURE(gdk_memory_texture_new(
      width, height,
      has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
      pixels, (gsize)rowstride));
  if (!texture) {
    g_task_return_new_error(task, GNOSTR_MEDIA_ERROR,
                            GNOSTR_MEDIA_ERROR_DECODE,
                            "Failed to create texture");
    return;
  }
  g_task_return_pointer(task, texture, g_object_unref);
}

static gboolean
subscriber_matches_job(Subscriber *subscriber, DecodeJob *job)
{
  return subscriber->kind == PENDING_TEXTURE &&
         subscriber->resource_class == job->resource_class &&
         subscriber->target_width == job->target_width &&
         subscriber->target_height == job->target_height;
}

static void
decode_texture_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  DecodeJob *job = user_data;
  PendingRequest *request = job->request;
  GnostrMediaService *self = request->service;
  g_autoptr(GError) error = NULL;
  GdkTexture *texture = g_task_propagate_pointer(G_TASK(result), &error);

  g_hash_table_remove(request->decode_variants, job->variant_key);
  if (texture) {
    request->any_worker_success = TRUE;
    texture_cache_store(self, job->resource_class, request->url,
                        job->target_width, job->target_height, texture);
  } else {
    request->any_worker_failure = TRUE;
  }

  for (guint i = 0; i < request->subscribers->len; i++) {
    Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
    if (!subscriber->completed && subscriber_matches_job(subscriber, job))
      subscriber_complete_texture(subscriber, texture, error);
  }

  if (texture)
    g_object_unref(texture);
  g_assert(request->outstanding_workers > 0);
  request->outstanding_workers--;
  decode_job_free(job);

  if (request->outstanding_workers == 0 &&
      request->any_worker_failure && !request->any_worker_success)
    negative_store(self, request->url, request->kind);
  pending_maybe_finish(request);
}

static void
schedule_texture_decode(PendingRequest *request, Subscriber *subscriber)
{
  g_autofree char *variant =
      g_strdup_printf("%u:%dx%d", subscriber->resource_class,
                      subscriber->target_width, subscriber->target_height);
  if (g_hash_table_contains(request->decode_variants, variant))
    return;

  DecodeJob *job = g_new0(DecodeJob, 1);
  job->request = request;
  job->variant_key = g_steal_pointer(&variant);
  job->resource_class = subscriber->resource_class;
  job->target_width = subscriber->target_width;
  job->target_height = subscriber->target_height;
  g_hash_table_insert(request->decode_variants, job->variant_key, job);
  request->outstanding_workers++;

  TextureTaskData *task_data = g_new0(TextureTaskData, 1);
  task_data->bytes = g_bytes_ref(request->body);
  task_data->target_width = job->target_width;
  task_data->target_height = job->target_height;

  GTask *task = g_task_new(NULL, request->network_cancellable,
                           decode_texture_done, job);
  g_task_set_task_data(task, task_data,
                       (GDestroyNotify)texture_task_data_free);
  g_task_run_in_thread(task, decode_texture_worker);
  g_object_unref(task);
}

static void
process_texture_body(PendingRequest *request)
{
  gsize body_size = g_bytes_get_size(request->body);
  for (guint i = 0; i < request->subscribers->len; i++) {
    Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
    if (subscriber->completed)
      continue;
    if (body_size > subscriber->body_cap) {
      g_autoptr(GError) error =
          g_error_new(GNOSTR_MEDIA_ERROR, GNOSTR_MEDIA_ERROR_TOO_LARGE,
                      "Response is %" G_GSIZE_FORMAT
                      " bytes; class cap is %" G_GSIZE_FORMAT,
                      body_size, subscriber->body_cap);
      subscriber_complete_texture(subscriber, NULL, error);
      request->any_worker_failure = TRUE;
      continue;
    }
    schedule_texture_decode(request, subscriber);
  }

  if (request->outstanding_workers == 0) {
    if (request->any_worker_failure)
      negative_store(request->service, request->url, request->kind);
    pending_maybe_finish(request);
  }
}

static const char *
ascii_strcasestr(const char *haystack, const char *needle)
{
  if (!haystack || !needle || !*needle)
    return haystack;
  gsize needle_len = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    if (g_ascii_strncasecmp(p, needle, needle_len) == 0)
      return p;
  }
  return NULL;
}

static char *
html_attr_value(const char *tag_start, const char *tag_end, const char *name)
{
  const char *p = tag_start;
  gsize name_len = strlen(name);
  while (p && p < tag_end) {
    p = ascii_strcasestr(p, name);
    if (!p || p >= tag_end)
      return NULL;
    if ((p == tag_start || !g_ascii_isalnum(*(p - 1))) &&
        p + name_len < tag_end &&
        !g_ascii_isalnum(p[name_len]) && p[name_len] != '-') {
      const char *q = p + name_len;
      while (q < tag_end && g_ascii_isspace(*q))
        q++;
      if (q < tag_end && *q == '=') {
        q++;
        while (q < tag_end && g_ascii_isspace(*q))
          q++;
        if (q >= tag_end)
          return NULL;
        if (*q == '\'' || *q == '"') {
          char quote = *q++;
          const char *end = memchr(q, quote, tag_end - q);
          return end ? g_strndup(q, end - q) : NULL;
        }
        const char *end = q;
        while (end < tag_end && !g_ascii_isspace(*end) && *end != '>')
          end++;
        return g_strndup(q, end - q);
      }
    }
    p += name_len;
  }
  return NULL;
}

static char *
extract_meta_content(const char *html, const char *wanted)
{
  const char *p = html;
  while ((p = ascii_strcasestr(p, "<meta")) != NULL) {
    const char *end = strchr(p, '>');
    if (!end)
      break;
    g_autofree char *property = html_attr_value(p, end, "property");
    if (!property)
      property = html_attr_value(p, end, "name");
    if (property && g_ascii_strcasecmp(property, wanted) == 0)
      return html_attr_value(p, end, "content");
    p = end + 1;
  }
  return NULL;
}

static char *
extract_html_title(const char *html)
{
  const char *start = ascii_strcasestr(html, "<title");
  if (!start)
    return NULL;
  start = strchr(start, '>');
  if (!start)
    return NULL;
  start++;
  const char *end = ascii_strcasestr(start, "</title>");
  if (!end)
    return NULL;
  char *title = g_strndup(start, end - start);
  g_strstrip(title);
  if (!*title) {
    g_free(title);
    return NULL;
  }
  return title;
}

typedef struct {
  GBytes *bytes;
  char *url;
} OgTaskData;

static void
og_task_data_free(OgTaskData *data)
{
  g_bytes_unref(data->bytes);
  g_free(data->url);
  g_free(data);
}

static void
parse_og_worker(GTask *task,
                gpointer source_object,
                gpointer task_data,
                GCancellable *cancellable)
{
  (void)source_object;
  OgTaskData *data = task_data;
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Open Graph parse cancelled");
    return;
  }

  gsize length = 0;
  const char *raw = g_bytes_get_data(data->bytes, &length);
  g_autofree char *html = g_strndup(raw, length);
  GnostrOgMetadata *metadata = g_new0(GnostrOgMetadata, 1);
  g_atomic_ref_count_init(&metadata->ref_count);
  metadata->source_url = g_strdup(data->url);
  metadata->title = extract_meta_content(html, "og:title");
  if (!metadata->title)
    metadata->title = extract_meta_content(html, "twitter:title");
  if (!metadata->title)
    metadata->title = extract_html_title(html);
  metadata->description = extract_meta_content(html, "og:description");
  if (!metadata->description)
    metadata->description = extract_meta_content(html, "twitter:description");
  if (!metadata->description)
    metadata->description = extract_meta_content(html, "description");
  metadata->image_url = extract_meta_content(html, "og:image");
  if (!metadata->image_url)
    metadata->image_url = extract_meta_content(html, "twitter:image");

  if (metadata->image_url && *metadata->image_url) {
    g_autoptr(GError) resolve_error = NULL;
    char *absolute = g_uri_resolve_relative(data->url, metadata->image_url,
                                            G_URI_FLAGS_PARSE_RELAXED,
                                            &resolve_error);
    if (absolute) {
      g_free(metadata->image_url);
      metadata->image_url = absolute;
    }
  }

  if ((!metadata->title || !*metadata->title) &&
      (!metadata->description || !*metadata->description) &&
      (!metadata->image_url || !*metadata->image_url)) {
    gnostr_og_metadata_unref(metadata);
    g_task_return_new_error(task, GNOSTR_MEDIA_ERROR,
                            GNOSTR_MEDIA_ERROR_DECODE,
                            "Response contains no Open Graph metadata");
    return;
  }
  g_task_return_pointer(task, metadata,
                        (GDestroyNotify)gnostr_og_metadata_unref);
}

static void
parse_og_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  PendingRequest *request = user_data;
  GnostrMediaService *self = request->service;
  g_autoptr(GError) error = NULL;
  GnostrOgMetadata *metadata =
      g_task_propagate_pointer(G_TASK(result), &error);

  if (metadata)
    og_cache_store(self, request->url, metadata);
  else
    negative_store(self, request->url, request->kind);

  for (guint i = 0; i < request->subscribers->len; i++) {
    Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
    if (!subscriber->completed)
      subscriber_complete_og(subscriber, metadata, error);
  }
  if (metadata)
    gnostr_og_metadata_unref(metadata);

  g_assert(request->outstanding_workers > 0);
  request->outstanding_workers--;
  pending_maybe_finish(request);
}

static void
process_og_body(PendingRequest *request)
{
  if (g_bytes_get_size(request->body) >
      request->service->config.og_metadata_body_size_cap) {
    pending_fail(request,
                 g_error_new(GNOSTR_MEDIA_ERROR,
                             GNOSTR_MEDIA_ERROR_TOO_LARGE,
                             "Open Graph response exceeds %" G_GSIZE_FORMAT
                             " byte cap",
                             request->service->config.og_metadata_body_size_cap),
                 TRUE);
    return;
  }

  OgTaskData *data = g_new0(OgTaskData, 1);
  data->bytes = g_bytes_ref(request->body);
  data->url = g_strdup(request->url);
  request->outstanding_workers++;
  GTask *task = g_task_new(NULL, request->network_cancellable,
                           parse_og_done, request);
  g_task_set_task_data(task, data, (GDestroyNotify)og_task_data_free);
  g_task_run_in_thread(task, parse_og_worker);
  g_object_unref(task);
}

static void
process_downloaded_body(PendingRequest *request)
{
  if (request->kind == PENDING_TEXTURE)
    process_texture_body(request);
  else
    process_og_body(request);
}

#ifdef HAVE_SOUP3
static void read_response_chunk(GObject *source, GAsyncResult *result,
                                gpointer user_data);

static void
queue_response_read(PendingRequest *request)
{
  g_input_stream_read_bytes_async(request->stream, READ_CHUNK_SIZE,
                                  G_PRIORITY_DEFAULT,
                                  request->network_cancellable,
                                  read_response_chunk, request);
}

static void
read_response_chunk(GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
  GInputStream *stream = G_INPUT_STREAM(source);
  PendingRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  GBytes *chunk = g_input_stream_read_bytes_finish(stream, result, &error);
  if (!chunk) {
    gboolean cancelled =
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    pending_fail(request, g_steal_pointer(&error), !cancelled);
    return;
  }

  gsize size = 0;
  const guint8 *data = g_bytes_get_data(chunk, &size);
  if (size == 0) {
    g_bytes_unref(chunk);
    request->body =
        g_byte_array_free_to_bytes(g_steal_pointer(&request->download));
    pending_network_done(request);
    process_downloaded_body(request);
    return;
  }

  if (size > request->download_cap ||
      request->download->len > request->download_cap - size) {
    g_bytes_unref(chunk);
    pending_fail(request,
                 g_error_new(GNOSTR_MEDIA_ERROR,
                             GNOSTR_MEDIA_ERROR_TOO_LARGE,
                             "Response exceeds %" G_GSIZE_FORMAT
                             " byte download cap", request->download_cap),
                 TRUE);
    return;
  }

  g_byte_array_append(request->download, data, size);
  g_bytes_unref(chunk);
  queue_response_read(request);
}

static void
response_headers_ready(GObject *source,
                       GAsyncResult *result,
                       gpointer user_data)
{
  PendingRequest *request = user_data;
  g_autoptr(GError) error = NULL;
  request->stream =
      soup_session_send_finish(SOUP_SESSION(source), result, &error);
  if (!request->stream) {
    gboolean cancelled =
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    pending_fail(request, g_steal_pointer(&error), !cancelled);
    return;
  }

  guint status = soup_message_get_status(request->message);
  if (status < 200 || status >= 300) {
    pending_fail(request,
                 g_error_new(GNOSTR_MEDIA_ERROR, GNOSTR_MEDIA_ERROR_HTTP,
                             "HTTP status %u", status),
                 TRUE);
    return;
  }

  goffset content_length = soup_message_headers_get_content_length(
      soup_message_get_response_headers(request->message));
  if (content_length > 0 && (guint64)content_length > request->download_cap) {
    pending_fail(request,
                 g_error_new(GNOSTR_MEDIA_ERROR,
                             GNOSTR_MEDIA_ERROR_TOO_LARGE,
                             "Content-Length %" G_GOFFSET_FORMAT
                             " exceeds %" G_GSIZE_FORMAT " byte cap",
                             content_length, request->download_cap),
                 TRUE);
    return;
  }

  request->download = g_byte_array_sized_new(
      content_length > 0
          ? (guint)MIN((guint64)content_length, (guint64)G_MAXUINT)
          : READ_CHUNK_SIZE);
  queue_response_read(request);
}
#endif

static void
start_request_download(PendingRequest *request)
{
  GnostrMediaService *self = request->service;
  request->queued = FALSE;
  request->network_active = TRUE;
  self->active_downloads++;

#ifdef HAVE_SOUP3
  if (!gnostr_is_remote_media_allowed()) {
    pending_fail(request,
                 g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                     GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                                     "Remote media loading is disabled"),
                 FALSE);
    return;
  }

  SoupSession *session = gnostr_get_shared_soup_session();
  if (!session) {
    pending_fail(request,
                 g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                     GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                                     "Shared HTTP session is unavailable"),
                 FALSE);
    return;
  }

  request->message = soup_message_new("GET", request->url);
  if (!request->message) {
    pending_fail(request,
                 g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                     GNOSTR_MEDIA_ERROR_INVALID_ARGUMENT,
                                     "Invalid media URL"),
                 FALSE);
    return;
  }
  soup_message_set_priority(request->message,
                            request->kind == PENDING_OG_METADATA
                                ? SOUP_MESSAGE_PRIORITY_LOW
                                : SOUP_MESSAGE_PRIORITY_NORMAL);
  soup_session_send_async(session, request->message, G_PRIORITY_DEFAULT,
                          request->network_cancellable,
                          response_headers_ready, request);
#else
  pending_fail(request,
               g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                   GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                                   "This build has no libsoup support"),
               FALSE);
#endif
}

static void
start_queued_downloads(GnostrMediaService *self)
{
  while (self->active_downloads < self->config.max_concurrent_downloads &&
         !g_queue_is_empty(&self->download_queue)) {
    PendingRequest *request = g_queue_pop_head(&self->download_queue);
    if (!pending_all_completed(request))
      start_request_download(request);
    else {
      request->queued = FALSE;
      request->network_done = TRUE;
      pending_maybe_finish(request);
    }
  }
}

static PendingRequest *
pending_request_new(GnostrMediaService *self,
                    const char *url,
                    PendingKind kind)
{
  PendingRequest *request = g_new0(PendingRequest, 1);
  request->service = g_object_ref(self);
  request->url = g_strdup(url);
  request->kind = kind;
  request->table_key = operation_key(url, kind);
  request->subscribers =
      g_ptr_array_new_with_free_func((GDestroyNotify)subscriber_unref);
  request->network_cancellable = g_cancellable_new();
  request->decode_variants = g_hash_table_new(g_str_hash, g_str_equal);
  request->queued = TRUE;
  return request;
}

static gboolean
valid_http_url(const char *url)
{
  if (!url || !*url)
    return FALSE;
  const char *scheme = g_uri_peek_scheme(url);
  return scheme &&
         (g_ascii_strcasecmp(scheme, "http") == 0 ||
          g_ascii_strcasecmp(scheme, "https") == 0);
}

static void
attach_subscriber(PendingRequest *request, Subscriber *subscriber)
{
  subscriber->pending = request;
  g_ptr_array_add(request->subscribers, subscriber_ref(subscriber));
  request->download_cap = MAX(request->download_cap, subscriber->body_cap);

  /* A subscriber can arrive after the body has downloaded but before another
   * variant's worker completes. */
  if (request->network_done && request->body &&
      request->kind == PENDING_TEXTURE &&
      !subscriber->completed) {
    if (g_bytes_get_size(request->body) > subscriber->body_cap) {
      g_autoptr(GError) error =
          g_error_new_literal(GNOSTR_MEDIA_ERROR,
                              GNOSTR_MEDIA_ERROR_TOO_LARGE,
                              "Downloaded image exceeds class body cap");
      subscriber_complete_texture(subscriber, NULL, error);
    } else {
      schedule_texture_decode(request, subscriber);
    }
  }
}

static gboolean
reject_common(GnostrMediaService *self,
              Subscriber *subscriber,
              const char *url,
              PendingKind kind)
{
  if (!valid_http_url(url)) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                          GNOSTR_MEDIA_ERROR_INVALID_ARGUMENT,
                                          "Only http and https URLs are supported"));
    return TRUE;
  }
  if (subscriber->cancellable &&
      g_cancellable_is_cancelled(subscriber->cancellable)) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                          "Media request cancelled"));
    return TRUE;
  }
  if (negative_lookup(self, url, kind)) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                          GNOSTR_MEDIA_ERROR_NEGATIVE_CACHED,
                                          "URL is temporarily negative-cached"));
    return TRUE;
  }
  return FALSE;
}

void
gnostr_media_service_request_texture(GnostrMediaService *self,
                                     const char *url,
                                     GnostrMediaResourceClass resource_class,
                                     int target_width,
                                     int target_height,
                                     GCancellable *cancellable,
                                     GnostrMediaTextureCallback callback,
                                     gpointer user_data,
                                     GDestroyNotify user_data_destroy)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  if (!callback) {
    if (user_data_destroy)
      user_data_destroy(user_data);
    return;
  }

  Subscriber *subscriber =
      subscriber_new(self, url, PENDING_TEXTURE, cancellable, user_data,
                     user_data_destroy);
  subscriber->callback.texture = callback;
  subscriber->resource_class = resource_class;
  subscriber->target_width = target_width;
  subscriber->target_height = target_height;

  if (resource_class < 0 ||
      resource_class >= GNOSTR_MEDIA_RESOURCE_N_CLASSES ||
      target_width <= 0 || target_height <= 0 ||
      target_width > MAX_TARGET_DIMENSION ||
      target_height > MAX_TARGET_DIMENSION) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                          GNOSTR_MEDIA_ERROR_INVALID_ARGUMENT,
                                          "Invalid class or target dimensions"));
    subscriber_unref(subscriber);
    return;
  }
  subscriber->body_cap = self->config.body_size_caps[resource_class];

  if (!valid_http_url(url)) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                          GNOSTR_MEDIA_ERROR_INVALID_ARGUMENT,
                                          "Only http and https URLs are supported"));
    subscriber_unref(subscriber);
    return;
  }

  GdkTexture *cached =
      texture_cache_lookup(self, resource_class, url,
                           target_width, target_height);
  if (cached) {
    schedule_delivery(self, subscriber, url, cached, NULL, NULL);
    g_object_unref(cached);
    subscriber_unref(subscriber);
    return;
  }

  if (reject_common(self, subscriber, url, PENDING_TEXTURE)) {
    subscriber_unref(subscriber);
    return;
  }

  g_autofree char *key = operation_key(url, PENDING_TEXTURE);
  PendingRequest *request = g_hash_table_lookup(self->pending, key);
  if (request) {
    attach_subscriber(request, subscriber);
    subscriber_unref(subscriber);
    return;
  }

  if (g_hash_table_size(self->pending) >= self->config.max_in_flight) {
    self->overflow_rejections++;
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                          GNOSTR_MEDIA_ERROR_OVERFLOW,
                                          "Media in-flight request cap reached"));
    subscriber_unref(subscriber);
    return;
  }

  request = pending_request_new(self, url, PENDING_TEXTURE);
  attach_subscriber(request, subscriber);
  g_hash_table_insert(self->pending, request->table_key, request);
  g_queue_push_tail(&self->download_queue, request);
  start_queued_downloads(self);
  subscriber_unref(subscriber);
}

void
gnostr_media_service_request_og_metadata(GnostrMediaService *self,
                                         const char *url,
                                         GCancellable *cancellable,
                                         GnostrMediaOgCallback callback,
                                         gpointer user_data,
                                         GDestroyNotify user_data_destroy)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  if (!callback) {
    if (user_data_destroy)
      user_data_destroy(user_data);
    return;
  }

  Subscriber *subscriber =
      subscriber_new(self, url, PENDING_OG_METADATA, cancellable, user_data,
                     user_data_destroy);
  subscriber->callback.og = callback;
  subscriber->body_cap = self->config.og_metadata_body_size_cap;

  GnostrOgMetadata *cached = valid_http_url(url)
      ? og_cache_lookup(self, url) : NULL;
  if (cached) {
    schedule_delivery(self, subscriber, url, NULL, cached, NULL);
    gnostr_og_metadata_unref(cached);
    subscriber_unref(subscriber);
    return;
  }

  if (reject_common(self, subscriber, url, PENDING_OG_METADATA)) {
    subscriber_unref(subscriber);
    return;
  }

  g_autofree char *key = operation_key(url, PENDING_OG_METADATA);
  PendingRequest *request = g_hash_table_lookup(self->pending, key);
  if (request) {
    attach_subscriber(request, subscriber);
    subscriber_unref(subscriber);
    return;
  }

  if (g_hash_table_size(self->pending) >= self->config.max_in_flight) {
    self->overflow_rejections++;
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                          GNOSTR_MEDIA_ERROR_OVERFLOW,
                                          "Media in-flight request cap reached"));
    subscriber_unref(subscriber);
    return;
  }

  request = pending_request_new(self, url, PENDING_OG_METADATA);
  attach_subscriber(request, subscriber);
  g_hash_table_insert(self->pending, request->table_key, request);
  g_queue_push_tail(&self->download_queue, request);
  start_queued_downloads(self);
  subscriber_unref(subscriber);
}

static guint64
budget_from_settings(GSettings *settings,
                     const char *key,
                     guint64 fallback)
{
  gint mb = g_settings_get_int(settings, key);
  if (mb < 0)
    return fallback;
  return (guint64)mb * MIB;
}

static void
reload_settings_budgets(GnostrMediaService *self)
{
  if (!self->settings)
    return;
  self->texture_caches[GNOSTR_MEDIA_RESOURCE_INLINE].stats.budget_bytes =
      budget_from_settings(self->settings, INLINE_BUDGET_KEY,
                           DEFAULT_INLINE_BUDGET);
  self->texture_caches[GNOSTR_MEDIA_RESOURCE_OG_IMAGE].stats.budget_bytes =
      budget_from_settings(self->settings, OG_IMAGE_BUDGET_KEY,
                           DEFAULT_OG_IMAGE_BUDGET);
  self->texture_caches[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER].stats.budget_bytes =
      budget_from_settings(self->settings, VIDEO_POSTER_BUDGET_KEY,
                           DEFAULT_VIDEO_POSTER_BUDGET);

  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++) {
    self->config.memory_budget_bytes[i] =
        self->texture_caches[i].stats.budget_bytes;
    texture_cache_trim(&self->texture_caches[i]);
  }
}

static void
settings_budget_changed(GSettings *settings, const char *key, gpointer user_data)
{
  (void)settings;
  (void)key;
  reload_settings_budgets(GNOSTR_MEDIA_SERVICE(user_data));
}

GnostrMediaService *
gnostr_media_service_new(const GnostrMediaServiceConfig *config)
{
  GnostrMediaService *self =
      g_object_new(GNOSTR_TYPE_MEDIA_SERVICE, NULL);
  if (config)
    self->config = *config;
  else
    gnostr_media_service_config_init(&self->config);

  self->config.max_in_flight = MAX(self->config.max_in_flight, 1);
  self->config.max_concurrent_downloads =
      CLAMP(self->config.max_concurrent_downloads, 1,
            self->config.max_in_flight);
  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++)
    self->texture_caches[i].stats.budget_bytes =
        self->config.memory_budget_bytes[i];
  return self;
}

GnostrMediaService *
gnostr_media_service_get_default(void)
{
  G_LOCK(default_service);
  if (!default_service) {
    default_service = gnostr_media_service_new(NULL);
    GSettingsSchemaSource *source =
        g_settings_schema_source_get_default();
    g_autoptr(GSettingsSchema) schema = source
        ? g_settings_schema_source_lookup(source, SETTINGS_SCHEMA, TRUE)
        : NULL;
    if (schema) {
      default_service->settings = g_settings_new(SETTINGS_SCHEMA);
      reload_settings_budgets(default_service);
      g_signal_connect(default_service->settings,
                       "changed::" INLINE_BUDGET_KEY,
                       G_CALLBACK(settings_budget_changed), default_service);
      g_signal_connect(default_service->settings,
                       "changed::" OG_IMAGE_BUDGET_KEY,
                       G_CALLBACK(settings_budget_changed), default_service);
      g_signal_connect(default_service->settings,
                       "changed::" VIDEO_POSTER_BUDGET_KEY,
                       G_CALLBACK(settings_budget_changed), default_service);
    }
  }
  G_UNLOCK(default_service);
  return default_service;
}

static void
purge_expired_negative(GnostrMediaService *self)
{
  gint64 now = g_get_monotonic_time();
  GList *link = self->negative_lru.head;
  while (link) {
    GList *next = link->next;
    NegativeEntry *entry = link->data;
    if (entry->expires_at <= now)
      negative_remove_entry(self, entry);
    link = next;
  }
}

static void
purge_expired_og(GnostrMediaService *self)
{
  gint64 now = g_get_monotonic_time();
  GList *link = self->og_lru.head;
  while (link) {
    GList *next = link->next;
    OgCacheEntry *entry = link->data;
    if (entry->expires_at <= now)
      og_cache_remove_entry(self, entry, FALSE);
    link = next;
  }
}

void
gnostr_media_service_get_stats(GnostrMediaService *self,
                               GnostrMediaCacheStats *out_stats)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(out_stats != NULL);
  purge_expired_negative(self);
  purge_expired_og(self);
  memset(out_stats, 0, sizeof(*out_stats));
  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++)
    out_stats->classes[i] = self->texture_caches[i].stats;
  out_stats->pending_requests = g_hash_table_size(self->pending);
  out_stats->queued_downloads = g_queue_get_length(&self->download_queue);
  out_stats->active_downloads = self->active_downloads;
  out_stats->negative_entries = g_hash_table_size(self->negative);
  out_stats->og_metadata_entries = g_hash_table_size(self->og_cache);
  out_stats->negative_hits = self->negative_hits;
  out_stats->overflow_rejections = self->overflow_rejections;
  out_stats->og_metadata_hits = self->og_hits;
  out_stats->og_metadata_misses = self->og_misses;
  out_stats->og_metadata_evictions = self->og_evictions;
}

guint
gnostr_media_service_evict_url(GnostrMediaService *self, const char *url)
{
  g_return_val_if_fail(GNOSTR_IS_MEDIA_SERVICE(self), 0);
  g_return_val_if_fail(url != NULL, 0);
  guint removed = 0;

  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++) {
    TextureClassCache *cache = &self->texture_caches[i];
    GList *link = cache->lru.head;
    while (link) {
      GList *next = link->next;
      TextureCacheEntry *entry = link->data;
      if (g_str_equal(entry->url, url)) {
        texture_cache_remove_entry(cache, entry, TRUE);
        removed++;
      }
      link = next;
    }
  }

  OgCacheEntry *og = g_hash_table_lookup(self->og_cache, url);
  if (og) {
    og_cache_remove_entry(self, og, TRUE);
    removed++;
  }

  for (guint kind = PENDING_TEXTURE; kind <= PENDING_OG_METADATA; kind++) {
    g_autofree char *key = operation_key(url, (PendingKind)kind);
    NegativeEntry *entry = g_hash_table_lookup(self->negative, key);
    if (entry) {
      negative_remove_entry(self, entry);
      removed++;
    }
  }
  return removed;
}

void
gnostr_media_service_clear_class(GnostrMediaService *self,
                                 GnostrMediaResourceClass resource_class)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(resource_class >= 0 &&
                   resource_class < GNOSTR_MEDIA_RESOURCE_N_CLASSES);
  TextureClassCache *cache = &self->texture_caches[resource_class];
  while (!g_queue_is_empty(&cache->lru)) {
    TextureCacheEntry *entry = g_queue_peek_head(&cache->lru);
    texture_cache_remove_entry(cache, entry, TRUE);
  }
}

void
gnostr_media_service_clear_all(GnostrMediaService *self)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++)
    gnostr_media_service_clear_class(self, i);
  while (!g_queue_is_empty(&self->negative_lru))
    negative_remove_entry(self, g_queue_peek_head(&self->negative_lru));
  while (!g_queue_is_empty(&self->og_lru))
    og_cache_remove_entry(self, g_queue_peek_head(&self->og_lru), TRUE);
}

#ifdef GNOSTR_MEDIA_SERVICE_TESTING
void
gnostr_media_service_test_store_texture(GnostrMediaService *self,
                                        const char *url,
                                        GnostrMediaResourceClass resource_class,
                                        int target_width,
                                        int target_height,
                                        GdkTexture *texture)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  texture_cache_store(self, resource_class, url, target_width, target_height,
                      texture);
}

void
gnostr_media_service_test_store_negative(GnostrMediaService *self,
                                         const char *url,
                                         gboolean metadata_request)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  negative_store(self, url,
                 metadata_request ? PENDING_OG_METADATA : PENDING_TEXTURE);
}

gboolean
gnostr_media_service_test_is_negative(GnostrMediaService *self,
                                      const char *url,
                                      gboolean metadata_request)
{
  g_return_val_if_fail(GNOSTR_IS_MEDIA_SERVICE(self), FALSE);
  return negative_lookup(self, url,
                         metadata_request ? PENDING_OG_METADATA
                                          : PENDING_TEXTURE);
}
#endif

static void
gnostr_media_service_dispose(GObject *object)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(object);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gnostr_media_service_parent_class)->dispose(object);
}

static void
gnostr_media_service_finalize(GObject *object)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(object);
  g_assert(g_hash_table_size(self->pending) == 0);
  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++) {
    while (!g_queue_is_empty(&self->texture_caches[i].lru))
      texture_cache_remove_entry(&self->texture_caches[i],
                                 g_queue_peek_head(&self->texture_caches[i].lru),
                                 FALSE);
    g_hash_table_unref(self->texture_caches[i].entries);
  }
  while (!g_queue_is_empty(&self->negative_lru))
    negative_remove_entry(self, g_queue_peek_head(&self->negative_lru));
  while (!g_queue_is_empty(&self->og_lru))
    og_cache_remove_entry(self, g_queue_peek_head(&self->og_lru), FALSE);
  g_hash_table_unref(self->pending);
  g_hash_table_unref(self->negative);
  g_hash_table_unref(self->og_cache);
  g_main_context_unref(self->context);
  G_OBJECT_CLASS(gnostr_media_service_parent_class)->finalize(object);
}

static void
gnostr_media_service_class_init(GnostrMediaServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_media_service_dispose;
  object_class->finalize = gnostr_media_service_finalize;
}

static void
gnostr_media_service_init(GnostrMediaService *self)
{
  self->context = g_main_context_ref_thread_default();
  self->pending = g_hash_table_new(g_str_hash, g_str_equal);
  self->negative = g_hash_table_new(g_str_hash, g_str_equal);
  self->og_cache = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++)
    self->texture_caches[i].entries =
        g_hash_table_new(g_str_hash, g_str_equal);
}
