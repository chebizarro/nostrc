#include "gh-clock.h"

#include <sodium.h>

struct _GhClock {
  gatomicrefcount ref_count;
  GhClockVTable vtable;
  gpointer data;
  GDestroyNotify destroy;
  gboolean fake;
};

GhClock *
gh_clock_new(const GhClockVTable *vtable, gpointer data, GDestroyNotify destroy)
{
  g_return_val_if_fail(vtable != NULL, NULL);
  g_return_val_if_fail(vtable->get_real_time && vtable->get_monotonic_time &&
                       vtable->timeout_add && vtable->source_remove &&
                       vtable->random_uniform, NULL);

  GhClock *clock = g_new0(GhClock, 1);
  g_atomic_ref_count_init(&clock->ref_count);
  clock->vtable = *vtable;
  clock->data = data;
  clock->destroy = destroy;
  return clock;
}

GhClock *
gh_clock_ref(GhClock *clock)
{
  g_return_val_if_fail(clock != NULL, NULL);
  g_atomic_ref_count_inc(&clock->ref_count);
  return clock;
}

void
gh_clock_unref(GhClock *clock)
{
  if (!clock || !g_atomic_ref_count_dec(&clock->ref_count))
    return;
  if (clock->destroy)
    clock->destroy(clock->data);
  g_free(clock);
}

gint64
gh_clock_get_real_time(GhClock *clock)
{
  g_return_val_if_fail(clock != NULL, 0);
  return clock->vtable.get_real_time(clock->data);
}

gint64
gh_clock_get_unix(GhClock *clock)
{
  return gh_clock_get_real_time(clock) / G_USEC_PER_SEC;
}

gint64
gh_clock_get_monotonic_time(GhClock *clock)
{
  g_return_val_if_fail(clock != NULL, 0);
  return clock->vtable.get_monotonic_time(clock->data);
}

guint
gh_clock_timeout_add(GhClock *clock, guint64 interval_ms, GSourceFunc func,
                     gpointer data, GDestroyNotify notify)
{
  g_return_val_if_fail(clock != NULL, 0);
  g_return_val_if_fail(func != NULL, 0);
  return clock->vtable.timeout_add(clock->data, interval_ms, func, data, notify);
}

gboolean
gh_clock_source_remove(GhClock *clock, guint id)
{
  g_return_val_if_fail(clock != NULL, FALSE);
  if (id == 0)
    return FALSE;
  return clock->vtable.source_remove(clock->data, id);
}

guint32
gh_clock_random_uniform(GhClock *clock, guint32 upper_bound)
{
  g_return_val_if_fail(clock != NULL, 0);
  g_return_val_if_fail(upper_bound > 0, 0);
  guint32 value = clock->vtable.random_uniform(clock->data, upper_bound);
  g_return_val_if_fail(value < upper_bound, 0);
  return value;
}

gint64
gh_clock_random_range(GhClock *clock, gint64 min, gint64 max)
{
  g_return_val_if_fail(clock != NULL, min);
  g_return_val_if_fail(max >= min, min);
  g_return_val_if_fail((guint64) (max - min) < G_MAXUINT32, min);
  return min + gh_clock_random_uniform(clock, (guint32) (max - min) + 1);
}

/* ---- System clock --------------------------------------------------------- */

static gint64
system_real_time(gpointer data)
{
  (void) data;
  return g_get_real_time();
}

static gint64
system_monotonic_time(gpointer data)
{
  (void) data;
  return g_get_monotonic_time();
}

static guint
system_timeout_add(gpointer data, guint64 interval_ms, GSourceFunc func,
                   gpointer func_data, GDestroyNotify notify)
{
  (void) data;
  /* Whole-second intervals use coalescing second sources (idle wakeups);
   * intervals beyond G_MAXUINT milliseconds (~49 days) are clamped. */
  GSource *source;
  if (interval_ms >= 1000 && interval_ms % 1000 == 0)
    source = g_timeout_source_new_seconds((guint) MIN(interval_ms / 1000, (guint64) G_MAXUINT));
  else
    source = g_timeout_source_new((guint) MIN(interval_ms, (guint64) G_MAXUINT));
  g_source_set_callback(source, func, func_data, notify);
  GMainContext *context = g_main_context_ref_thread_default();
  guint id = g_source_attach(source, context);
  g_main_context_unref(context);
  g_source_unref(source);
  return id;
}

static gboolean
system_source_remove(gpointer data, guint id)
{
  (void) data;
  GMainContext *context = g_main_context_ref_thread_default();
  GSource *source = g_main_context_find_source_by_id(context, id);
  if (source)
    g_source_destroy(source);
  g_main_context_unref(context);
  return source != NULL;
}

static guint32
system_random_uniform(gpointer data, guint32 upper_bound)
{
  (void) data;
  return randombytes_uniform(upper_bound);
}

GhClock *
gh_clock_new_system(void)
{
  static const GhClockVTable vtable = {
    system_real_time, system_monotonic_time, system_timeout_add,
    system_source_remove, system_random_uniform,
  };
  if (sodium_init() < 0)
    g_error("libsodium failed to initialize; no CSPRNG for jitter");
  return gh_clock_new(&vtable, NULL, NULL);
}

/* ---- Fake clock ------------------------------------------------------------ */

typedef struct {
  guint id;
  gint64 deadline;      /* monotonic microseconds */
  guint64 interval_ms;
  GSourceFunc func;
  gpointer data;
  GDestroyNotify notify;
  gboolean removed;
  gboolean dispatching;
} FakeTimeout;

typedef struct {
  gint64 monotonic;     /* microseconds */
  gint64 real_offset;   /* real time = monotonic + real_offset */
  guint next_id;
  GPtrArray *timeouts;  /* FakeTimeout*, unordered */
  GQueue randoms;       /* scripted guint32 values (GUINT_TO_POINTER) */
  GRand *rand;
} FakeClock;

static void
fake_timeout_free(FakeTimeout *timeout)
{
  if (timeout->notify)
    timeout->notify(timeout->data);
  g_free(timeout);
}

static void
fake_clock_free(gpointer data)
{
  FakeClock *fake = data;
  /* Detach first: a destroy notify may not re-enter a clock being freed. */
  GPtrArray *timeouts = g_steal_pointer(&fake->timeouts);
  for (guint i = 0; i < timeouts->len; i++)
    fake_timeout_free(g_ptr_array_index(timeouts, i));
  g_ptr_array_unref(timeouts);
  g_queue_clear(&fake->randoms);
  g_rand_free(fake->rand);
  g_free(fake);
}

static gint64
fake_real_time(gpointer data)
{
  FakeClock *fake = data;
  return fake->monotonic + fake->real_offset;
}

static gint64
fake_monotonic_time(gpointer data)
{
  FakeClock *fake = data;
  return fake->monotonic;
}

static gint64
fake_interval_us(guint64 interval_ms)
{
  /* A zero interval still makes progress, so a repeating callback cannot
   * spin forever inside a single advance. */
  if (interval_ms == 0)
    return 1;
  return (gint64) MIN(interval_ms, (guint64) (G_MAXINT64 / 2000)) * 1000;
}

static guint
fake_timeout_add(gpointer data, guint64 interval_ms, GSourceFunc func,
                 gpointer func_data, GDestroyNotify notify)
{
  FakeClock *fake = data;
  if (!fake->timeouts) /* a destroy notify running while the clock is freed */
    return 0;
  FakeTimeout *timeout = g_new0(FakeTimeout, 1);
  timeout->id = ++fake->next_id;
  timeout->interval_ms = interval_ms;
  timeout->deadline = fake->monotonic + fake_interval_us(interval_ms);
  timeout->func = func;
  timeout->data = func_data;
  timeout->notify = notify;
  g_ptr_array_add(fake->timeouts, timeout);
  return timeout->id;
}

static gboolean
fake_source_remove(gpointer data, guint id)
{
  FakeClock *fake = data;
  if (!fake->timeouts)
    return FALSE;
  for (guint i = 0; i < fake->timeouts->len; i++) {
    FakeTimeout *timeout = g_ptr_array_index(fake->timeouts, i);
    if (timeout->id != id || timeout->removed)
      continue;
    timeout->removed = TRUE;
    if (!timeout->dispatching) {
      g_ptr_array_remove_index(fake->timeouts, i);
      fake_timeout_free(timeout);
    }
    return TRUE;
  }
  return FALSE;
}

static guint32
fake_random_uniform(gpointer data, guint32 upper_bound)
{
  FakeClock *fake = data;
  if (!g_queue_is_empty(&fake->randoms))
    return GPOINTER_TO_UINT(g_queue_pop_head(&fake->randoms)) % upper_bound;
  return g_rand_int(fake->rand) % upper_bound;
}

GhClock *
gh_clock_new_fake(gint64 start_real_time_us)
{
  static const GhClockVTable vtable = {
    fake_real_time, fake_monotonic_time, fake_timeout_add,
    fake_source_remove, fake_random_uniform,
  };
  FakeClock *fake = g_new0(FakeClock, 1);
  /* Monotonic time starts at an arbitrary positive origin, like a real one. */
  fake->monotonic = 1000 * G_USEC_PER_SEC;
  fake->real_offset = start_real_time_us - fake->monotonic;
  fake->timeouts = g_ptr_array_new();
  g_queue_init(&fake->randoms);
  fake->rand = g_rand_new_with_seed(0x9e3779b9u);
  GhClock *clock = gh_clock_new(&vtable, fake, fake_clock_free);
  clock->fake = TRUE;
  return clock;
}

gboolean
gh_clock_is_fake(GhClock *clock)
{
  g_return_val_if_fail(clock != NULL, FALSE);
  return clock->fake;
}

static FakeClock *
fake_from_clock(GhClock *clock)
{
  g_return_val_if_fail(clock != NULL, NULL);
  g_return_val_if_fail(clock->fake, NULL);
  return clock->data;
}

/* Earliest live timeout due at or before limit; ties go to the older id. */
static FakeTimeout *
fake_next_due(FakeClock *fake, gint64 limit)
{
  FakeTimeout *best = NULL;
  for (guint i = 0; i < fake->timeouts->len; i++) {
    FakeTimeout *timeout = g_ptr_array_index(fake->timeouts, i);
    if (timeout->removed || timeout->deadline > limit)
      continue;
    if (!best || timeout->deadline < best->deadline ||
        (timeout->deadline == best->deadline && timeout->id < best->id))
      best = timeout;
  }
  return best;
}

void
gh_clock_fake_advance(GhClock *clock, gint64 delta_us)
{
  FakeClock *fake = fake_from_clock(clock);
  g_return_if_fail(fake != NULL);
  g_return_if_fail(delta_us >= 0);

  gh_clock_ref(clock);
  const gint64 target = fake->monotonic + delta_us;
  FakeTimeout *timeout;
  while ((timeout = fake_next_due(fake, target))) {
    fake->monotonic = MAX(fake->monotonic, timeout->deadline);
    timeout->dispatching = TRUE;
    gboolean again = timeout->func(timeout->data);
    timeout->dispatching = FALSE;
    if (again && !timeout->removed) {
      timeout->deadline += fake_interval_us(timeout->interval_ms);
      continue;
    }
    g_ptr_array_remove_fast(fake->timeouts, timeout);
    fake_timeout_free(timeout);
  }
  fake->monotonic = target;
  gh_clock_unref(clock);
}

void
gh_clock_fake_set_real_time(GhClock *clock, gint64 real_time_us)
{
  FakeClock *fake = fake_from_clock(clock);
  g_return_if_fail(fake != NULL);
  fake->real_offset = real_time_us - fake->monotonic;
}

void
gh_clock_fake_push_random(GhClock *clock, guint32 value)
{
  FakeClock *fake = fake_from_clock(clock);
  g_return_if_fail(fake != NULL);
  g_queue_push_tail(&fake->randoms, GUINT_TO_POINTER(value));
}

guint
gh_clock_fake_get_n_timeouts(GhClock *clock)
{
  FakeClock *fake = fake_from_clock(clock);
  g_return_val_if_fail(fake != NULL, 0);
  guint n = 0;
  for (guint i = 0; i < fake->timeouts->len; i++)
    n += !((FakeTimeout *) g_ptr_array_index(fake->timeouts, i))->removed;
  return n;
}

gint64
gh_clock_fake_get_next_deadline(GhClock *clock)
{
  FakeClock *fake = fake_from_clock(clock);
  g_return_val_if_fail(fake != NULL, -1);
  FakeTimeout *timeout = fake_next_due(fake, G_MAXINT64);
  return timeout ? timeout->deadline : -1;
}

guint64
gh_clock_fake_get_shortest_interval_ms(GhClock *clock)
{
  FakeClock *fake = fake_from_clock(clock);
  g_return_val_if_fail(fake != NULL, G_MAXUINT64);
  guint64 shortest = G_MAXUINT64;
  for (guint i = 0; i < fake->timeouts->len; i++) {
    FakeTimeout *timeout = g_ptr_array_index(fake->timeouts, i);
    if (!timeout->removed)
      shortest = MIN(shortest, timeout->interval_ms);
  }
  return shortest;
}
