/**
 * trending-hashtags.h - Local trending hashtag computation
 *
 * Scans recent kind-1 notes in the local NDB store, extracts "t" (hashtag)
 * tags, counts occurrences, and returns the top N trending hashtags.
 *
 * This is a purely local computation — no relay queries are made. The quality
 * of results depends on the volume of events already ingested into NDB.
 */

#ifndef GNOSTR_TRENDING_HASHTAGS_H
#define GNOSTR_TRENDING_HASHTAGS_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrTrendingHashtag:
 * @tag: The hashtag string (without '#' prefix), owned by the struct.
 * @count: Number of distinct events containing this hashtag.
 *
 * A single trending hashtag entry with its occurrence count.
 */
typedef struct {
    char *tag;
    guint count;
} GnostrTrendingHashtag;

/**
 * gnostr_trending_hashtag_free:
 * @ht: (nullable): trending hashtag entry to free
 *
 * Frees a GnostrTrendingHashtag and its tag string.
 */
void gnostr_trending_hashtag_free(GnostrTrendingHashtag *ht);

/**
 * gnostr_compute_trending_hashtags:
 * @max_events: Maximum number of recent kind-1 events to scan (e.g. 500).
 *              Larger values give better results but take longer.
 * @top_n: Number of top hashtags to return (e.g. 15).
 *
 * Scans the most recent @max_events kind-1 notes in NDB, extracts all "t"
 * tags, and returns the @top_n most frequently occurring hashtags.
 *
 * Hashtags are normalized to lowercase for counting. Single-character tags
 * and common spam patterns are filtered out.
 *
 * This function opens its own NDB read transaction internally.
 *
 * Returns: (transfer full) (element-type GnostrTrendingHashtag): A GPtrArray
 *          of GnostrTrendingHashtag entries sorted by count (descending).
 *          The array has g_free as element destructor. May be empty but never NULL.
 *          Caller owns the array and must call g_ptr_array_unref() when done.
 */
GPtrArray *gnostr_compute_trending_hashtags(guint max_events, guint top_n);

/**
 * gnostr_compute_trending_hashtags_async:
 * @max_events: Maximum number of recent kind-1 events to scan.
 * @top_n: Number of top hashtags to return.
 * @callback: (scope async): Called on the main thread with the result.
 * @user_data: Passed to @callback.
 * @cancellable: (nullable): Optional GCancellable to cancel the operation.
 *
 * Async wrapper that runs the computation in a GLib worker thread and
 * delivers results via @callback on the main thread. If @cancellable is
 * cancelled before completion, the callback will not be invoked.
 */
typedef void (*GnostrTrendingHashtagsCallback)(GPtrArray *hashtags, gpointer user_data);

void gnostr_compute_trending_hashtags_async(guint max_events,
                                            guint top_n,
                                            GnostrTrendingHashtagsCallback callback,
                                            gpointer user_data,
                                            GCancellable *cancellable);

G_END_DECLS

#endif /* GNOSTR_TRENDING_HASHTAGS_H */
