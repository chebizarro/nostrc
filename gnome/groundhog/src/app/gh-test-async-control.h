/* Shared, test-only completion scheduler for Groundhog GUI lifetime tests.
 * A site/key pair has a stable delay for a given seed, independent of the
 * order in which other callbacks happen to arrive. Production never enables
 * this path: g_test_init() and GH_TEST_ASYNC_JITTER_MS are both required. */
#ifndef GH_TEST_ASYNC_CONTROL_H
#define GH_TEST_ASYNC_CONTROL_H

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define GH_TEST_ASYNC_DEFAULT_SEED 20261004u

static inline gboolean
gh_test_async_parse_uint(const gchar *text, guint64 limit, guint64 *value)
{
  if (!text || !*text)
    return FALSE;
  for (const gchar *p = text; *p; p++)
    if (!g_ascii_isdigit(*p))
      return FALSE;
  gchar *end = NULL;
  guint64 parsed = g_ascii_strtoull(text, &end, 10);
  if (!end || *end || parsed > limit)
    return FALSE;
  *value = parsed;
  return TRUE;
}

static inline gboolean
gh_test_async_settings(guint *maximum, guint32 *seed)
{
  guint64 parsed;
  if (!g_test_initialized() ||
      !gh_test_async_parse_uint(g_getenv("GH_TEST_ASYNC_JITTER_MS"), 50, &parsed) ||
      parsed == 0)
    return FALSE;
  *maximum = (guint)parsed;
  const gchar *seed_text = g_getenv("GH_TEST_ASYNC_SEED");
  *seed = GH_TEST_ASYNC_DEFAULT_SEED;
  if (seed_text && !gh_test_async_parse_uint(seed_text, G_MAXUINT32, &parsed))
    g_error("GH_TEST_ASYNC_SEED must be an unsigned 32-bit integer");
  if (seed_text)
    *seed = (guint32)parsed;
  static gsize logged;
  if (g_once_init_enter(&logged)) {
    g_test_message("Groundhog async jitter: seed=%u%s, maximum=%u ms",
                   *seed, seed_text ? "" : " (default)", *maximum);
    g_once_init_leave(&logged, 1);
  }
  return TRUE;
}

static inline guint
gh_test_async_delay_ms(const gchar *site, const gchar *key, guint maximum, guint32 seed)
{
  guint32 hash = 2166136261u ^ seed;
  for (const guchar *p = (const guchar *)site; *p; p++)
    hash = (hash ^ *p) * 16777619u;
  hash = (hash ^ 0xffu) * 16777619u;
  for (const guchar *p = (const guchar *)(key ? key : ""); *p; p++)
    hash = (hash ^ *p) * 16777619u;
  return hash % (maximum + 1u);
}

static inline void
gh_test_async_trace(const gchar *phase, const gchar *site, guint delay, guint32 seed,
                    gconstpointer id)
{
  const gchar *path = g_getenv("GH_TEST_ASYNC_TRACE");
  if (!path || !*path)
    return;
  int fd = g_open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0)
    return;
  g_autofree gchar *line = g_strdup_printf("%s %s seed=%u delay=%u pid=%ld id=%p\n",
                                           phase, site, seed, delay, (long)getpid(), id);
  const gchar *cursor = line;
  gsize remaining = strlen(line);
  while (remaining) {
    ssize_t written = write(fd, cursor, remaining);
    if (written <= 0)
      break;
    cursor += written;
    remaining -= (gsize)written;
  }
  close(fd);
}

typedef struct {
  GSourceFunc callback;
  gpointer data;
  GDestroyNotify destroy;
  gchar *site;
  guint delay;
  guint32 seed;
} GhTestAsyncPending;

static inline gboolean
gh_test_async_deliver(gpointer data)
{
  GhTestAsyncPending *pending = data;
  gh_test_async_trace("delivered", pending->site, pending->delay, pending->seed, pending);
  pending->callback(pending->data);
  return G_SOURCE_REMOVE;
}

static inline void
gh_test_async_pending_free(gpointer data)
{
  GhTestAsyncPending *pending = data;
  if (pending->destroy)
    pending->destroy(pending->data);
  g_free(pending->site);
  g_free(pending);
}

/* Calls inline with no test control; otherwise owns data until its source
 * runs in the callback's context. A zero-ms choice still crosses an idle. */
static inline void
gh_test_async_complete(const gchar *site, const gchar *key, GMainContext *context,
                       GSourceFunc callback, gpointer data, GDestroyNotify destroy)
{
  guint maximum;
  guint32 seed;
  if (!gh_test_async_settings(&maximum, &seed)) {
    callback(data);
    if (destroy)
      destroy(data);
    return;
  }
  GhTestAsyncPending *pending = g_new0(GhTestAsyncPending, 1);
  pending->callback = callback;
  pending->data = data;
  pending->destroy = destroy;
  pending->site = g_strdup(site);
  pending->seed = seed;
  pending->delay = gh_test_async_delay_ms(site, key, maximum, seed);
  gh_test_async_trace("queued", site, pending->delay, seed, pending);
  GSource *source = g_timeout_source_new(pending->delay);
  g_source_set_callback(source, gh_test_async_deliver, pending, gh_test_async_pending_free);
  GMainContext *target = context ? g_main_context_ref(context) :
                                   g_main_context_ref_thread_default();
  g_source_attach(source, target);
  g_main_context_unref(target);
  g_source_unref(source);
}

typedef struct {
  GObject *source;
  GAsyncResult *result;
  GAsyncReadyCallback callback;
  gpointer data;
} GhTestAsyncResult;

static inline GPrivate *
gh_test_async_active_result(void)
{
  static GPrivate active = G_PRIVATE_INIT(NULL);
  return &active;
}

static inline gboolean
gh_test_async_deliver_result(gpointer data)
{
  GhTestAsyncResult *pending = data;
  GPrivate *active = gh_test_async_active_result();
  gpointer previous = g_private_get(active);
  g_private_set(active, pending->result);
  pending->callback(pending->source, pending->result, pending->data);
  g_private_set(active, previous);
  return G_SOURCE_REMOVE;
}

static inline void
gh_test_async_result_free(gpointer data)
{
  GhTestAsyncResult *pending = data;
  g_clear_object(&pending->source);
  g_object_unref(pending->result);
  g_free(pending);
}

/* Put this at the top of an async completion and return when it says TRUE.
 * Its timer re-enters the same callback with the same result; the guard only
 * exempts that delivery, not a nested completion for a different result. */
static inline gboolean
gh_test_async_defer_result(const gchar *site, const gchar *key, GObject *source,
                           GAsyncResult *result, GAsyncReadyCallback callback, gpointer data)
{
  guint maximum;
  guint32 seed;
  if (g_private_get(gh_test_async_active_result()) == result ||
      !gh_test_async_settings(&maximum, &seed))
    return FALSE;
  GhTestAsyncResult *pending = g_new0(GhTestAsyncResult, 1);
  pending->source = source ? g_object_ref(source) : NULL;
  pending->result = g_object_ref(result);
  pending->callback = callback;
  pending->data = data;
  gh_test_async_complete(site, key, NULL, gh_test_async_deliver_result, pending,
                         gh_test_async_result_free);
  return TRUE;
}

#endif
