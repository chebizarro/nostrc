#include <glib.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

/* Storage (app-local header path) */
#include <nostr-gobject-1.0/storage_ndb.h>
/* GObject pool wrapper */
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_subscription.h>
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

static void on_event(GNostrSubscription *sub, const gchar *event_json, gpointer user_data) {
  (void)sub; (void)user_data;
  if (!event_json) return;

  NostrEvent *evt = nostr_event_new();
  if (!evt || nostr_event_deserialize(evt, event_json) != 0) {
    if (evt) nostr_event_free(evt);
    return;
  }

  int kind = nostr_event_get_kind(evt);
  const char *id = nostr_event_get_id(evt);
  int irc = storage_ndb_ingest_event_json(event_json, NULL);
  g_message("ndb_ingest(profile_sub): kind=%d id=%s rc=%d", kind, id ? id : "<null>", irc);
  if (kind == 0) {
    const char *pk = nostr_event_get_pubkey(evt);
    if (pk && strlen(pk) == 64) {
      uint8_t pk32[32];
      if (hex_to_bytes32(pk, pk32)) {
        /* verify event presence for author/kind=0 */
        {
          void *txn = NULL;
          if (storage_ndb_begin_query(&txn, NULL) == 0) {
            char filt[256];
            snprintf(filt, sizeof(filt), "{\"kinds\":[0],\"authors\":[\"%s\"],\"limit\":1}", pk);
            char **arr = NULL; int n = 0;
            int qrc = storage_ndb_query(txn, filt, &arr, &n, NULL);
            g_message("ndb_events_by_author(profile_sub): pk=%s qrc=%d count=%d present=%s", pk, qrc, n, (qrc==0 && n>0)?"yes":"no");
            if (qrc == 0 && arr) storage_ndb_free_results(arr, n);
            storage_ndb_end_query(txn);
          }
        }
        /* Timeout-audit: Single attempt, fail-fast. Profile indexing is async
         * so it may not be available yet — that's fine for diagnostics. */
        {
          void *txn = NULL;
          if (storage_ndb_begin_query(&txn, NULL) == 0) {
            char *pjson = NULL; int plen = 0;
            int prc = storage_ndb_get_profile_by_pubkey(txn, pk32, &pjson, &plen, NULL);
            g_message("ndb_profile_readback(profile_sub): pk=%s rc=%d len=%d present=%s", pk, prc, plen, pjson?"yes":"no");
            storage_ndb_end_query(txn);
            if (pjson) { free(pjson); }
          }
        }
      }
    }
  }
  nostr_event_free(evt);
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
  if (!storage_ndb_init(dbdir, opts, NULL)) {
    g_warning("Failed to initialize storage at %s", dbdir);
  }
  g_free(dbdir);

  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_autoptr(GNostrPool) pool = gnostr_pool_new();

  const char **urls = NULL; size_t url_count = 0; build_defaults(&urls, &url_count);
  fprintf(stdout, "gnostr-live-log: urls(%zu):\n", url_count);
  for (size_t i = 0; i < url_count; i++) { fprintf(stdout, "  %s\n", urls[i]); }
  fflush(stdout);

  gnostr_pool_sync_relays(pool, (const gchar **)urls, url_count);
  NostrFilters *filters = build_filters();

  GError *sub_error = NULL;
  g_autoptr(GNostrSubscription) sub = gnostr_pool_subscribe(pool, filters, &sub_error);
  /* gnostr_pool_subscribe takes ownership of filters on success */
  if (!sub) {
    nostr_filters_free(filters); /* caller retains ownership on failure */
    fprintf(stderr, "gnostr-live-log: subscribe failed: %s\n",
            sub_error ? sub_error->message : "(unknown)");
    g_clear_error(&sub_error);
    for (size_t i = 0; i < url_count; i++) g_free((char*)urls[i]);
    g_free((void*)urls);
    g_main_loop_unref(loop);
    return 1;
  }
  g_signal_connect(sub, "event", G_CALLBACK(on_event), NULL);
  fprintf(stdout, "gnostr-live-log: subscription started\n"); fflush(stdout);

  GCancellable *canc = g_cancellable_new();
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
  gnostr_subscription_close(sub);
  g_main_loop_unref(loop);
  fprintf(stdout, "gnostr-live-log: exit\n"); fflush(stdout);
  return 0;
}
