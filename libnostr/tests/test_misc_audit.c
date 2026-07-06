/* Observability + misc robustness regression tests (nostrc-lc7, nostrc-dui).
 *
 * This test compiles the source files under test directly (see CMakeLists.txt)
 * with NOSTR_ENABLE_METRICS=1 so the real metrics/collector code paths are
 * exercised (the library build defaults to NOSTR_ENABLE_METRICS=OFF, which
 * turns those functions into no-ops).
 *
 * Coverage:
 *   1. read_ll_env (security_limits_runtime.c): malformed/overflow env values
 *      are rejected to the fallback; valid values parse; the caches are read
 *      race-free (pthread_once).
 *   2. metrics_collector start/stop cycle: repeated start/stop must not
 *      use-after-free export_path (the worker is now joined before free).
 *   3. metrics.c cross-thread counter aggregation: an export from one thread
 *      must include counters still pending in OTHER live threads' TLS caches.
 *   4. storage_registry.c: concurrent register/create must be race-free and
 *      not crash.
 */
#include "security_limits_runtime.h"
#include "security_limits.h"
#include "nostr/metrics.h"
#include "nostr/metrics_collector.h"
#include "nostr-storage.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); g_failures++; } \
  } while (0)

#define CHECK_EQ(got, want, msg) do { \
    long long _g = (long long)(got), _w = (long long)(want); \
    if (_g != _w) { fprintf(stderr, "FAIL: %s (got %lld, want %lld)\n", (msg), _g, _w); g_failures++; } \
  } while (0)

/* ---------------------------------------------------------------------------
 * 1. read_ll_env via public limit getters.
 *
 * All limits are resolved once (pthread_once) on the first getter call, so we
 * must set every env var we care about BEFORE calling any getter.
 * ------------------------------------------------------------------------- */
static void test_limits_env_parsing(void) {
  /* Malformed / out-of-range -> fallback (compile-time default). */
  setenv("NOSTR_MAX_FRAME_LEN_BYTES", "123abc", 1);                 /* trailing junk */
  setenv("NOSTR_MAX_FRAMES_PER_SEC", "999999999999999999999", 1);  /* overflow (ERANGE) */
  setenv("NOSTR_MAX_TAGS_PER_EVENT", "0", 1);                      /* non-positive */
  setenv("NOSTR_MAX_TAG_DEPTH", "-5", 1);                         /* negative */
  setenv("NOSTR_MAX_IDS_PER_FILTER", "  12x", 1);                 /* junk after ws+digits */

  /* Valid values -> parsed (incl. surrounding whitespace). */
  setenv("NOSTR_MAX_BYTES_PER_SEC", "4096", 1);
  setenv("NOSTR_MAX_EVENT_SIZE_BYTES", "  789  ", 1);
  setenv("NOSTR_MAX_FILTERS_PER_REQ", "7", 1);

  /* First getter call triggers init of ALL limits from the env above. */
  CHECK_EQ(nostr_limit_max_frame_len(), NOSTR_MAX_FRAME_LEN_BYTES,
           "trailing junk '123abc' must fall back to default");
  CHECK_EQ(nostr_limit_max_frames_per_sec(), NOSTR_MAX_FRAMES_PER_SEC,
           "overflow value must fall back to default");
  CHECK_EQ(nostr_limit_max_tags_per_event(), NOSTR_MAX_TAGS_PER_EVENT,
           "zero must fall back to default");
  CHECK_EQ(nostr_limit_max_tag_depth(), NOSTR_MAX_TAG_DEPTH,
           "negative must fall back to default");
  CHECK_EQ(nostr_limit_max_ids_per_filter(), NOSTR_MAX_IDS_PER_FILTER,
           "junk after digits must fall back to default");

  CHECK_EQ(nostr_limit_max_bytes_per_sec(), 4096,
           "valid '4096' must parse");
  CHECK_EQ(nostr_limit_max_event_size(), 789,
           "valid '  789  ' with surrounding whitespace must parse");
  CHECK_EQ(nostr_limit_max_filters_per_req(), 7,
           "valid '7' must parse");
}

/* Concurrent first-call: many threads reading a getter must all agree and not
 * crash. With pthread_once this is race-free; the value is stable. */
static void *limits_reader(void *arg) {
  int64_t *out = (int64_t *)arg;
  *out = nostr_limit_max_bytes_per_sec();
  return NULL;
}

static void test_limits_concurrent_read(void) {
  enum { N = 16 };
  pthread_t th[N];
  int64_t vals[N];
  for (int i = 0; i < N; i++) pthread_create(&th[i], NULL, limits_reader, &vals[i]);
  for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
  for (int i = 0; i < N; i++)
    CHECK_EQ(vals[i], 4096, "concurrent limit reads must all see the same value");
}

/* ---------------------------------------------------------------------------
 * 2. metrics_collector start/stop cycle (UAF regression).
 * ------------------------------------------------------------------------- */
static void test_collector_start_stop_cycle(void) {
  const char *path = "/tmp/nostr_misc_audit_metrics.prom";
  /* Give the exporter something to write. */
  nostr_metric_counter_add("misc_audit_cycle", 42);

  for (int i = 0; i < 200; i++) {
    nostr_metrics_collector_start(1 /*ms*/, path);
    CHECK(nostr_metrics_collector_running(), "collector should report running after start");
    /* On odd iterations let the worker run a beat so stop() may land while the
     * worker is mid-collect/mid-export (the window the old detach+free raced). */
    if (i & 1) usleep(300);
    nostr_metric_counter_add("misc_audit_cycle", 1);
    nostr_metrics_collector_stop();
    CHECK(!nostr_metrics_collector_running(), "collector should be stopped after stop");
  }
  unlink(path);
}

/* ---------------------------------------------------------------------------
 * 3. Cross-thread counter aggregation.
 *
 * Each worker adds to a distinct counter and then BLOCKS (stays alive) without
 * ever flushing its own TLS cache. The main thread then exports via Prometheus
 * and must see every worker's counter. Before the fix, the export only flushed
 * the calling thread's cache, so these counters would be missing/zero.
 * ------------------------------------------------------------------------- */
#define AGG_THREADS 8
#define AGG_PER_THREAD 1000

typedef struct {
  int id;
  char name[32];
} agg_arg;

static pthread_mutex_t g_agg_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_agg_cv = PTHREAD_COND_INITIALIZER;
static int g_agg_ready = 0;
static int g_agg_release = 0;

static void *agg_worker(void *arg) {
  agg_arg *a = (agg_arg *)arg;
  /* Single add: stays pending in this thread's TLS cache (no time-based flush,
   * since there is only one add). */
  nostr_metric_counter_add(a->name, AGG_PER_THREAD);

  pthread_mutex_lock(&g_agg_mu);
  g_agg_ready++;
  pthread_cond_broadcast(&g_agg_cv);
  while (!g_agg_release) pthread_cond_wait(&g_agg_cv, &g_agg_mu);
  pthread_mutex_unlock(&g_agg_mu);
  return NULL;
}

static void test_cross_thread_aggregation(void) {
  pthread_t th[AGG_THREADS];
  agg_arg args[AGG_THREADS];

  for (int i = 0; i < AGG_THREADS; i++) {
    args[i].id = i;
    snprintf(args[i].name, sizeof(args[i].name), "agg_ctr_%d", i);
    pthread_create(&th[i], NULL, agg_worker, &args[i]);
  }

  /* Wait until every worker has added and is now blocked (still alive). */
  pthread_mutex_lock(&g_agg_mu);
  while (g_agg_ready < AGG_THREADS) pthread_cond_wait(&g_agg_cv, &g_agg_mu);
  pthread_mutex_unlock(&g_agg_mu);

  /* Export from the main thread; must aggregate all live threads' caches. */
  size_t needed = nostr_metrics_prometheus(NULL, 0);
  CHECK(needed > 0, "prometheus export should be non-empty");
  char *buf = (char *)malloc(needed + 1);
  CHECK(buf != NULL, "alloc prometheus buffer");
  if (buf) {
    nostr_metrics_prometheus(buf, needed + 1);
    for (int i = 0; i < AGG_THREADS; i++) {
      char needle[64];
      snprintf(needle, sizeof(needle), "nostr_agg_ctr_%d %d", i, AGG_PER_THREAD);
      char msg[96];
      snprintf(msg, sizeof(msg), "export must include live thread counter '%s'", needle);
      CHECK(strstr(buf, needle) != NULL, msg);
    }
    free(buf);
  }

  /* Release the workers and join. */
  pthread_mutex_lock(&g_agg_mu);
  g_agg_release = 1;
  pthread_cond_broadcast(&g_agg_cv);
  pthread_mutex_unlock(&g_agg_mu);
  for (int i = 0; i < AGG_THREADS; i++) pthread_join(th[i], NULL);
}

/* ---------------------------------------------------------------------------
 * 4. Concurrent storage-registry register/create smoke test.
 * ------------------------------------------------------------------------- */
static NostrStorage *dummy_factory(void) {
  return (NostrStorage *)calloc(1, sizeof(NostrStorage));
}

#define REG_THREADS 8
#define REG_ITERS 2000

static void *reg_worker(void *arg) {
  int id = *(int *)arg;
  for (int i = 0; i < REG_ITERS; i++) {
    char name[32];
    /* Mix of shared names (exercise the replace path + contention) and
     * per-thread names. */
    snprintf(name, sizeof(name), "drv_%d", (i % 4 == 0) ? (i % 8) : id);
    nostr_storage_register(name, dummy_factory);
    NostrStorage *s = nostr_storage_create(name);
    if (s) free(s);
  }
  return NULL;
}

static void test_storage_registry_concurrent(void) {
  pthread_t th[REG_THREADS];
  int ids[REG_THREADS];
  for (int i = 0; i < REG_THREADS; i++) {
    ids[i] = i;
    pthread_create(&th[i], NULL, reg_worker, &ids[i]);
  }
  for (int i = 0; i < REG_THREADS; i++) pthread_join(th[i], NULL);

  /* After the storm, a registered driver must resolve. */
  nostr_storage_register("drv_final", dummy_factory);
  NostrStorage *s = nostr_storage_create("drv_final");
  CHECK(s != NULL, "registered driver must be creatable");
  if (s) free(s);
  CHECK(nostr_storage_create("drv_does_not_exist") == NULL,
        "unknown driver must return NULL");
}

int main(void) {
  test_limits_env_parsing();
  test_limits_concurrent_read();
  test_collector_start_stop_cycle();
  test_cross_thread_aggregation();
  test_storage_registry_concurrent();

  if (g_failures == 0) {
    printf("test_misc_audit: ALL PASS\n");
    return 0;
  }
  fprintf(stderr, "test_misc_audit: %d FAILURE(S)\n", g_failures);
  return 1;
}
