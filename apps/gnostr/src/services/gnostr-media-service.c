#include "gnostr-media-service.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

#ifdef HAVE_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#endif

#ifndef GNOSTR_MEDIA_SERVICE_TESTING
#include <nostr-gobject-1.0/gnostr-identity.h>
#endif

#ifdef HAVE_SOUP3
#include <libsoup/soup.h>

/* Implemented by apps/gnostr/src/util/utils.c. */
SoupSession *gnostr_get_shared_soup_session(void);
gboolean gnostr_media_fetch_intent_is_allowed(GnostrMediaFetchIntent intent);
gboolean gnostr_media_url_is_safe(const char *url, GError **error);
gboolean gnostr_media_redirect_is_safe(const char *from_url,
                                       const char *location,
                                       char **out_url,
                                       GError **error);
#endif

#define MIB ((guint64)1024 * 1024)
#define DEFAULT_INLINE_BUDGET (500 * MIB)
#define DEFAULT_OG_IMAGE_BUDGET (64 * MIB)
#define DEFAULT_VIDEO_POSTER_BUDGET (64 * MIB)
#define DEFAULT_AVATAR_BUDGET (8 * MIB)
#define DEFAULT_INLINE_BODY_CAP ((gsize)24 * 1024 * 1024)
#define DEFAULT_OG_IMAGE_BODY_CAP ((gsize)12 * 1024 * 1024)
#define DEFAULT_VIDEO_POSTER_BODY_CAP ((gsize)12 * 1024 * 1024)
#define DEFAULT_AVATAR_BODY_CAP ((gsize)8 * 1024 * 1024)
#define DEFAULT_OG_METADATA_BODY_CAP ((gsize)2 * 1024 * 1024)
#define READ_CHUNK_SIZE ((gsize)64 * 1024)
#define MAX_TARGET_DIMENSION 4096
#define SETTINGS_SCHEMA "org.gnostr.Client"
#define INLINE_BUDGET_KEY "image-cache-max-mb"
#define OG_IMAGE_BUDGET_KEY "og-image-cache-max-mb"
#define VIDEO_POSTER_BUDGET_KEY "video-poster-cache-max-mb"
#define OG_METADATA_DIR "og-metadata"
#define OG_METADATA_VERSION 1
#define MAX_THUMBNAIL_CONCURRENCY 2
#define DEFAULT_THUMBNAIL_TIMEOUT_MSEC 10000
#define DEFAULT_DISK_WORKER_COUNT 2
#define DEFAULT_DISK_MAX_QUEUED_JOBS 32
#define DEFAULT_DISK_MAX_QUEUED_BYTES ((guint64)32 * MIB)
#define MAX_DISK_WORKER_COUNT 2

G_LOCK_DEFINE_STATIC(media_disk);

typedef enum {
  PENDING_TEXTURE,
  PENDING_OG_METADATA
} PendingKind;

typedef struct _PendingRequest PendingRequest;
typedef struct _Subscriber Subscriber;
typedef struct _DiskExecutorEntry DiskExecutorEntry;

typedef struct {
  char *key;
  char *url;
  char *cache_namespace;
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
  char *cache_namespace;
  char *url;
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
  char *key;
  char *url;
  char *cache_namespace;
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
  gatomicrefcount ref_count;
  GnostrMediaService *service; /* strong: keeps service alive through callbacks */
  char *table_key;
  char *url;
  PendingKind kind;
  GPtrArray *subscribers; /* Subscriber* */
  GCancellable *network_cancellable;
  gsize download_cap;
  gboolean queued;
  gboolean thumbnail_queued;
  gboolean thumbnail_active;
  gboolean network_active;
  gboolean network_done;
  guint outstanding_workers;
  gboolean any_worker_success;
  gboolean any_worker_failure;
  gboolean disk_lookup_active;
  gboolean body_from_disk;
  guint disk_write_classes;
  char *cache_namespace;
  guint64 namespace_epoch;
  GnostrMediaResourceClass disk_resource_class;
  GnostrMediaFetchIntent intent;
  guint redirect_count;
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
  GQueue thumbnail_queue; /* PendingRequest* */
  guint active_thumbnails;

  GHashTable *negative; /* NegativeEntry.key -> NegativeEntry */
  GQueue negative_lru;
  guint64 negative_hits;
  guint64 overflow_rejections;

  GHashTable *og_cache; /* OgCacheEntry.url -> OgCacheEntry */
  GQueue og_lru;
  guint64 og_hits;
  guint64 og_misses;
  guint64 og_evictions;

  char *disk_root;
  GHashTable *known_namespaces; /* namespace strings already scheduled */
  GHashTable *namespace_epochs; /* namespace -> guint64*, guarded by media_disk */
  GHashTable *disk_directory_states; /* directory -> DiskDirectoryState*, guarded by media_disk */
  guint outstanding_disk_jobs;
  GThreadPool *disk_pool;
  GQueue disk_queue; /* DiskExecutorEntry*; main-context-only */
  GHashTable *disk_coalesced; /* DiskExecutorEntry.key -> entry */
  guint active_disk_jobs;
  guint64 queued_disk_bytes;
  guint64 dropped_disk_jobs;
#ifdef GNOSTR_MEDIA_SERVICE_TESTING
  char *namespace_override;
  GnostrMediaTestThumbnailExtractor test_thumbnail_extractor;
  gpointer test_thumbnail_data;
  GDestroyNotify test_thumbnail_data_destroy;
#endif

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
static void schedule_disk_write(PendingRequest *request,
                                GnostrMediaResourceClass resource_class);
static void schedule_og_write(PendingRequest *request,
                              GnostrOgMetadata *metadata);
static void start_disk_lookup(PendingRequest *request);
static void ensure_namespace_sweep(GnostrMediaService *self,
                                   const char *cache_namespace);
static void start_queued_thumbnails(GnostrMediaService *self);
static void disk_executor_pool_worker(gpointer data, gpointer user_data);

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
  config->memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_AVATAR] = DEFAULT_AVATAR_BUDGET;
  config->body_size_caps[GNOSTR_MEDIA_RESOURCE_INLINE] = DEFAULT_INLINE_BODY_CAP;
  config->body_size_caps[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] = DEFAULT_OG_IMAGE_BODY_CAP;
  config->body_size_caps[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER] = DEFAULT_VIDEO_POSTER_BODY_CAP;
  config->body_size_caps[GNOSTR_MEDIA_RESOURCE_AVATAR] = DEFAULT_AVATAR_BODY_CAP;
  config->og_metadata_body_size_cap = DEFAULT_OG_METADATA_BODY_CAP;
  config->max_in_flight = 128;
  config->max_concurrent_downloads = 6;
  config->max_concurrent_thumbnails = MAX_THUMBNAIL_CONCURRENCY;
  config->thumbnail_timeout_msec = DEFAULT_THUMBNAIL_TIMEOUT_MSEC;
  config->negative_cache_max_entries = 256;
  config->negative_cache_ttl_usec = 60 * G_USEC_PER_SEC;
  config->og_metadata_max_entries = 256;
  config->og_metadata_ttl_usec = 30 * 60 * G_USEC_PER_SEC;
  config->disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_INLINE] = DEFAULT_INLINE_BUDGET;
  config->disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] = DEFAULT_OG_IMAGE_BUDGET;
  config->disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER] = DEFAULT_VIDEO_POSTER_BUDGET;
  config->disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_AVATAR] = DEFAULT_AVATAR_BUDGET;
  config->disk_worker_count = DEFAULT_DISK_WORKER_COUNT;
  config->disk_max_queued_jobs = DEFAULT_DISK_MAX_QUEUED_JOBS;
  config->disk_max_queued_bytes = DEFAULT_DISK_MAX_QUEUED_BYTES;
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

static const char *
disk_class_name(GnostrMediaResourceClass resource_class)
{
  static const char *names[GNOSTR_MEDIA_RESOURCE_N_CLASSES] = {
    "inline", "og-image", "video-poster", "avatar"
  };
  return resource_class >= 0 && resource_class < GNOSTR_MEDIA_RESOURCE_N_CLASSES
      ? names[resource_class] : NULL;
}

static char *
url_digest(const char *url)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
}

static char *
current_cache_namespace(GnostrMediaService *self)
{
#ifdef GNOSTR_MEDIA_SERVICE_TESTING
  if (self->namespace_override)
    return g_strdup(self->namespace_override);
#else
  GNostrIdentity *identity = gnostr_identity_get_current();
  if (identity && identity->npub && g_str_has_prefix(identity->npub, "npub1")) {
    gboolean safe = TRUE;
    for (const char *p = identity->npub; *p; p++) {
      if (!g_ascii_isalnum(*p)) {
        safe = FALSE;
        break;
      }
    }
    if (safe) {
      char *result = g_strdup(identity->npub);
      gnostr_identity_free(identity);
      return result;
    }
  }
  gnostr_identity_free(identity);
#endif
  return g_strdup("anon");
}

static gboolean
valid_account_namespace(const char *cache_namespace)
{
  if (!cache_namespace || !g_str_has_prefix(cache_namespace, "npub1"))
    return FALSE;
  for (const char *p = cache_namespace; *p; p++) {
    if (!g_ascii_isalnum(*p))
      return FALSE;
  }
  return TRUE;
}

static guint64
namespace_epoch_get_locked(GnostrMediaService *self,
                           const char *cache_namespace)
{
  guint64 *epoch = g_hash_table_lookup(self->namespace_epochs,
                                       cache_namespace);
  if (!epoch) {
    epoch = g_new0(guint64, 1);
    *epoch = 1;
    g_hash_table_insert(self->namespace_epochs, g_strdup(cache_namespace),
                        epoch);
  }
  return *epoch;
}

static guint64
namespace_epoch_get(GnostrMediaService *self, const char *cache_namespace)
{
  G_LOCK(media_disk);
  guint64 epoch = namespace_epoch_get_locked(self, cache_namespace);
  G_UNLOCK(media_disk);
  return epoch;
}

static gboolean
namespace_epoch_matches_locked(GnostrMediaService *self,
                               const char *cache_namespace,
                               guint64 epoch)
{
  return namespace_epoch_get_locked(self, cache_namespace) == epoch;
}

static void
remove_tree_locked(const char *path)
{
  g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
  if (dir) {
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
      g_autofree char *child = g_build_filename(path, name, NULL);
      if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
          !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
        remove_tree_locked(child);
      else
        g_unlink(child);
    }
  }
  g_rmdir(path);
}

static char *
disk_class_dir(GnostrMediaService *self,
               const char *cache_namespace,
               GnostrMediaResourceClass resource_class)
{
  return g_build_filename(self->disk_root, cache_namespace,
                          disk_class_name(resource_class), NULL);
}

static char *
disk_texture_path(GnostrMediaService *self,
                  const char *cache_namespace,
                  GnostrMediaResourceClass resource_class,
                  const char *url)
{
  g_autofree char *dir = disk_class_dir(self, cache_namespace, resource_class);
  g_autofree char *digest = url_digest(url);
  return g_build_filename(dir, digest, NULL);
}

static char *
disk_og_metadata_path(GnostrMediaService *self,
                      const char *cache_namespace,
                      const char *url)
{
  g_autofree char *digest = url_digest(url);
  return g_build_filename(self->disk_root, cache_namespace,
                          OG_METADATA_DIR, digest, NULL);
}

typedef struct {
  char *path;
  guint64 size;
  gint64 mtime;
  GList *lru_link;
} DiskFileEntry;

typedef struct {
  GHashTable *entries; /* borrowed DiskFileEntry.path -> DiskFileEntry */
  GQueue lru;          /* oldest first */
  guint64 total_bytes;
} DiskDirectoryState;

static void
disk_file_entry_free(DiskFileEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->path);
  g_free(entry);
}

static void
disk_directory_state_free(DiskDirectoryState *state)
{
  if (!state)
    return;
  while (!g_queue_is_empty(&state->lru))
    disk_file_entry_free(g_queue_pop_head(&state->lru));
  g_hash_table_unref(state->entries);
  g_free(state);
}

static gint
disk_file_entry_compare(gconstpointer a, gconstpointer b)
{
  const DiskFileEntry *ea = *(DiskFileEntry * const *)a;
  const DiskFileEntry *eb = *(DiskFileEntry * const *)b;
  if (ea->mtime < eb->mtime)
    return -1;
  if (ea->mtime > eb->mtime)
    return 1;
  return g_strcmp0(ea->path, eb->path);
}

static GPtrArray *
scan_regular_files(const char *dir, guint64 *out_total)
{
  g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
  GPtrArray *files = g_ptr_array_new_with_free_func(
      (GDestroyNotify)disk_file_entry_free);
  guint64 total = 0;
  if (handle) {
    const char *name;
    while ((name = g_dir_read_name(handle)) != NULL) {
      g_autofree char *path = g_build_filename(dir, name, NULL);
      GStatBuf st;
      if (g_lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        continue;
      DiskFileEntry *entry = g_new0(DiskFileEntry, 1);
      entry->path = g_steal_pointer(&path);
      entry->size = st.st_size > 0 ? (guint64)st.st_size : 0;
      entry->mtime = (gint64)st.st_mtime;
      total += entry->size;
      g_ptr_array_add(files, entry);
    }
  }
  if (out_total)
    *out_total = total;
  return files;
}

static DiskDirectoryState *
disk_directory_state_scan(const char *dir)
{
  guint64 total = 0;
  g_autoptr(GPtrArray) files = scan_regular_files(dir, &total);
  g_ptr_array_sort(files, disk_file_entry_compare);

  DiskDirectoryState *state = g_new0(DiskDirectoryState, 1);
  state->entries = g_hash_table_new(g_str_hash, g_str_equal);
  state->total_bytes = total;
  for (guint i = 0; i < files->len; i++) {
    DiskFileEntry *entry = g_ptr_array_index(files, i);
    g_queue_push_tail(&state->lru, entry);
    entry->lru_link = g_queue_peek_tail_link(&state->lru);
    g_hash_table_insert(state->entries, entry->path, entry);
  }
  g_ptr_array_set_free_func(files, NULL);
  return state;
}

static DiskDirectoryState *
disk_directory_state_get_locked(GnostrMediaService *self, const char *dir)
{
  DiskDirectoryState *state =
      g_hash_table_lookup(self->disk_directory_states, dir);
  if (!state) {
    state = disk_directory_state_scan(dir);
    g_hash_table_insert(self->disk_directory_states, g_strdup(dir), state);
  }
  return state;
}

static void
disk_directory_state_replace_locked(GnostrMediaService *self,
                                    const char *dir,
                                    DiskDirectoryState *state)
{
  g_hash_table_replace(self->disk_directory_states, g_strdup(dir), state);
}

static void
disk_directory_state_forget(DiskDirectoryState *state,
                            DiskFileEntry *entry)
{
  g_hash_table_remove(state->entries, entry->path);
  if (entry->lru_link)
    g_queue_delete_link(&state->lru, entry->lru_link);
  state->total_bytes -= MIN(state->total_bytes, entry->size);
  disk_file_entry_free(entry);
}

static void
disk_directory_state_record(DiskDirectoryState *state,
                            const char *path,
                            guint64 size,
                            gint64 mtime)
{
  DiskFileEntry *old = g_hash_table_lookup(state->entries, path);
  if (old)
    disk_directory_state_forget(state, old);

  DiskFileEntry *entry = g_new0(DiskFileEntry, 1);
  entry->path = g_strdup(path);
  entry->size = size;
  entry->mtime = mtime;
  g_queue_push_tail(&state->lru, entry);
  entry->lru_link = g_queue_peek_tail_link(&state->lru);
  g_hash_table_insert(state->entries, entry->path, entry);
  state->total_bytes += size;
}

static gboolean
disk_directory_state_make_byte_room(DiskDirectoryState *state,
                                    const char *replace_path,
                                    guint64 incoming_size,
                                    guint64 budget)
{
  DiskFileEntry *replaced = g_hash_table_lookup(state->entries, replace_path);
  guint64 replaced_size = replaced ? replaced->size : 0;
  guint64 retained = state->total_bytes - MIN(state->total_bytes, replaced_size);
  if (incoming_size > budget)
    return FALSE;

  for (GList *link = state->lru.head;
       retained > budget - incoming_size && link;) {
    GList *next = link->next;
    DiskFileEntry *entry = link->data;
    if (entry != replaced &&
        (g_unlink(entry->path) == 0 || errno == ENOENT)) {
      retained -= MIN(retained, entry->size);
      disk_directory_state_forget(state, entry);
    }
    link = next;
  }
  return retained <= budget - incoming_size;
}

static gboolean
disk_directory_state_make_entry_room(DiskDirectoryState *state,
                                     const char *replace_path,
                                     guint max_entries)
{
  DiskFileEntry *replaced = g_hash_table_lookup(state->entries, replace_path);
  guint retained = g_hash_table_size(state->entries) - (replaced ? 1u : 0u);
  for (GList *link = state->lru.head;
       retained >= max_entries && link;) {
    GList *next = link->next;
    DiskFileEntry *entry = link->data;
    if (entry != replaced &&
        (g_unlink(entry->path) == 0 || errno == ENOENT)) {
      retained--;
      disk_directory_state_forget(state, entry);
    }
    link = next;
  }
  return retained < max_entries;
}

static void
disk_directory_states_remove_namespace_locked(GnostrMediaService *self,
                                               const char *cache_namespace)
{
  g_autofree char *namespace_dir =
      g_build_filename(self->disk_root, cache_namespace, NULL);
  gsize prefix_len = strlen(namespace_dir);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, self->disk_directory_states);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const char *dir = key;
    if (g_str_has_prefix(dir, namespace_dir) &&
        (dir[prefix_len] == G_DIR_SEPARATOR || dir[prefix_len] == '\0'))
      g_hash_table_iter_remove(&iter);
  }
}

static DiskDirectoryState *
prune_directory_to_budget(const char *dir, guint64 budget)
{
  DiskDirectoryState *state = disk_directory_state_scan(dir);
  for (GList *link = state->lru.head;
       state->total_bytes > budget && link;) {
    GList *next = link->next;
    DiskFileEntry *entry = link->data;
    if (g_unlink(entry->path) == 0 || errno == ENOENT)
      disk_directory_state_forget(state, entry);
    link = next;
  }
  return state;
}

static char *
texture_key(const char *cache_namespace,
            const char *url,
            int width,
            int height)
{
  return g_strdup_printf("%s\n%dx%d\n%s", cache_namespace, width, height,
                         url);
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
  g_free(entry->cache_namespace);
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
                     const char *cache_namespace,
                     const char *url,
                     int width,
                     int height)
{
  TextureClassCache *cache = &self->texture_caches[resource_class];
  g_autofree char *key = texture_key(cache_namespace, url, width, height);
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
                    const char *cache_namespace,
                    const char *url,
                    int width,
                    int height,
                    GdkTexture *texture)
{
  TextureClassCache *cache = &self->texture_caches[resource_class];
  guint64 bytes = texture_decoded_bytes(texture);
  g_autofree char *lookup_key = texture_key(cache_namespace, url, width,
                                              height);
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
  entry->cache_namespace = g_strdup(cache_namespace);
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
operation_key(const char *cache_namespace,
              const char *url,
              PendingKind kind)
{
  return g_strdup_printf("%c\n%s\n%s",
                         kind == PENDING_TEXTURE ? 'T' : 'M',
                         cache_namespace, url);
}

static char *
pending_operation_key(const char *url,
                      PendingKind kind,
                      const char *cache_namespace)
{
  return g_strdup_printf("%c\n%s\n%s",
                         kind == PENDING_TEXTURE ? 'T' : 'M',
                         cache_namespace, url);
}

static void
negative_entry_free(NegativeEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->key);
  g_free(entry->cache_namespace);
  g_free(entry->url);
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
negative_lookup(GnostrMediaService *self,
                const char *cache_namespace,
                const char *url,
                PendingKind kind)
{
  g_autofree char *key = operation_key(cache_namespace, url, kind);
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
negative_store(GnostrMediaService *self,
               const char *cache_namespace,
               const char *url,
               PendingKind kind)
{
  if (self->config.negative_cache_max_entries == 0 ||
      self->config.negative_cache_ttl_usec <= 0)
    return;

  g_autofree char *key = operation_key(cache_namespace, url, kind);
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
  entry->cache_namespace = g_strdup(cache_namespace);
  entry->url = g_strdup(url);
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
  g_free(entry->key);
  g_free(entry->url);
  g_free(entry->cache_namespace);
  gnostr_og_metadata_unref(entry->metadata);
  g_free(entry);
}

static void
og_cache_remove_entry(GnostrMediaService *self,
                      OgCacheEntry *entry,
                      gboolean count_eviction)
{
  g_hash_table_remove(self->og_cache, entry->key);
  if (entry->lru_link)
    g_queue_delete_link(&self->og_lru, entry->lru_link);
  if (count_eviction)
    self->og_evictions++;
  og_cache_entry_free(entry);
}

static GnostrOgMetadata *
og_cache_lookup(GnostrMediaService *self,
                const char *cache_namespace,
                const char *url)
{
  g_autofree char *key = operation_key(cache_namespace, url,
                                        PENDING_OG_METADATA);
  OgCacheEntry *entry = g_hash_table_lookup(self->og_cache, key);
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
og_cache_store_until(GnostrMediaService *self,
                     const char *cache_namespace,
                     const char *url,
                     GnostrOgMetadata *metadata,
                     gint64 expires_at)
{
  if (self->config.og_metadata_max_entries == 0 ||
      expires_at <= g_get_monotonic_time())
    return;

  g_autofree char *key = operation_key(cache_namespace, url,
                                        PENDING_OG_METADATA);
  OgCacheEntry *old = g_hash_table_lookup(self->og_cache, key);
  if (old)
    og_cache_remove_entry(self, old, FALSE);

  OgCacheEntry *entry = g_new0(OgCacheEntry, 1);
  entry->key = g_steal_pointer(&key);
  entry->url = g_strdup(url);
  entry->cache_namespace = g_strdup(cache_namespace);
  entry->metadata = gnostr_og_metadata_ref(metadata);
  entry->expires_at = expires_at;
  g_queue_push_tail(&self->og_lru, entry);
  entry->lru_link = g_queue_peek_tail_link(&self->og_lru);
  g_hash_table_insert(self->og_cache, entry->key, entry);

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
        } else if (request->thumbnail_queued) {
          g_queue_remove(&request->service->thumbnail_queue, request);
          request->thumbnail_queued = FALSE;
          request->network_done = TRUE;
          pending_maybe_finish(request);
          start_queued_thumbnails(dispatch->service);
        } else if (request->network_active || request->thumbnail_active ||
                   request->disk_lookup_active) {
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

static PendingRequest *
pending_ref(PendingRequest *request)
{
  g_atomic_ref_count_inc(&request->ref_count);
  return request;
}

static void
pending_unref(PendingRequest *request)
{
  if (!request || !g_atomic_ref_count_dec(&request->ref_count))
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
  g_free(request->cache_namespace);
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
  /* Only the frame that actually removes the table entry releases its
   * ownership reference; recursive completion may have removed it already. */
  if (g_hash_table_remove(self->pending, request->table_key))
    pending_unref(request);
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
  /* Subscriber callbacks may recursively evict this account and remove the
   * request from the service table. Keep the delivery frame alive. */
  pending_ref(request);
  pending_network_done(request);
  if (negative)
    negative_store(request->service, request->cache_namespace, request->url,
                   request->kind);

  for (guint i = 0; i < request->subscribers->len; i++) {
    Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
    if (subscriber->kind == PENDING_TEXTURE)
      subscriber_complete_texture(subscriber, NULL, error);
    else
      subscriber_complete_og(subscriber, NULL, error);
  }
  g_error_free(error);
  pending_maybe_finish(request);
  pending_unref(request);
}

typedef struct {
  GBytes *bytes;
  GnostrMediaResourceClass resource_class;
  int target_width;
  int target_height;
} TextureTaskData;

static void
texture_task_data_free(TextureTaskData *data)
{
  g_bytes_unref(data->bytes);
  g_free(data);
}

static GdkPixbuf *
pixbuf_scale_cover(GdkPixbuf *source, int target_width, int target_height)
{
  int source_width = gdk_pixbuf_get_width(source);
  int source_height = gdk_pixbuf_get_height(source);
  if (source_width <= 0 || source_height <= 0)
    return NULL;

  gboolean has_alpha = gdk_pixbuf_get_has_alpha(source);
  GdkPixbuf *result = gdk_pixbuf_new(GDK_COLORSPACE_RGB, has_alpha, 8,
                                     target_width, target_height);
  if (!result)
    return NULL;

  double scale = MAX((double)target_width / source_width,
                     (double)target_height / source_height);
  double offset_x = (target_width - source_width * scale) / 2.0;
  double offset_y = (target_height - source_height * scale) / 2.0;
  gdk_pixbuf_scale(source, result, 0, 0, target_width, target_height,
                   offset_x, offset_y, scale, scale, GDK_INTERP_BILINEAR);
  return result;
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
  int decode_width = data->target_width;
  int decode_height = data->target_height;
  if (data->resource_class == GNOSTR_MEDIA_RESOURCE_AVATAR) {
    int load_size = MIN(MAX(data->target_width, data->target_height) * 2, 512);
    decode_width = load_size;
    decode_height = load_size;
  }
  g_autoptr(GdkPixbuf) loaded =
      gdk_pixbuf_new_from_stream_at_scale(stream, decode_width, decode_height,
                                          TRUE, cancellable, &error);
  if (!loaded) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  g_autoptr(GdkPixbuf) covered = NULL;
  GdkPixbuf *pixbuf = loaded;
  if (data->resource_class == GNOSTR_MEDIA_RESOURCE_AVATAR) {
    covered = pixbuf_scale_cover(loaded, data->target_width,
                                 data->target_height);
    if (!covered) {
      g_task_return_new_error(task, GNOSTR_MEDIA_ERROR,
                              GNOSTR_MEDIA_ERROR_DECODE,
                              "Failed to crop avatar texture");
      return;
    }
    pixbuf = covered;
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
  PendingRequest *request = pending_ref(job->request);
  GnostrMediaService *self = request->service;
  g_autoptr(GError) error = NULL;
  GdkTexture *texture = g_task_propagate_pointer(G_TASK(result), &error);
  if (texture && namespace_epoch_get(self, request->cache_namespace) !=
                     request->namespace_epoch) {
    g_object_unref(texture);
    texture = NULL;
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Account media cache evicted");
  }

  g_hash_table_remove(request->decode_variants, job->variant_key);
  if (texture) {
    request->any_worker_success = TRUE;
    texture_cache_store(self, job->resource_class, request->cache_namespace,
                        request->url, job->target_width, job->target_height,
                        texture);
    schedule_disk_write(request, job->resource_class);
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
      request->any_worker_failure && !request->any_worker_success &&
      namespace_epoch_get(self, request->cache_namespace) ==
          request->namespace_epoch)
    negative_store(self, request->cache_namespace, request->url,
                   request->kind);
  pending_maybe_finish(request);
  pending_unref(request);
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
  task_data->resource_class = job->resource_class;
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
  pending_ref(request);
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
      negative_store(request->service, request->cache_namespace, request->url,
                     request->kind);
    pending_maybe_finish(request);
  }
  pending_unref(request);
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
  PendingRequest *request = pending_ref(user_data);
  GnostrMediaService *self = request->service;
  g_autoptr(GError) error = NULL;
  GnostrOgMetadata *metadata =
      g_task_propagate_pointer(G_TASK(result), &error);
  if (metadata && namespace_epoch_get(self, request->cache_namespace) !=
                      request->namespace_epoch) {
    gnostr_og_metadata_unref(metadata);
    metadata = NULL;
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Account media cache evicted");
  }

  if (metadata) {
    og_cache_store_until(self, request->cache_namespace, request->url,
                         metadata,
                         g_get_monotonic_time() +
                         self->config.og_metadata_ttl_usec);
    schedule_og_write(request, metadata);
  } else if (namespace_epoch_get(self, request->cache_namespace) ==
                 request->namespace_epoch) {
    negative_store(self, request->cache_namespace, request->url,
                   request->kind);
  }

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
  pending_unref(request);
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

typedef struct {
  PendingKind kind;
  char *path;
  char *url;
  char *cache_namespace;
  guint64 namespace_epoch;
  gsize size_cap;
  guint64 disk_budget;
  gint64 og_ttl_usec;
} DiskLookupJob;

typedef struct {
  GBytes *bytes;
  GnostrOgMetadata *metadata;
  gint64 expires_real_usec;
} DiskLookupResult;

static void
disk_lookup_job_free(DiskLookupJob *job)
{
  if (!job)
    return;
  g_free(job->path);
  g_free(job->url);
  g_free(job->cache_namespace);
  g_free(job);
}

static void
disk_lookup_result_free(DiskLookupResult *result)
{
  if (!result)
    return;
  g_clear_pointer(&result->bytes, g_bytes_unref);
  gnostr_og_metadata_unref(result->metadata);
  g_free(result);
}

static char *
keyfile_optional_string(GKeyFile *keyfile, const char *key)
{
  return g_key_file_has_key(keyfile, "entry", key, NULL)
      ? g_key_file_get_string(keyfile, "entry", key, NULL) : NULL;
}

static void
disk_lookup_worker(GTask *task,
                   gpointer source_object,
                   gpointer task_data,
                   GCancellable *cancellable)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(source_object);
  DiskLookupJob *job = task_data;
  DiskLookupResult *result = g_new0(DiskLookupResult, 1);
  if (job->disk_budget == 0 || g_cancellable_is_cancelled(cancellable)) {
    if (g_cancellable_is_cancelled(cancellable)) {
      disk_lookup_result_free(result);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Disk cache lookup cancelled");
    } else {
      g_task_return_pointer(task, result,
                            (GDestroyNotify)disk_lookup_result_free);
    }
    return;
  }

  G_LOCK(media_disk);
  if (!namespace_epoch_matches_locked(self, job->cache_namespace,
                                      job->namespace_epoch)) {
    G_UNLOCK(media_disk);
    g_task_return_pointer(task, result,
                          (GDestroyNotify)disk_lookup_result_free);
    return;
  }
  GStatBuf st;
  gboolean usable = g_stat(job->path, &st) == 0 && S_ISREG(st.st_mode) &&
      st.st_size >= 0 && (guint64)st.st_size <= (guint64)job->size_cap;
  if (usable && job->kind == PENDING_TEXTURE) {
    char *contents = NULL;
    gsize length = 0;
    if (g_file_get_contents(job->path, &contents, &length, NULL)) {
      result->bytes = g_bytes_new_take(contents, length);
      g_utime(job->path, NULL);
    }
  } else if (usable && job->kind == PENDING_OG_METADATA) {
    g_autoptr(GKeyFile) keyfile = g_key_file_new();
    g_autoptr(GError) error = NULL;
    if (g_key_file_load_from_file(keyfile, job->path, G_KEY_FILE_NONE, &error)) {
      gint version = g_key_file_get_integer(keyfile, "entry", "version", &error);
      g_autofree char *stored_url = error ? NULL :
          g_key_file_get_string(keyfile, "entry", "url", &error);
      gint64 stored_at = error ? 0 :
          g_key_file_get_int64(keyfile, "entry", "stored-at-usec", &error);
      gint64 expires_at = error ? 0 :
          g_key_file_get_int64(keyfile, "entry", "expires-at-usec", &error);
      gint64 configured_expiry = (stored_at > 0 && job->og_ttl_usec > 0 &&
                                   stored_at <= G_MAXINT64 - job->og_ttl_usec)
          ? stored_at + job->og_ttl_usec : 0;
      gint64 effective_expiry = configured_expiry > 0
          ? MIN(expires_at, configured_expiry) : 0;
      if (!error && version == OG_METADATA_VERSION &&
          g_strcmp0(stored_url, job->url) == 0 &&
          effective_expiry > g_get_real_time()) {
        GnostrOgMetadata *metadata = g_new0(GnostrOgMetadata, 1);
        g_atomic_ref_count_init(&metadata->ref_count);
        metadata->title = keyfile_optional_string(keyfile, "title");
        metadata->description = keyfile_optional_string(keyfile, "description");
        metadata->image_url = keyfile_optional_string(keyfile, "image-url");
        metadata->source_url = keyfile_optional_string(keyfile, "source-url");
        if (!metadata->source_url)
          metadata->source_url = g_strdup(job->url);
        if (metadata->title || metadata->description || metadata->image_url) {
          result->metadata = metadata;
          result->expires_real_usec = effective_expiry;
          g_utime(job->path, NULL);
        } else {
          gnostr_og_metadata_unref(metadata);
        }
      } else if (!error && effective_expiry <= g_get_real_time()) {
        g_unlink(job->path);
      }
    }
  }
  G_UNLOCK(media_disk);

  if (g_cancellable_is_cancelled(cancellable)) {
    disk_lookup_result_free(result);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Disk cache lookup cancelled");
    return;
  }
  g_task_return_pointer(task, result, (GDestroyNotify)disk_lookup_result_free);
}

typedef GBytes *(*ThumbnailExtractor)(const char *url,
                                      guint timeout_msec,
                                      GCancellable *cancellable,
                                      GError **error,
                                      gpointer user_data);

typedef struct {
  PendingRequest *request;
  ThumbnailExtractor extractor;
  gpointer extractor_data;
} ThumbnailJob;

static gboolean
looks_like_video_url(const char *url)
{
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL);
  const char *path = uri ? g_uri_get_path(uri) : url;
  if (!path)
    return FALSE;
  static const char *extensions[] = {
    ".mp4", ".m4v", ".mov", ".webm", ".mkv", ".avi", ".ogv", NULL
  };
  g_autofree char *lower = g_ascii_strdown(path, -1);
  for (guint i = 0; extensions[i]; i++) {
    if (g_str_has_suffix(lower, extensions[i]))
      return TRUE;
  }
  return FALSE;
}

#ifdef HAVE_GSTREAMER
static void
pixbuf_pixels_free(guchar *pixels, gpointer user_data)
{
  (void)user_data;
  g_free(pixels);
}

static GBytes *
gstreamer_extract_thumbnail(const char *url,
                            guint timeout_msec,
                            GCancellable *cancellable,
                            GError **error,
                            gpointer user_data)
{
  (void)user_data;
  static gsize initialized;
  static gboolean available;
  if (g_once_init_enter(&initialized)) {
    g_autoptr(GError) init_error = NULL;
    available = gst_init_check(NULL, NULL, &init_error);
    g_once_init_leave(&initialized, 1);
  }
  if (!available) {
    g_set_error_literal(error, GNOSTR_MEDIA_ERROR,
                        GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                        "GStreamer could not be initialized");
    return NULL;
  }
  if (g_cancellable_is_cancelled(cancellable)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "Video thumbnail extraction cancelled");
    return NULL;
  }

  g_autofree char *quoted_url = g_shell_quote(url);
  g_autofree char *description = g_strdup_printf(
      "uridecodebin uri=%s ! videoconvert ! videoscale ! "
      "video/x-raw,format=RGBA,width=640,height=360,pixel-aspect-ratio=1/1 ! "
      "appsink name=poster-sink max-buffers=1 drop=true sync=false",
      quoted_url);
  GstElement *pipeline = gst_parse_launch(description, error);
  if (!pipeline)
    return NULL;
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "poster-sink");
  if (!sink) {
    gst_object_unref(pipeline);
    g_set_error_literal(error, GNOSTR_MEDIA_ERROR,
                        GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                        "Required GStreamer video elements are unavailable");
    return NULL;
  }

  GstStateChangeReturn state = gst_element_set_state(pipeline,
                                                      GST_STATE_PLAYING);
  GstSample *sample = NULL;
  if (state != GST_STATE_CHANGE_FAILURE)
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
        (GstClockTime)timeout_msec * GST_MSECOND);
  if (!sample && state != GST_STATE_CHANGE_FAILURE)
    sample = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 0);

  GBytes *result = NULL;
  if (sample && !g_cancellable_is_cancelled(cancellable)) {
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    GstMapInfo map;
    if (caps && buffer && gst_video_info_from_caps(&info, caps) &&
        gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      guint width = GST_VIDEO_INFO_WIDTH(&info);
      guint height = GST_VIDEO_INFO_HEIGHT(&info);
      gsize row_bytes = (gsize)width * 4;
      gint stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
      if (width > 0 && height > 0 && stride > 0 &&
          map.size >= (gsize)stride * height) {
        guchar *pixels = g_malloc(row_bytes * height);
        for (guint y = 0; y < height; y++)
          memcpy(pixels + y * row_bytes, map.data + y * stride, row_bytes);
        g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_data(
            pixels, GDK_COLORSPACE_RGB, TRUE, 8, width, height, row_bytes,
            pixbuf_pixels_free, NULL);
        gchar *png = NULL;
        gsize png_size = 0;
        if (gdk_pixbuf_save_to_buffer(pixbuf, &png, &png_size, "png", error,
                                      NULL))
          result = g_bytes_new_take(png, png_size);
        else
          g_free(png);
      }
      gst_buffer_unmap(buffer, &map);
    }
  }

  if (!result && !*error) {
    if (g_cancellable_is_cancelled(cancellable))
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                          "Video thumbnail extraction cancelled");
    else
      g_set_error_literal(error, GNOSTR_MEDIA_ERROR,
                          GNOSTR_MEDIA_ERROR_DECODE,
                          "Video thumbnail extraction timed out or produced no frame");
  }
  if (sample)
    gst_sample_unref(sample);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  return result;
}
#else
static GBytes *
gstreamer_extract_thumbnail(const char *url,
                            guint timeout_msec,
                            GCancellable *cancellable,
                            GError **error,
                            gpointer user_data)
{
  (void)url;
  (void)timeout_msec;
  (void)cancellable;
  (void)user_data;
  g_set_error_literal(error, GNOSTR_MEDIA_ERROR, GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                      "This build has no GStreamer thumbnail support");
  return NULL;
}
#endif

static void
thumbnail_worker(GTask *task,
                 gpointer source_object,
                 gpointer task_data,
                 GCancellable *cancellable)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(source_object);
  ThumbnailJob *job = task_data;
  g_autoptr(GError) error = NULL;
  GBytes *bytes = job->extractor(job->request->url,
                                 self->config.thumbnail_timeout_msec,
                                 cancellable, &error, job->extractor_data);
  if (bytes)
    g_task_return_pointer(task, bytes, (GDestroyNotify)g_bytes_unref);
  else {
    if (!error)
      error = g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                  GNOSTR_MEDIA_ERROR_DECODE,
                                  "Video thumbnail extractor returned no frame");
    g_task_return_error(task, g_steal_pointer(&error));
  }
}

static void
thumbnail_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(source);
  ThumbnailJob *job = user_data;
  PendingRequest *request = pending_ref(job->request);
  g_autoptr(GError) error = NULL;
  GBytes *bytes = g_task_propagate_pointer(G_TASK(result), &error);

  g_assert(self->active_thumbnails > 0);
  self->active_thumbnails--;
  request->thumbnail_active = FALSE;
  request->network_done = TRUE;
  g_assert(request->outstanding_workers > 0);
  request->outstanding_workers--;

  if (bytes && namespace_epoch_get(self, request->cache_namespace) ==
                   request->namespace_epoch &&
      !g_cancellable_is_cancelled(request->network_cancellable)) {
    request->body = g_bytes_ref(bytes);
    process_texture_body(request);
  } else {
    if (!error)
      error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                  "Video thumbnail extraction cancelled");
    for (guint i = 0; i < request->subscribers->len; i++) {
      Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
      if (!subscriber->completed)
        subscriber_complete_texture(subscriber, NULL, error);
    }
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      negative_store(self, request->cache_namespace, request->url,
                     request->kind);
    pending_maybe_finish(request);
  }
  if (bytes)
    g_bytes_unref(bytes);
  g_free(job);
  start_queued_thumbnails(self);
  pending_unref(request);
}

static void
start_queued_thumbnails(GnostrMediaService *self)
{
  while (self->active_thumbnails < self->config.max_concurrent_thumbnails &&
         !g_queue_is_empty(&self->thumbnail_queue)) {
    PendingRequest *request = g_queue_pop_head(&self->thumbnail_queue);
    request->thumbnail_queued = FALSE;
    if (pending_all_completed(request)) {
      request->network_done = TRUE;
      pending_maybe_finish(request);
      continue;
    }
    request->thumbnail_active = TRUE;
    request->outstanding_workers++;
    self->active_thumbnails++;
    ThumbnailJob *job = g_new0(ThumbnailJob, 1);
    job->request = request;
#ifdef GNOSTR_MEDIA_SERVICE_TESTING
    job->extractor = self->test_thumbnail_extractor
        ? (ThumbnailExtractor)self->test_thumbnail_extractor
        : gstreamer_extract_thumbnail;
    job->extractor_data = self->test_thumbnail_data;
#else
    job->extractor = gstreamer_extract_thumbnail;
#endif
    GTask *task = g_task_new(self, request->network_cancellable,
                             thumbnail_done, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, thumbnail_worker);
    g_object_unref(task);
  }
}

static void
queue_thumbnail(PendingRequest *request)
{
  request->thumbnail_queued = TRUE;
  g_queue_push_tail(&request->service->thumbnail_queue, request);
  start_queued_thumbnails(request->service);
}

static void
queue_network_after_disk_miss(PendingRequest *request)
{
  if (pending_all_completed(request)) {
    request->network_done = TRUE;
    pending_maybe_finish(request);
    return;
  }
  if (negative_lookup(request->service, request->cache_namespace,
                      request->url, request->kind)) {
    pending_fail(request,
                 g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                     GNOSTR_MEDIA_ERROR_NEGATIVE_CACHED,
                                     "URL is temporarily negative-cached"),
                 FALSE);
    return;
  }
  if (request->kind == PENDING_TEXTURE &&
      request->disk_resource_class == GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER &&
      looks_like_video_url(request->url)) {
    queue_thumbnail(request);
    return;
  }
  request->queued = TRUE;
  g_queue_push_tail(&request->service->download_queue, request);
  start_queued_downloads(request->service);
}

static void
disk_lookup_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  PendingRequest *request = pending_ref(user_data);
  g_autoptr(GError) error = NULL;
  DiskLookupResult *lookup = g_task_propagate_pointer(G_TASK(result), &error);
  request->disk_lookup_active = FALSE;
  g_assert(request->outstanding_workers > 0);
  request->outstanding_workers--;

  if (error) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
        pending_all_completed(request)) {
      request->network_done = TRUE;
      pending_maybe_finish(request);
      pending_unref(request);
      return;
    }
    queue_network_after_disk_miss(request);
    pending_unref(request);
    return;
  }

  if (request->kind == PENDING_TEXTURE && lookup && lookup->bytes) {
    request->body = g_bytes_ref(lookup->bytes);
    request->body_from_disk = TRUE;
    request->network_done = TRUE;
    process_texture_body(request);
  } else if (request->kind == PENDING_OG_METADATA && lookup &&
             lookup->metadata &&
             lookup->expires_real_usec > g_get_real_time()) {
    gint64 remaining = lookup->expires_real_usec - g_get_real_time();
    og_cache_store_until(request->service, request->cache_namespace,
                         request->url, lookup->metadata,
                         g_get_monotonic_time() + remaining);
    for (guint i = 0; i < request->subscribers->len; i++) {
      Subscriber *subscriber = g_ptr_array_index(request->subscribers, i);
      if (!subscriber->completed)
        subscriber_complete_og(subscriber, lookup->metadata, NULL);
    }
    request->network_done = TRUE;
    pending_maybe_finish(request);
  } else {
    queue_network_after_disk_miss(request);
  }
  disk_lookup_result_free(lookup);
  pending_unref(request);
}

static void
start_disk_lookup(PendingRequest *request)
{
  GnostrMediaService *self = request->service;
  DiskLookupJob *job = g_new0(DiskLookupJob, 1);
  job->kind = request->kind;
  job->url = g_strdup(request->url);
  job->cache_namespace = g_strdup(request->cache_namespace);
  job->namespace_epoch = request->namespace_epoch;
  if (request->kind == PENDING_TEXTURE) {
    job->path = disk_texture_path(self, request->cache_namespace,
                                  request->disk_resource_class, request->url);
    job->disk_budget =
        self->config.disk_budget_bytes[request->disk_resource_class];
    job->size_cap = request->download_cap;
  } else {
    job->path = disk_og_metadata_path(self, request->cache_namespace,
                                      request->url);
    job->disk_budget =
        self->config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE];
    job->size_cap = self->config.og_metadata_body_size_cap;
    job->og_ttl_usec = self->config.og_metadata_ttl_usec;
  }
  request->disk_lookup_active = TRUE;
  request->outstanding_workers++;
  GTask *task = g_task_new(self, request->network_cancellable,
                           disk_lookup_done, request);
  g_task_set_priority(task, G_PRIORITY_DEFAULT_IDLE);
  g_task_set_task_data(task, job, (GDestroyNotify)disk_lookup_job_free);
  g_task_run_in_thread(task, disk_lookup_worker);
  g_object_unref(task);
}

typedef struct {
  char *cache_namespace;
  guint64 namespace_epoch;
  char *url;
  GnostrMediaResourceClass resource_class;
  GBytes *bytes;
  guint64 budget;
} DiskWriteJob;

static void
disk_write_job_free(DiskWriteJob *job)
{
  if (!job)
    return;
  g_free(job->cache_namespace);
  g_free(job->url);
  g_bytes_unref(job->bytes);
  g_free(job);
}

static void
disk_write_worker(GTask *task,
                  gpointer source_object,
                  gpointer task_data,
                  GCancellable *cancellable)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(source_object);
  DiskWriteJob *job = task_data;
  gsize length = 0;
  const char *data = g_bytes_get_data(job->bytes, &length);
  if (job->budget == 0 || (guint64)length > job->budget ||
      g_cancellable_is_cancelled(cancellable)) {
    g_task_return_boolean(task, TRUE);
    return;
  }

  g_autofree char *dir = disk_class_dir(self, job->cache_namespace,
                                        job->resource_class);
  g_autofree char *path = disk_texture_path(self, job->cache_namespace,
                                            job->resource_class, job->url);
  g_autoptr(GError) error = NULL;
  gboolean written = FALSE;
  G_LOCK(media_disk);
  if (!namespace_epoch_matches_locked(self, job->cache_namespace,
                                      job->namespace_epoch)) {
    G_UNLOCK(media_disk);
    g_task_return_boolean(task, TRUE);
    return;
  }
  if (g_mkdir_with_parents(dir, 0700) == 0) {
    DiskDirectoryState *state = disk_directory_state_get_locked(self, dir);
    if (disk_directory_state_make_byte_room(state, path, length,
                                             job->budget))
      written = g_file_set_contents(path, data, length, &error);
    if (written) {
      GStatBuf st;
      g_chmod(path, 0600);
      disk_directory_state_record(
          state, path, length,
          g_stat(path, &st) == 0 ? (gint64)st.st_mtime : g_get_real_time());
    }
  } else {
    g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Failed to create media cache directory: %s",
                g_strerror(errno));
  }
  G_UNLOCK(media_disk);
  if (error)
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_boolean(task, TRUE);
}

struct _DiskExecutorEntry {
  char *key;
  GTask *task;
  GTaskThreadFunc worker;
  guint64 retained_bytes;
  gboolean low_priority;
  gboolean active;
  gboolean discarded;
  DiskExecutorEntry *replacement; /* latest queued duplicate */
  DiskExecutorEntry *predecessor; /* active job this entry follows */
};

static void disk_executor_start_queued(GnostrMediaService *self);

static void
disk_executor_entry_free(DiskExecutorEntry *entry)
{
  if (!entry)
    return;
  g_clear_object(&entry->task);
  g_free(entry->key);
  g_free(entry);
}

static DiskExecutorEntry *
disk_executor_oldest_write(GnostrMediaService *self)
{
  for (GList *link = self->disk_queue.head; link; link = link->next) {
    DiskExecutorEntry *entry = link->data;
    if (entry->low_priority)
      return entry;
  }
  return NULL;
}

static void
disk_executor_discard(DiskExecutorEntry *entry)
{
  entry->discarded = TRUE;
  GTask *task = g_steal_pointer(&entry->task);
  /* Release retained bytes immediately; the completed GTask callback only
   * keeps the tiny executor entry alive until the main context dispatches. */
  g_task_set_task_data(task, NULL, NULL);
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void
disk_executor_drop_queued(GnostrMediaService *self,
                          DiskExecutorEntry *entry)
{
  g_assert(!entry->active);
  g_assert(g_queue_remove(&self->disk_queue, entry));
  if (entry->predecessor && entry->predecessor->replacement == entry)
    entry->predecessor->replacement = NULL;
  g_assert(self->queued_disk_bytes >= entry->retained_bytes);
  self->queued_disk_bytes -= entry->retained_bytes;
  if (g_hash_table_lookup(self->disk_coalesced, entry->key) == entry)
    g_hash_table_remove(self->disk_coalesced, entry->key);
  g_assert(self->outstanding_disk_jobs > 0);
  self->outstanding_disk_jobs--;
  self->dropped_disk_jobs++;
  disk_executor_discard(entry);
}

static void
disk_executor_launch(GnostrMediaService *self, DiskExecutorEntry *entry)
{
  g_autoptr(GError) error = NULL;
  if (!self->disk_pool) {
    self->disk_pool = g_thread_pool_new(
        disk_executor_pool_worker, NULL, (gint)self->config.disk_worker_count,
        TRUE, &error);
  }

  entry->active = TRUE;
  self->active_disk_jobs++;
  if (!self->disk_pool || !g_thread_pool_push(self->disk_pool, entry, &error)) {
    if (!error)
      error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                  "Failed to dispatch media disk job");
    GTask *task = g_steal_pointer(&entry->task);
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
  }
}

static void
disk_executor_start_queued(GnostrMediaService *self)
{
  while (self->active_disk_jobs < self->config.disk_worker_count &&
         !g_queue_is_empty(&self->disk_queue)) {
    DiskExecutorEntry *entry = g_queue_pop_head(&self->disk_queue);
    g_assert(self->queued_disk_bytes >= entry->retained_bytes);
    self->queued_disk_bytes -= entry->retained_bytes;
    disk_executor_launch(self, entry);
  }
}

static void
disk_executor_complete(GnostrMediaService *self,
                       DiskExecutorEntry *entry)
{
  g_assert(entry->active);
  g_assert(self->active_disk_jobs > 0);
  self->active_disk_jobs--;
  DiskExecutorEntry *replacement = entry->replacement;
  g_hash_table_remove(self->disk_coalesced, entry->key);
  if (replacement) {
    replacement->predecessor = NULL;
    g_hash_table_insert(self->disk_coalesced, replacement->key, replacement);
  }
  g_assert(self->outstanding_disk_jobs > 0);
  self->outstanding_disk_jobs--;
  disk_executor_entry_free(entry);
  disk_executor_start_queued(self);
}

static void
detached_disk_job_done(GObject *source,
                       GAsyncResult *result,
                       gpointer user_data)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(source);
  DiskExecutorEntry *entry = user_data;
  g_autoptr(GError) error = NULL;
  g_task_propagate_boolean(G_TASK(result), &error);
  if (error)
    g_debug("media disk maintenance failed: %s", error->message);
  if (entry->discarded)
    disk_executor_entry_free(entry);
  else
    disk_executor_complete(self, entry);
}

static DiskExecutorEntry *
disk_executor_entry_new(GnostrMediaService *self,
                        char *key,
                        guint64 retained_bytes,
                        gboolean low_priority,
                        GTaskThreadFunc worker,
                        gpointer task_data,
                        GDestroyNotify task_data_destroy)
{
  DiskExecutorEntry *entry = g_new0(DiskExecutorEntry, 1);
  entry->key = key;
  entry->worker = worker;
  entry->retained_bytes = retained_bytes;
  entry->low_priority = low_priority;
  entry->task = g_task_new(self, NULL, detached_disk_job_done, entry);
  g_task_set_priority(entry->task,
                      low_priority ? G_PRIORITY_LOW : G_PRIORITY_DEFAULT_IDLE);
  g_task_set_task_data(entry->task, task_data, task_data_destroy);
  return entry;
}

static gboolean
disk_executor_submit(GnostrMediaService *self, DiskExecutorEntry *entry)
{
  DiskExecutorEntry *active_predecessor = NULL;
  DiskExecutorEntry *existing =
      g_hash_table_lookup(self->disk_coalesced, entry->key);
  if (existing) {
    if (existing->active) {
      /* The running job cannot be replaced safely.  Retain at most one latest
       * follow-up so the newest payload wins without duplicate concurrency. */
      active_predecessor = existing;
      if (existing->replacement) {
        DiskExecutorEntry *old_replacement = existing->replacement;
        existing->replacement = NULL;
        disk_executor_drop_queued(self, old_replacement);
      }
    } else {
      disk_executor_drop_queued(self, existing);
    }
  }

  if (!active_predecessor &&
      self->active_disk_jobs < self->config.disk_worker_count &&
      g_queue_is_empty(&self->disk_queue)) {
    g_hash_table_insert(self->disk_coalesced, entry->key, entry);
    self->outstanding_disk_jobs++;
    disk_executor_launch(self, entry);
    return TRUE;
  }

  while (g_queue_get_length(&self->disk_queue) >=
             self->config.disk_max_queued_jobs ||
         entry->retained_bytes > self->config.disk_max_queued_bytes -
                                     MIN(self->queued_disk_bytes,
                                         self->config.disk_max_queued_bytes)) {
    DiskExecutorEntry *oldest = disk_executor_oldest_write(self);
    if (!oldest) {
      self->dropped_disk_jobs++;
      disk_executor_discard(entry);
      return FALSE;
    }
    disk_executor_drop_queued(self, oldest);
  }

  if (active_predecessor) {
    active_predecessor->replacement = entry;
    entry->predecessor = active_predecessor;
  } else
    g_hash_table_insert(self->disk_coalesced, entry->key, entry);
  if (entry->low_priority)
    g_queue_push_tail(&self->disk_queue, entry);
  else
    g_queue_push_head(&self->disk_queue, entry);
  self->queued_disk_bytes += entry->retained_bytes;
  self->outstanding_disk_jobs++;
  disk_executor_start_queued(self);
  return TRUE;
}

static void
schedule_disk_write(PendingRequest *request,
                    GnostrMediaResourceClass resource_class)
{
  GnostrMediaService *self = request->service;
  guint bit = 1u << resource_class;
  if (request->body_from_disk || (request->disk_write_classes & bit) != 0 ||
      self->config.disk_budget_bytes[resource_class] == 0)
    return;
  request->disk_write_classes |= bit;
  DiskWriteJob *job = g_new0(DiskWriteJob, 1);
  job->cache_namespace = g_strdup(request->cache_namespace);
  job->namespace_epoch = request->namespace_epoch;
  job->url = g_strdup(request->url);
  job->resource_class = resource_class;
  job->bytes = g_bytes_ref(request->body);
  job->budget = self->config.disk_budget_bytes[resource_class];
  gsize retained_bytes = g_bytes_get_size(job->bytes);
  char *key = g_strdup_printf("texture:%s:%u:%s", job->cache_namespace,
                              (guint)job->resource_class, job->url);
  DiskExecutorEntry *entry = disk_executor_entry_new(
      self, key, retained_bytes, TRUE, disk_write_worker, job,
      (GDestroyNotify)disk_write_job_free);
  disk_executor_submit(self, entry);
}

typedef struct {
  char *cache_namespace;
  guint64 namespace_epoch;
  char *url;
  char *title;
  char *description;
  char *image_url;
  char *source_url;
  gint64 ttl_usec;
  guint max_entries;
  guint64 disk_budget;
} OgWriteJob;

static void
og_write_job_free(OgWriteJob *job)
{
  if (!job)
    return;
  g_free(job->cache_namespace);
  g_free(job->url);
  g_free(job->title);
  g_free(job->description);
  g_free(job->image_url);
  g_free(job->source_url);
  g_free(job);
}

static DiskDirectoryState *
prune_og_metadata_dir(const char *dir, guint max_entries)
{
  DiskDirectoryState *state = disk_directory_state_scan(dir);
  gint64 now = g_get_real_time();
  for (GList *link = state->lru.head; link;) {
    GList *next = link->next;
    DiskFileEntry *entry = link->data;
    g_autoptr(GKeyFile) keyfile = g_key_file_new();
    g_autoptr(GError) error = NULL;
    gboolean valid = g_key_file_load_from_file(keyfile, entry->path,
                                               G_KEY_FILE_NONE, &error);
    gint64 expires = valid ? g_key_file_get_int64(
        keyfile, "entry", "expires-at-usec", &error) : 0;
    if ((!valid || error || expires <= now) &&
        (g_unlink(entry->path) == 0 || errno == ENOENT))
      disk_directory_state_forget(state, entry);
    link = next;
  }
  for (GList *link = state->lru.head;
       g_hash_table_size(state->entries) > max_entries && link;) {
    GList *next = link->next;
    DiskFileEntry *entry = link->data;
    if (g_unlink(entry->path) == 0 || errno == ENOENT)
      disk_directory_state_forget(state, entry);
    link = next;
  }
  return state;
}

static void
og_write_worker(GTask *task,
                gpointer source_object,
                gpointer task_data,
                GCancellable *cancellable)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(source_object);
  OgWriteJob *job = task_data;
  if (job->disk_budget == 0 || job->ttl_usec <= 0 ||
      job->max_entries == 0 || g_cancellable_is_cancelled(cancellable)) {
    g_task_return_boolean(task, TRUE);
    return;
  }
  gint64 stored_at = g_get_real_time();
  if (stored_at > G_MAXINT64 - job->ttl_usec) {
    g_task_return_boolean(task, TRUE);
    return;
  }
  g_autoptr(GKeyFile) keyfile = g_key_file_new();
  g_key_file_set_integer(keyfile, "entry", "version", OG_METADATA_VERSION);
  g_key_file_set_string(keyfile, "entry", "url", job->url);
  g_key_file_set_int64(keyfile, "entry", "stored-at-usec", stored_at);
  g_key_file_set_int64(keyfile, "entry", "expires-at-usec",
                       stored_at + job->ttl_usec);
  if (job->title)
    g_key_file_set_string(keyfile, "entry", "title", job->title);
  if (job->description)
    g_key_file_set_string(keyfile, "entry", "description", job->description);
  if (job->image_url)
    g_key_file_set_string(keyfile, "entry", "image-url", job->image_url);
  if (job->source_url)
    g_key_file_set_string(keyfile, "entry", "source-url", job->source_url);
  gsize length = 0;
  g_autofree char *data = g_key_file_to_data(keyfile, &length, NULL);
  if (!data || length > self->config.og_metadata_body_size_cap) {
    g_task_return_boolean(task, TRUE);
    return;
  }

  g_autofree char *path = disk_og_metadata_path(
      self, job->cache_namespace, job->url);
  g_autofree char *dir = g_path_get_dirname(path);
  g_autoptr(GError) error = NULL;
  G_LOCK(media_disk);
  if (!namespace_epoch_matches_locked(self, job->cache_namespace,
                                      job->namespace_epoch)) {
    G_UNLOCK(media_disk);
    g_task_return_boolean(task, TRUE);
    return;
  }
  gboolean written = FALSE;
  if (g_mkdir_with_parents(dir, 0700) == 0) {
    DiskDirectoryState *state = disk_directory_state_get_locked(self, dir);
    if (disk_directory_state_make_entry_room(state, path, job->max_entries))
      written = g_file_set_contents(path, data, length, &error);
    if (written) {
      GStatBuf st;
      g_chmod(path, 0600);
      disk_directory_state_record(
          state, path, length,
          g_stat(path, &st) == 0 ? (gint64)st.st_mtime : g_get_real_time());
    }
  } else if (!error) {
    g_set_error(&error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Failed to create OG metadata cache directory: %s",
                g_strerror(errno));
  }
  G_UNLOCK(media_disk);
  if (error)
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_boolean(task, TRUE);
}

static void
schedule_og_write(PendingRequest *request, GnostrOgMetadata *metadata)
{
  GnostrMediaService *self = request->service;
  if (self->config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE] == 0 ||
      self->config.og_metadata_max_entries == 0 ||
      self->config.og_metadata_ttl_usec <= 0)
    return;
  OgWriteJob *job = g_new0(OgWriteJob, 1);
  job->cache_namespace = g_strdup(request->cache_namespace);
  job->namespace_epoch = request->namespace_epoch;
  job->url = g_strdup(request->url);
  job->title = g_strdup(metadata->title);
  job->description = g_strdup(metadata->description);
  job->image_url = g_strdup(metadata->image_url);
  job->source_url = g_strdup(metadata->source_url);
  job->ttl_usec = self->config.og_metadata_ttl_usec;
  job->max_entries = self->config.og_metadata_max_entries;
  job->disk_budget =
      self->config.disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_OG_IMAGE];
  guint64 retained_bytes = strlen(job->url) + strlen(job->cache_namespace);
  retained_bytes += job->title ? strlen(job->title) : 0;
  retained_bytes += job->description ? strlen(job->description) : 0;
  retained_bytes += job->image_url ? strlen(job->image_url) : 0;
  retained_bytes += job->source_url ? strlen(job->source_url) : 0;
  char *key = g_strdup_printf("og:%s:%s", job->cache_namespace, job->url);
  DiskExecutorEntry *entry = disk_executor_entry_new(
      self, key, retained_bytes, TRUE, og_write_worker, job,
      (GDestroyNotify)og_write_job_free);
  disk_executor_submit(self, entry);
}

typedef struct {
  char *cache_namespace;
  guint64 budgets[GNOSTR_MEDIA_RESOURCE_N_CLASSES];
  guint og_metadata_max_entries;
} SweepJob;

static void
sweep_job_free(SweepJob *job)
{
  if (!job)
    return;
  g_free(job->cache_namespace);
  g_free(job);
}

static void
sweep_worker(GTask *task,
             gpointer source_object,
             gpointer task_data,
             GCancellable *cancellable)
{
  GnostrMediaService *self = GNOSTR_MEDIA_SERVICE(source_object);
  SweepJob *job = task_data;
  if (g_cancellable_is_cancelled(cancellable)) {
    g_task_return_boolean(task, TRUE);
    return;
  }
  G_LOCK(media_disk);
  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++) {
    g_autofree char *dir = disk_class_dir(self, job->cache_namespace, i);
    disk_directory_state_replace_locked(
        self, dir, prune_directory_to_budget(dir, job->budgets[i]));
  }
  g_autofree char *metadata_dir = g_build_filename(
      self->disk_root, job->cache_namespace, OG_METADATA_DIR, NULL);
  disk_directory_state_replace_locked(
      self, metadata_dir,
      prune_og_metadata_dir(metadata_dir, job->og_metadata_max_entries));
  G_UNLOCK(media_disk);
  g_task_return_boolean(task, TRUE);
}

static void
disk_executor_pool_worker(gpointer data, gpointer user_data)
{
  (void)user_data;
  DiskExecutorEntry *entry = data;
  GTask *task = g_steal_pointer(&entry->task);
  entry->worker(task, g_task_get_source_object(task),
                g_task_get_task_data(task), g_task_get_cancellable(task));
  g_object_unref(task);
}

static void
ensure_namespace_sweep(GnostrMediaService *self, const char *cache_namespace)
{
  if (g_hash_table_contains(self->known_namespaces, cache_namespace))
    return;
  g_hash_table_add(self->known_namespaces, g_strdup(cache_namespace));
  SweepJob *job = g_new0(SweepJob, 1);
  job->cache_namespace = g_strdup(cache_namespace);
  memcpy(job->budgets, self->config.disk_budget_bytes,
         sizeof(job->budgets));
  job->og_metadata_max_entries = self->config.og_metadata_max_entries;
  char *key = g_strdup_printf("sweep:%s", cache_namespace);
  DiskExecutorEntry *entry = disk_executor_entry_new(
      self, key, 0, FALSE, sweep_worker, job, (GDestroyNotify)sweep_job_free);
  if (!disk_executor_submit(self, entry))
    g_hash_table_remove(self->known_namespaces, cache_namespace);
}

#ifdef HAVE_SOUP3
static void read_response_chunk(GObject *source, GAsyncResult *result,
                                gpointer user_data);
static gboolean send_network_request(PendingRequest *request,
                                     const char *url);

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
  if (status == SOUP_STATUS_MOVED_PERMANENTLY ||
      status == SOUP_STATUS_FOUND ||
      status == SOUP_STATUS_SEE_OTHER ||
      status == SOUP_STATUS_TEMPORARY_REDIRECT ||
      status == SOUP_STATUS_PERMANENT_REDIRECT) {
    const char *location = soup_message_headers_get_one(
        soup_message_get_response_headers(request->message), "Location");
    g_autofree char *redirect_url = NULL;
    g_autoptr(GError) redirect_error = NULL;
    g_autofree char *current_url = soup_message_get_uri(request->message)
      ? g_uri_to_string(soup_message_get_uri(request->message))
      : g_strdup(request->url);
    if (request->redirect_count >= 5 ||
        !gnostr_media_redirect_is_safe(
            current_url, location, &redirect_url, &redirect_error)) {
      pending_fail(request,
                   g_error_new(GNOSTR_MEDIA_ERROR, GNOSTR_MEDIA_ERROR_HTTP,
                               "Unsafe media redirect: %s",
                               redirect_error ? redirect_error->message
                                              : "redirect limit exceeded"),
                   FALSE);
      return;
    }
    request->redirect_count++;
    g_input_stream_close(request->stream, NULL, NULL);
    g_clear_object(&request->stream);
    g_clear_object(&request->message);
    if (!send_network_request(request, redirect_url)) {
      pending_fail(request,
                   g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                       GNOSTR_MEDIA_ERROR_INVALID_ARGUMENT,
                                       "Invalid redirect URL"),
                   FALSE);
    }
    return;
  }
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

#ifdef HAVE_SOUP3
static gboolean
send_network_request(PendingRequest *request, const char *url)
{
  g_autoptr(GError) policy_error = NULL;
  if (!gnostr_media_url_is_safe(url, &policy_error))
    return FALSE;

  g_autoptr(SoupSession) session = gnostr_get_shared_soup_session();
  if (!session)
    return FALSE;

  request->message = soup_message_new("GET", url);
  if (!request->message)
    return FALSE;
  soup_message_set_flags(request->message, SOUP_MESSAGE_NO_REDIRECT);
  soup_message_set_priority(request->message,
                            request->kind == PENDING_OG_METADATA
                                ? SOUP_MESSAGE_PRIORITY_LOW
                                : SOUP_MESSAGE_PRIORITY_NORMAL);
  soup_session_send_async(session, request->message, G_PRIORITY_DEFAULT,
                          request->network_cancellable,
                          response_headers_ready, request);
  return TRUE;
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
  if (!gnostr_media_fetch_intent_is_allowed(request->intent) ||
      !send_network_request(request, request->url)) {
    pending_fail(request,
                 g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                     GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                                     "Media request blocked by network policy"),
                 FALSE);
    return;
  }
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
                    PendingKind kind,
                    const char *cache_namespace,
                    GnostrMediaResourceClass disk_resource_class)
{
  PendingRequest *request = g_new0(PendingRequest, 1);
  g_atomic_ref_count_init(&request->ref_count);
  request->service = g_object_ref(self);
  request->url = g_strdup(url);
  request->kind = kind;
  request->cache_namespace = g_strdup(cache_namespace);
  request->namespace_epoch = namespace_epoch_get(self, cache_namespace);
  request->disk_resource_class = disk_resource_class;
  request->table_key = pending_operation_key(url, kind, cache_namespace);
  request->subscribers =
      g_ptr_array_new_with_free_func((GDestroyNotify)subscriber_unref);
  request->network_cancellable = g_cancellable_new();
  request->decode_variants = g_hash_table_new(g_str_hash, g_str_equal);
  return request;
}

static gboolean
valid_http_url(const char *url)
{
  if (!url || !*url)
    return FALSE;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const char *host = uri ? g_uri_get_host(uri) : NULL;
  return scheme && host && *host && g_uri_get_userinfo(uri) == NULL &&
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
              PendingKind kind,
              GnostrMediaFetchIntent intent)
{
  if (!gnostr_media_fetch_intent_is_allowed(intent)) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(GNOSTR_MEDIA_ERROR,
                                          GNOSTR_MEDIA_ERROR_UNAVAILABLE,
                                          "Remote media requires user activation"));
    return TRUE;
  }
  g_autoptr(GError) policy_error = NULL;
  if (!gnostr_media_url_is_safe(url, &policy_error)) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new(GNOSTR_MEDIA_ERROR,
                                  GNOSTR_MEDIA_ERROR_INVALID_ARGUMENT,
                                  "Unsafe media URL: %s",
                                  policy_error ? policy_error->message
                                               : "invalid URL"));
    return TRUE;
  }
  if (subscriber->cancellable &&
      g_cancellable_is_cancelled(subscriber->cancellable)) {
    schedule_delivery(self, subscriber, url, NULL, NULL,
                      g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                          "Media request cancelled"));
    return TRUE;
  }
  return FALSE;
}

GdkTexture *
gnostr_media_service_lookup_cached_texture(
    GnostrMediaService *self,
    const char *url,
    GnostrMediaResourceClass resource_class,
    int target_width,
    int target_height)
{
  g_return_val_if_fail(GNOSTR_IS_MEDIA_SERVICE(self), NULL);
  g_return_val_if_fail(url != NULL && *url != '\0', NULL);
  g_return_val_if_fail(resource_class >= 0 &&
                       resource_class < GNOSTR_MEDIA_RESOURCE_N_CLASSES, NULL);
  g_return_val_if_fail(target_width > 0 && target_height > 0 &&
                       target_width <= MAX_TARGET_DIMENSION &&
                       target_height <= MAX_TARGET_DIMENSION, NULL);

  g_autofree char *cache_namespace = current_cache_namespace(self);
  return texture_cache_lookup(self, resource_class, cache_namespace, url,
                              target_width, target_height);
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
  gnostr_media_service_request_texture_with_intent(
      self, url, resource_class, target_width, target_height,
      GNOSTR_MEDIA_FETCH_AUTOMATIC, cancellable, callback, user_data,
      user_data_destroy);
}

void
gnostr_media_service_request_texture_with_intent(
                                     GnostrMediaService *self,
                                     const char *url,
                                     GnostrMediaResourceClass resource_class,
                                     int target_width,
                                     int target_height,
                                     GnostrMediaFetchIntent intent,
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

  g_autofree char *cache_namespace = current_cache_namespace(self);
  GdkTexture *cached =
      texture_cache_lookup(self, resource_class, cache_namespace, url,
                           target_width, target_height);
  if (cached) {
    schedule_delivery(self, subscriber, url, cached, NULL, NULL);
    g_object_unref(cached);
    subscriber_unref(subscriber);
    return;
  }

  if (reject_common(self, subscriber, url, PENDING_TEXTURE, intent)) {
    subscriber_unref(subscriber);
    return;
  }

  ensure_namespace_sweep(self, cache_namespace);
  g_autofree char *key = pending_operation_key(
      url, PENDING_TEXTURE, cache_namespace);
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

  request = pending_request_new(self, url, PENDING_TEXTURE,
                                cache_namespace, resource_class);
  request->intent = intent;
  attach_subscriber(request, subscriber);
  g_hash_table_insert(self->pending, request->table_key, request);
  start_disk_lookup(request);
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
  gnostr_media_service_request_og_metadata_with_intent(
      self, url, GNOSTR_MEDIA_FETCH_AUTOMATIC, cancellable, callback,
      user_data, user_data_destroy);
}

void
gnostr_media_service_request_og_metadata_with_intent(
                                         GnostrMediaService *self,
                                         const char *url,
                                         GnostrMediaFetchIntent intent,
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

  g_autofree char *cache_namespace = current_cache_namespace(self);
  GnostrOgMetadata *cached = valid_http_url(url)
      ? og_cache_lookup(self, cache_namespace, url) : NULL;
  if (cached) {
    schedule_delivery(self, subscriber, url, NULL, cached, NULL);
    gnostr_og_metadata_unref(cached);
    subscriber_unref(subscriber);
    return;
  }

  if (reject_common(self, subscriber, url, PENDING_OG_METADATA,
                    intent)) {
    subscriber_unref(subscriber);
    return;
  }

  ensure_namespace_sweep(self, cache_namespace);
  g_autofree char *key = pending_operation_key(
      url, PENDING_OG_METADATA, cache_namespace);
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

  request = pending_request_new(self, url, PENDING_OG_METADATA,
                                cache_namespace,
                                GNOSTR_MEDIA_RESOURCE_OG_IMAGE);
  request->intent = intent;
  attach_subscriber(request, subscriber);
  g_hash_table_insert(self->pending, request->table_key, request);
  start_disk_lookup(request);
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
    self->config.disk_budget_bytes[i] =
        self->texture_caches[i].stats.budget_bytes;
    texture_cache_trim(&self->texture_caches[i]);
  }

  /* Re-sweep the active namespace after any budget change. */
  g_hash_table_remove_all(self->known_namespaces);
  g_autofree char *cache_namespace = current_cache_namespace(self);
  ensure_namespace_sweep(self, cache_namespace);
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
  self->config.max_concurrent_thumbnails =
      CLAMP(self->config.max_concurrent_thumbnails, 1,
            MAX_THUMBNAIL_CONCURRENCY);
  self->config.thumbnail_timeout_msec =
      MAX(self->config.thumbnail_timeout_msec, 1);
  self->config.disk_worker_count =
      CLAMP(self->config.disk_worker_count, 1, MAX_DISK_WORKER_COUNT);
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
  out_stats->queued_thumbnails = g_queue_get_length(&self->thumbnail_queue);
  out_stats->active_thumbnails = self->active_thumbnails;
  out_stats->negative_entries = g_hash_table_size(self->negative);
  out_stats->og_metadata_entries = g_hash_table_size(self->og_cache);
  out_stats->negative_hits = self->negative_hits;
  out_stats->overflow_rejections = self->overflow_rejections;
  out_stats->og_metadata_hits = self->og_hits;
  out_stats->og_metadata_misses = self->og_misses;
  out_stats->og_metadata_evictions = self->og_evictions;
  out_stats->queued_disk_jobs = g_queue_get_length(&self->disk_queue);
  out_stats->queued_disk_bytes = self->queued_disk_bytes;
  out_stats->active_disk_jobs = self->active_disk_jobs;
  out_stats->dropped_disk_jobs = self->dropped_disk_jobs;
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

  GList *link = self->og_lru.head;
  while (link) {
    GList *next = link->next;
    OgCacheEntry *og = link->data;
    if (g_str_equal(og->url, url)) {
      og_cache_remove_entry(self, og, TRUE);
      removed++;
    }
    link = next;
  }

  link = self->negative_lru.head;
  while (link) {
    GList *next = link->next;
    NegativeEntry *entry = link->data;
    if (g_str_equal(entry->url, url)) {
      negative_remove_entry(self, entry);
      removed++;
    }
    link = next;
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

void
gnostr_media_service_evict_account(GnostrMediaService *self,
                                    const char *npub)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(valid_account_namespace(npub));

  /* Advance the generation before cancellation or deletion.  Detached disk
   * jobs capture the old generation and therefore cannot recreate the account
   * namespace after this function returns. */
  g_autofree char *namespace_dir = g_build_filename(self->disk_root, npub, NULL);
  G_LOCK(media_disk);
  guint64 *epoch = g_hash_table_lookup(self->namespace_epochs, npub);
  if (!epoch) {
    epoch = g_new0(guint64, 1);
    *epoch = 1;
    g_hash_table_insert(self->namespace_epochs, g_strdup(npub), epoch);
  }
  (*epoch)++;
  remove_tree_locked(namespace_dir);
  disk_directory_states_remove_namespace_locked(self, npub);
  G_UNLOCK(media_disk);
  g_hash_table_remove(self->known_namespaces, npub);

  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++) {
    TextureClassCache *cache = &self->texture_caches[i];
    GList *link = cache->lru.head;
    while (link) {
      GList *next = link->next;
      TextureCacheEntry *entry = link->data;
      if (g_str_equal(entry->cache_namespace, npub))
        texture_cache_remove_entry(cache, entry, TRUE);
      link = next;
    }
  }

  GList *link = self->og_lru.head;
  while (link) {
    GList *next = link->next;
    OgCacheEntry *entry = link->data;
    if (g_str_equal(entry->cache_namespace, npub))
      og_cache_remove_entry(self, entry, TRUE);
    link = next;
  }
  link = self->negative_lru.head;
  while (link) {
    GList *next = link->next;
    NegativeEntry *entry = link->data;
    if (g_str_equal(entry->cache_namespace, npub))
      negative_remove_entry(self, entry);
    link = next;
  }

  /* Snapshot strong refs: a subscriber callback may recursively evict the
   * account and remove this or another request from the pending table. */
  g_autoptr(GPtrArray) requests = g_ptr_array_new_with_free_func(
      (GDestroyNotify)pending_unref);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->pending);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    PendingRequest *request = value;
    if (g_str_equal(request->cache_namespace, npub))
      g_ptr_array_add(requests, pending_ref(request));
  }
  for (guint i = 0; i < requests->len; i++) {
    PendingRequest *request = g_ptr_array_index(requests, i);
    g_autoptr(GError) error = g_error_new_literal(
        G_IO_ERROR, G_IO_ERROR_CANCELLED, "Account media cache evicted");
    for (guint j = 0; j < request->subscribers->len; j++) {
      Subscriber *subscriber = g_ptr_array_index(request->subscribers, j);
      if (!subscriber->completed) {
        if (subscriber->kind == PENDING_TEXTURE)
          subscriber_complete_texture(subscriber, NULL, error);
        else
          subscriber_complete_og(subscriber, NULL, error);
      }
    }
    if (request->queued) {
      g_queue_remove(&self->download_queue, request);
      request->queued = FALSE;
      request->network_done = TRUE;
    }
    if (request->thumbnail_queued) {
      g_queue_remove(&self->thumbnail_queue, request);
      request->thumbnail_queued = FALSE;
      request->network_done = TRUE;
    }
    g_cancellable_cancel(request->network_cancellable);
    pending_maybe_finish(request);
  }
  start_queued_downloads(self);
  start_queued_thumbnails(self);
}

#ifdef GNOSTR_MEDIA_SERVICE_TESTING
void
gnostr_media_service_test_set_disk_root(GnostrMediaService *self,
                                         const char *disk_root)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(disk_root != NULL);
  g_return_if_fail(g_hash_table_size(self->known_namespaces) == 0);
  G_LOCK(media_disk);
  g_hash_table_remove_all(self->disk_directory_states);
  g_free(self->disk_root);
  self->disk_root = g_strdup(disk_root);
  G_UNLOCK(media_disk);
}

void
gnostr_media_service_test_set_namespace(GnostrMediaService *self,
                                         const char *cache_namespace)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(cache_namespace != NULL && *cache_namespace != '\0');
  g_return_if_fail(strchr(cache_namespace, G_DIR_SEPARATOR) == NULL);
  g_free(self->namespace_override);
  self->namespace_override = g_strdup(cache_namespace);
}

guint
gnostr_media_service_test_get_outstanding_disk_jobs(GnostrMediaService *self)
{
  g_return_val_if_fail(GNOSTR_IS_MEDIA_SERVICE(self), 0);
  return self->outstanding_disk_jobs;
}

void
gnostr_media_service_test_enqueue_disk_write(
    GnostrMediaService *self,
    const char *cache_namespace,
    GnostrMediaResourceClass resource_class,
    const char *url,
    GBytes *bytes)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(cache_namespace != NULL);
  g_return_if_fail(resource_class >= 0 &&
                   resource_class < GNOSTR_MEDIA_RESOURCE_N_CLASSES);
  g_return_if_fail(url != NULL);
  g_return_if_fail(bytes != NULL);

  DiskWriteJob *job = g_new0(DiskWriteJob, 1);
  job->cache_namespace = g_strdup(cache_namespace);
  job->namespace_epoch = namespace_epoch_get(self, cache_namespace);
  job->url = g_strdup(url);
  job->resource_class = resource_class;
  job->bytes = g_bytes_ref(bytes);
  job->budget = self->config.disk_budget_bytes[resource_class];
  char *key = g_strdup_printf("texture:%s:%u:%s", cache_namespace,
                              (guint)resource_class, url);
  DiskExecutorEntry *entry = disk_executor_entry_new(
      self, key, g_bytes_get_size(bytes), TRUE, disk_write_worker, job,
      (GDestroyNotify)disk_write_job_free);
  disk_executor_submit(self, entry);
}

void
gnostr_media_service_test_enqueue_sweep(
    GnostrMediaService *self,
    const char *cache_namespace)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(cache_namespace != NULL);

  SweepJob *job = g_new0(SweepJob, 1);
  job->cache_namespace = g_strdup(cache_namespace);
  memcpy(job->budgets, self->config.disk_budget_bytes,
         sizeof(job->budgets));
  job->og_metadata_max_entries = self->config.og_metadata_max_entries;
  char *key = g_strdup_printf("sweep:%s", cache_namespace);
  DiskExecutorEntry *entry = disk_executor_entry_new(
      self, key, 0, FALSE, sweep_worker, job, (GDestroyNotify)sweep_job_free);
  (void)disk_executor_submit(self, entry);
}

void
gnostr_media_service_test_set_thumbnail_extractor(
    GnostrMediaService *self,
    GnostrMediaTestThumbnailExtractor extractor,
    gpointer user_data,
    GDestroyNotify user_data_destroy)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_return_if_fail(self->active_thumbnails == 0 &&
                   g_queue_is_empty(&self->thumbnail_queue));
  if (self->test_thumbnail_data_destroy)
    self->test_thumbnail_data_destroy(self->test_thumbnail_data);
  self->test_thumbnail_extractor = extractor;
  self->test_thumbnail_data = user_data;
  self->test_thumbnail_data_destroy = user_data_destroy;
}

void
gnostr_media_service_test_store_texture(GnostrMediaService *self,
                                        const char *url,
                                        GnostrMediaResourceClass resource_class,
                                        int target_width,
                                        int target_height,
                                        GdkTexture *texture)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_autofree char *cache_namespace = current_cache_namespace(self);
  texture_cache_store(self, resource_class, cache_namespace, url, target_width,
                      target_height, texture);
}

void
gnostr_media_service_test_store_negative(GnostrMediaService *self,
                                         const char *url,
                                         gboolean metadata_request)
{
  g_return_if_fail(GNOSTR_IS_MEDIA_SERVICE(self));
  g_autofree char *cache_namespace = current_cache_namespace(self);
  negative_store(self, cache_namespace, url,
                 metadata_request ? PENDING_OG_METADATA : PENDING_TEXTURE);
}

gboolean
gnostr_media_service_test_is_negative(GnostrMediaService *self,
                                      const char *url,
                                      gboolean metadata_request)
{
  g_return_val_if_fail(GNOSTR_IS_MEDIA_SERVICE(self), FALSE);
  g_autofree char *cache_namespace = current_cache_namespace(self);
  return negative_lookup(self, cache_namespace, url,
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
  g_assert(self->outstanding_disk_jobs == 0);
  g_assert(self->active_disk_jobs == 0);
  g_assert(g_queue_is_empty(&self->disk_queue));
  g_assert(g_hash_table_size(self->disk_coalesced) == 0);
  if (self->disk_pool)
    g_thread_pool_free(self->disk_pool, FALSE, TRUE);
  g_hash_table_unref(self->disk_coalesced);
  g_hash_table_unref(self->known_namespaces);
  g_hash_table_unref(self->namespace_epochs);
  g_hash_table_unref(self->disk_directory_states);
  g_free(self->disk_root);
#ifdef GNOSTR_MEDIA_SERVICE_TESTING
  if (self->test_thumbnail_data_destroy)
    self->test_thumbnail_data_destroy(self->test_thumbnail_data);
  g_free(self->namespace_override);
#endif
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
  self->disk_root = g_build_filename(g_get_user_cache_dir(),
                                     "gnostr", "media", NULL);
  self->known_namespaces = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, NULL);
  self->disk_coalesced = g_hash_table_new(g_str_hash, g_str_equal);
  self->namespace_epochs = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, g_free);
  self->disk_directory_states = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free,
      (GDestroyNotify)disk_directory_state_free);
  for (guint i = 0; i < GNOSTR_MEDIA_RESOURCE_N_CLASSES; i++)
    self->texture_caches[i].entries =
        g_hash_table_new(g_str_hash, g_str_equal);
}
