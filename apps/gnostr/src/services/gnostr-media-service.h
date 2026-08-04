#ifndef GNOSTR_MEDIA_SERVICE_H
#define GNOSTR_MEDIA_SERVICE_H

#include <gdk/gdk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_MEDIA_SERVICE (gnostr_media_service_get_type())
G_DECLARE_FINAL_TYPE(GnostrMediaService, gnostr_media_service,
                     GNOSTR, MEDIA_SERVICE, GObject)

/**
 * GnostrMediaResourceClass:
 * @GNOSTR_MEDIA_RESOURCE_INLINE: inline note images
 * @GNOSTR_MEDIA_RESOURCE_OG_IMAGE: Open Graph preview images
 * @GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER: video poster images
 *
 * Independently-budgeted media cache classes.  Memory budgets account
 * decoded texture bytes; disk budgets account encoded response bytes.
 */
typedef enum {
  GNOSTR_MEDIA_RESOURCE_INLINE = 0,
  GNOSTR_MEDIA_RESOURCE_OG_IMAGE,
  GNOSTR_MEDIA_RESOURCE_VIDEO_POSTER,
  GNOSTR_MEDIA_RESOURCE_N_CLASSES
} GnostrMediaResourceClass;

typedef enum {
  GNOSTR_MEDIA_ERROR_INVALID_ARGUMENT,
  GNOSTR_MEDIA_ERROR_UNAVAILABLE,
  GNOSTR_MEDIA_ERROR_OVERFLOW,
  GNOSTR_MEDIA_ERROR_TOO_LARGE,
  GNOSTR_MEDIA_ERROR_HTTP,
  GNOSTR_MEDIA_ERROR_DECODE,
  GNOSTR_MEDIA_ERROR_NEGATIVE_CACHED
} GnostrMediaError;

#define GNOSTR_MEDIA_ERROR (gnostr_media_error_quark())
GQuark gnostr_media_error_quark(void);

/**
 * GnostrMediaServiceConfig:
 *
 * Explicit construction limits.  Zero memory budget disables caching for that
 * class; it never means an unbounded cache.  Use
 * gnostr_media_service_config_init() before overriding selected fields.
 */
typedef struct {
  guint64 memory_budget_bytes[GNOSTR_MEDIA_RESOURCE_N_CLASSES];
  gsize body_size_caps[GNOSTR_MEDIA_RESOURCE_N_CLASSES];
  gsize og_metadata_body_size_cap;
  guint max_in_flight;
  guint max_concurrent_downloads;
  /* One-shot video poster extraction is independently capped at two workers. */
  guint max_concurrent_thumbnails;
  guint thumbnail_timeout_msec;
  guint negative_cache_max_entries;
  gint64 negative_cache_ttl_usec;
  guint og_metadata_max_entries;
  gint64 og_metadata_ttl_usec;
  /* Encoded-byte disk ceilings, independently enforced per namespace and
   * resource class.  Zero disables the disk tier for that class. */
  guint64 disk_budget_bytes[GNOSTR_MEDIA_RESOURCE_N_CLASSES];
} GnostrMediaServiceConfig;

typedef struct {
  guint64 budget_bytes;
  guint64 resident_bytes;
  guint64 peak_resident_bytes;
  guint entries;
  guint64 hits;
  guint64 misses;
  guint64 evictions;
} GnostrMediaClassStats;

typedef struct {
  GnostrMediaClassStats classes[GNOSTR_MEDIA_RESOURCE_N_CLASSES];
  guint pending_requests;
  guint queued_downloads;
  guint active_downloads;
  guint queued_thumbnails;
  guint active_thumbnails;
  guint negative_entries;
  guint og_metadata_entries;
  guint64 negative_hits;
  guint64 overflow_rejections;
  guint64 og_metadata_hits;
  guint64 og_metadata_misses;
  guint64 og_metadata_evictions;
} GnostrMediaCacheStats;

typedef struct _GnostrOgMetadata GnostrOgMetadata;

GnostrOgMetadata *gnostr_og_metadata_ref(GnostrOgMetadata *metadata);
void gnostr_og_metadata_unref(GnostrOgMetadata *metadata);
const char *gnostr_og_metadata_get_title(const GnostrOgMetadata *metadata);
const char *gnostr_og_metadata_get_description(const GnostrOgMetadata *metadata);
const char *gnostr_og_metadata_get_image_url(const GnostrOgMetadata *metadata);
const char *gnostr_og_metadata_get_source_url(const GnostrOgMetadata *metadata);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnostrOgMetadata, gnostr_og_metadata_unref)

typedef void (*GnostrMediaTextureCallback)(GnostrMediaService *service,
                                           const char *url,
                                           GdkTexture *texture,
                                           const GError *error,
                                           gpointer user_data);

typedef void (*GnostrMediaOgCallback)(GnostrMediaService *service,
                                      const char *url,
                                      GnostrOgMetadata *metadata,
                                      const GError *error,
                                      gpointer user_data);

/**
 * gnostr_media_service_config_init:
 * @config: configuration to initialize
 *
 * Initializes production-safe defaults.  The default singleton additionally
 * replaces the three memory and disk budgets from org.gnostr.Client GSettings
 * and tracks later setting changes.  Each setting is a separate ceiling for
 * each tier, not a combined memory-plus-disk quota.
 */
void gnostr_media_service_config_init(GnostrMediaServiceConfig *config);

/**
 * gnostr_media_service_new:
 * @config: (nullable): explicit limits, or defaults
 *
 * Creates an isolated service.  This is primarily useful to tests and embedders;
 * application code should normally use gnostr_media_service_get_default().
 */
GnostrMediaService *gnostr_media_service_new(const GnostrMediaServiceConfig *config);

/**
 * gnostr_media_service_get_default:
 *
 * Gets the process service.  Its cache budgets are read from GSettings keys
 * image-cache-max-mb, og-image-cache-max-mb, and
 * video-poster-cache-max-mb.  The returned object is owned by the service.
 *
 * Returns: (transfer none): the default media service
 */
GnostrMediaService *gnostr_media_service_get_default(void);

/**
 * gnostr_media_service_request_texture:
 *
 * Requests a texture asynchronously.  Download work is URL-deduplicated;
 * subscribers may independently choose a cache class, target dimensions and
 * cancellable.  Both target dimensions must be positive.  The callback always
 * runs on the main context that constructed @service.  @texture and @error are
 * borrowed for the duration of the callback.
 */
void gnostr_media_service_request_texture(GnostrMediaService *service,
                                          const char *url,
                                          GnostrMediaResourceClass resource_class,
                                          int target_width,
                                          int target_height,
                                          GCancellable *cancellable,
                                          GnostrMediaTextureCallback callback,
                                          gpointer user_data,
                                          GDestroyNotify user_data_destroy);

/**
 * gnostr_media_service_request_og_metadata:
 *
 * Requests shared, TTL-cached Open Graph metadata.  @metadata is borrowed for
 * the callback; use gnostr_og_metadata_ref() to retain it.
 */
void gnostr_media_service_request_og_metadata(GnostrMediaService *service,
                                              const char *url,
                                              GCancellable *cancellable,
                                              GnostrMediaOgCallback callback,
                                              gpointer user_data,
                                              GDestroyNotify user_data_destroy);

void gnostr_media_service_get_stats(GnostrMediaService *service,
                                    GnostrMediaCacheStats *out_stats);

/* URL eviction remains synchronous and memory-only. */
guint gnostr_media_service_evict_url(GnostrMediaService *service,
                                     const char *url);
void gnostr_media_service_clear_class(GnostrMediaService *service,
                                      GnostrMediaResourceClass resource_class);
void gnostr_media_service_clear_all(GnostrMediaService *service);

/**
 * gnostr_media_service_evict_account:
 * @npub: account npub whose private media namespace should be removed
 *
 * Cancels account-scoped in-flight work, purges its memory entries, and removes
 * its disk namespace.  This is an account-removal hook; logout must not call it.
 */
void gnostr_media_service_evict_account(GnostrMediaService *service,
                                        const char *npub);

#ifdef GNOSTR_MEDIA_SERVICE_TESTING
/* These must be set before the service's first request. */
void gnostr_media_service_test_set_disk_root(GnostrMediaService *service,
                                             const char *disk_root);
void gnostr_media_service_test_set_namespace(GnostrMediaService *service,
                                             const char *cache_namespace);
guint gnostr_media_service_test_get_outstanding_disk_jobs(
    GnostrMediaService *service);

typedef GBytes *(*GnostrMediaTestThumbnailExtractor)(
    const char *url,
    guint timeout_msec,
    GCancellable *cancellable,
    GError **error,
    gpointer user_data);
void gnostr_media_service_test_set_thumbnail_extractor(
    GnostrMediaService *service,
    GnostrMediaTestThumbnailExtractor extractor,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void gnostr_media_service_test_store_texture(GnostrMediaService *service,
                                             const char *url,
                                             GnostrMediaResourceClass resource_class,
                                             int target_width,
                                             int target_height,
                                             GdkTexture *texture);
void gnostr_media_service_test_store_negative(GnostrMediaService *service,
                                              const char *url,
                                              gboolean metadata_request);
gboolean gnostr_media_service_test_is_negative(GnostrMediaService *service,
                                               const char *url,
                                               gboolean metadata_request);
#endif

G_END_DECLS

#endif /* GNOSTR_MEDIA_SERVICE_H */
