#include <glib.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

/* Storage (app-local header path) */
#include "storage_ndb.h"
/* GObject SimplePool wrapper */
#include "nostr_simple_pool.h"
/* Canonical nostr headers used in the app */
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-json.h"

/* Local helper: hex_to_bytes32 (copied minimal impl) */
static int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static gboolean hex_to_bytes32(const char *hex, uint8_t out[32]) {
  if (!hex || !out) return FALSE;
  size_t n = strlen(hex);
  if (n != 64) return FALSE;
  for (size_t i = 0; i < 32; i++) {
    int hi = nibble(hex[2*i]);
    int lo = nibble(hex[2*i + 1]);
    if (hi < 0 || lo < 0) return FALSE;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return TRUE;
}

static void on_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data) {
  (void)pool; (void)user_data;
  if (!batch) return;
  for (guint i = 0; i < batch->len; i++) {
    NostrEvent *evt = (NostrEvent*)g_ptr_array_index(batch, i);
    int kind = nostr_event_get_kind(evt);
    const char *id = nostr_event_get_id(evt);
    char *json = nostr_event_serialize(evt);
    int irc = storage_ndb_ingest_event_json(json, NULL);
    g_message("ndb_ingest(profile_sub): kind=%d id=%s rc=%d", kind, id ? id : "<null>", irc);
    if (kind == 0) {
      const char *pk = nostr_event_get_pubkey(evt);
      if (pk && strlen(pk) == 64) {
        uint8_t pk32[32];
        if (hex_to_bytes32(pk, pk32)) {
          /* verify event presence for author/kind=0 */
          {
            void *txn = NULL;
            if (storage_ndb_begin_query(&txn) == 0) {
              char filt[256];
              snprintf(filt, sizeof(filt), "{\"kinds\":[0],\"authors\":[\"%s\"],\"limit\":1}", pk);
              char **arr = NULL; int n = 0;
              int qrc = storage_ndb_query(txn, filt, &arr, &n);
              g_message("ndb_events_by_author(profile_sub): pk=%s qrc=%d count=%d present=%s", pk, qrc, n, (qrc==0 && n>0)?"yes":"no");
              if (qrc == 0 && arr) storage_ndb_free_results(arr, n);
              storage_ndb_end_query(txn);
            }
          }
          /* retry readback more times to account for async indexing */
          for (int attempt = 0; attempt < 10; attempt++) {
            void *txn = NULL;
            if (storage_ndb_begin_query(&txn) == 0) {
              char *pjson = NULL; int plen = 0;
              int prc = storage_ndb_get_profile_by_pubkey(txn, pk32, &pjson, &plen);
              g_message("ndb_profile_readback(profile_sub): pk=%s rc=%d len=%d present=%s attempt=%d", pk, prc, plen, pjson?"yes":"no", attempt+1);
              storage_ndb_end_query(txn);
              if (pjson) { free(pjson); }
              if (pjson || prc == 0) break;
            }
            g_usleep(1000 * 250); /* 250ms */
          }
        }
      }
    }
    free(json);
  }
}

static void build_defaults(const char ***out_urls, size_t *out_count) {
  const char *env_relays = g_getenv("GNOSTR_RELAYS");
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  if (env_relays && *env_relays) {
    gchar **tok = g_strsplit(env_relays, ",", -1);
    for (gchar **p = tok; p && *p; p++) {
      const char *s = g_strstrip(*p);
      if (s && *s) g_ptr_array_add(arr, g_strdup(s));
    }
    g_strfreev(tok);
  }
  if (arr->len == 0) {
    /* Default relays for CLI tool (standalone, no GSettings dependency) */
    g_ptr_array_add(arr, g_strdup("wss://relay.damus.io"));
    g_ptr_array_add(arr, g_strdup("wss://nos.lol"));
    g_ptr_array_add(arr, g_strdup("wss://relay.nostr.band"));
  }
  const char **urls = g_new0(const char*, arr->len);
  for (guint i = 0; i < arr->len; i++) urls[i] = (const char*)g_ptr_array_index(arr, i);
  *out_urls = urls;
  *out_count = arr->len;
  /* Do not free strings here; ownership transferred to urls[] */
  g_ptr_array_free(arr, FALSE);
}

static NostrFilters* build_filters(void) {
  NostrFilters *fs = nostr_filters_new();
  gboolean only0 = FALSE;
  const char *e = g_getenv("GNOSTR_ONLY_KIND0");
  if (e && *e && g_strcmp0(e, "0") != 0) only0 = TRUE;
  if (!only0) {
    /* notes */
    NostrFilter *f1 = nostr_filter_new();
    int kinds1[] = { 1 };
    nostr_filter_set_kinds(f1, kinds1, 1);
    nostr_filters_add(fs, f1);
  }
  /* profiles */
  NostrFilter *f0 = nostr_filter_new();
  int kinds0[] = { 0 };
  nostr_filter_set_kinds(f0, kinds0, 1);
  nostr_filters_add(fs, f0);
  return fs;
}

static gboolean quit_loop_cb(gpointer data) {
  GMainLoop *loop = (GMainLoop*)data;
  fprintf(stdout, "gnostr-live-log: quitting main loop\n");
  fflush(stdout);
  if (loop) g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  g_set_prgname("gnostr-live-log");
  fprintf(stdout, "gnostr-live-log: start\n"); fflush(stdout);

  /* Init storage */
  gchar *dbdir = g_build_filename(g_get_user_cache_dir(), "gnostr", "ndb", NULL);
  g_mkdir_with_parents(dbdir, 0700);
  const char *opts = "{\"mapsize\":1073741824,\"ingester_threads\":1}";
  if (!storage_ndb_init(dbdir, opts)) {
    g_warning("Failed to initialize storage at %s", dbdir);
  }
  g_free(dbdir);

  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  GnostrSimplePool *pool = gnostr_simple_pool_new();
  g_signal_connect(pool, "events", G_CALLBACK(on_events), NULL);

  const char **urls = NULL; size_t url_count = 0; build_defaults(&urls, &url_count);
  fprintf(stdout, "gnostr-live-log: urls(%zu):\n", url_count);
  for (size_t i = 0; i < url_count; i++) { fprintf(stdout, "  %s\n", urls[i]); }
  fflush(stdout);
  NostrFilters *filters = build_filters();

  GCancellable *canc = g_cancellable_new();
  gnostr_simple_pool_subscribe_many_async(pool, urls, url_count, filters, canc, NULL, NULL);
  fprintf(stdout, "gnostr-live-log: subscription started\n"); fflush(stdout);

  for (size_t i = 0; i < url_count; i++) g_free((char*)urls[i]);
  g_free((void*)urls);

  guint secs = 60;
  const char *env_secs = g_getenv("GNOSTR_RUN_SECS");
  if (env_secs && *env_secs) secs = (guint)g_ascii_strtoull(env_secs, NULL, 10);

  g_timeout_add_seconds(secs, quit_loop_cb, loop);
  fprintf(stdout, "gnostr-live-log: running for %u seconds...\n", secs); fflush(stdout);
  g_main_loop_run(loop);

  g_cancellable_cancel(canc);
  g_clear_object(&canc);
  g_object_unref(pool);
  g_main_loop_unref(loop);
  fprintf(stdout, "gnostr-live-log: exit\n"); fflush(stdout);
  return 0;
}
