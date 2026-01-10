#ifndef GNOSTR_AVATAR_CACHE_H
#define GNOSTR_AVATAR_CACHE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Avatar metrics for pipeline health. */
typedef struct {
  guint64 requests_total;     /* total prefetch + UI avatar set attempts with valid http(s) URL */
  guint64 mem_cache_hits;     /* in-memory texture cache hits */
  guint64 disk_cache_hits;    /* disk cache hits promoted to memory */
  guint64 http_start;         /* HTTP fetches started */
  guint64 http_ok;            /* HTTP fetches successfully completed */
  guint64 http_error;         /* HTTP fetches failed */
  guint64 initials_shown;     /* times we fell back to initials in UI */
  guint64 cache_write_error;  /* errors writing fetched bytes to disk */
} GnostrAvatarMetrics;

/* Public API for avatar cache */
void gnostr_avatar_prefetch(const char *url);
GdkTexture *gnostr_avatar_try_load_cached(const char *url);
void gnostr_avatar_download_async(const char *url, GtkWidget *image, GtkWidget *initials);

#ifdef HAVE_SOUP3
void gnostr_avatar_download_async_soup(const char *url, GtkWidget *image, GtkWidget *initials);
#endif

/* Metrics helpers */
void gnostr_avatar_metrics_get(GnostrAvatarMetrics *out);
void gnostr_avatar_metrics_log(void);

G_END_DECLS

#endif /* GNOSTR_AVATAR_CACHE_H */
