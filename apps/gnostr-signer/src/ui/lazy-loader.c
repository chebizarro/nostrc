/* lazy-loader.c - Lazy loading implementation
 *
 * Implements deferred initialization of UI components for improved
 * startup performance and memory efficiency.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lazy-loader.h"
#include "../memory-profile.h"

/* Component entry */
typedef struct {
  GnLazyConfig config;
  GnLazyState state;
  GtkWidget *widget;
  gint64 last_access;
  gint64 load_time;
  guint unload_timer_id;
} LazyEntry;

/* Async load request */
typedef struct {
  GnLazyComponent id;
  GnLazyCallback callback;
  gpointer user_data;
} AsyncLoadRequest;

/* Async load idle callback */
static gboolean async_load_idle_cb(gpointer data) {
  AsyncLoadRequest *r = data;
  GtkWidget *widget = gn_lazy_get(r->id);
  r->callback(r->id, widget, r->user_data);
  g_free(r);
  return G_SOURCE_REMOVE;
}

/* Module state */
static struct {
  gboolean initialized;
  GMutex lock;
  LazyEntry entries[GN_LAZY_MAX];
  GQueue *preload_queue;
  guint preload_idle_id;
} state = { FALSE };

/* Default unload timeout: 5 minutes */
#define DEFAULT_UNLOAD_TIMEOUT_SEC 300

/* Forward declarations */
static gboolean preload_idle_cb(gpointer user_data);
static gboolean unload_timer_cb(gpointer user_data);
static void start_unload_timer(LazyEntry *entry);
static void stop_unload_timer(LazyEntry *entry);

void
gn_lazy_init(void)
{
  if (state.initialized) {
    return;
  }

  g_mutex_init(&state.lock);
  state.preload_queue = g_queue_new();
  state.preload_idle_id = 0;

  /* Initialize all entries */
  for (int i = 0; i < GN_LAZY_MAX; i++) {
    LazyEntry *entry = &state.entries[i];
    entry->config.id = (GnLazyComponent)i;
    entry->config.name = NULL;
    entry->config.factory = NULL;
    entry->config.unload_timeout_sec = DEFAULT_UNLOAD_TIMEOUT_SEC;
    entry->config.preload_on_idle = FALSE;
    entry->config.estimated_size = 0;
    entry->state = GN_LAZY_STATE_UNLOADED;
    entry->widget = NULL;
    entry->last_access = 0;
    entry->load_time = 0;
    entry->unload_timer_id = 0;
  }

  state.initialized = TRUE;
  g_debug("lazy-loader: Initialized");
}

void
gn_lazy_shutdown(void)
{
  if (!state.initialized) {
    return;
  }

  g_mutex_lock(&state.lock);

  /* Cancel preload */
  if (state.preload_idle_id > 0) {
    g_source_remove(state.preload_idle_id);
    state.preload_idle_id = 0;
  }

  /* Clear preload queue */
  g_queue_free(state.preload_queue);
  state.preload_queue = NULL;

  /* Unload all components */
  for (int i = 0; i < GN_LAZY_MAX; i++) {
    LazyEntry *entry = &state.entries[i];

    stop_unload_timer(entry);

    if (entry->widget) {
      /* Track memory reduction */
      if (entry->config.estimated_size > 0) {
        GN_MEM_FREE(GN_MEM_COMPONENT_UI, entry->config.estimated_size);
      }

      /* Widget may be owned by parent - don't unref directly */
      entry->widget = NULL;
    }

    entry->state = GN_LAZY_STATE_UNLOADED;
  }

  g_mutex_unlock(&state.lock);
  g_mutex_clear(&state.lock);

  state.initialized = FALSE;
  g_debug("lazy-loader: Shutdown complete");
}

void
gn_lazy_register(const GnLazyConfig *config)
{
  if (!state.initialized || !config) {
    return;
  }

  if (config->id >= GN_LAZY_MAX) {
    g_warning("lazy-loader: Invalid component ID %d", config->id);
    return;
  }

  g_mutex_lock(&state.lock);

  LazyEntry *entry = &state.entries[config->id];
  entry->config = *config;

  g_debug("lazy-loader: Registered component '%s' (id=%d, preload=%d)",
          config->name ? config->name : "unknown",
          config->id,
          config->preload_on_idle);

  /* Schedule preload if configured */
  if (config->preload_on_idle) {
    g_queue_push_tail(state.preload_queue, GINT_TO_POINTER(config->id));

    if (state.preload_idle_id == 0) {
      state.preload_idle_id = g_idle_add(preload_idle_cb, NULL);
    }
  }

  g_mutex_unlock(&state.lock);
}

GtkWidget *
gn_lazy_get(GnLazyComponent id)
{
  if (!state.initialized || id >= GN_LAZY_MAX) {
    return NULL;
  }

  g_mutex_lock(&state.lock);

  LazyEntry *entry = &state.entries[id];

  /* Already loaded? */
  if (entry->state == GN_LAZY_STATE_LOADED && entry->widget) {
    entry->last_access = g_get_monotonic_time();

    /* Reset unload timer */
    if (entry->config.unload_timeout_sec > 0) {
      stop_unload_timer(entry);
      start_unload_timer(entry);
    }

    GtkWidget *widget = entry->widget;
    g_mutex_unlock(&state.lock);
    return widget;
  }

  /* Can't load without factory */
  if (!entry->config.factory) {
    g_warning("lazy-loader: No factory for component %d", id);
    g_mutex_unlock(&state.lock);
    return NULL;
  }

  /* Mark as loading */
  entry->state = GN_LAZY_STATE_LOADING;

  /* Unlock during factory call (may be slow) */
  GnLazyFactory factory = entry->config.factory;
  g_mutex_unlock(&state.lock);

  gint64 start = g_get_monotonic_time();
  GtkWidget *widget = factory();
  gint64 elapsed = g_get_monotonic_time() - start;

  g_mutex_lock(&state.lock);

  if (widget) {
    entry->widget = widget;
    entry->state = GN_LAZY_STATE_LOADED;
    entry->last_access = g_get_monotonic_time();
    entry->load_time = elapsed;

    /* Track memory usage */
    if (entry->config.estimated_size > 0) {
      GN_MEM_ALLOC(GN_MEM_COMPONENT_UI, entry->config.estimated_size);
    }

    /* Start unload timer if configured */
    if (entry->config.unload_timeout_sec > 0) {
      start_unload_timer(entry);
    }

    g_debug("lazy-loader: Loaded '%s' in %lld ms",
            entry->config.name ? entry->config.name : "unknown",
            (long long)(elapsed / 1000));
  } else {
    entry->state = GN_LAZY_STATE_ERROR;
    g_warning("lazy-loader: Failed to load component '%s'",
              entry->config.name ? entry->config.name : "unknown");
  }

  g_mutex_unlock(&state.lock);
  return widget;
}

void
gn_lazy_get_async(GnLazyComponent id,
                  GnLazyCallback callback,
                  gpointer user_data)
{
  if (!callback) {
    return;
  }

  /* For simplicity, just call sync version from idle
   * A full implementation would use a thread pool */
  AsyncLoadRequest *req = g_new0(AsyncLoadRequest, 1);
  req->id = id;
  req->callback = callback;
  req->user_data = user_data;

  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, async_load_idle_cb, req, NULL);
}

void
gn_lazy_preload(GnLazyComponent id)
{
  if (!state.initialized || id >= GN_LAZY_MAX) {
    return;
  }

  g_mutex_lock(&state.lock);

  LazyEntry *entry = &state.entries[id];

  /* Already loaded or loading? */
  if (entry->state == GN_LAZY_STATE_LOADED ||
      entry->state == GN_LAZY_STATE_LOADING) {
    g_mutex_unlock(&state.lock);
    return;
  }

  /* Add to preload queue */
  g_queue_push_tail(state.preload_queue, GINT_TO_POINTER(id));

  if (state.preload_idle_id == 0) {
    state.preload_idle_id = g_idle_add(preload_idle_cb, NULL);
  }

  g_mutex_unlock(&state.lock);
}

void
gn_lazy_preload_all(void)
{
  if (!state.initialized) {
    return;
  }

  g_mutex_lock(&state.lock);

  for (int i = 0; i < GN_LAZY_MAX; i++) {
    LazyEntry *entry = &state.entries[i];

    if (entry->config.factory &&
        entry->state == GN_LAZY_STATE_UNLOADED) {
      g_queue_push_tail(state.preload_queue, GINT_TO_POINTER(i));
    }
  }

  if (state.preload_idle_id == 0 && !g_queue_is_empty(state.preload_queue)) {
    state.preload_idle_id = g_idle_add(preload_idle_cb, NULL);
  }

  g_mutex_unlock(&state.lock);
}

void
gn_lazy_unload(GnLazyComponent id)
{
  if (!state.initialized || id >= GN_LAZY_MAX) {
    return;
  }

  g_mutex_lock(&state.lock);

  LazyEntry *entry = &state.entries[id];

  stop_unload_timer(entry);

  if (entry->widget) {
    g_debug("lazy-loader: Unloading '%s'",
            entry->config.name ? entry->config.name : "unknown");

    /* Track memory reduction */
    if (entry->config.estimated_size > 0) {
      GN_MEM_FREE(GN_MEM_COMPONENT_UI, entry->config.estimated_size);
    }

    /* Note: Widget lifecycle is complex - it may be owned by a parent.
     * We clear our reference but don't destroy it here. */
    entry->widget = NULL;
  }

  entry->state = GN_LAZY_STATE_UNLOADED;

  g_mutex_unlock(&state.lock);
}

guint
gn_lazy_unload_unused(guint max_age_sec)
{
  if (!state.initialized) {
    return 0;
  }

  guint count = 0;
  gint64 now = g_get_monotonic_time();
  gint64 max_age_usec = (gint64)max_age_sec * G_USEC_PER_SEC;

  g_mutex_lock(&state.lock);

  for (int i = 0; i < GN_LAZY_MAX; i++) {
    LazyEntry *entry = &state.entries[i];

    if (entry->state != GN_LAZY_STATE_LOADED) {
      continue;
    }

    if (entry->config.unload_timeout_sec == 0) {
      continue;  /* Never unload */
    }

    gint64 age = now - entry->last_access;
    if (age > max_age_usec) {
      g_mutex_unlock(&state.lock);
      gn_lazy_unload((GnLazyComponent)i);
      g_mutex_lock(&state.lock);
      count++;
    }
  }

  g_mutex_unlock(&state.lock);

  if (count > 0) {
    g_debug("lazy-loader: Unloaded %u unused components", count);
  }

  return count;
}

GnLazyState
gn_lazy_get_state(GnLazyComponent id)
{
  if (!state.initialized || id >= GN_LAZY_MAX) {
    return GN_LAZY_STATE_ERROR;
  }

  g_mutex_lock(&state.lock);
  GnLazyState s = state.entries[id].state;
  g_mutex_unlock(&state.lock);

  return s;
}

void
gn_lazy_touch(GnLazyComponent id)
{
  if (!state.initialized || id >= GN_LAZY_MAX) {
    return;
  }

  g_mutex_lock(&state.lock);

  LazyEntry *entry = &state.entries[id];
  entry->last_access = g_get_monotonic_time();

  /* Reset unload timer */
  if (entry->config.unload_timeout_sec > 0 &&
      entry->state == GN_LAZY_STATE_LOADED) {
    stop_unload_timer(entry);
    start_unload_timer(entry);
  }

  g_mutex_unlock(&state.lock);
}

gsize
gn_lazy_get_memory_usage(void)
{
  if (!state.initialized) {
    return 0;
  }

  gsize total = 0;

  g_mutex_lock(&state.lock);

  for (int i = 0; i < GN_LAZY_MAX; i++) {
    LazyEntry *entry = &state.entries[i];
    if (entry->state == GN_LAZY_STATE_LOADED) {
      total += entry->config.estimated_size;
    }
  }

  g_mutex_unlock(&state.lock);

  return total;
}

const gchar *
gn_lazy_component_name(GnLazyComponent id)
{
  if (!state.initialized || id >= GN_LAZY_MAX) {
    return "unknown";
  }

  const gchar *name = state.entries[id].config.name;
  return name ? name : "unnamed";
}

/* Idle callback for background preloading */
static gboolean
preload_idle_cb(gpointer user_data)
{
  (void)user_data;

  if (!state.initialized) {
    return G_SOURCE_REMOVE;
  }

  g_mutex_lock(&state.lock);

  if (g_queue_is_empty(state.preload_queue)) {
    state.preload_idle_id = 0;
    g_mutex_unlock(&state.lock);
    return G_SOURCE_REMOVE;
  }

  /* Load one component per idle call to avoid blocking */
  GnLazyComponent id = GPOINTER_TO_INT(g_queue_pop_head(state.preload_queue));

  g_mutex_unlock(&state.lock);

  /* Actually load the component */
  g_debug("lazy-loader: Preloading component %d", id);
  gn_lazy_get(id);

  /* Continue if more to preload */
  g_mutex_lock(&state.lock);
  gboolean more = !g_queue_is_empty(state.preload_queue);
  if (!more) {
    state.preload_idle_id = 0;
  }
  g_mutex_unlock(&state.lock);

  return more ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* Timer callback for auto-unloading */
static gboolean
unload_timer_cb(gpointer user_data)
{
  GnLazyComponent id = GPOINTER_TO_INT(user_data);

  if (!state.initialized || id >= GN_LAZY_MAX) {
    return G_SOURCE_REMOVE;
  }

  g_mutex_lock(&state.lock);
  LazyEntry *entry = &state.entries[id];
  entry->unload_timer_id = 0;
  g_mutex_unlock(&state.lock);

  gn_lazy_unload(id);

  return G_SOURCE_REMOVE;
}

static void
start_unload_timer(LazyEntry *entry)
{
  if (!entry || entry->config.unload_timeout_sec == 0) {
    return;
  }

  stop_unload_timer(entry);

  entry->unload_timer_id = g_timeout_add_seconds(
    entry->config.unload_timeout_sec,
    unload_timer_cb,
    GINT_TO_POINTER(entry->config.id)
  );
}

static void
stop_unload_timer(LazyEntry *entry)
{
  if (!entry) {
    return;
  }

  if (entry->unload_timer_id > 0) {
    g_source_remove(entry->unload_timer_id);
    entry->unload_timer_id = 0;
  }
}
