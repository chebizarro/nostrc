#include "gnostr-timeline-view.h"
#include <nostr-gtk-1.0/gn-timeline-tabs.h>
#include <nostr-gtk-1.0/gnostr-timeline-view-private.h>
#include "gnostr-main-window.h"
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include "gnostr-zap-dialog.h"
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include "../model/gn-nostr-event-item.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include "nostr-event.h"
#include "nostr-json.h"
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "../util/utils.h"
#include "../util/bookmarks.h"
#include "../util/pin_list.h"
#include "../util/nip32_labels.h"
#include "../util/nip23.h"
#include "../util/nip34.h"
#include "../util/nip71.h"
#include "nostr-filter.h"
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
#include <json-glib/json-glib.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* Widget template now loaded from nostr-gtk library resource */

/* Cache size limits to prevent unbounded memory growth */
#define EMBED_CACHE_MAX 500
#define INFLIGHT_MAX 100

/* Forward decl: NoteCardRow embed handler */
static void on_row_request_embed(NostrGtkNoteCardRow *row, const char *target, gpointer user_data);

/* Weak ref wrapper used by async completions */
typedef struct { GWeakRef ref; } RowRef;

/* Inflight dedup entry: shared cancellable and attached rows waiting for result */
typedef struct {
  GCancellable *canc;   /* owned */
  GPtrArray    *rows;   /* RowRef* with free func */
} Inflight;

static void rowref_free(gpointer p) { RowRef *rr = (RowRef*)p; if (!rr) return; g_weak_ref_clear(&rr->ref); g_free(rr); }
static void inflight_free(gpointer p) {
  Inflight *in = (Inflight*)p; if (!in) return;
  if (in->canc) g_object_unref(in->canc);
  if (in->rows) g_ptr_array_free(in->rows, TRUE);
  g_free(in);
}

/* Global inflight map: key -> Inflight* */
static GHashTable *s_inflight = NULL;
static void ensure_inflight(void) {
  if (!s_inflight) s_inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, inflight_free);
}

static void free_rowref(RowRef *rr) {
  if (!rr) return;
  g_weak_ref_clear(&rr->ref);
  g_free(rr);
}

/* Build URL array preferring pointer-provided relay hints, followed by config relays; removes duplicates. */
static void build_urls_with_hints(const char *const *hints, size_t hints_count, const char ***out_urls, size_t *out_count) {
  if (out_urls) *out_urls = NULL; if (out_count) *out_count = 0;
  GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  /* Add hints first */
  for (size_t i = 0; hints && i < hints_count; i++) {
    const char *u = hints[i]; if (!u || !*u) continue;
    if (g_hash_table_add(set, (gpointer)u)) g_ptr_array_add(arr, g_strdup(u));
  }
  /* Add config relays (read-capable only for fetching, per NIP-65) */
  GPtrArray *cfg = gnostr_get_read_relay_urls();
  for (guint i = 0; i < cfg->len; i++) {
    const char *u = (const char*)g_ptr_array_index(cfg, i);
    if (!u || !*u) continue;
    if (g_hash_table_contains(set, u)) continue;
    g_hash_table_add(set, (gpointer)u);
    g_ptr_array_add(arr, g_strdup(u));
  }
  g_ptr_array_unref(cfg);
  g_hash_table_destroy(set);
  if (out_urls && out_count) {
    size_t n = arr->len;
    const char **v = g_new0(const char*, n);
    for (size_t i=0;i<n;i++) v[i] = (const char*)g_ptr_array_index(arr, i);
    *out_urls = v; *out_count = n;
  }
  /* Keep arr strings alive by not freeing them; caller frees out_urls vector only */
  g_ptr_array_free(arr, FALSE);
}

/* Owned variant: duplicates strings and provides a free helper */
static void build_urls_with_hints_owned(const char *const *hints, size_t hints_count, char ***out_urls, size_t *out_count) {
  if (out_urls) *out_urls = NULL; if (out_count) *out_count = 0;
  const char **tmp = NULL; size_t n = 0;
  build_urls_with_hints(hints, hints_count, &tmp, &n);
  if (!tmp || n == 0) { if (tmp) g_free((gpointer)tmp); return; }
  char **owned = g_new0(char*, n);
  for (size_t i = 0; i < n; i++) owned[i] = g_strdup(tmp[i]);
  if (out_urls) *out_urls = owned; if (out_count) *out_count = n;
  g_free((gpointer)tmp);
}

static void free_urls_owned(char **urls, size_t count) {
  if (!urls) return;
  for (size_t i = 0; i < count; i++) g_free(urls[i]);
  g_free(urls);
}

/* NIP-09: Get current user's pubkey as 64-char hex from GSettings.
 * Returns newly allocated string or NULL if not signed in. Caller must free. */
static char *get_current_user_pubkey_hex(void) {
  GSettings *settings = g_settings_new("org.gnostr.Client");
  if (!settings) return NULL;
  char *npub = g_settings_get_string(settings, "current-npub");
  g_object_unref(settings);
  if (!npub || !*npub) {
    g_free(npub);
    return NULL;
  }
  /* Decode bech32 npub to get raw pubkey hex */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
  g_free(npub);
  if (!n19) {
    return NULL;
  }
  const char *pubkey_hex = gnostr_nip19_get_pubkey(n19);
  if (!pubkey_hex) {
    return NULL;
  }
  return g_strdup(pubkey_hex);
}

/* Small TTL cache for embed results */
typedef struct { char *json; time_t when; gboolean negative; } EmbedCacheEntry;
static GHashTable *s_embed_cache = NULL;
static void embed_cache_free(gpointer p) { EmbedCacheEntry *e = (EmbedCacheEntry*)p; if (!e) return; g_free(e->json); g_free(e); }
static void ensure_embed_cache(void) { if (!s_embed_cache) s_embed_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, embed_cache_free); }
static EmbedCacheEntry *embed_cache_get(const char *key, int ttl_sec) {
  ensure_embed_cache(); if (!key) return NULL; EmbedCacheEntry *e = (EmbedCacheEntry*)g_hash_table_lookup(s_embed_cache, key); if (!e) return NULL; time_t now = time(NULL); if (ttl_sec > 0 && (now - e->when) > ttl_sec) return NULL; return e; }
static void embed_cache_put_json(const char *key, const char *json) {
  ensure_embed_cache();
  if (!key) return;
  /* Enforce size limit - clear cache if too large (no LRU tracking) */
  if (g_hash_table_size(s_embed_cache) > EMBED_CACHE_MAX) {
    g_debug("[EMBED_CACHE] Clearing cache (size %u > %u)", g_hash_table_size(s_embed_cache), EMBED_CACHE_MAX);
    g_hash_table_remove_all(s_embed_cache);
  }
  EmbedCacheEntry *e = g_new0(EmbedCacheEntry, 1);
  e->json = json ? g_strdup(json) : NULL;
  e->when = time(NULL);
  e->negative = json ? FALSE : TRUE;
  g_hash_table_replace(s_embed_cache, g_strdup(key), e);
}
static void embed_cache_put_negative(const char *key) { embed_cache_put_json(key, NULL); }

/* Shared async completion (multi): look up inflight by key, update all attached rows, then remove entry */
typedef struct { char *key; } InflightCtx;

static void on_query_single_done_multi(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  InflightCtx *ctx = (InflightCtx*)user_data;
  GError *err = NULL;
  GPtrArray *results = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &err);
  ensure_inflight();
  Inflight *in = ctx && ctx->key ? (Inflight*)g_hash_table_lookup(s_inflight, ctx->key) : NULL;
  /* Prepare parsed view if we got a result */
  char meta[128] = {0}; char snipbuf[281] = {0}; const char *title = "Note"; gboolean have = FALSE;
  if (results && results->len > 0) {
    const char *json = (const char*)g_ptr_array_index(results, 0);
    if (json) {
      /* cache positive */
      if (ctx && ctx->key) embed_cache_put_json(ctx->key, json);
      {
        GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(b, g_strdup(json));
        storage_ndb_ingest_events_async(b);
      }
      NostrEvent *evt = nostr_event_new();
      if (evt && nostr_event_deserialize(evt, json) == 0) {
        const char *content = nostr_event_get_content(evt);
        const char *author_hex = nostr_event_get_pubkey(evt);
        gint64 created_at = (gint64)nostr_event_get_created_at(evt);
        char author_short[17] = {0};
        if (author_hex && strlen(author_hex) >= 8) snprintf(author_short, sizeof(author_short), "%.*s…", 8, author_hex);
        char timebuf[64] = {0};
        if (created_at > 0) { time_t t = (time_t)created_at; struct tm tmv; localtime_r(&t, &tmv); strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", &tmv); }
        if (author_short[0] && timebuf[0]) snprintf(meta, sizeof(meta), "%s · %s", author_short, timebuf);
        else if (author_short[0]) g_strlcpy(meta, author_short, sizeof(meta));
        else if (timebuf[0]) g_strlcpy(meta, timebuf, sizeof(meta));
        if (content && *content) {
          size_t n = 0; char prev_space = 0; for (const char *p = content; *p && n < sizeof(snipbuf)-1; p++) { char c = *p; if (c=='\n'||c=='\r'||c=='\t') c=' '; if (g_ascii_isspace(c)) { c=' '; if (prev_space) continue; prev_space=1; } else prev_space=0; snipbuf[n++]=c; } snipbuf[n]='\0';
        } else { g_strlcpy(snipbuf, "(empty)", sizeof(snipbuf)); }
        have = TRUE;
      }
      if (evt) nostr_event_free(evt);
    }
  }
  /* Update all rows attached to this inflight */
  if (in && in->rows) {
    for (guint i = 0; i < in->rows->len; i++) {
      RowRef *rr = (RowRef*)g_ptr_array_index(in->rows, i);
      if (!rr) continue;
      /* Get the weak ref as GObject first, then validate before casting */
      GObject *obj = g_weak_ref_get(&rr->ref);
      if (!obj || !NOSTR_GTK_IS_NOTE_CARD_ROW(obj)) { 
        if (obj) g_object_unref(obj); 
        continue; 
      }
      NostrGtkNoteCardRow *r = NOSTR_GTK_NOTE_CARD_ROW(obj);
      if (have) nostr_gtk_note_card_row_set_embed_rich(r, title, meta, snipbuf);
      else nostr_gtk_note_card_row_set_embed(r, "Note", "Not found on selected relays");
      g_object_unref(r);
    }
  }
  if (results) g_ptr_array_unref(results);
  if (err) g_clear_error(&err);
  if (!have && ctx && ctx->key) {
    /* cache negative if nothing found */
    embed_cache_put_negative(ctx->key);
  }
  /* Remove and free inflight entry */
  if (ctx && ctx->key) g_hash_table_remove(s_inflight, ctx->key);
  if (ctx) { g_free(ctx->key); g_free(ctx); }
}

/* Avatar metrics are provided by the centralized avatar_cache module. */

/* ---- Embed helpers ------------------------------------------------------- */
static gboolean hex32_from_string(const char *hex, unsigned char out[32]) {
  if (!hex) return FALSE;
  size_t len = strlen(hex);
  if (len != 64) return FALSE;
  for (size_t i = 0; i < 32; i++) {
    char byte_str[3] = { hex[2*i], hex[2*i+1], 0 };
    char *end = NULL; long v = strtol(byte_str, &end, 16);
    if (!end || *end) return FALSE;
    out[i] = (unsigned char)v;
  }
  return TRUE;
}

/* Remove a row from all inflight waits; cancel any request that becomes unused */
static void inflight_detach_row(GtkWidget *row) {
  ensure_inflight();
  if (!s_inflight) return;
  GHashTableIter it; gpointer k, v; g_hash_table_iter_init(&it, s_inflight);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    Inflight *in = (Inflight*)v; if (!in || !in->rows) continue;
    guint write = 0; for (guint i = 0; i < in->rows->len; i++) {
      RowRef *rr = (RowRef*)g_ptr_array_index(in->rows, i);
      if (!rr) continue;
      /* Get weak ref as GObject first, validate before casting */
      GObject *obj = g_weak_ref_get(&rr->ref);
      gboolean keep = TRUE;
      if (!obj || !NOSTR_GTK_IS_NOTE_CARD_ROW(obj) || GTK_WIDGET(obj) == row) keep = FALSE;
      if (obj) g_object_unref(obj);
      if (keep) {
        g_ptr_array_index(in->rows, write++) = rr;
      } else {
        rowref_free(rr);
      }
    }
    in->rows->len = write;
    if (write == 0 && in->canc) g_cancellable_cancel(in->canc);
  }
}

static char *build_key_for_note_hex(const char *idhex) { return g_strdup_printf("id:%s", idhex ? idhex : ""); }
static char *build_key_for_naddr_fields(int kind, const char *pubkey, const char *identifier) { return g_strdup_printf("a:%d:%s:%s", kind, pubkey ? pubkey : "", identifier ? identifier : ""); }

/* Enforce INFLIGHT_MAX: cancel and remove oldest inflight when at capacity */
static void inflight_enforce_limit(void) {
  if (!s_inflight || g_hash_table_size(s_inflight) < INFLIGHT_MAX)
    return;

  /* Cancel the first entry found (hash table iteration order is arbitrary,
   * which is acceptable — we just need to shed load) */
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, s_inflight);
  if (g_hash_table_iter_next(&it, &k, &v)) {
    Inflight *old = (Inflight*)v;
    if (old && old->canc) g_cancellable_cancel(old->canc);
    g_hash_table_iter_remove(&it);
    g_debug("[INFLIGHT] Evicted oldest request (table at %u)", INFLIGHT_MAX);
  }
}

/* Start or attach to an inflight request */
static void start_or_attach_request(const char *key, const char **urls, size_t url_count, NostrFilter *f, NostrGtkNoteCardRow *row) {
  ensure_inflight();
  Inflight *in = (Inflight*)g_hash_table_lookup(s_inflight, key);
  if (!in) {
    inflight_enforce_limit();
    in = g_new0(Inflight, 1);
    in->canc = g_cancellable_new();
    in->rows = g_ptr_array_new_with_free_func(rowref_free);
    g_hash_table_insert(s_inflight, g_strdup(key), in);
    /* Start the network request */
    static GNostrPool *s_pool = NULL; if (!s_pool) s_pool = gnostr_pool_new();
    InflightCtx *ctx = g_new0(InflightCtx, 1); ctx->key = g_strdup(key);
    gnostr_pool_sync_relays(s_pool, (const gchar **)urls, url_count);
    {
      NostrFilters *_qf = nostr_filters_new();
      nostr_filters_add(_qf, f);
      gnostr_pool_query_async(s_pool, _qf, in->canc, on_query_single_done_multi, ctx);
    }
  }
  /* Attach row */
  RowRef *rr = g_new0(RowRef, 1); g_weak_ref_init(&rr->ref, row);
  g_ptr_array_add(in->rows, rr);
}

static void on_row_request_embed(NostrGtkNoteCardRow *row, const char *target, gpointer user_data) {
  (void)user_data;
  if (!NOSTR_GTK_IS_NOTE_CARD_ROW(row) || !target || !*target) return;
  /* Normalize nostr: URIs */
  const char *ref = target;
  if (g_str_has_prefix(ref, "nostr:")) ref = target + 6;

  /* Decode bech32 reference using GNostrNip19 */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(ref, NULL);
  if (!n19) {
    nostr_gtk_note_card_row_set_embed(row, "Reference", ref);
    return;
  }

  GNostrBech32Type btype = gnostr_nip19_get_entity_type(n19);

  /* Handle bare note1 */
  if (btype == GNOSTR_BECH32_NOTE) {
    const char *event_id_hex = gnostr_nip19_get_event_id(n19);
    if (!event_id_hex) { nostr_gtk_note_card_row_set_embed(row, "Reference", ref); return; }
    unsigned char id32[32];
    if (!hex32_from_string(event_id_hex, id32)) { nostr_gtk_note_card_row_set_embed(row, "Reference", ref); return; }
    /* Query local store */
    void *txn = NULL; if (storage_ndb_begin_query(&txn) == 0 && txn) {
      char *json = NULL; int jlen = 0;
      if (storage_ndb_get_note_by_id(txn, id32, &json, &jlen) == 0 && json) {
        NostrEvent *evt = nostr_event_new();
        if (evt && nostr_event_deserialize(evt, json) == 0) {
          const char *content = nostr_event_get_content(evt);
          if (content && *content) {
            /* Simple title/snippet */
            char title[64]; g_strlcpy(title, content, sizeof(title));
            nostr_gtk_note_card_row_set_embed(row, title, content);
          } else {
            nostr_gtk_note_card_row_set_embed(row, "Note", "(empty)");
          }
        }
        if (evt) nostr_event_free(evt);
        g_free(json);
      } else {
        nostr_gtk_note_card_row_set_embed(row, "Note", "Not found in local cache (fetching…)");
      }
      storage_ndb_end_query(txn);
    }
    /* Kick off relay fetch for this note id (dedup + cancellable) */
    {
      /* Build filter by id */
      NostrFilter *f = nostr_filter_new();
      const char *ids[1] = { event_id_hex };
      nostr_filter_set_ids(f, ids, 1);
      /* Cache pre-check */
      g_autofree char *key = build_key_for_note_hex(event_id_hex);
      EmbedCacheEntry *ce = embed_cache_get(key, 60);
      if (ce) {
        if (!ce->negative && ce->json) {
          NostrEvent *evt = nostr_event_new();
          if (evt && nostr_event_deserialize(evt, ce->json) == 0) {
            const char *content = nostr_event_get_content(evt);
            const char *author_hex = nostr_event_get_pubkey(evt);
            gint64 created_at = (gint64)nostr_event_get_created_at(evt);
            char meta[128] = {0}; char author_short[17] = {0}; char timebuf[64] = {0};
            if (author_hex && strlen(author_hex) >= 8) snprintf(author_short, sizeof(author_short), "%.*s…", 8, author_hex);
            if (created_at > 0) { time_t t=(time_t)created_at; struct tm tmv; localtime_r(&t,&tmv); strftime(timebuf,sizeof(timebuf), "%Y-%m-%d %H:%M", &tmv); }
            if (author_short[0] && timebuf[0]) snprintf(meta, sizeof(meta), "%s · %s", author_short, timebuf);
            else if (author_short[0]) g_strlcpy(meta, author_short, sizeof(meta));
            else if (timebuf[0]) g_strlcpy(meta, timebuf, sizeof(meta));
            char snipbuf[281]; if (content && *content) { size_t n=0; char ps=0; for (const char *p=content; *p && n<280; p++){ char c=*p; if (c=='\n'||c=='\r'||c=='\t') c=' '; if (g_ascii_isspace(c)){ c=' '; if (ps) continue; ps=1; } else ps=0; snipbuf[n++]=c; } snipbuf[n]='\0'; } else { g_strlcpy(snipbuf, "(empty)", sizeof(snipbuf)); }
            nostr_gtk_note_card_row_set_embed_rich(row, "Note", meta, snipbuf);
          }
          if (evt) nostr_event_free(evt);
          nostr_filter_free(f);
          goto done_note_fetch;
        } else {
          nostr_gtk_note_card_row_set_embed(row, "Note", "Not found on selected relays");
          nostr_filter_free(f);
          goto done_note_fetch;
        }
      }
      /* Load relay URLs (owned) */
      char **urls = NULL; size_t url_count = 0;
      build_urls_with_hints_owned(NULL, 0, &urls, &url_count);
      if (url_count > 0 && urls) {
        start_or_attach_request(key, (const char**)urls, url_count, f, row);
      }
      free_urls_owned(urls, url_count);
      nostr_filter_free(f);
done_note_fetch:
      ;
    }
    return;
  }

  /* Handle nevent */
  if (btype == GNOSTR_BECH32_NEVENT) {
    const char *event_id_hex = gnostr_nip19_get_event_id(n19);
    if (!event_id_hex) { nostr_gtk_note_card_row_set_embed(row, "Reference", ref); return; }
    unsigned char id32[32];
    if (hex32_from_string(event_id_hex, id32)) {
      void *txn = NULL; if (storage_ndb_begin_query(&txn) == 0 && txn) {
        char *json = NULL; int jlen = 0;
        if (storage_ndb_get_note_by_id(txn, id32, &json, &jlen) == 0 && json) {
          NostrEvent *evt = nostr_event_new();
          if (evt && nostr_event_deserialize(evt, json) == 0) {
            const char *content = nostr_event_get_content(evt);
            if (content && *content) {
              char title[64]; g_strlcpy(title, content, sizeof(title));
              nostr_gtk_note_card_row_set_embed(row, title, content);
            } else {
              nostr_gtk_note_card_row_set_embed(row, "Note", "(empty)");
            }
          }
          if (evt) nostr_event_free(evt);
          g_free(json);
        } else {
          nostr_gtk_note_card_row_set_embed(row, "Note", "Not found in local cache (fetching…)");
        }
        storage_ndb_end_query(txn);
      }
      /* Kick off relay fetch using id hex from pointer (dedup + cancellable) */
      {
        NostrFilter *f = nostr_filter_new();
        const char *ids[1] = { event_id_hex };
        nostr_filter_set_ids(f, ids, 1);
        /* Cache pre-check */
        g_autofree char *key = build_key_for_note_hex(event_id_hex);
        EmbedCacheEntry *ce = embed_cache_get(key, 60);
        if (ce) {
          if (!ce->negative && ce->json) {
            NostrEvent *evt = nostr_event_new();
            if (evt && nostr_event_deserialize(evt, ce->json) == 0) {
              const char *content = nostr_event_get_content(evt);
              const char *author_hex = nostr_event_get_pubkey(evt);
              gint64 created_at = (gint64)nostr_event_get_created_at(evt);
              char meta[128] = {0}; char author_short[17] = {0}; char timebuf[64] = {0};
              if (author_hex && strlen(author_hex) >= 8) snprintf(author_short, sizeof(author_short), "%.*s…", 8, author_hex);
              if (created_at > 0) { time_t t=(time_t)created_at; struct tm tmv; localtime_r(&t,&tmv); strftime(timebuf,sizeof(timebuf), "%Y-%m-%d %H:%M", &tmv); }
              if (author_short[0] && timebuf[0]) snprintf(meta, sizeof(meta), "%s · %s", author_short, timebuf);
              else if (author_short[0]) g_strlcpy(meta, author_short, sizeof(meta));
              else if (timebuf[0]) g_strlcpy(meta, timebuf, sizeof(meta));
              char snipbuf[281]; if (content && *content) { size_t n=0; char ps=0; for (const char *p=content; *p && n<280; p++){ char c=*p; if (c=='\n'||c=='\r'||c=='\t') c=' '; if (g_ascii_isspace(c)){ c=' '; if (ps) continue; ps=1; } else ps=0; snipbuf[n++]=c; } snipbuf[n]='\0'; } else { g_strlcpy(snipbuf, "(empty)", sizeof(snipbuf)); }
              nostr_gtk_note_card_row_set_embed_rich(row, "Note", meta, snipbuf);
            }
            if (evt) nostr_event_free(evt);
            nostr_filter_free(f);
            goto done_nevent_fetch;
          } else {
            nostr_gtk_note_card_row_set_embed(row, "Note", "Not found on selected relays");
            nostr_filter_free(f);
            goto done_nevent_fetch;
          }
        }
        char **urls = NULL; size_t url_count = 0;
        /* Prefer relay hints from pointer */
        const gchar *const *hint_relays = gnostr_nip19_get_relays(n19);
        size_t hint_count = 0;
        if (hint_relays) { for (; hint_relays[hint_count]; hint_count++) ; }
        build_urls_with_hints_owned(hint_relays, hint_count, &urls, &url_count);
        if (url_count > 0 && urls) {
          start_or_attach_request(key, (const char**)urls, url_count, f, row);
        }
        free_urls_owned(urls, url_count);
        nostr_filter_free(f);
done_nevent_fetch:
        ;
      }
    } else {
      nostr_gtk_note_card_row_set_embed(row, "Reference", event_id_hex);
    }
  } else if (btype == GNOSTR_BECH32_NADDR) {
    /* Addressable entity: build filter (kind+author+tag d) and fetch */
    nostr_gtk_note_card_row_set_embed(row, "Addressable entity", ref);
    const char *naddr_identifier = gnostr_nip19_get_identifier(n19);
    const char *naddr_pubkey = gnostr_nip19_get_pubkey(n19);
    gint naddr_kind = gnostr_nip19_get_kind(n19);
    if (naddr_identifier && naddr_pubkey && naddr_kind > 0) {
      NostrFilter *f = nostr_filter_new();
      int kinds[1] = { naddr_kind }; nostr_filter_set_kinds(f, kinds, 1);
      const char *authors[1] = { naddr_pubkey }; nostr_filter_set_authors(f, authors, 1);
      /* tag d */
      nostr_filter_tags_append(f, "d", naddr_identifier, NULL);
      /* Cache pre-check */
      {
        g_autofree char *cache_key = build_key_for_naddr_fields(naddr_kind, naddr_pubkey, naddr_identifier);
        EmbedCacheEntry *ce = embed_cache_get(cache_key, 60);
        if (ce) {
          if (!ce->negative && ce->json) {
            NostrEvent *evt = nostr_event_new();
            if (evt && nostr_event_deserialize(evt, ce->json) == 0) {
              const char *content = nostr_event_get_content(evt);
              const char *author_hex = nostr_event_get_pubkey(evt);
              gint64 created_at = (gint64)nostr_event_get_created_at(evt);
              char meta[128] = {0}; char author_short[17] = {0}; char timebuf[64] = {0};
              if (author_hex && strlen(author_hex) >= 8) snprintf(author_short, sizeof(author_short), "%.*s…", 8, author_hex);
              if (created_at > 0) { time_t t=(time_t)created_at; struct tm tmv; localtime_r(&t,&tmv); strftime(timebuf,sizeof(timebuf), "%Y-%m-%d %H:%M", &tmv); }
              if (author_short[0] && timebuf[0]) snprintf(meta, sizeof(meta), "%s · %s", author_short, timebuf);
              else if (author_short[0]) g_strlcpy(meta, author_short, sizeof(meta));
              else if (timebuf[0]) g_strlcpy(meta, timebuf, sizeof(meta));
              char snipbuf[281]; const char *title = "Note"; if (content && *content) { size_t n=0; char ps=0; for (const char *p=content; *p && n<280; p++){ char c=*p; if (c=='\n'||c=='\r'||c=='\t') c=' '; if (g_ascii_isspace(c)){ c=' '; if (ps) continue; ps=1; } else ps=0; snipbuf[n++]=c; } snipbuf[n]='\0'; } else { g_strlcpy(snipbuf, "(empty)", sizeof(snipbuf)); }
              nostr_gtk_note_card_row_set_embed_rich(row, title, meta, snipbuf);
            }
            if (evt) nostr_event_free(evt);
            nostr_filter_free(f);
            goto done_naddr_fetch;
          } else {
            nostr_gtk_note_card_row_set_embed(row, "Note", "Not found on selected relays");
            nostr_filter_free(f);
            goto done_naddr_fetch;
          }
        }
      }
      char **urls = NULL; size_t url_count = 0;
      const gchar *const *naddr_relays = gnostr_nip19_get_relays(n19);
      size_t naddr_relay_count = 0;
      if (naddr_relays) { for (; naddr_relays[naddr_relay_count]; naddr_relay_count++) ; }
      build_urls_with_hints_owned(naddr_relays, naddr_relay_count, &urls, &url_count);
      if (url_count > 0 && urls) {
        g_autofree char *key = build_key_for_naddr_fields(naddr_kind, naddr_pubkey, naddr_identifier);
        start_or_attach_request(key, (const char**)urls, url_count, f, row);
      }
      free_urls_owned(urls, url_count);
      nostr_filter_free(f);
done_naddr_fetch:
      ;
    }
  } else if (btype == GNOSTR_BECH32_NPROFILE) {
    nostr_gtk_note_card_row_set_embed(row, "Profile", ref);
  } else {
    nostr_gtk_note_card_row_set_embed(row, "Reference", ref);
  }
}

/* gnostr_avatar_metrics_log() now comes from avatar_cache. */

/* Avatar cache helpers are implemented in avatar_cache.c */

/* TimelineItem type defined in nostr-gtk library (gnostr-timeline-view-private.h) */

/* TimelineItem type lifecycle (property/class/init) defined in nostr-gtk library */

static TimelineItem *timeline_item_new(const char *display, const char *handle, const char *ts, const char *content, guint depth) {
  TimelineItem *it = g_object_new(timeline_item_get_type(),
                                  "display-name", display ? display : "Anonymous",
                                  "handle",       handle  ? handle  : "@anon",
                                  "timestamp",    ts      ? ts      : "now",
                                  "content",      content ? content : "",
                                  "depth",        depth,
                                  NULL);
  it->children = g_list_store_new(timeline_item_get_type());
  return it;
}

static void timeline_item_set_meta(TimelineItem *it, const char *id, const char *pubkey, gint64 created_at) {
  if (!it) return;
  g_object_set(it,
               "id", id,
               "pubkey", pubkey,
               "created-at", created_at,
               NULL);
}

/* NIP-18: Set repost info on a timeline item */
static void timeline_item_set_repost_info(TimelineItem *it,
                                           const char *reposter_pubkey,
                                           const char *reposter_display_name,
                                           gint64 repost_created_at) {
  if (!it) return;
  g_clear_pointer(&it->reposter_pubkey, g_free);
  g_clear_pointer(&it->reposter_display_name, g_free);
  it->is_repost = TRUE;
  it->reposter_pubkey = g_strdup(reposter_pubkey);
  it->reposter_display_name = g_strdup(reposter_display_name);
  it->repost_created_at = repost_created_at;
}

/* NIP-18: Set quote info on a timeline item */
static void timeline_item_set_quote_info(TimelineItem *it,
                                          const char *quoted_event_id,
                                          const char *quoted_content,
                                          const char *quoted_author) {
  if (!it) return;
  g_clear_pointer(&it->quoted_event_id, g_free);
  g_clear_pointer(&it->quoted_content, g_free);
  g_clear_pointer(&it->quoted_author, g_free);
  it->has_quote = TRUE;
  it->quoted_event_id = g_strdup(quoted_event_id);
  it->quoted_content = g_strdup(quoted_content);
  it->quoted_author = g_strdup(quoted_author);
}

static GListModel *timeline_item_get_children_model(TimelineItem *it) {
  return it ? G_LIST_MODEL(it->children) : NULL;
}

static void timeline_item_add_child(TimelineItem *parent, TimelineItem *child) {
  if (!parent || !child) return;
  g_list_store_append(parent->children, child);
}

/* Public wrappers now in nostr-gtk library */

/* NostrGtkTimelineView type, struct, signals, dispose/finalize, and tab handler
 * all defined in nostr-gtk library (gnostr-timeline-view.c). */

/* Setup: load row UI from resource and set as child. Cache subwidgets on the row. */
static void on_note_card_open_profile_relay(NostrGtkNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, emit signal or call method */
      extern void gnostr_main_window_open_profile(GtkWidget *window, const char *pubkey_hex);
      gnostr_main_window_open_profile(widget, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* Handler for reply button - relay to main window */
static void on_note_card_reply_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *root_id, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to set reply context */
      extern void gnostr_main_window_request_reply(GtkWidget *window, const char *id_hex, const char *root_id, const char *pubkey_hex);
      gnostr_main_window_request_reply(widget, id_hex, root_id, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* Handler for repost button - relay to main window */
static void on_note_card_repost_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to handle repost */
      extern void gnostr_main_window_request_repost(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
      gnostr_main_window_request_repost(widget, id_hex, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* Handler for quote button - relay to main window */
static void on_note_card_quote_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to open composer in quote mode */
      extern void gnostr_main_window_request_quote(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
      gnostr_main_window_request_quote(widget, id_hex, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* Handler for like button - relay to main window */
static void on_note_card_like_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window - call the like function */
      gnostr_main_window_request_like(widget, id_hex, pubkey_hex, event_kind, reaction_content, row);
      break;
    }
  }
  (void)user_data;
}

/* Handler for comment button (NIP-22) - relay to main window */
static void on_note_card_comment_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, int kind, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window - call the comment function */
      extern void gnostr_main_window_request_comment(GtkWidget *window, const char *id_hex, int kind, const char *pubkey_hex);
      gnostr_main_window_request_comment(widget, id_hex, kind, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* Handler for zap button - show zap dialog */
static void on_note_card_zap_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, const char *lud16, gpointer user_data) {
  (void)user_data;

  if (!id_hex || !pubkey_hex) {
    g_warning("[TIMELINE] Zap requested but missing id or pubkey");
    return;
  }

  if (!lud16 || !*lud16) {
    g_message("[TIMELINE] Zap requested but user has no lightning address");
    return;
  }

  /* Find the parent window */
  GtkWidget *widget = GTK_WIDGET(row);
  GtkWindow *parent = NULL;
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && GTK_IS_WINDOW(widget)) {
      parent = GTK_WINDOW(widget);
      break;
    }
  }

  /* Create and show zap dialog */
  GnostrZapDialog *dialog = gnostr_zap_dialog_new(parent);

  /* Look up display name from profile cache, fall back to npub prefix */
  g_autofree gchar *display_name = NULL;
  GnostrProfileMeta *profile = gnostr_profile_provider_get(pubkey_hex);
  if (profile) {
    /* Prefer display_name, fall back to name */
    if (profile->display_name && *profile->display_name) {
      display_name = g_strdup(profile->display_name);
    } else if (profile->name && *profile->name) {
      display_name = g_strdup(profile->name);
    }
    gnostr_profile_meta_free(profile);
  }

  /* Fall back to npub prefix if no profile name available */
  if (!display_name && pubkey_hex) {
    /* Convert hex to npub and use first 12 chars + "..." */
    g_autoptr(GNostrNip19) npub_n19 = gnostr_nip19_encode_npub(pubkey_hex, NULL);
    if (npub_n19) {
      const char *npub = gnostr_nip19_get_bech32(npub_n19);
      if (npub) {
        display_name = g_strdup_printf("%.12s...", npub);
      }
    }
  }

  gnostr_zap_dialog_set_recipient(dialog, pubkey_hex, display_name, lud16);

  /* Set the event being zapped */
  gnostr_zap_dialog_set_event(dialog, id_hex, 1);  /* kind 1 = text note */

  /* Get relays from config (GSettings defaults if none configured) */
  GPtrArray *relay_arr = gnostr_get_write_relay_urls();
  const gchar **relay_strs = g_new0(const gchar*, relay_arr->len + 1);
  for (guint i = 0; i < relay_arr->len; i++) {
    relay_strs[i] = g_ptr_array_index(relay_arr, i);
  }
  gnostr_zap_dialog_set_relays(dialog, relay_strs);
  g_free(relay_strs);
  g_ptr_array_unref(relay_arr);

  gtk_window_present(GTK_WINDOW(dialog));

  g_message("[TIMELINE] Zap dialog opened for id=%s lud16=%s", id_hex, lud16);
}

/* Handler for view-thread button - relay to main window */
static void on_note_card_view_thread_requested_relay(NostrGtkNoteCardRow *row, const char *root_event_id, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* nostrc-a2zd: Try to get event JSON from nostrdb to avoid race condition.
       * If the event is in nostrdb, pass it directly to thread view. */
      char *event_json = NULL;
      int json_len = 0;
      storage_ndb_get_note_by_id_nontxn(root_event_id, &event_json, &json_len);

      extern void gnostr_main_window_view_thread_with_json(GtkWidget *window, const char *root_event_id, const char *event_json);
      gnostr_main_window_view_thread_with_json(widget, root_event_id, event_json);

      if (event_json) free(event_json);
      break;
    }
  }
  (void)user_data;
}

/* Handler for navigate-to-note signal - opens thread view focused on target note */
static void on_note_card_navigate_to_note_relay(NostrGtkNoteCardRow *row, const char *event_id, gpointer user_data) {
  /* Relay the signal up to the main window - opens thread view focused on the target note */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* nostrc-a2zd: Try to get event JSON from nostrdb to avoid race condition. */
      char *event_json = NULL;
      int json_len = 0;
      storage_ndb_get_note_by_id_nontxn(event_id, &event_json, &json_len);

      extern void gnostr_main_window_view_thread_with_json(GtkWidget *window, const char *root_event_id, const char *event_json);
      gnostr_main_window_view_thread_with_json(widget, event_id, event_json);

      if (event_json) free(event_json);
      break;
    }
  }
  (void)user_data;
}

/* Handler for mute-user button - relay to main window */
static void on_note_card_mute_user_requested_relay(NostrGtkNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to mute user */
      extern void gnostr_main_window_mute_user(GtkWidget *window, const char *pubkey_hex);
      gnostr_main_window_mute_user(widget, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* Handler for mute-thread button - relay to main window */
static void on_note_card_mute_thread_requested_relay(NostrGtkNoteCardRow *row, const char *event_id_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to mute thread */
      extern void gnostr_main_window_mute_thread(GtkWidget *window, const char *event_id_hex);
      gnostr_main_window_mute_thread(widget, event_id_hex);
      break;
    }
  }
  (void)user_data;
}

/* Handler for show-toast signal - relay to main window */
static void on_note_card_show_toast_relay(NostrGtkNoteCardRow *row, const char *message, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to show toast */
      extern void gnostr_main_window_show_toast(GtkWidget *window, const char *message);
      gnostr_main_window_show_toast(widget, message);
      break;
    }
  }
  (void)user_data;
}

/* nostrc-ch2v: Handler for pin-toggled signal - update NIP-51 pin list */
static void on_note_card_pin_toggled_cb(NostrGtkNoteCardRow *row, const char *event_id, gboolean is_pinned, gpointer user_data) {
  (void)user_data;
  (void)row;

  if (!event_id || strlen(event_id) != 64) {
    g_warning("[PIN] Invalid event ID for pin toggle");
    return;
  }

  GnostrPinList *pin_list = gnostr_pin_list_get_default();
  if (!pin_list) {
    g_warning("[PIN] Failed to get pin list instance");
    return;
  }

  if (is_pinned) {
    gnostr_pin_list_add(pin_list, event_id, NULL);
  } else {
    gnostr_pin_list_remove(pin_list, event_id);
  }

  gnostr_pin_list_save_async(pin_list, NULL, NULL);

  g_message("[PIN] Pin %s for event %s", is_pinned ? "added" : "removed", event_id);
}

/* Handler for bookmark-toggled signal - update NIP-51 bookmark list */
static void on_note_card_bookmark_toggled_cb(NostrGtkNoteCardRow *row, const char *event_id, gboolean is_bookmarked, gpointer user_data) {
  (void)user_data;
  (void)row;

  if (!event_id || strlen(event_id) != 64) {
    g_warning("[BOOKMARK] Invalid event ID for bookmark toggle");
    return;
  }

  GnostrBookmarks *bookmarks = gnostr_bookmarks_get_default();
  if (!bookmarks) {
    g_warning("[BOOKMARK] Failed to get bookmarks instance");
    return;
  }

  /* Update the bookmark list */
  if (is_bookmarked) {
    gnostr_bookmarks_add(bookmarks, event_id, NULL, FALSE);
  } else {
    gnostr_bookmarks_remove(bookmarks, event_id);
  }

  /* Save the updated bookmark list to relays asynchronously */
  gnostr_bookmarks_save_async(bookmarks, NULL, NULL);

  g_message("[BOOKMARK] Bookmark %s for event %s", is_bookmarked ? "added" : "removed", event_id);
}

/* NIP-09: Handler for delete-note-requested signal - relay to main window */
static void on_note_card_delete_note_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to delete note */
      extern void gnostr_main_window_request_delete_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
      gnostr_main_window_request_delete_note(widget, id_hex, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* NIP-56: Handler for report-note-requested signal - relay to main window */
static void on_note_card_report_note_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to report note */
      extern void gnostr_main_window_request_report_note(GtkWidget *window, const char *id_hex, const char *pubkey_hex);
      gnostr_main_window_request_report_note(widget, id_hex, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* NIP-32: Handler for label-note-requested signal - relay to main window */
static void on_note_card_label_note_requested_relay(NostrGtkNoteCardRow *row, const char *id_hex, const char *namespace, const char *label, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window, call method to create label event */
      extern void gnostr_main_window_request_label_note(GtkWidget *window, const char *id_hex, const char *namespace, const char *label, const char *pubkey_hex);
      gnostr_main_window_request_label_note(widget, id_hex, namespace, label, pubkey_hex);
      break;
    }
  }
  (void)user_data;
}

/* Callback when profile is loaded for an event item - show the row */
static void on_event_item_profile_changed(GObject *event_item, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkListItem *list_item = GTK_LIST_ITEM(user_data);

  /* Get the row widget */
  GtkWidget *row = gtk_list_item_get_child(list_item);
  if (!GTK_IS_WIDGET(row)) return;

  /* CRITICAL: Check if row is being disposed before updating.
   * Profile updates can be queued via idle callbacks and may arrive while
   * the row is being disposed, causing Pango layout corruption (nostrc-ipp). */
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
    NostrGtkNoteCardRow *card_row = NOSTR_GTK_NOTE_CARD_ROW(row);
    if (nostr_gtk_note_card_row_is_disposed(card_row)) return;
  }

  /* Check if profile is now available */
  GObject *profile = NULL;
  g_object_get(event_item, "profile", &profile, NULL);

  if (profile) {
    /* Profile loaded - show the row and update author info */
    gchar *display = NULL, *handle = NULL, *avatar_url = NULL, *nip05 = NULL;
    g_object_get(profile,
                 "display-name", &display,
                 "name",         &handle,
                 "picture-url",  &avatar_url,
                 "nip05",        &nip05,
                 NULL);

    if (NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
      nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row), display, handle, avatar_url);
      gtk_widget_set_visible(row, TRUE);

      /* NIP-05: Set verification identifier if available */
      if (nip05 && *nip05) {
        gchar *pubkey = NULL;
        g_object_get(event_item, "pubkey", &pubkey, NULL);
        if (pubkey && strlen(pubkey) == 64) {
          nostr_gtk_note_card_row_set_nip05(NOSTR_GTK_NOTE_CARD_ROW(row), nip05, pubkey);
        }
        g_free(pubkey);
      }
    }

    g_free(display);
    g_free(handle);
    g_free(avatar_url);
    g_free(nip05);
    g_object_unref(profile);
  }
}

/* Handler for search-hashtag signal from note card rows */
static void on_note_card_search_hashtag(NostrGtkNoteCardRow *row, const char *hashtag, gpointer user_data) {
  (void)row;
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(user_data);
  if (!self || !hashtag || !*hashtag) return;

  g_debug("timeline_view: search-hashtag signal received for #%s", hashtag);

  /* Add a new hashtag tab */
  nostr_gtk_timeline_view_add_hashtag_tab(self, hashtag);
}

static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f;
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(data);
  GtkWidget *row = GTK_WIDGET(nostr_gtk_note_card_row_new());

  /* Connect the open-profile signal */
  g_signal_connect(row, "open-profile", G_CALLBACK(on_note_card_open_profile_relay), NULL);
  /* Connect the reply-requested signal */
  g_signal_connect(row, "reply-requested", G_CALLBACK(on_note_card_reply_requested_relay), NULL);
  /* Connect the repost-requested signal */
  g_signal_connect(row, "repost-requested", G_CALLBACK(on_note_card_repost_requested_relay), NULL);
  /* Connect the quote-requested signal */
  g_signal_connect(row, "quote-requested", G_CALLBACK(on_note_card_quote_requested_relay), NULL);
  /* Connect the like-requested signal */
  g_signal_connect(row, "like-requested", G_CALLBACK(on_note_card_like_requested_relay), NULL);
  /* Connect the comment-requested signal (NIP-22) */
  g_signal_connect(row, "comment-requested", G_CALLBACK(on_note_card_comment_requested_relay), NULL);
  /* Connect the zap-requested signal */
  g_signal_connect(row, "zap-requested", G_CALLBACK(on_note_card_zap_requested_relay), NULL);
  /* Connect the view-thread-requested signal */
  g_signal_connect(row, "view-thread-requested", G_CALLBACK(on_note_card_view_thread_requested_relay), NULL);
  /* Connect the navigate-to-note signal (for reply indicator click) */
  g_signal_connect(row, "navigate-to-note", G_CALLBACK(on_note_card_navigate_to_note_relay), NULL);
  /* Connect the mute-user-requested signal */
  g_signal_connect(row, "mute-user-requested", G_CALLBACK(on_note_card_mute_user_requested_relay), NULL);
  /* Connect the mute-thread-requested signal */
  g_signal_connect(row, "mute-thread-requested", G_CALLBACK(on_note_card_mute_thread_requested_relay), NULL);
  /* Connect the show-toast signal */
  g_signal_connect(row, "show-toast", G_CALLBACK(on_note_card_show_toast_relay), NULL);
  /* Connect the pin-toggled signal (nostrc-ch2v) */
  g_signal_connect(row, "pin-toggled", G_CALLBACK(on_note_card_pin_toggled_cb), NULL);
  /* Connect the bookmark-toggled signal */
  g_signal_connect(row, "bookmark-toggled", G_CALLBACK(on_note_card_bookmark_toggled_cb), NULL);
  /* Connect the delete-note-requested signal (NIP-09) */
  g_signal_connect(row, "delete-note-requested", G_CALLBACK(on_note_card_delete_note_requested_relay), NULL);
  /* Connect the report-note-requested signal (NIP-56) */
  g_signal_connect(row, "report-note-requested", G_CALLBACK(on_note_card_report_note_requested_relay), NULL);
  /* Connect the label-note-requested signal (NIP-32) */
  g_signal_connect(row, "label-note-requested", G_CALLBACK(on_note_card_label_note_requested_relay), NULL);
  /* Connect the search-hashtag signal (Phase 3: timeline tabs) */
  if (self) {
    g_signal_connect(row, "search-hashtag", G_CALLBACK(on_note_card_search_hashtag), self);
  }

  gtk_list_item_set_child(item, row);
}

/* Avatar loading now handled by centralized gnostr-avatar-cache module */
static void try_set_avatar(GtkWidget *row, const char *avatar_url, const char *display, const char *handle) {
  GtkWidget *w_init = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "avatar_initials"));
  GtkWidget *w_img  = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "avatar_image"));
  
  /* Derive initials fallback */
  const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
  char initials[3] = {0};
  int i = 0; 
  for (const char *p = src; *p && i < 2; p++) { 
    if (g_ascii_isalnum(*p)) initials[i++] = g_ascii_toupper(*p); 
  }
  if (i == 0) { initials[0] = 'A'; initials[1] = 'N'; }
  if (GTK_IS_LABEL(w_init)) gtk_label_set_text(GTK_LABEL(w_init), initials);

  if (!avatar_url || !*avatar_url || !str_has_prefix_http(avatar_url) || !GTK_IS_PICTURE(w_img)) {
    if (GTK_IS_WIDGET(w_img)) gtk_widget_set_visible(w_img, FALSE);
    if (GTK_IS_WIDGET(w_init)) gtk_widget_set_visible(w_init, TRUE);
    return;
  }

  /* Try loading from cache first */
  GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
  if (cached) {
    gtk_picture_set_paintable(GTK_PICTURE(w_img), GDK_PAINTABLE(cached));
    gtk_widget_set_visible(w_img, TRUE);
    if (GTK_IS_WIDGET(w_init)) gtk_widget_set_visible(w_init, FALSE);
    g_object_unref(cached);
    return;
  }
  
  /* Cache miss - download asynchronously */
  gnostr_avatar_download_async(avatar_url, w_img, w_init);
}

/* Notify handlers to react to TimelineItem property changes after initial bind */
static void on_item_notify_display_name(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL;
  g_object_get(obj, "display-name", &display, "handle", &handle, "avatar-url", &avatar_url, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row), display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url);
}

static void on_item_notify_handle(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL;
  g_object_get(obj, "display-name", &display, "handle", &handle, "avatar-url", &avatar_url, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row), display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url);
}

static void on_item_notify_avatar_url(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  gchar *url = NULL, *display = NULL, *handle = NULL;
  g_object_get(obj, "avatar-url", &url, "display-name", &display, "handle", &handle, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row), display, handle, url);
  g_free(url); g_free(display); g_free(handle);
}

/* NIP-25: Notify handler for like count changes */
static void on_item_notify_like_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  guint like_count = 0;
  g_object_get(obj, "like-count", &like_count, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_like_count(NOSTR_GTK_NOTE_CARD_ROW(row), like_count);
}

/* NIP-18: Notify handler for repost count changes */
static void on_item_notify_repost_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  guint repost_count = 0;
  g_object_get(obj, "repost-count", &repost_count, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_repost_count(NOSTR_GTK_NOTE_CARD_ROW(row), repost_count);
}

/* NIP-25: Notify handler for is_liked changes */
static void on_item_notify_is_liked(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  gboolean is_liked = FALSE;
  g_object_get(obj, "is-liked", &is_liked, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_liked(NOSTR_GTK_NOTE_CARD_ROW(row), is_liked);
}

/* NIP-57: Notify handler for zap count changes */
static void on_item_notify_zap_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  guint zap_count = 0;
  gint64 total_msat = 0;
  g_object_get(obj, "zap-count", &zap_count, "zap-total-msat", &total_msat, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_zap_stats(NOSTR_GTK_NOTE_CARD_ROW(row), zap_count, total_msat);
}

/* hq-vvmzu: Notify handler for reply count changes */
static void on_item_notify_reply_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  guint reply_count = 0;
  g_object_get(obj, "reply-count", &reply_count, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_reply_count(NOSTR_GTK_NOTE_CARD_ROW(row), reply_count);
}

/* NIP-57: Notify handler for zap total changes */
static void on_item_notify_zap_total_msat(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  guint zap_count = 0;
  gint64 total_msat = 0;
  g_object_get(obj, "zap-count", &zap_count, "zap-total-msat", &total_msat, NULL);
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    nostr_gtk_note_card_row_set_zap_stats(NOSTR_GTK_NOTE_CARD_ROW(row), zap_count, total_msat);
}

/* Unbind cleanup: disconnect signal handlers and detach inflight operations.
 *
 * nostrc-sig1: We MUST explicitly disconnect g_signal_connect_object handlers
 * here. These handlers are on the item (obj), watching the row. They only
 * auto-disconnect when the row is FINALIZED — but rows are RECYCLED (not
 * finalized) between unbind/rebind. Without explicit disconnect, handlers
 * accumulate across rebinds: after N rebinds, the old items have N sets of
 * stale handlers whose closure invalidation notifiers reference this row.
 * When those old items are finalized, the notifiers try to remove already-
 * removed handlers → invalid_closure_notify assertion → FATAL.
 *
 * GTK4 guarantees the list_item's "item" property is still valid during
 * the unbind signal (it's cleared AFTER the signal fires). */
static void factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *row = gtk_list_item_get_child(item);
  gpointer item_ptr = gtk_list_item_get_item(item);

  /* nostrc-heap-fix: Disconnect g_signal_connect_object handlers BEFORE unbind.
   * These handlers are on the obj (GnNostrEventItem) watching the row.
   * When the model removes items, the obj may be finalized while handlers
   * still reference the row, causing heap corruption in closure_array_destroy_all.
   * We must disconnect these handlers explicitly during unbind.
   * Note: item_ptr may be NULL if the model item was already removed. */
  if (item_ptr && G_IS_OBJECT(item_ptr) && row && GTK_IS_WIDGET(row)) {
    g_signal_handlers_disconnect_by_data(G_OBJECT(item_ptr), row);
  }

  /* Disconnect accumulated "request-embed" handlers on the row.
   * Plain g_signal_connect adds a new handler on every bind without disconnect. */
  if (row && GTK_IS_WIDGET(row)) {
    g_signal_handlers_disconnect_by_func(row, G_CALLBACK(on_row_request_embed), NULL);

    /* Disconnect Tier 2 map handler if still active (not yet fired) */
    gulong map_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "tv-tier2-map-id"));
    if (map_id > 0) {
      g_signal_handler_disconnect(row, map_id);
      g_object_set_data(G_OBJECT(row), "tv-tier2-map-id", GUINT_TO_POINTER(0));
    }
  }

  if (row && GTK_IS_WIDGET(row)) {
    /* Detach this row from any inflight embed fetches */
    inflight_detach_row(row);

    /* CRITICAL: Prepare the row for unbinding BEFORE GTK disposes it.
     * This cancels all async operations and sets the disposed flag to prevent
     * callbacks from corrupting Pango state during the unbind/dispose process. */
    if (NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
      nostr_gtk_note_card_row_prepare_for_unbind(NOSTR_GTK_NOTE_CARD_ROW(row));
    }
  }
}

/* Context for hashtag extraction callback */
typedef struct {
  GPtrArray *hashtags;
} HashtagExtractContext;

/* Callback for extracting hashtags */
static gboolean extract_hashtag_callback(gsize index, const gchar *tag_json, gpointer user_data) {
  (void)index;
  HashtagExtractContext *ctx = (HashtagExtractContext *)user_data;
  if (!tag_json || !ctx) return true;

  if (!gnostr_json_is_array_str(tag_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(tag_json, NULL, 0, NULL);
  if (!tag_name) {
    return true;
  }

  if (g_strcmp0(tag_name, "t") != 0) {
    free(tag_name);
    return true;
  }
  free(tag_name);

  char *hashtag = NULL;
  if ((hashtag = gnostr_json_get_array_string(tag_json, NULL, 1, NULL)) != NULL && hashtag && *hashtag) {
    g_ptr_array_add(ctx->hashtags, g_strdup(hashtag));
  }
  free(hashtag);

  return true;
}

/**
 * parse_hashtags_from_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Parses the tags array to find all "t" (hashtag) tags.
 * Format: ["t", "hashtag"]
 *
 * Returns: (transfer full) (nullable): NULL-terminated array of hashtag strings,
 *          or NULL if none found. Free with g_strfreev().
 */
static gchar **parse_hashtags_from_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;
  if (!gnostr_json_is_array_str(tags_json)) return NULL;

  HashtagExtractContext ctx = { .hashtags = g_ptr_array_new() };
  gnostr_json_array_foreach_root(tags_json, extract_hashtag_callback, &ctx);

  if (ctx.hashtags->len == 0) {
    g_ptr_array_free(ctx.hashtags, TRUE);
    return NULL;
  }

  g_ptr_array_add(ctx.hashtags, NULL);  /* NULL-terminate */
  return (gchar **)g_ptr_array_free(ctx.hashtags, FALSE);
}

/* Context for content-warning extraction callback */
typedef struct {
  gchar *reason;
} ContentWarningContext;

/* Callback for extracting content-warning tag */
static gboolean extract_content_warning_callback(gsize index, const gchar *tag_json, gpointer user_data) {
  (void)index;
  ContentWarningContext *ctx = (ContentWarningContext *)user_data;
  if (!tag_json || !ctx || ctx->reason) return true; /* Stop if already found */

  if (!gnostr_json_is_array_str(tag_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(tag_json, NULL, 0, NULL);
  if (!tag_name) {
    return true;
  }

  /* NIP-36: Look for "content-warning" tag */
  if (g_strcmp0(tag_name, "content-warning") != 0) {
    free(tag_name);
    return true;
  }
  free(tag_name);

  /* Get reason if present */
  char *reason_str = NULL;
  reason_str = gnostr_json_get_array_string(tag_json, NULL, 1, NULL);
  if (reason_str) {
    ctx->reason = g_strdup(reason_str);
    free(reason_str);
  } else {
    ctx->reason = g_strdup("");  /* Tag exists but no reason provided */
  }

  return false; /* Stop iteration - found content-warning */
}

/**
 * parse_content_warning_from_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Parses the tags array to find a "content-warning" tag (NIP-36).
 * Format: ["content-warning", "optional reason"]
 *
 * Returns: (transfer full) (nullable): The content-warning reason string,
 *          empty string if tag exists without reason, or NULL if not present.
 */
static gchar *parse_content_warning_from_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;
  if (!gnostr_json_is_array_str(tags_json)) return NULL;

  ContentWarningContext ctx = { .reason = NULL };
  gnostr_json_array_foreach_root(tags_json, extract_content_warning_callback, &ctx);

  return ctx.reason;
}

/* nostrc-lig9: NIP-65 reaction relay fetching removed.
 * Reaction counts are handled entirely by nostrdb's ndb_note_meta (O(1) lookup).
 * The relay-fetch path was O(A*R) network overhead for negligible marginal reactions. */

/* nostrc-qff: Batch metadata idle callback.
 * Fires after all factory_bind_cb calls in the current main loop iteration,
 * processes pending items' reaction/zap metadata in batch. */
/* Data passed from the NDB query worker thread back to the main thread. */
typedef struct {
  GHashTable *reaction_counts;  /* event_id -> GUINT count */
  GHashTable *user_reacted;     /* event_id -> TRUE */
  GHashTable *zap_stats;        /* event_id -> StorageNdbZapStats* */
  GHashTable *repost_counts;    /* event_id -> GUINT count */
  GHashTable *reply_counts;     /* hq-vvmzu: event_id -> GUINT count */
  GPtrArray  *items;            /* refs to pending GObjects */
} MetadataBatchResult;

static void metadata_batch_result_free(MetadataBatchResult *r) {
  if (!r) return;
  g_clear_pointer(&r->reaction_counts, g_hash_table_unref);
  g_clear_pointer(&r->user_reacted, g_hash_table_unref);
  g_clear_pointer(&r->zap_stats, g_hash_table_unref);
  g_clear_pointer(&r->repost_counts, g_hash_table_unref);
  g_clear_pointer(&r->reply_counts, g_hash_table_unref);
  g_clear_pointer(&r->items, g_ptr_array_unref);
  g_free(r);
}

/* Worker thread: runs NDB batch queries off the main thread. */
static void
metadata_batch_query_thread(GTask *task, gpointer source_object,
                            gpointer task_data G_GNUC_UNUSED,
                            GCancellable *cancellable G_GNUC_UNUSED)
{
  GPtrArray *items = g_object_get_data(G_OBJECT(task), "items");
  if (!items || items->len == 0) {
    g_task_return_pointer(task, NULL, NULL);
    return;
  }

  guint n = items->len;
  const char **event_ids = g_new0(const char *, n);
  gchar **owned_ids = g_new0(gchar *, n);
  guint id_count = 0;

  for (guint i = 0; i < n; i++) {
    GObject *obj = g_ptr_array_index(items, i);
    if (!G_IS_OBJECT(obj)) continue;
    gchar *id_hex = NULL;
    g_object_get(obj, "event-id", &id_hex, NULL);
    if (id_hex && strlen(id_hex) == 64) {
      owned_ids[id_count] = id_hex;
      event_ids[id_count] = id_hex;
      id_count++;
    } else {
      g_free(id_hex);
    }
  }

  MetadataBatchResult *result = NULL;
  if (id_count > 0) {
    result = g_new0(MetadataBatchResult, 1);
    result->reaction_counts = storage_ndb_count_reactions_batch(event_ids, id_count);

    gchar *user_pubkey = get_current_user_pubkey_hex();
    result->user_reacted = user_pubkey
      ? storage_ndb_user_has_reacted_batch(event_ids, id_count, user_pubkey)
      : NULL;
    g_free(user_pubkey);

    result->zap_stats = storage_ndb_get_zap_stats_batch(event_ids, id_count);
    result->repost_counts = storage_ndb_count_reposts_batch(event_ids, id_count);
    result->reply_counts = storage_ndb_count_replies_batch(event_ids, id_count);
    result->items = g_ptr_array_ref(items);
  }

  for (guint i = 0; i < id_count; i++)
    g_free(owned_ids[i]);
  g_free(owned_ids);
  g_free(event_ids);

  g_task_return_pointer(task, result, (GDestroyNotify)metadata_batch_result_free);
}

/* Main thread callback: applies NDB query results to GObject items. */
static void
on_metadata_batch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)user_data;
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(source);
  if (!NOSTR_GTK_IS_TIMELINE_VIEW(self)) return;

  MetadataBatchResult *r = g_task_propagate_pointer(G_TASK(res), NULL);
  if (!r || !r->items) {
    metadata_batch_result_free(r);
    return;
  }

  guint n = r->items->len;
  for (guint i = 0; i < n; i++) {
    GObject *obj = g_ptr_array_index(r->items, i);
    if (!G_IS_OBJECT(obj)) continue;

    gchar *id_hex = NULL;
    g_object_get(obj, "event-id", &id_hex, NULL);
    if (!id_hex || strlen(id_hex) != 64) { g_free(id_hex); continue; }

    if (r->reaction_counts) {
      gpointer val = g_hash_table_lookup(r->reaction_counts, id_hex);
      if (val) {
        guint count = GPOINTER_TO_UINT(val);
        if (count > 0)
          gn_nostr_event_item_set_like_count(GN_NOSTR_EVENT_ITEM(obj), count);
      }
    }

    if (r->user_reacted && g_hash_table_lookup(r->user_reacted, id_hex)) {
      gn_nostr_event_item_set_is_liked(GN_NOSTR_EVENT_ITEM(obj), TRUE);
    }

    if (r->zap_stats) {
      StorageNdbZapStats *zs = g_hash_table_lookup(r->zap_stats, id_hex);
      if (zs && zs->zap_count > 0) {
        gn_nostr_event_item_set_zap_count(GN_NOSTR_EVENT_ITEM(obj), zs->zap_count);
        gn_nostr_event_item_set_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj), zs->total_msat);
      }
    }

    if (r->repost_counts) {
      gpointer rval = g_hash_table_lookup(r->repost_counts, id_hex);
      if (rval) {
        guint rcount = GPOINTER_TO_UINT(rval);
        if (rcount > 0)
          gn_nostr_event_item_set_repost_count(GN_NOSTR_EVENT_ITEM(obj), rcount);
      }
    }

    /* hq-vvmzu: Reply counts from ndb_note_meta */
    if (r->reply_counts) {
      gpointer rpval = g_hash_table_lookup(r->reply_counts, id_hex);
      if (rpval) {
        guint rpcount = GPOINTER_TO_UINT(rpval);
        if (rpcount > 0)
          gn_nostr_event_item_set_reply_count(GN_NOSTR_EVENT_ITEM(obj), rpcount);
      }
    }

    g_free(id_hex);
  }

  g_debug("[BATCH] Completed async metadata load for %u events", n);
  metadata_batch_result_free(r);
}

/* Idle callback: collects pending items and dispatches NDB queries to worker thread.
 * The NDB queries (reactions, zaps, reposts, replies) run off the main thread to avoid
 * blocking the UI — storage_ndb_begin_query_retry can usleep up to 512ms. */
static gboolean metadata_batch_idle_cb(gpointer user_data)
{
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(user_data);
  self->metadata_batch_idle_id = 0;

  if (!self->pending_metadata_items || self->pending_metadata_items->len == 0)
    return G_SOURCE_REMOVE;

  g_debug("[BATCH] Dispatching %u metadata items to worker thread",
          self->pending_metadata_items->len);

  /* Move items to the task — the worker thread owns them now */
  GPtrArray *items = self->pending_metadata_items;
  self->pending_metadata_items = g_ptr_array_new_with_free_func(g_object_unref);

  GTask *task = g_task_new(self, NULL, on_metadata_batch_done, NULL);
  g_object_set_data_full(G_OBJECT(task), "items", items,
                         (GDestroyNotify)g_ptr_array_unref);
  g_task_run_in_thread(task, metadata_batch_query_thread);
  g_object_unref(task);

  return G_SOURCE_REMOVE;
}

/* nostrc-qff: Schedule a batch metadata load for items that need it.
 * Adds the item to the pending list and schedules an idle callback. */
static void schedule_metadata_batch(NostrGtkTimelineView *self, GObject *item)
{
  if (!self->pending_metadata_items) {
    self->pending_metadata_items = g_ptr_array_new_with_free_func(g_object_unref);
  }

  g_ptr_array_add(self->pending_metadata_items, g_object_ref(item));

  if (self->metadata_batch_idle_id == 0) {
    /* nostrc-x52i: Use _full variant with g_object_ref/unref to prevent UAF if
     * widget is destroyed while idle callback is pending. */
    self->metadata_batch_idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                    metadata_batch_idle_cb,
                                                    g_object_ref(self),
                                                    g_object_unref);
  }
}

/*
 * Tier 2 map handler for timeline view: creates embeds/media/OG when the row
 * becomes visible. During Tier 1 (bind), only Pango markup is set via
 * set_content_markup_only(). This avoids creating NoteEmbed widgets with
 * synchronous NDB queries during the factory bind callback, which was blocking
 * the main thread and causing freezes when scrolling to events with embeds.
 * One-shot: disconnects itself after first run.
 */
static void
on_tv_row_mapped_tier2(GtkWidget *widget, gpointer user_data)
{
  GtkListItem *list_item = GTK_LIST_ITEM(user_data);
  if (!GTK_IS_LIST_ITEM(list_item)) return;

  GtkWidget *row = gtk_list_item_get_child(list_item);
  if (!NOSTR_GTK_IS_NOTE_CARD_ROW(row) || row != widget) return;
  if (nostr_gtk_note_card_row_is_disposed(NOSTR_GTK_NOTE_CARD_ROW(row))) return;
  if (!nostr_gtk_note_card_row_is_bound(NOSTR_GTK_NOTE_CARD_ROW(row))) return;

  GObject *obj = gtk_list_item_get_item(list_item);
  if (!obj) return;

  extern GType gn_nostr_event_item_get_type(void);
  if (!G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) return;

  const GnContentRenderResult *cached =
      gn_nostr_event_item_get_render_result(GN_NOSTR_EVENT_ITEM(obj));
  if (cached) {
    nostr_gtk_note_card_row_apply_deferred_content(NOSTR_GTK_NOTE_CARD_ROW(row), cached);
  }

  /* One-shot: disconnect after first run */
  gulong map_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "tv-tier2-map-id"));
  if (map_id > 0) {
    g_signal_handler_disconnect(row, map_id);
    g_object_set_data(G_OBJECT(row), "tv-tier2-map-id", GUINT_TO_POINTER(0));
  }
}

static void factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f;
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(data);
  GObject *obj = gtk_list_item_get_item(item);
  
  if (!obj) {
    return;
  }
  
  gchar *display = NULL, *handle = NULL, *ts = NULL, *content = NULL, *root_id = NULL, *avatar_url = NULL;
  gchar *pubkey = NULL, *id_hex = NULL, *parent_id = NULL, *parent_pubkey = NULL, *nip05 = NULL;
  guint depth = 0; gboolean is_reply = FALSE; gint64 created_at = 0;
  
  /* Check if this is a GnNostrEventItem (new model) */
  extern GType gn_nostr_event_item_get_type(void);
  if (G_IS_OBJECT(obj) && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
    /* NEW: GnNostrEventItem binding */
    gboolean item_is_reply = FALSE;
    g_object_get(obj,
                 "event-id",      &id_hex,
                 "pubkey",        &pubkey,
                 "created-at",    &created_at,
                 "content",       &content,
                 "thread-root-id", &root_id,
                 "parent-id",     &parent_id,
                 "reply-depth",   &depth,
                 "is-reply",      &item_is_reply,
                 NULL);

    is_reply = item_is_reply || (parent_id != NULL);

    /* Get profile information */
    GObject *profile = NULL;
    g_object_get(obj, "profile", &profile, NULL);
    if (profile) {
      g_object_get(profile,
                   "display-name", &display,
                   "name",         &handle,
                   "picture-url",  &avatar_url,
                   "nip05",        &nip05,
                   NULL);
      g_object_unref(profile);
    }

    /* Format timestamp */
    if (created_at > 0) {
      time_t t = (time_t)created_at;
      struct tm *tm_info = localtime(&t);
      char buf[64];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
      ts = g_strdup(buf);
    }
    /* Debug logging removed - too verbose for per-item binding */
  }
  /* OLD: TimelineItem binding (fallback for compatibility) */
  else if (G_IS_OBJECT(obj) && G_TYPE_CHECK_INSTANCE_TYPE(obj, timeline_item_get_type())) {
    g_object_get(obj,
                 "display-name", &display,
                 "handle",       &handle,
                 "timestamp",    &ts,
                 "content",      &content,
                 "depth",        &depth,
                 "id",           &id_hex,
                 "root-id",      &root_id,
                 "created-at",   &created_at,
                 "avatar-url",   &avatar_url,
                 "pubkey",       &pubkey,
                 NULL);
    is_reply = depth > 0;
  }
  GtkWidget *row = gtk_list_item_get_child(item);
  if (!GTK_IS_WIDGET(row)) return;
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
    /* CRITICAL: Prepare row for binding - resets disposed flag and creates fresh
     * cancellable. Must be called BEFORE populating the row with data.
     * nostrc-o7pp: Matches NoteCardFactory pattern. */
    nostr_gtk_note_card_row_prepare_for_bind(NOSTR_GTK_NOTE_CARD_ROW(row));

    /* Use pubkey prefix as fallback if no profile info available */
    g_autofree gchar *display_fallback = NULL;
    if (!display && !handle && pubkey && strlen(pubkey) >= 8) {
      display_fallback = g_strdup_printf("%.8s...", pubkey);
    }

    nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row),
                                     display ? display : display_fallback,
                                     handle, avatar_url);
    nostr_gtk_note_card_row_set_timestamp(NOSTR_GTK_NOTE_CARD_ROW(row), created_at, ts);

    /* Connect embed request signal EARLY — before content setting, because
     * the Tier 2 map handler may fire immediately during bind (if row is
     * already mapped) and emit request-embed before we reach the signal
     * connection point at the end of bind.  Must be connected before
     * any code path that creates NoteEmbed widgets.
     * Disconnected in factory_unbind_cb to prevent accumulation. */
    g_signal_connect(row, "request-embed", G_CALLBACK(on_row_request_embed), NULL);

    /* NIP-92: Use imeta-aware setter if this is a GnNostrEventItem */
    const char *tags_json = NULL;
    gint event_kind = 1;  /* Default to kind 1 text note */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      tags_json = gn_nostr_event_item_get_tags_json(GN_NOSTR_EVENT_ITEM(obj));
      g_object_get(obj, "kind", &event_kind, NULL);
    }

    /* NIP-23: Handle long-form content (kind 30023) */
    if (gnostr_article_is_article(event_kind) && tags_json) {
      GnostrArticleMeta *article_meta = gnostr_article_parse_tags(tags_json);
      if (article_meta) {
        /* Use summary if available, otherwise use content excerpt */
        const char *summary = article_meta->summary;
        if (!summary && content) {
          /* Extract first ~300 chars as summary */
          summary = content;
        }

        nostr_gtk_note_card_row_set_article_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                               article_meta->title,
                                               summary,
                                               article_meta->image,
                                               article_meta->published_at > 0 ? article_meta->published_at : created_at,
                                               article_meta->d_tag,
                                               (const char * const *)article_meta->hashtags);

        gnostr_article_meta_free(article_meta);
        /* Debug logging removed - too verbose */
      } else {
        /* Fallback to regular note display if parsing fails */
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-71: Handle video events (kind 34235/34236) */
    else if (gnostr_video_is_video(event_kind) && tags_json) {
      GnostrVideoMeta *video_meta = gnostr_video_parse_tags(tags_json, event_kind);
      if (video_meta) {
        nostr_gtk_note_card_row_set_video_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                             video_meta->url,
                                             video_meta->thumb_url,
                                             video_meta->title,
                                             video_meta->summary,
                                             video_meta->duration,
                                             video_meta->orientation == GNOSTR_VIDEO_VERTICAL,
                                             video_meta->d_tag,
                                             (const char * const *)video_meta->hashtags);

        gnostr_video_meta_free(video_meta);
        /* Debug logging removed - too verbose */
      } else {
        /* Fallback to regular note display if parsing fails */
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-34: Handle git repository events (kind 30617) */
    else if (gnostr_nip34_is_repo(event_kind) && tags_json) {
      GnostrRepoMeta *repo_meta = gnostr_repo_parse_tags(tags_json);
      if (repo_meta) {
        nostr_gtk_note_card_row_set_git_repo_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                repo_meta->name,
                                                repo_meta->description,
                                                (const char *const *)repo_meta->clone_urls,
                                                (const char *const *)repo_meta->web_urls,
                                                (const char *const *)repo_meta->topics,
                                                repo_meta->maintainers_count,
                                                repo_meta->license);
        gnostr_repo_meta_free(repo_meta);
      } else {
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-34: Handle git patch events (kind 1617) */
    else if (gnostr_nip34_is_patch(event_kind) && tags_json) {
      GnostrPatchMeta *patch_meta = gnostr_patch_parse_tags(tags_json, content);
      if (patch_meta) {
        gchar *repo_name = gnostr_nip34_get_repo_identifier(patch_meta->repo_a_tag);
        nostr_gtk_note_card_row_set_git_patch_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                 patch_meta->title,
                                                 repo_name,
                                                 patch_meta->commit_id);
        g_free(repo_name);
        gnostr_patch_meta_free(patch_meta);
      } else {
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-34: Handle git issue events (kind 1621) */
    else if (gnostr_nip34_is_issue(event_kind) && tags_json) {
      GnostrIssueMeta *issue_meta = gnostr_issue_parse_tags(tags_json, content);
      if (issue_meta) {
        gchar *repo_name = gnostr_nip34_get_repo_identifier(issue_meta->repo_a_tag);
        nostr_gtk_note_card_row_set_git_issue_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                 issue_meta->title,
                                                 repo_name,
                                                 issue_meta->is_open,
                                                 (const char *const *)issue_meta->labels);
        g_free(repo_name);
        gnostr_issue_meta_free(issue_meta);
      } else {
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    } else if (tags_json) {
      nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);

      /* NIP-36: Check for content-warning tag */
      gchar *content_warning = parse_content_warning_from_tags_json(tags_json);
      if (content_warning) {
        nostr_gtk_note_card_row_set_content_warning(NOSTR_GTK_NOTE_CARD_ROW(row), content_warning);
        g_free(content_warning);
      }

      /* Extract and set hashtags from "t" tags for regular notes */
      gchar **hashtags = parse_hashtags_from_tags_json(tags_json);
      if (hashtags) {
        nostr_gtk_note_card_row_set_hashtags(NOSTR_GTK_NOTE_CARD_ROW(row), (const char * const *)hashtags);
        g_strfreev(hashtags);
      }
    } else {
      /* Tier 1: markup only — defer embeds/media/OG to Tier 2 map handler.
       * set_content_rendered and set_content create NoteEmbed widgets with
       * synchronous NDB queries during bind, blocking the main thread. */
      if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
        const GnContentRenderResult *cached = gn_nostr_event_item_get_render_result(GN_NOSTR_EVENT_ITEM(obj));
        if (cached) {
          nostr_gtk_note_card_row_set_content_markup_only(NOSTR_GTK_NOTE_CARD_ROW(row), content, cached);
        } else {
          /* No cache: fall back to full render (first bind) */
          nostr_gtk_note_card_row_set_content(NOSTR_GTK_NOTE_CARD_ROW(row), content);
        }
        /* Connect Tier 2 map handler for deferred embed/media/OG creation */
        gulong map_id = g_signal_connect(row, "map",
                                          G_CALLBACK(on_tv_row_mapped_tier2), item);
        g_object_set_data(G_OBJECT(row), "tv-tier2-map-id", GUINT_TO_POINTER(map_id));
        if (gtk_widget_get_mapped(row)) {
          on_tv_row_mapped_tier2(row, item);
        }
      } else {
        nostr_gtk_note_card_row_set_content(NOSTR_GTK_NOTE_CARD_ROW(row), content);
      }
    }

    /* Set hashtags from GnNostrEventItem if available (works even when tags_json is disabled) */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      const char * const *item_hashtags = gn_nostr_event_item_get_hashtags(GN_NOSTR_EVENT_ITEM(obj));
      if (item_hashtags && item_hashtags[0]) {
        nostr_gtk_note_card_row_set_hashtags(NOSTR_GTK_NOTE_CARD_ROW(row), item_hashtags);
      }
    }
    nostr_gtk_note_card_row_set_depth(NOSTR_GTK_NOTE_CARD_ROW(row), depth);
    nostr_gtk_note_card_row_set_ids(NOSTR_GTK_NOTE_CARD_ROW(row), id_hex, root_id, pubkey);

    /* Set NIP-10 thread info (reply indicator, view thread button) */
    nostr_gtk_note_card_row_set_thread_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                          root_id,
                                          parent_id,
                                          NULL, /* parent_author_name - will be resolved asynchronously if needed */
                                          is_reply);

    /* NIP-18: Set repost info if this is a TimelineItem with repost data */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, timeline_item_get_type())) {
      TimelineItem *ti = (TimelineItem *)obj;
      if (ti->is_repost) {
        nostr_gtk_note_card_row_set_is_repost(NOSTR_GTK_NOTE_CARD_ROW(row), TRUE);
        nostr_gtk_note_card_row_set_repost_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                              ti->reposter_pubkey,
                                              ti->reposter_display_name,
                                              ti->repost_created_at);
      }
      /* NIP-18: Set quote info if this item has a quote */
      if (ti->has_quote && ti->quoted_event_id) {
        nostr_gtk_note_card_row_set_quote_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                             ti->quoted_event_id,
                                             ti->quoted_content,
                                             ti->quoted_author);
      }
    }
    /* NIP-18: Handle GnNostrEventItem kind 6 reposts */
    else if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      gboolean is_repost = gn_nostr_event_item_get_is_repost(GN_NOSTR_EVENT_ITEM(obj));
      if (is_repost) {
        /* Get the referenced event ID from the repost's tags */
        char *reposted_id = gn_nostr_event_item_get_reposted_event_id(GN_NOSTR_EVENT_ITEM(obj));
        if (reposted_id) {
          /* Mark this as a repost and set reposter info */
          nostr_gtk_note_card_row_set_is_repost(NOSTR_GTK_NOTE_CARD_ROW(row), TRUE);
          nostr_gtk_note_card_row_set_repost_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                pubkey,   /* reposter pubkey */
                                                display ? display : (handle ? handle : NULL), /* reposter name */
                                                created_at); /* repost timestamp */

          /* Try to fetch the original note from local storage */
          char *orig_json = NULL;
          int orig_len = 0;
          if (storage_ndb_get_note_by_id_nontxn(reposted_id, &orig_json, &orig_len) == 0 && orig_json) {
            /* Parse the original event to get author and content */
            NostrEvent *orig_evt = nostr_event_new();
            if (orig_evt && nostr_event_deserialize(orig_evt, orig_json) == 0) {
              const char *orig_content = nostr_event_get_content(orig_evt);
              const char *orig_pubkey = nostr_event_get_pubkey(orig_evt);
              gint64 orig_created_at = (gint64)nostr_event_get_created_at(orig_evt);

              /* Update the card with original note's content */
              if (orig_content) {
                nostr_gtk_note_card_row_set_content(NOSTR_GTK_NOTE_CARD_ROW(row), orig_content);
              }

              /* Update timestamp to original note's time */
              if (orig_created_at > 0) {
                time_t t = (time_t)orig_created_at;
                struct tm *tm_info = localtime(&t);
                char orig_ts_buf[64];
                strftime(orig_ts_buf, sizeof(orig_ts_buf), "%Y-%m-%d %H:%M", tm_info);
                nostr_gtk_note_card_row_set_timestamp(NOSTR_GTK_NOTE_CARD_ROW(row), orig_created_at, orig_ts_buf);
              }

              /* Try to get original author's profile */
              if (orig_pubkey && strlen(orig_pubkey) == 64) {
                void *txn = NULL;
                if (storage_ndb_begin_query(&txn) == 0 && txn) {
                  unsigned char pk_bytes[32];
                  if (hex32_from_string(orig_pubkey, pk_bytes)) {
                    char *profile_json = NULL;
                    int profile_len = 0;
                    if (storage_ndb_get_profile_by_pubkey(txn, pk_bytes, &profile_json, &profile_len) == 0 && profile_json) {
                      /* Parse profile JSON to get display name */
                      if (gnostr_json_is_valid(profile_json)) {
                        /* Profile is stored as event - need to parse content */
                        char *profile_content = NULL;
                        profile_content = gnostr_json_get_string(profile_json, "content", NULL);
                        if (profile_content) {
                          if (gnostr_json_is_valid(profile_content)) {
                            char *orig_name = NULL;
                            char *orig_display = NULL;
                            char *orig_avatar = NULL;
                            char *orig_nip05_str = NULL;

                            orig_display = gnostr_json_get_string(profile_content, "display_name", NULL);
                            orig_name = gnostr_json_get_string(profile_content, "name", NULL);
                            orig_avatar = gnostr_json_get_string(profile_content, "picture", NULL);
                            orig_nip05_str = gnostr_json_get_string(profile_content, "nip05", NULL);

                            /* Update author display with original author */
                            nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                             orig_display && *orig_display ? orig_display : orig_name,
                                                             orig_name,
                                                             orig_avatar);

                            /* Update IDs to use original note's pubkey for actions */
                            nostr_gtk_note_card_row_set_ids(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                          reposted_id, root_id, (char*)orig_pubkey);

                            /* Update NIP-05 if available */
                            if (orig_nip05_str && *orig_nip05_str) {
                              nostr_gtk_note_card_row_set_nip05(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                              orig_nip05_str, orig_pubkey);
                            }

                            free(orig_name);
                            free(orig_display);
                            free(orig_avatar);
                            free(orig_nip05_str);
                          }
                          free(profile_content);
                        }
                      }
                    }
                  }
                  storage_ndb_end_query(txn);
                }
              }
            }
            if (orig_evt) nostr_event_free(orig_evt);
          } else {
            /* Original note not in local storage - request embed fetch */
            g_autofree gchar *nostr_uri = g_strdup_printf("nostr:note1%s", reposted_id);
            g_signal_emit_by_name(row, "request-embed", nostr_uri);
          }
          g_free(reposted_id);
        }
      }
    }

    /* NIP-57: Handle zap receipt events (kind 9735) */
    if (event_kind == 9735 && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      /* For zap receipts, mark them as such and set up the indicator */
      nostr_gtk_note_card_row_set_is_zap_receipt(NOSTR_GTK_NOTE_CARD_ROW(row), TRUE);

      /* Get zap stats from the event item if available */
      gint64 zap_total = gn_nostr_event_item_get_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj));

      /* Simple zap display - show "⚡ Zap" with amount if available */
      nostr_gtk_note_card_row_set_zap_receipt_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                 pubkey,    /* sender is the event pubkey for now */
                                                 display,   /* sender name */
                                                 NULL,      /* recipient - parsed from tags if needed */
                                                 NULL,      /* recipient name */
                                                 NULL,      /* target event - parsed from tags if needed */
                                                 zap_total > 0 ? zap_total : 21000); /* Default 21 sats if unknown */
    }

    /* NIP-05: Set verification identifier for async verification badge */
    if (nip05 && *nip05 && pubkey && strlen(pubkey) == 64) {
      nostr_gtk_note_card_row_set_nip05(NOSTR_GTK_NOTE_CARD_ROW(row), nip05, pubkey);
    }

    /* NIP-51: Set bookmark and pin state from local cache */
    if (id_hex && strlen(id_hex) == 64) {
      GnostrBookmarks *bookmarks = gnostr_bookmarks_get_default();
      if (bookmarks) {
        gboolean is_bookmarked = gnostr_bookmarks_is_bookmarked(bookmarks, id_hex);
        nostr_gtk_note_card_row_set_bookmarked(NOSTR_GTK_NOTE_CARD_ROW(row), is_bookmarked);
      }
      GnostrPinList *pin_list = gnostr_pin_list_get_default();
      if (pin_list) {
        gboolean is_pinned = gnostr_pin_list_is_pinned(pin_list, id_hex);
        nostr_gtk_note_card_row_set_pinned(NOSTR_GTK_NOTE_CARD_ROW(row), is_pinned);
      }
    }

    /* NIP-09: Check if this is the current user's own note (enables delete option) */
    /* Also set login state for authentication-required buttons */
    gchar *user_pubkey = get_current_user_pubkey_hex();
    gboolean is_logged_in = (user_pubkey != NULL);
    nostr_gtk_note_card_row_set_logged_in(NOSTR_GTK_NOTE_CARD_ROW(row), is_logged_in);
    if (pubkey && strlen(pubkey) == 64 && user_pubkey) {
      gboolean is_own = (g_ascii_strcasecmp(pubkey, user_pubkey) == 0);
      nostr_gtk_note_card_row_set_is_own_note(NOSTR_GTK_NOTE_CARD_ROW(row), is_own);
    } else {
      nostr_gtk_note_card_row_set_is_own_note(NOSTR_GTK_NOTE_CARD_ROW(row), FALSE);
    }

    /* nostrc-7o7: Apply no-animation class if item was added outside visible viewport */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      gboolean skip_anim = gn_nostr_event_item_get_skip_animation(GN_NOSTR_EVENT_ITEM(obj));
      if (skip_anim) {
        gtk_widget_add_css_class(row, "no-animation");
      } else {
        gtk_widget_remove_css_class(row, "no-animation");
      }

      /* NIP-25: Set reaction count and liked state from model or local storage */
      guint like_count = gn_nostr_event_item_get_like_count(GN_NOSTR_EVENT_ITEM(obj));
      gboolean is_liked = gn_nostr_event_item_get_is_liked(GN_NOSTR_EVENT_ITEM(obj));
      guint repost_count = gn_nostr_event_item_get_repost_count(GN_NOSTR_EVENT_ITEM(obj));
      guint reply_count = gn_nostr_event_item_get_reply_count(GN_NOSTR_EVENT_ITEM(obj));
      guint zap_count = gn_nostr_event_item_get_zap_count(GN_NOSTR_EVENT_ITEM(obj));
      gint64 zap_total = gn_nostr_event_item_get_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj));

      /* nostrc-nke8: Skip expensive DB lookups and network fetches for off-screen items
       * Defer if: (1) fast scrolling OR (2) item position is outside visible range */
      guint item_position = gtk_list_item_get_position(item);
      gboolean is_visible = nostr_gtk_timeline_view_is_item_visible(self, item_position);
      gboolean defer_metadata = self && (nostr_gtk_timeline_view_is_fast_scrolling(self) || !is_visible);

      /* nostrc-qff: Batch metadata loading. Instead of 3 individual DB queries
       * per item (N+1 pattern), schedule for batch processing. The idle callback
       * fires after all bind calls in this main loop iteration complete, doing
       * 3 total queries instead of 3*N. */
      if (!defer_metadata) {
        if ((like_count == 0 || zap_count == 0) && id_hex && strlen(id_hex) == 64) {
          schedule_metadata_batch(self, obj);
        }
        gtk_widget_remove_css_class(row, "needs-metadata-refresh");
      } else {
        /* Mark for deferred refresh when scroll stops or item becomes visible */
        gtk_widget_add_css_class(row, "needs-metadata-refresh");
        g_debug("[SCROLL] Deferring metadata load for item position=%u (fast=%s visible=%s)",
                item_position,
                nostr_gtk_timeline_view_is_fast_scrolling(self) ? "Y" : "N",
                is_visible ? "Y" : "N");
      }

      nostr_gtk_note_card_row_set_like_count(NOSTR_GTK_NOTE_CARD_ROW(row), like_count);
      nostr_gtk_note_card_row_set_liked(NOSTR_GTK_NOTE_CARD_ROW(row), is_liked);
      nostr_gtk_note_card_row_set_repost_count(NOSTR_GTK_NOTE_CARD_ROW(row), repost_count);
      nostr_gtk_note_card_row_set_reply_count(NOSTR_GTK_NOTE_CARD_ROW(row), reply_count);
      nostr_gtk_note_card_row_set_zap_stats(NOSTR_GTK_NOTE_CARD_ROW(row), zap_count, zap_total);
    }

    g_free(user_pubkey);

    /* Always show row - use fallback display if no profile */
    gtk_widget_set_visible(row, TRUE);

    /* Connect to profile change notification to update author when profile loads.
     * Use plain g_signal_connect (NOT g_signal_connect_object) because unbind
     * explicitly disconnects via disconnect_by_data. Combining explicit disconnect
     * with g_signal_connect_object causes double-removal of invalidation notifiers
     * → "unable to remove uninstalled invalidation notifier" → SIGABRT. */
    if (!display && !handle) {
      g_signal_connect(obj, "notify::profile",
                       G_CALLBACK(on_event_item_profile_changed), item);
    }

    /* request-embed signal connected early (before content setting) — see above. */
  }

  /* Debug logging removed - too verbose for per-item binding */
  g_free(display); g_free(handle); g_free(ts); g_free(content); g_free(root_id); g_free(id_hex);
  g_free(avatar_url); g_free(parent_id); g_free(parent_pubkey); g_free(nip05);

  /* Model-level profile gating handles profile fetching; no bind-time enqueue here. */
  g_free(pubkey);

  /* Connect reactive updates so that later metadata changes update UI.
   * CRITICAL: Use g_signal_connect_object to prevent Pango crash during recycling.
   * When using g_signal_connect, manual disconnection has a race condition:
   * profile updates can trigger label updates on widgets in transitional state,
   * causing: AddressSanitizer: SEGV in pango_layout_line_unref.
   * g_signal_connect_object auto-disconnects when row is destroyed, eliminating race. */
  if (obj && G_IS_OBJECT(obj) && GTK_IS_WIDGET(row)) {
    g_signal_connect_object(obj, "notify::display-name", G_CALLBACK(on_item_notify_display_name), row, 0);
    g_signal_connect_object(obj, "notify::handle",       G_CALLBACK(on_item_notify_handle),       row, 0);
    g_signal_connect_object(obj, "notify::avatar-url",   G_CALLBACK(on_item_notify_avatar_url),   row, 0);

    /* NIP-25: Connect reaction count/state change handlers */
    g_signal_connect_object(obj, "notify::like-count",   G_CALLBACK(on_item_notify_like_count),   row, 0);
    g_signal_connect_object(obj, "notify::is-liked",     G_CALLBACK(on_item_notify_is_liked),     row, 0);

    /* NIP-18: Connect repost count change handler */
    g_signal_connect_object(obj, "notify::repost-count", G_CALLBACK(on_item_notify_repost_count), row, 0);

    /* NIP-57: Connect zap stats change handlers */
    g_signal_connect_object(obj, "notify::zap-count",      G_CALLBACK(on_item_notify_zap_count),      row, 0);
    g_signal_connect_object(obj, "notify::zap-total-msat", G_CALLBACK(on_item_notify_zap_total_msat), row, 0);

    /* hq-vvmzu: Connect reply count change handler */
    g_signal_connect_object(obj, "notify::reply-count",    G_CALLBACK(on_item_notify_reply_count),    row, 0);
  }
}

/* nostrc-heap-fix: Teardown handler to ensure signal handlers are disconnected
 * when items are removed from the model. GTK may call teardown without unbind
 * during rapid model changes, causing heap corruption in closure_array_destroy_all. */
static void factory_teardown_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *row = gtk_list_item_get_child(item);
  gpointer item_ptr = gtk_list_item_get_item(item);

  /* Disconnect signal handlers from obj to prevent heap corruption.
   * Note: item_ptr may be NULL during teardown if the model item was already removed. */
  if (item_ptr && G_IS_OBJECT(item_ptr) && row && GTK_IS_WIDGET(row)) {
    g_signal_handlers_disconnect_by_data(G_OBJECT(item_ptr), row);
  }

  /* Prepare row for disposal */
  if (row && NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
    nostr_gtk_note_card_row_prepare_for_unbind(NOSTR_GTK_NOTE_CARD_ROW(row));
  }
}

void nostr_gtk_timeline_view_setup_app_factory(NostrGtkTimelineView *self) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), self);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), self);
  g_signal_connect(factory, "teardown", G_CALLBACK(factory_teardown_cb), self);
  nostr_gtk_timeline_view_set_factory(self, factory);
  g_object_unref(factory);

  /* Load app stylesheet for note cards */
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(prov, "/org/gnostr/ui/ui/styles/gnostr.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(prov);

  g_debug("nostr_gtk_timeline_view_setup_app_factory: list_view=%p", (void*)self->list_view);
}

/* Child model function for GtkTreeListModel (passthrough) */
static GListModel *timeline_child_model_func(gpointer item, gpointer user_data) {
  (void)user_data;
  g_debug("[TREE] Child model func called for item %p", item);
  
  if (!item || !G_TYPE_CHECK_INSTANCE_TYPE(item, timeline_item_get_type())) {
    g_debug("[TREE] Child model func: invalid item type");
    return NULL;
  }
  
  TimelineItem *timeline_item = (TimelineItem*)item;
  GListModel *children = timeline_item_get_children_model(timeline_item);
  guint child_count = children ? g_list_model_get_n_items(children) : 0;
  g_debug("[TREE] Child model func: returning %u children", child_count);
  
  return children;
}

/* ensure_list_model, scroll tracking, class_init, init, new, and all public API
 * functions are now defined in the nostr-gtk library. Only the factory setup
 * (nostr_gtk_timeline_view_setup_app_factory) remains in the app. */

