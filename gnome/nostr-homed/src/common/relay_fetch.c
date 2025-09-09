#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include "nostr_manifest.h"
#include "nostr-simple-pool.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include <jansson.h>

typedef struct FetchCtx {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int done;
  char *json;
} FetchCtx;

/* legacy helper removed; using global callback below */

/* To pass context into the middleware, we set a static pointer for this short-lived call. */
static FetchCtx *g_ctx = NULL;
static void fetch_event_cb_global(NostrIncomingEvent *in){
  if (!g_ctx) return;
  if (!in || !in->event) return;
  if (nostr_event_get_kind(in->event) != 30081) return;
  const char *content = nostr_event_get_content(in->event);
  if (!content) return;
  pthread_mutex_lock(&g_ctx->mu);
  if (!g_ctx->done){
    g_ctx->json = strdup(content);
    g_ctx->done = 1;
    pthread_cond_broadcast(&g_ctx->cv);
  }
  pthread_mutex_unlock(&g_ctx->mu);
}

int nh_fetch_latest_manifest_json(const char **relays, size_t num_relays,
                                  const char *namespace_name,
                                  char **out_json) {
  (void)namespace_name; /* Reserved for future namespace scoping (tags) */
  if (!out_json || !relays || num_relays == 0) return -1;

  *out_json = NULL;
  NostrSimplePool *pool = nostr_simple_pool_new();
  if (!pool) return -1;
  for (size_t i=0;i<num_relays;i++){
    if (relays[i] && *relays[i]) nostr_simple_pool_ensure_relay(pool, relays[i]);
  }
  /* De-dup and auto-unsub on EOSE for efficiency */
  nostr_simple_pool_set_auto_unsub_on_eose(pool, true);
  nostr_simple_pool_set_event_middleware(pool, fetch_event_cb_global);

  /* Build a filter for the latest replaceable manifest (kind 30081), limit=1 */
  NostrFilter *f = nostr_filter_new();
  nostr_filter_add_kind(f, 30081);
  nostr_filter_set_limit(f, 1);

  /* Query once across provided relays */
  nostr_simple_pool_query_single(pool, relays, num_relays, *f);
  nostr_filter_free(f);

  /* Start pool and wait for first manifest event or timeout */
  FetchCtx ctx; memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.mu, NULL);
  pthread_cond_init(&ctx.cv, NULL);
  g_ctx = &ctx;
  nostr_simple_pool_start(pool);

  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 3; /* 3s timeout */
  pthread_mutex_lock(&ctx.mu);
  while (!ctx.done){
    int prc = pthread_cond_timedwait(&ctx.cv, &ctx.mu, &ts);
    if (prc == ETIMEDOUT) break;
  }
  pthread_mutex_unlock(&ctx.mu);

  nostr_simple_pool_stop(pool);
  nostr_simple_pool_free(pool);
  g_ctx = NULL;

  if (ctx.json){
    *out_json = ctx.json;
    pthread_mutex_destroy(&ctx.mu);
    pthread_cond_destroy(&ctx.cv);
    return 0;
  }

  pthread_mutex_destroy(&ctx.mu);
  pthread_cond_destroy(&ctx.cv);
  return -1;
}

/* Profile relays fetch (kind 30078) */
typedef struct RelaysCtx {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int done;
  char *content_json;
} RelaysCtx;

static RelaysCtx *g_relays_ctx = NULL;
static void fetch_relays_cb(NostrIncomingEvent *in){
  if (!g_relays_ctx) return;
  if (!in || !in->event) return;
  if (nostr_event_get_kind(in->event) != 30078) return;
  const char *content = nostr_event_get_content(in->event);
  if (!content) return;
  pthread_mutex_lock(&g_relays_ctx->mu);
  if (!g_relays_ctx->done){
    g_relays_ctx->content_json = strdup(content);
    g_relays_ctx->done = 1;
    pthread_cond_broadcast(&g_relays_ctx->cv);
  }
  pthread_mutex_unlock(&g_relays_ctx->mu);
}

int nh_fetch_profile_relays(const char **relays, size_t num_relays,
                            char ***out_relays, size_t *out_count){
  if (!out_relays || !out_count || !relays || num_relays==0) return -1;
  *out_relays = NULL; *out_count = 0;
  NostrSimplePool *pool = nostr_simple_pool_new();
  if (!pool) return -1;
  for (size_t i=0;i<num_relays;i++) if (relays[i] && *relays[i]) nostr_simple_pool_ensure_relay(pool, relays[i]);
  nostr_simple_pool_set_auto_unsub_on_eose(pool, true);
  nostr_simple_pool_set_event_middleware(pool, fetch_relays_cb);

  NostrFilter *f = nostr_filter_new();
  nostr_filter_add_kind(f, 30078);
  nostr_filter_set_limit(f, 1);
  nostr_simple_pool_query_single(pool, relays, num_relays, *f);
  nostr_filter_free(f);

  RelaysCtx ctx; memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.mu, NULL);
  pthread_cond_init(&ctx.cv, NULL);
  g_relays_ctx = &ctx;
  nostr_simple_pool_start(pool);

  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 3;
  pthread_mutex_lock(&ctx.mu);
  while (!ctx.done){ if (pthread_cond_timedwait(&ctx.cv, &ctx.mu, &ts) == ETIMEDOUT) break; }
  pthread_mutex_unlock(&ctx.mu);

  nostr_simple_pool_stop(pool);
  nostr_simple_pool_free(pool);
  g_relays_ctx = NULL;

  int ret = -1;
  if (ctx.content_json){
    /* Parse content: expect {"relays":["wss://...", ...]} */
    json_error_t jerr; json_t *root = json_loads(ctx.content_json, 0, &jerr);
    if (root){
      json_t *arr = json_object_get(root, "relays");
      if (arr && json_is_array(arr)){
        size_t n = json_array_size(arr);
        if (n > 0){
          char **vec = (char**)calloc(n, sizeof(char*));
          if (vec){
            size_t outn = 0;
            for (size_t i=0;i<n;i++){
              json_t *it = json_array_get(arr, i);
              const char *s = json_is_string(it) ? json_string_value(it) : NULL;
              if (s){ vec[outn++] = strdup(s); }
            }
            if (outn > 0){ *out_relays = vec; *out_count = outn; ret = 0; }
            else { for (size_t i=0;i<n;i++) free(vec[i]); free(vec); }
          }
        }
      }
      json_decref(root);
    }
    free(ctx.content_json);
  }
  pthread_mutex_destroy(&ctx.mu);
  pthread_cond_destroy(&ctx.cv);
  return ret;
}
