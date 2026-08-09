#include "gnostr-avatar-cache.h"

#include "../services/gnostr-media-service.h"

#define DEFAULT_AVATAR_SIZE 96
#define MAX_AVATAR_SIZE 512
#define GNOSTR_AVATAR_EXPECTED_URL_KEY "gnostr-avatar-expected-url"

typedef struct {
  GWeakRef image_ref;
  GWeakRef initials_ref;
  char *url;
} AvatarRequest;

static GnostrAvatarMetrics avatar_metrics;
static guint avatar_size;

static gboolean
avatar_url_is_http(const char *url)
{
  return url && *url &&
         (g_str_has_prefix(url, "https://") ||
          g_str_has_prefix(url, "http://"));
}

static guint
avatar_target_size(void)
{
  if (avatar_size != 0)
    return avatar_size;

  avatar_size = DEFAULT_AVATAR_SIZE;
  const char *value = g_getenv("GNOSTR_AVATAR_SIZE");
  if (value && *value) {
    char *end = NULL;
    gint64 parsed = g_ascii_strtoll(value, &end, 10);
    if (end && *end == '\0' && parsed >= 32 && parsed <= MAX_AVATAR_SIZE)
      avatar_size = (guint)parsed;
    else
      g_warning("[AVATAR_CACHE] Invalid GNOSTR_AVATAR_SIZE=%s "
                "(must be 32-%d), using default", value, MAX_AVATAR_SIZE);
  }
  return avatar_size;
}

static void
avatar_widget_set_expected_url(GtkWidget *image, const char *url)
{
  if (!image || !GTK_IS_WIDGET(image))
    return;
  g_object_set_data_full(G_OBJECT(image), GNOSTR_AVATAR_EXPECTED_URL_KEY,
                         g_strdup(url ? url : ""), g_free);
}

static gboolean
avatar_widget_still_expects_url(GtkWidget *image, const char *url)
{
  if (!image || !GTK_IS_WIDGET(image))
    return FALSE;
  const char *expected =
      g_object_get_data(G_OBJECT(image), GNOSTR_AVATAR_EXPECTED_URL_KEY);
  return !expected || g_strcmp0(expected, url) == 0;
}

static gboolean
avatar_apply_texture(GtkWidget *image,
                     GtkWidget *initials,
                     const char *url,
                     GdkTexture *texture)
{
  if (!image || !texture || !avatar_widget_still_expects_url(image, url) ||
      gtk_widget_get_native(image) == NULL || !gtk_widget_get_mapped(image))
    return FALSE;

  if (GTK_IS_PICTURE(image))
    gtk_picture_set_paintable(GTK_PICTURE(image), GDK_PAINTABLE(texture));
  else if (GTK_IS_IMAGE(image))
    gtk_image_set_from_paintable(GTK_IMAGE(image), GDK_PAINTABLE(texture));
  else
    return FALSE;

  gtk_widget_set_visible(image, TRUE);
  if (initials && GTK_IS_WIDGET(initials) &&
      gtk_widget_get_native(initials) != NULL)
    gtk_widget_set_visible(initials, FALSE);
  return TRUE;
}

static AvatarRequest *
avatar_request_new(const char *url, GtkWidget *image, GtkWidget *initials)
{
  AvatarRequest *request = g_new0(AvatarRequest, 1);
  g_weak_ref_init(&request->image_ref, image);
  g_weak_ref_init(&request->initials_ref, initials);
  request->url = g_strdup(url);
  return request;
}

static void
avatar_request_free(AvatarRequest *request)
{
  if (!request)
    return;
  g_weak_ref_clear(&request->image_ref);
  g_weak_ref_clear(&request->initials_ref);
  g_free(request->url);
  g_free(request);
}

static void
avatar_texture_ready(GnostrMediaService *service,
                     const char *url,
                     GdkTexture *texture,
                     const GError *error,
                     gpointer user_data)
{
  (void)service;
  AvatarRequest *request = user_data;
  g_autoptr(GtkWidget) image = g_weak_ref_get(&request->image_ref);
  g_autoptr(GtkWidget) initials = g_weak_ref_get(&request->initials_ref);

  if (texture) {
    avatar_metrics.http_ok++;
    avatar_apply_texture(image, initials, url, texture);
  } else {
    avatar_metrics.http_error++;
    if (initials && GTK_IS_WIDGET(initials) &&
        gtk_widget_get_native(initials) != NULL &&
        avatar_widget_still_expects_url(image, request->url)) {
      gtk_widget_set_visible(initials, TRUE);
      avatar_metrics.initials_shown++;
    }
    if (error)
      g_debug("avatar request failed for %s: %s", url, error->message);
  }
}

static void
avatar_prefetch_ready(GnostrMediaService *service,
                      const char *url,
                      GdkTexture *texture,
                      const GError *error,
                      gpointer user_data)
{
  (void)service;
  (void)url;
  (void)user_data;
  if (texture)
    avatar_metrics.http_ok++;
  else {
    avatar_metrics.http_error++;
    if (error)
      g_debug("avatar prefetch failed: %s", error->message);
  }
}

void
gnostr_avatar_prefetch(const char *url)
{
  if (!avatar_url_is_http(url))
    return;

  avatar_metrics.requests_total++;
  avatar_metrics.http_start++;
  guint size = avatar_target_size();
  gnostr_media_service_request_texture(
      gnostr_media_service_get_default(), url, GNOSTR_MEDIA_RESOURCE_AVATAR,
      size, size, NULL, avatar_prefetch_ready, NULL, NULL);
}

GdkTexture *
gnostr_avatar_try_load_cached(const char *url)
{
  if (!avatar_url_is_http(url))
    return NULL;

  guint size = avatar_target_size();
  GdkTexture *texture = gnostr_media_service_lookup_cached_texture(
      gnostr_media_service_get_default(), url, GNOSTR_MEDIA_RESOURCE_AVATAR,
      size, size);
  if (texture)
    avatar_metrics.mem_cache_hits++;
  return texture;
}

void
gnostr_avatar_download_async(const char *url,
                             GtkWidget *image,
                             GtkWidget *initials)
{
  gnostr_avatar_download_async_with_intent(
      url, image, initials, GNOSTR_MEDIA_FETCH_AUTOMATIC);
}

void
gnostr_avatar_download_async_with_intent(const char *url,
                                         GtkWidget *image,
                                         GtkWidget *initials,
                                         GnostrMediaFetchIntent intent)
{
  if (!avatar_url_is_http(url))
    return;

  avatar_widget_set_expected_url(image, url);
  avatar_metrics.requests_total++;

  g_autoptr(GdkTexture) cached = gnostr_avatar_try_load_cached(url);
  if (cached && avatar_apply_texture(image, initials, url, cached))
    return;

  AvatarRequest *request = avatar_request_new(url, image, initials);
  avatar_metrics.http_start++;
  guint size = avatar_target_size();
  gnostr_media_service_request_texture_with_intent(
      gnostr_media_service_get_default(), url, GNOSTR_MEDIA_RESOURCE_AVATAR,
      size, size, intent, NULL, avatar_texture_ready, request,
      (GDestroyNotify)avatar_request_free);
}

void
gnostr_avatar_cache_set_startup_mode(gboolean enabled)
{
  (void)enabled;
  /* The shared service owns one bounded queue for every media class. */
}

#ifdef HAVE_SOUP3
void
gnostr_avatar_download_async_soup(const char *url,
                                  GtkWidget *image,
                                  GtkWidget *initials)
{
  gnostr_avatar_download_async(url, image, initials);
}
#endif

guint
gnostr_avatar_cache_size(void)
{
  GnostrMediaCacheStats stats;
  gnostr_media_service_get_stats(gnostr_media_service_get_default(), &stats);
  return stats.classes[GNOSTR_MEDIA_RESOURCE_AVATAR].entries;
}

void
gnostr_avatar_metrics_get(GnostrAvatarMetrics *out)
{
  if (out)
    *out = avatar_metrics;
}

void
gnostr_avatar_metrics_log(void)
{
  GnostrMediaCacheStats stats;
  gnostr_media_service_get_stats(gnostr_media_service_get_default(), &stats);
  GnostrMediaClassStats *avatar =
      &stats.classes[GNOSTR_MEDIA_RESOURCE_AVATAR];

  g_message("avatar_metrics: requests=%" G_GUINT64_FORMAT
            " mem_hits=%" G_GUINT64_FORMAT
            " disk_hits=%" G_GUINT64_FORMAT
            " http_start=%" G_GUINT64_FORMAT
            " http_ok=%" G_GUINT64_FORMAT
            " http_err=%" G_GUINT64_FORMAT
            " initials=%" G_GUINT64_FORMAT
            " cache_write_err=%" G_GUINT64_FORMAT
            " entries=%u resident=%" G_GUINT64_FORMAT
            "/%" G_GUINT64_FORMAT " evictions=%" G_GUINT64_FORMAT,
            avatar_metrics.requests_total,
            avatar_metrics.mem_cache_hits,
            avatar_metrics.disk_cache_hits,
            avatar_metrics.http_start,
            avatar_metrics.http_ok,
            avatar_metrics.http_error,
            avatar_metrics.initials_shown,
            avatar_metrics.cache_write_error,
            avatar->entries,
            avatar->resident_bytes,
            avatar->budget_bytes,
            avatar->evictions);
}
