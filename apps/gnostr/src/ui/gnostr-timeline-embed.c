/**
 * gnostr-timeline-embed.c — Embed resolution subsystem
 *
 * Extracted from gnostr-timeline-view.c. Handles resolving bech32 references
 * (note1, nevent, naddr, nprofile) embedded in note content. Provides:
 *   - Inflight request deduplication and lifecycle management
 *   - TTL embed result cache with size limits
 *   - Relay URL construction with hint preference
 *   - Async multi-row completion callback
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-timeline-embed-private.h"
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr-filter.h"
#include <string.h>
#include <time.h>
#include <errno.h>

/* Cache size limits to prevent unbounded memory growth */
#define EMBED_CACHE_MAX 500
#define INFLIGHT_MAX 100

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
char *gnostr_timeline_embed_get_current_user_pubkey_hex(void) {
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (!settings) return NULL;
  char *npub = g_settings_get_string(settings, "current-npub");
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
gboolean gnostr_timeline_embed_hex32_from_string(const char *hex, unsigned char out[32]) {
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
void gnostr_timeline_embed_inflight_detach_row(GtkWidget *row) {
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

void gnostr_timeline_embed_on_row_request_embed(NostrGtkNoteCardRow *row, const char *target, gpointer user_data) {
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
    if (!gnostr_timeline_embed_hex32_from_string(event_id_hex, id32)) { nostr_gtk_note_card_row_set_embed(row, "Reference", ref); return; }
    /* Query local store */
    void *txn = NULL; if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
      char *json = NULL; int jlen = 0;
      if (storage_ndb_get_note_by_id(txn, id32, &json, &jlen, NULL) == 0 && json) {
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
    if (gnostr_timeline_embed_hex32_from_string(event_id_hex, id32)) {
      void *txn = NULL; if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
        char *json = NULL; int jlen = 0;
        if (storage_ndb_get_note_by_id(txn, id32, &json, &jlen, NULL) == 0 && json) {
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
