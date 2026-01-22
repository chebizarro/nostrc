#include "gnostr-timeline-view.h"
#include "gnostr-main-window.h"
#include "note_card_row.h"
#include "../storage_ndb.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr/nip19/nip19.h"
#include "nostr_simple_pool.h"
#include "../util/relays.h"
#include "../util/utils.h"
#include "nostr-filter.h"
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/gnostr-timeline-view.ui"

/* Cache size limits to prevent unbounded memory growth */
#define EMBED_CACHE_MAX 500
#define INFLIGHT_MAX 100

/* Forward decl: NoteCardRow embed handler */
static void on_row_request_embed(GnostrNoteCardRow *row, const char *target, gpointer user_data);

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
  /* Add config relays */
  GPtrArray *cfg = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(cfg);
  for (guint i = 0; i < cfg->len; i++) {
    const char *u = (const char*)g_ptr_array_index(cfg, i);
    if (!u || !*u) continue;
    if (g_hash_table_contains(set, u)) continue;
    g_hash_table_add(set, (gpointer)u);
    g_ptr_array_add(arr, g_strdup(u));
  }
  g_ptr_array_free(cfg, TRUE);
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
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &err);
  ensure_inflight();
  Inflight *in = ctx && ctx->key ? (Inflight*)g_hash_table_lookup(s_inflight, ctx->key) : NULL;
  /* Prepare parsed view if we got a result */
  char meta[128] = {0}; char snipbuf[281] = {0}; const char *title = "Note"; gboolean have = FALSE;
  if (results && results->len > 0) {
    const char *json = (const char*)g_ptr_array_index(results, 0);
    if (json) {
      /* cache positive */
      if (ctx && ctx->key) embed_cache_put_json(ctx->key, json);
      storage_ndb_ingest_event_json(json, NULL);
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
      GnostrNoteCardRow *r = rr ? GNOSTR_NOTE_CARD_ROW(g_weak_ref_get(&rr->ref)) : NULL;
      if (!GNOSTR_IS_NOTE_CARD_ROW(r)) { if (r) g_object_unref(r); continue; }
      if (have) gnostr_note_card_row_set_embed_rich(r, title, meta, snipbuf);
      else gnostr_note_card_row_set_embed(r, "Note", "Not found on selected relays");
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
      GnostrNoteCardRow *r = rr ? GNOSTR_NOTE_CARD_ROW(g_weak_ref_get(&rr->ref)) : NULL;
      gboolean keep = TRUE;
      if (!GNOSTR_IS_NOTE_CARD_ROW(r) || GTK_WIDGET(r) == row) keep = FALSE;
      if (r) g_object_unref(r);
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
static char *build_key_for_naddr(const NostrEntityPointer *a) { return g_strdup_printf("a:%d:%s:%s", a ? a->kind : 0, a && a->public_key ? a->public_key : "", a && a->identifier ? a->identifier : ""); }

/* Start or attach to an inflight request */
static void start_or_attach_request(const char *key, const char **urls, size_t url_count, NostrFilter *f, GnostrNoteCardRow *row) {
  ensure_inflight();
  Inflight *in = (Inflight*)g_hash_table_lookup(s_inflight, key);
  if (!in) {
    in = g_new0(Inflight, 1);
    in->canc = g_cancellable_new();
    in->rows = g_ptr_array_new_with_free_func(rowref_free);
    g_hash_table_insert(s_inflight, g_strdup(key), in);
    /* Start the network request */
    static GnostrSimplePool *s_pool = NULL; if (!s_pool) s_pool = gnostr_simple_pool_new();
    InflightCtx *ctx = g_new0(InflightCtx, 1); ctx->key = g_strdup(key);
    gnostr_simple_pool_query_single_async(s_pool, urls, url_count, f, in->canc, on_query_single_done_multi, ctx);
  }
  /* Attach row */
  RowRef *rr = g_new0(RowRef, 1); g_weak_ref_init(&rr->ref, row);
  g_ptr_array_add(in->rows, rr);
}

static void on_row_request_embed(GnostrNoteCardRow *row, const char *target, gpointer user_data) {
  (void)user_data;
  if (!GNOSTR_IS_NOTE_CARD_ROW(row) || !target || !*target) return;
  /* Normalize nostr: URIs */
  const char *ref = target;
  if (g_str_has_prefix(ref, "nostr:")) ref = target + 6;

  /* Parse bech32 pointer */
  NostrPointer *ptr = NULL;
  if (nostr_pointer_parse(ref, &ptr) != 0 || !ptr) {
    /* Maybe it's a bare note1 (bech32) without tlv pointer */
    unsigned char id32[32];
    if (nostr_nip19_decode_note(ref, id32) == 0) {
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
              gnostr_note_card_row_set_embed(row, title, content);
            } else {
              gnostr_note_card_row_set_embed(row, "Note", "(empty)");
            }
          }
          if (evt) nostr_event_free(evt);
          g_free(json);
        } else {
          gnostr_note_card_row_set_embed(row, "Note", "Not found in local cache (fetching…)");
        }
        storage_ndb_end_query(txn);
      }
      /* Kick off relay fetch for this note id (dedup + cancellable) */
      {
        /* Build filter by id */
        NostrFilter *f = nostr_filter_new();
        char idhex[65];
        for (int i=0;i<32;i++) sprintf(&idhex[i*2], "%02x", id32[i]);
        idhex[64] = '\0';
        const char *ids[1] = { idhex };
        nostr_filter_set_ids(f, ids, 1);
        /* Cache pre-check */
        g_autofree char *key = build_key_for_note_hex(idhex);
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
              gnostr_note_card_row_set_embed_rich(row, "Note", meta, snipbuf);
            }
            if (evt) nostr_event_free(evt);
            nostr_filter_free(f);
            goto done_note_fetch;
          } else {
            gnostr_note_card_row_set_embed(row, "Note", "Not found on selected relays");
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
    } else {
      gnostr_note_card_row_set_embed(row, "Reference", ref);
    }
    return;
  }

  /* Pointer OK: handle nevent/naddr */
  if (ptr->kind == NOSTR_PTR_NEVENT && ptr->u.nevent && ptr->u.nevent->id) {
    unsigned char id32[32];
    if (hex32_from_string(ptr->u.nevent->id, id32)) {
      void *txn = NULL; if (storage_ndb_begin_query(&txn) == 0 && txn) {
        char *json = NULL; int jlen = 0;
        if (storage_ndb_get_note_by_id(txn, id32, &json, &jlen) == 0 && json) {
          NostrEvent *evt = nostr_event_new();
          if (evt && nostr_event_deserialize(evt, json) == 0) {
            const char *content = nostr_event_get_content(evt);
            if (content && *content) {
              char title[64]; g_strlcpy(title, content, sizeof(title));
              gnostr_note_card_row_set_embed(row, title, content);
            } else {
              gnostr_note_card_row_set_embed(row, "Note", "(empty)");
            }
          }
          if (evt) nostr_event_free(evt);
          g_free(json);
        } else {
          gnostr_note_card_row_set_embed(row, "Note", "Not found in local cache (fetching…)");
        }
        storage_ndb_end_query(txn);
      }
      /* Kick off relay fetch using id hex from pointer (dedup + cancellable) */
      {
        NostrFilter *f = nostr_filter_new();
        const char *ids[1] = { ptr->u.nevent->id };
        nostr_filter_set_ids(f, ids, 1);
        /* Cache pre-check */
        g_autofree char *key = build_key_for_note_hex(ptr->u.nevent->id);
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
              gnostr_note_card_row_set_embed_rich(row, "Note", meta, snipbuf);
            }
            if (evt) nostr_event_free(evt);
            nostr_filter_free(f);
            goto done_nevent_fetch;
          } else {
            gnostr_note_card_row_set_embed(row, "Note", "Not found on selected relays");
            nostr_filter_free(f);
            goto done_nevent_fetch;
          }
        }
        char **urls = NULL; size_t url_count = 0;
        /* Prefer relay hints from pointer */
        build_urls_with_hints_owned((const char* const*)ptr->u.nevent->relays, ptr->u.nevent->relays_count, &urls, &url_count);
        if (url_count > 0 && urls) {
          start_or_attach_request(key, (const char**)urls, url_count, f, row);
        }
        free_urls_owned(urls, url_count);
        nostr_filter_free(f);
done_nevent_fetch:
        ;
      }
    } else {
      gnostr_note_card_row_set_embed(row, "Reference", ptr->u.nevent->id);
    }
  } else if (ptr->kind == NOSTR_PTR_NADDR) {
    /* Addressable entity: build filter (kind+author+tag d) and fetch */
    gnostr_note_card_row_set_embed(row, "Addressable entity", ref);
    NostrEntityPointer *a = ptr->u.naddr;
    if (a && a->identifier && a->public_key && a->kind > 0) {
      NostrFilter *f = nostr_filter_new();
      int kinds[1] = { a->kind }; nostr_filter_set_kinds(f, kinds, 1);
      const char *authors[1] = { a->public_key }; nostr_filter_set_authors(f, authors, 1);
      /* tag d */
      nostr_filter_tags_append(f, "d", a->identifier, NULL);
      /* Cache pre-check */
      {
        g_autofree char *cache_key = build_key_for_naddr(a);
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
              gnostr_note_card_row_set_embed_rich(row, title, meta, snipbuf);
            }
            if (evt) nostr_event_free(evt);
            nostr_filter_free(f);
            goto done_naddr_fetch;
          } else {
            gnostr_note_card_row_set_embed(row, "Note", "Not found on selected relays");
            nostr_filter_free(f);
            goto done_naddr_fetch;
          }
        }
      }
      char **urls = NULL; size_t url_count = 0;
      build_urls_with_hints_owned((const char* const*)a->relays, a->relays_count, &urls, &url_count);
      if (url_count > 0 && urls) {
        g_autofree char *key = build_key_for_naddr(a);
        start_or_attach_request(key, (const char**)urls, url_count, f, row);
      }
      free_urls_owned(urls, url_count);
      nostr_filter_free(f);
done_naddr_fetch:
      ;
    }
  } else if (ptr->kind == NOSTR_PTR_NPROFILE) {
    gnostr_note_card_row_set_embed(row, "Profile", ref);
  } else {
    gnostr_note_card_row_set_embed(row, "Reference", ref);
  }
  nostr_pointer_free(ptr);
}

/* gnostr_avatar_metrics_log() now comes from avatar_cache. */

/* Avatar cache helpers are implemented in avatar_cache.c */

/* Item representing a post row, optionally with children for threading. */
typedef struct _TimelineItem {
  GObject parent_instance;
  gchar *display_name;
  gchar *handle;
  gchar *timestamp;
  gchar *content;
  guint depth;
  /* metadata for threading */
  gchar *id;
  gchar *root_id;
  gchar *pubkey;
  gint64 created_at;
  /* avatar */
  gchar *avatar_url;
  /* visibility control */
  gboolean visible;
  /* children list when acting as a parent in a thread */
  GListStore *children; /* element-type: TimelineItem */
} TimelineItem;

typedef struct _TimelineItemClass {
  GObjectClass parent_class;
} TimelineItemClass;

G_DEFINE_TYPE(TimelineItem, timeline_item, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_DISPLAY_NAME,
  PROP_HANDLE,
  PROP_TIMESTAMP,
  PROP_CONTENT,
  PROP_DEPTH,
  PROP_ID,
  PROP_ROOT_ID,
  PROP_PUBKEY,
  PROP_CREATED_AT,
  PROP_AVATAR_URL,
  PROP_VISIBLE,
  N_PROPS
};

static GParamSpec *ti_props[N_PROPS];

static void timeline_item_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec) {
  TimelineItem *self = (TimelineItem*)obj;
  switch (prop_id) {
    case PROP_DISPLAY_NAME: g_free(self->display_name); self->display_name = g_value_dup_string(value); break;
    case PROP_HANDLE:       g_free(self->handle);       self->handle       = g_value_dup_string(value); break;
    case PROP_TIMESTAMP:    g_free(self->timestamp);    self->timestamp    = g_value_dup_string(value); break;
    case PROP_CONTENT:      g_free(self->content);      self->content      = g_value_dup_string(value); break;
    case PROP_DEPTH:        self->depth = g_value_get_uint(value); break;
    case PROP_ID:           g_free(self->id);           self->id           = g_value_dup_string(value); break;
    case PROP_ROOT_ID:      g_free(self->root_id);      self->root_id      = g_value_dup_string(value); break;
    case PROP_PUBKEY:       g_free(self->pubkey);       self->pubkey       = g_value_dup_string(value); break;
    case PROP_CREATED_AT:   self->created_at            = g_value_get_int64(value); break;
    case PROP_AVATAR_URL:   g_free(self->avatar_url);   self->avatar_url   = g_value_dup_string(value); break;
    case PROP_VISIBLE:     self->visible = g_value_get_boolean(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }

}

/* Forward declaration only if needed elsewhere before definition */
/* void gnostr_avatar_prefetch(const char *url); */

static void timeline_item_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec) {
  TimelineItem *self = (TimelineItem*)obj;
  switch (prop_id) {
    case PROP_DISPLAY_NAME: g_value_set_string(value, self->display_name); break;
    case PROP_HANDLE:       g_value_set_string(value, self->handle); break;
    case PROP_TIMESTAMP:    g_value_set_string(value, self->timestamp); break;
    case PROP_CONTENT:      g_value_set_string(value, self->content); break;
    case PROP_DEPTH:        g_value_set_uint(value, self->depth); break;
    case PROP_ID:           g_value_set_string(value, self->id); break;
    case PROP_ROOT_ID:      g_value_set_string(value, self->root_id); break;
    case PROP_PUBKEY:       g_value_set_string(value, self->pubkey); break;
    case PROP_CREATED_AT:   g_value_set_int64 (value, self->created_at); break;
    case PROP_AVATAR_URL:   g_value_set_string(value, self->avatar_url); break;
    case PROP_VISIBLE:     g_value_set_boolean(value, self->visible); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
  }
}

static void timeline_item_dispose(GObject *obj) {
  TimelineItem *self = (TimelineItem*)obj;
  g_debug("🔴 DISPOSE TimelineItem %p (pubkey=%.8s)", (void*)obj, self->pubkey ? self->pubkey : "null");
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->handle, g_free);
  g_clear_pointer(&self->timestamp, g_free);
  g_clear_pointer(&self->content, g_free);
  g_clear_pointer(&self->id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->pubkey, g_free);
  g_clear_pointer(&self->avatar_url, g_free);
  if (self->children) g_clear_object(&self->children);
  G_OBJECT_CLASS(timeline_item_parent_class)->dispose(obj);
  g_debug("🔴 DISPOSE TimelineItem %p COMPLETE", (void*)obj);
}

static void timeline_item_class_init(TimelineItemClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->set_property = timeline_item_set_property;
  oc->get_property = timeline_item_get_property;
  oc->dispose = timeline_item_dispose;
  ti_props[PROP_DISPLAY_NAME] = g_param_spec_string("display-name", "display-name", "Display Name", NULL, G_PARAM_READWRITE);
  ti_props[PROP_HANDLE]       = g_param_spec_string("handle",       "handle",       "Handle",       NULL, G_PARAM_READWRITE);
  ti_props[PROP_TIMESTAMP]    = g_param_spec_string("timestamp",    "timestamp",    "Timestamp",    NULL, G_PARAM_READWRITE);
  ti_props[PROP_CONTENT]      = g_param_spec_string("content",      "content",      "Content",      NULL, G_PARAM_READWRITE);
  ti_props[PROP_DEPTH]        = g_param_spec_uint   ("depth",        "depth",        "Depth",        0, 32, 0, G_PARAM_READWRITE);
  ti_props[PROP_ID]           = g_param_spec_string ("id",           "id",           "Event Id",     NULL, G_PARAM_READWRITE);
  ti_props[PROP_ROOT_ID]      = g_param_spec_string ("root-id",      "root-id",      "Root Event Id",NULL, G_PARAM_READWRITE);
  ti_props[PROP_PUBKEY]       = g_param_spec_string ("pubkey",       "pubkey",       "Pubkey",       NULL, G_PARAM_READWRITE);
  ti_props[PROP_CREATED_AT]   = g_param_spec_int64  ("created-at",   "created-at",   "Created At",   0, G_MAXINT64, 0, G_PARAM_READWRITE);
  ti_props[PROP_AVATAR_URL]   = g_param_spec_string("avatar-url",   "avatar-url",   "Avatar URL",    NULL, G_PARAM_READWRITE);
  ti_props[PROP_VISIBLE]     = g_param_spec_boolean("visible",     "visible",     "Visible",      TRUE, G_PARAM_READWRITE);
  g_object_class_install_properties(oc, N_PROPS, ti_props);
}

static void timeline_item_init(TimelineItem *self) { (void)self; }

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

static GListModel *timeline_item_get_children_model(TimelineItem *it) {
  return it ? G_LIST_MODEL(it->children) : NULL;
}

static void timeline_item_add_child(TimelineItem *parent, TimelineItem *child) {
  if (!parent || !child) return;
  g_list_store_append(parent->children, child);
}

/* Public wrappers for building trees from outside */
void gnostr_timeline_item_add_child(TimelineItem *parent, TimelineItem *child) {
  timeline_item_add_child(parent, child);
}

GListModel *gnostr_timeline_item_get_children(TimelineItem *item) {
  return timeline_item_get_children_model(item);
}

struct _GnostrTimelineView {
  GtkWidget parent_instance;
  GtkWidget *root_scroller;
  GtkWidget *list_view;
  GtkSelectionModel *selection_model; /* owned */
  GListStore *list_model;             /* owned: TimelineItem (flat) or tree roots when tree is active */
  GtkTreeListModel *tree_model;       /* owned when tree roots set */
  GListStore *flattened_model;        /* owned: flattened timeline with threading */
};

G_DEFINE_TYPE(GnostrTimelineView, gnostr_timeline_view, GTK_TYPE_WIDGET)

static void gnostr_timeline_view_dispose(GObject *obj) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(obj);
  g_warning("🔥🔥🔥 TIMELINE_VIEW DISPOSE STARTING: list_view=%p list_model=%p tree_model=%p", 
            (void*)self->list_view, (void*)self->list_model, (void*)self->tree_model);
  /* CRITICAL: Clear models in correct order to avoid GTK trying to disconnect
   * signals from already-freed TimelineItem objects.
   * Order: detach from view → clear selection → clear tree → clear list */
  if (self->list_view && GTK_IS_LIST_VIEW(self->list_view)) {
    g_warning("🔥 Detaching model from list view");
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), NULL);
  }
  
  /* Clear selection model FIRST - this drops the ref to tree model */
  if (self->selection_model) {
    g_warning("🔥 Clearing selection model");
    g_clear_object(&self->selection_model);
  }
  
  /* Then clear tree model - this should be the last ref */
  if (self->tree_model) {
    g_warning("🔥 Clearing tree model");
    g_clear_object(&self->tree_model);
  }
  
  /* Clear flattened model */
  if (self->flattened_model) {
    g_warning("🔥 Clearing flattened model");
    g_clear_object(&self->flattened_model);
  }
  
  if (self->list_model) {
    g_warning("🔥 Clearing list model");
    g_clear_object(&self->list_model);
  }
  /* Dispose template children before chaining up so they are unparented first */
  gtk_widget_dispose_template(GTK_WIDGET(obj), GNOSTR_TYPE_TIMELINE_VIEW);
  self->root_scroller = NULL;
  self->list_view = NULL;
  G_OBJECT_CLASS(gnostr_timeline_view_parent_class)->dispose(obj);
}

static void gnostr_timeline_view_finalize(GObject *obj) {
  G_OBJECT_CLASS(gnostr_timeline_view_parent_class)->finalize(obj);
}

/* Setup: load row UI from resource and set as child. Cache subwidgets on the row. */
static void on_note_card_open_profile_relay(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_reply_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *root_id, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_repost_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_quote_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_like_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
  /* Relay the signal up to the main window */
  GtkWidget *widget = GTK_WIDGET(row);
  while (widget) {
    widget = gtk_widget_get_parent(widget);
    if (widget && G_TYPE_CHECK_INSTANCE_TYPE(widget, gtk_application_window_get_type())) {
      /* Found the main window - like not yet implemented, just log */
      g_message("[TIMELINE] Like requested for id=%s (not yet implemented)", id_hex ? id_hex : "(null)");
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
  
  /* Check if profile is now available */
  GObject *profile = NULL;
  g_object_get(event_item, "profile", &profile, NULL);
  
  if (profile) {
    /* Profile loaded - show the row and update author info */
    gchar *display = NULL, *handle = NULL, *avatar_url = NULL;
    g_object_get(profile,
                 "display-name", &display,
                 "name",         &handle,
                 "picture-url",  &avatar_url,
                 NULL);
    
    if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
      gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, avatar_url);
      gtk_widget_set_visible(row, TRUE);
      
      gchar *event_id = NULL;
      g_object_get(event_item, "event-id", &event_id, NULL);
      g_debug("[PROFILE] Profile loaded for event %s - showing row", event_id ? event_id : "(null)");
      g_free(event_id);
    }
    
    g_free(display);
    g_free(handle);
    g_free(avatar_url);
    g_object_unref(profile);
  }
}

static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *row = GTK_WIDGET(gnostr_note_card_row_new());
  g_debug("factory_setup_cb: created note-card row=%p for item=%p", (void*)row, (void*)item);

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

  g_debug("avatar set: row=%p url=%s display=%.30s handle=%.30s", 
          (void*)row, avatar_url ? avatar_url : "(null)", 
          display ? display : "", handle ? handle : "");
  
  if (!avatar_url || !*avatar_url || !str_has_prefix_http(avatar_url) || !GTK_IS_PICTURE(w_img)) {
    g_debug("avatar set: invalid or no image widget, showing initials (url=%s, is_picture=%d)", 
            avatar_url ? avatar_url : "(null)", GTK_IS_PICTURE(w_img));
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
    g_debug("avatar set: cache hit url=%s", avatar_url);
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
  /* CRITICAL: Validate obj before accessing properties */
  if (!obj || !G_IS_OBJECT(obj)) {
    g_debug("on_item_notify_display_name: obj is invalid, ignoring");
    return;
  }
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL, *pubkey = NULL;
  g_object_get(obj, "display-name", &display, "handle", &handle, "avatar-url", &avatar_url, "pubkey", &pubkey, NULL);
  g_debug("🔔 on_item_notify_display_name: pubkey=%.*s display='%s' handle='%s' avatar='%s'", 
            pubkey ? 8 : 0, pubkey ? pubkey : "", 
            display ? display : "(null)", 
            handle ? handle : "(null)", 
            avatar_url ? avatar_url : "(null)");
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url); g_free(pubkey);
}

static void on_item_notify_handle(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  /* CRITICAL: Validate obj before accessing properties */
  if (!obj || !G_IS_OBJECT(obj)) {
    g_debug("on_item_notify_handle: obj is invalid, ignoring");
    return;
  }
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL, *pubkey = NULL;
  g_object_get(obj, "display-name", &display, "handle", &handle, "avatar-url", &avatar_url, "pubkey", &pubkey, NULL);
  g_debug("🔔 on_item_notify_handle: pubkey=%.*s display='%s' handle='%s' avatar='%s'", 
            pubkey ? 8 : 0, pubkey ? pubkey : "", 
            display ? display : "(null)", 
            handle ? handle : "(null)", 
            avatar_url ? avatar_url : "(null)");
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url); g_free(pubkey);
}

static void on_item_notify_avatar_url(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  /* CRITICAL: Validate obj before accessing properties */
  if (!obj || !G_IS_OBJECT(obj)) {
    g_debug("on_item_notify_avatar_url: obj is invalid, ignoring");
    return;
  }
  gchar *url = NULL, *display = NULL, *handle = NULL, *pubkey = NULL;
  g_object_get(obj, "avatar-url", &url, "display-name", &display, "handle", &handle, "pubkey", &pubkey, NULL);
  g_debug("🔔 on_item_notify_avatar_url: pubkey=%.*s display='%s' handle='%s' avatar='%s'", 
            pubkey ? 8 : 0, pubkey ? pubkey : "", 
            display ? display : "(null)", 
            handle ? handle : "(null)", 
            url ? url : "(null)");
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, url);
  g_free(url); g_free(display); g_free(handle);
}

/* Unbind cleanup: disconnect any notify handlers tied to this row */
static void factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GObject *obj = gtk_list_item_get_item(item);
  GtkWidget *row = gtk_list_item_get_child(item);
  
  /* CRITICAL: Validate objects before disconnecting signals
   * During scrolling, GTK may unbind items that are being recycled.
   * If obj is NULL or invalid, we can't safely disconnect signals. */
  if (!obj) {
    g_warning("factory_unbind: obj is NULL (item=%p row=%p)", (void*)item, (void*)row);
    /* Still try to clean up row if it's valid */
    if (GTK_IS_WIDGET(row)) {
      inflight_detach_row(row);
    }
    return;
  }
  
  if (!G_IS_OBJECT(obj)) {
    /* DON'T try to access obj here - it's invalid/freed memory!
     * Accessing G_OBJECT_TYPE_NAME(obj) would cause use-after-free */
    g_warning("factory_unbind: obj is not a valid GObject (obj=%p) - likely freed", (void*)obj);
    /* Still try to clean up row if it's valid */
    if (GTK_IS_WIDGET(row)) {
      inflight_detach_row(row);
    }
    return;
  }
  
  /* CRITICAL: Also check if it's actually a TimelineItem, not just any GObject
   * This catches cases where the memory has been reused for a different object type */
  if (!G_TYPE_CHECK_INSTANCE_TYPE(obj, timeline_item_get_type())) {
    g_warning("factory_unbind: obj is not a TimelineItem (obj=%p) - likely freed and reused", (void*)obj);
    /* Still try to clean up row if it's valid */
    if (GTK_IS_WIDGET(row)) {
      inflight_detach_row(row);
    }
    return;
  }
  
  if (!GTK_IS_WIDGET(row)) {
    g_debug("factory_unbind: row is not a valid widget");
    return;
  }
  
  g_debug("factory_unbind: cleaning up (obj=%p row=%p)", (void*)obj, (void*)row);
  /* NOTE: We no longer manually disconnect ANY signals here because:
   * 1. notify signals use g_signal_connect_object() - auto-disconnect when row destroyed
   * 2. request-embed signal is on row itself - auto-disconnect when row destroyed
   * This avoids race conditions where objects might be freed during disconnect. */
  
  /* Detach this row from any inflight embed fetches */
  inflight_detach_row(row);
}

static void factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GObject *obj = gtk_list_item_get_item(item);
  
  if (!obj) {
    return;
  }
  
  gchar *display = NULL, *handle = NULL, *ts = NULL, *content = NULL, *root_id = NULL, *avatar_url = NULL;
  gchar *pubkey = NULL, *id_hex = NULL, *parent_id = NULL;
  guint depth = 0; gboolean is_reply = FALSE; gint64 created_at = 0;
  
  /* Check if this is a GnNostrEventItem (new model) */
  extern GType gn_nostr_event_item_get_type(void);
  if (G_IS_OBJECT(obj) && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
    /* NEW: GnNostrEventItem binding */
    g_object_get(obj,
                 "event-id",      &id_hex,
                 "pubkey",        &pubkey,
                 "created-at",    &created_at,
                 "content",       &content,
                 "thread-root-id", &root_id,
                 "parent-id",     &parent_id,
                 "reply-depth",   &depth,
                 NULL);
    
    /* Get profile information */
    GObject *profile = NULL;
    g_object_get(obj, "profile", &profile, NULL);
    if (profile) {
      g_object_get(profile,
                   "display-name", &display,
                   "name",         &handle,
                   "picture-url",  &avatar_url,
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
    
    is_reply = depth > 0;
    g_debug("[BIND] GnNostrEventItem: id=%s depth=%u profile=%s", 
            id_hex ? id_hex : "(null)", depth, display ? display : "(none)");
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
  if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
    /* Use pubkey prefix as fallback if no profile info available */
    gchar *display_fallback = NULL;
    if (!display && !handle && pubkey && strlen(pubkey) >= 8) {
      display_fallback = g_strdup_printf("%.8s...", pubkey);
    }

    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row),
                                     display ? display : display_fallback,
                                     handle, avatar_url);
    gnostr_note_card_row_set_timestamp(GNOSTR_NOTE_CARD_ROW(row), created_at, ts);
    gnostr_note_card_row_set_content(GNOSTR_NOTE_CARD_ROW(row), content);
    gnostr_note_card_row_set_depth(GNOSTR_NOTE_CARD_ROW(row), depth);
    gnostr_note_card_row_set_ids(GNOSTR_NOTE_CARD_ROW(row), id_hex, root_id, pubkey);

    /* Always show row - use fallback display if no profile */
    gtk_widget_set_visible(row, TRUE);

    /* Connect to profile change notification to update author when profile loads */
    if (!display && !handle) {
      g_debug("[BIND] Using pubkey fallback for event %s", id_hex ? id_hex : "(null)");
      g_signal_connect_object(obj, "notify::profile",
                              G_CALLBACK(on_event_item_profile_changed),
                              item, 0);
    }

    g_free(display_fallback);
    
    /* Connect embed request signal
     * NOTE: We don't need to disconnect first - g_signal_connect_object handles duplicates
     * and auto-disconnects when row is destroyed */
    g_signal_connect(row, "request-embed", G_CALLBACK(on_row_request_embed), NULL);
  }

  g_debug("factory bind: item=%p content=%.60s depth=%u is_reply=%s id=%s root_id=%s", 
           (void*)item, content ? content : "", depth, 
           is_reply ? "TRUE" : "FALSE", 
           id_hex ? id_hex : "(null)",
           root_id ? root_id : "(null)");
  g_free(display); g_free(handle); g_free(ts); g_free(content); g_free(root_id); g_free(id_hex);
  g_free(avatar_url); /* Fix memory leak */

  /* Model-level profile gating handles profile fetching; no bind-time enqueue here. */
  g_free(pubkey);

  /* Connect reactive updates so that later metadata changes update UI
   * CRITICAL: Use g_signal_connect_object with row as the object parameter.
   * This makes the signals automatically disconnect when row is destroyed,
   * avoiding the need to manually disconnect in unbind (which causes race conditions). */
  if (obj && G_IS_OBJECT(obj) && GTK_IS_WIDGET(row)) {
    g_signal_connect_object(obj, "notify::display-name", G_CALLBACK(on_item_notify_display_name), row, 0);
    g_signal_connect_object(obj, "notify::handle",       G_CALLBACK(on_item_notify_handle),       row, 0);
    g_signal_connect_object(obj, "notify::avatar-url",   G_CALLBACK(on_item_notify_avatar_url),   row, 0);
  }
}

static void setup_default_factory(GnostrTimelineView *self) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), NULL);
  gtk_list_view_set_factory(GTK_LIST_VIEW(self->list_view), factory);
  g_object_unref(factory);
  g_debug("setup_default_factory: list_view=%p", (void*)self->list_view);
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

static void ensure_list_model(GnostrTimelineView *self) {
  if (self->list_model) return;
  self->list_model = g_list_store_new(timeline_item_get_type());
  self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->list_model)));
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
  g_debug("ensure_list_model: list_model=%p selection_model=%p", (void*)self->list_model, (void*)self->selection_model);
}

static void gnostr_timeline_view_class_init(GnostrTimelineViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_timeline_view_dispose;
  gobj_class->finalize = gnostr_timeline_view_finalize;
  /* Ensure this widget can have children in templates by declaring a layout manager type */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, root_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, list_view);
}

static void gnostr_timeline_view_init(GnostrTimelineView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->list_view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Timeline List", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->root_scroller),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Timeline Scroll", -1);
  /* Child widgets already have hexpand/vexpand in template */
  g_debug("timeline_view init: self=%p root_scroller=%p list_view=%p", (void*)self, (void*)self->root_scroller, (void*)self->list_view);
  setup_default_factory(self);

  /* Install minimal CSS for thread indicator and avatar */
  static const char *css =
    ".avatar { border-radius: 18px; background: @theme_bg_color; padding: 2px; }\n"
    ".dim-label { opacity: 0.7; }\n"
    ".thread-reply { background: alpha(@theme_bg_color, 0.5); border-left: 3px solid @theme_selected_bg_color; }\n"
    ".thread-root { }\n"
    ".thread-indicator { min-width: 4px; min-height: 4px; background: @theme_selected_bg_color; }\n"
    "note-card { border-radius: 8px; margin: 2px; }\n"
    "note-card.thread-depth-1 { margin-left: 20px; background: alpha(@theme_bg_color, 0.3); }\n"
    "note-card.thread-depth-2 { margin-left: 40px; background: alpha(@theme_bg_color, 0.4); }\n"
    "note-card.thread-depth-3 { margin-left: 60px; background: alpha(@theme_bg_color, 0.5); }\n"
    "note-card.thread-depth-4 { margin-left: 80px; background: alpha(@theme_bg_color, 0.6); }\n"
    ".root-0 { background: #6b7280; } .root-1 { background: #ef4444; } .root-2 { background: #f59e0b; } .root-3 { background: #10b981; }\n"
    ".root-4 { background: #3b82f6; } .root-5 { background: #8b5cf6; } .root-6 { background: #ec4899; } .root-7 { background: #22c55e; }\n"
    ".root-8 { background: #06b6d4; } .root-9 { background: #f97316; } .root-a { background: #0ea5e9; } .root-b { background: #84cc16; }\n"
    ".root-c { background: #a855f7; } .root-d { background: #eab308; } .root-e { background: #f43f5e; } .root-f { background: #14b8a6; }\n";
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_string(prov, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* Load app stylesheet for note cards */
  GtkCssProvider *prov2 = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(prov2, "/org/gnostr/ui/ui/styles/gnostr.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(prov2), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(prov2);
  g_object_unref(prov);
}

GtkWidget *gnostr_timeline_view_new(void) {
  return g_object_new(GNOSTR_TYPE_TIMELINE_VIEW, NULL);
}

void gnostr_timeline_view_set_model(GnostrTimelineView *self, GtkSelectionModel *model) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  if (self->selection_model == model) return;
  if (self->selection_model) g_clear_object(&self->selection_model);
  if (self->list_model) g_clear_object(&self->list_model);
  if (self->tree_model) g_clear_object(&self->tree_model);
  self->selection_model = model ? g_object_ref(model) : NULL;
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

/* Visibility filter function - only show items where visible=TRUE */
static gboolean visibility_filter_func(gpointer item, gpointer user_data) {
  (void)user_data;
  if (!item || !G_TYPE_CHECK_INSTANCE_TYPE(item, timeline_item_get_type())) {
    g_debug("[FILTER] Rejecting non-TimelineItem item");
    return FALSE;
  }
  
  TimelineItem *timeline_item = (TimelineItem*)item;
  gboolean visible = timeline_item->visible;
  g_debug("[FILTER] TimelineItem visible=%s", visible ? "TRUE" : "FALSE");
  return visible;
}

/* Forward declaration for populate_flattened_model */
static void populate_flattened_model(GnostrTimelineView *self, GListModel *roots);

/* Debug callback for root model changes */
static void on_root_items_changed(GListModel *list, guint position, guint removed, guint added, gpointer user_data) {
  GnostrTimelineView *self = (GnostrTimelineView *)user_data;
  g_debug("[TREE] Root items changed: position=%u removed=%u added=%u total=%u",
           position, removed, added, g_list_model_get_n_items(list));
  
  /* Repopulate flattened model when items change */
  if (self && self->flattened_model && self->tree_model) {
    g_debug("[TREE] Repopulating flattened model due to items changed");
    populate_flattened_model(self, G_LIST_MODEL(self->tree_model));
  }
}

/* Populate flattened model with roots and their children */
static void populate_flattened_model(GnostrTimelineView *self, GListModel *roots) {
  if (!self || !self->flattened_model || !roots) return;
  
  g_debug("[TREE] Populating flattened model with %u roots", g_list_model_get_n_items(roots));
  
  /* Clear existing items */
  g_list_store_remove_all(self->flattened_model);
  
  /* Add each root and its children */
  for (guint i = 0; i < g_list_model_get_n_items(roots); i++) {
    TimelineItem *root = (TimelineItem*)g_list_model_get_item(roots, i);
    if (!root) continue;
    
    /* Add the root */
    g_list_store_append(self->flattened_model, root);
    g_debug("[TREE] Added root item %p to flattened model", root);
    
    /* Add children recursively */
    GListModel *children = timeline_item_get_children_model(root);
    if (children) {
      guint child_count = g_list_model_get_n_items(children);
      g_debug("[TREE] Root has %u children", child_count);
      
      for (guint j = 0; j < child_count; j++) {
        TimelineItem *child = (TimelineItem*)g_list_model_get_item(children, j);
        if (child) {
          g_list_store_append(self->flattened_model, child);
          g_debug("[TREE] Added child item %p (depth=%u) to flattened model", child, child->depth);
        }
      }
    }
    
    g_object_unref(root);
  }
  
  g_debug("[TREE] Flattened model now has %u items", g_list_model_get_n_items(G_LIST_MODEL(self->flattened_model)));
}

/* New: set tree roots model (GListModel of TimelineItem), creating a flattened model */
void gnostr_timeline_view_set_tree_roots(GnostrTimelineView *self, GListModel *roots) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  g_debug("timeline_view_set_tree_roots: self=%p roots=%p list_view=%p", (void*)self, (void*)roots, (void*)self->list_view);
  
  /* Add debug signal to monitor changes to the roots model */
  if (roots) {
    g_signal_connect(roots, "items-changed", G_CALLBACK(on_root_items_changed), self);
    g_debug("[TREE] Connected to roots items-changed signal");
  }
  /* Detach any existing model from the list view FIRST to drop its ref safely */
  if (self->list_view) {
    gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), NULL);
  }
  /* Now clear our held refs */
  if (self->selection_model) g_clear_object(&self->selection_model);
  if (self->tree_model) g_clear_object(&self->tree_model);
  if (self->flattened_model) g_clear_object(&self->flattened_model);
  
  if (roots) {
    /* Create a flattened model that includes roots and their children */
    self->flattened_model = g_list_store_new(timeline_item_get_type());
    self->tree_model = (GtkTreeListModel*)roots; /* Store reference for debugging */
    
    /* Temporarily disable visibility filter to debug */
    /* GtkFilterListModel *filter_model = gtk_filter_list_model_new(G_LIST_MODEL(self->flattened_model), NULL);
    GtkCustomFilter *visibility_filter = gtk_custom_filter_new((GtkCustomFilterFunc)visibility_filter_func, NULL, NULL);
    gtk_filter_list_model_set_filter(filter_model, GTK_FILTER(visibility_filter)); */
    
    self->selection_model = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(self->flattened_model)));
    /* Take ownership of a non-floating ref so we can clear it later safely */
    g_object_ref_sink(self->selection_model);
    
    /* Populate the flattened model with existing roots and their children */
    populate_flattened_model(self, roots);
  } else {
    self->tree_model = NULL;
    self->flattened_model = NULL;
    self->selection_model = NULL;
  }
  g_debug("timeline_view_set_tree_roots: applying selection model=%p", (void*)self->selection_model);
  gtk_list_view_set_model(GTK_LIST_VIEW(self->list_view), self->selection_model);
}

void gnostr_timeline_view_prepend_text(GnostrTimelineView *self, const char *text) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  ensure_list_model(self);
  TimelineItem *item = timeline_item_new(NULL, NULL, NULL, text, 0);
  /* Prepend by inserting at position 0 */
  g_list_store_insert(self->list_model, 0, item);
  g_object_unref(item);
  /* Auto-scroll to top so the newly prepended row is visible */
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) {
      gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
    }
  }
  g_debug("prepend_text: added=%.40s count=%u", text ? text : "", (unsigned)g_list_model_get_n_items(G_LIST_MODEL(self->list_model)));
}

/* New: prepend a fully specified item */
void gnostr_timeline_view_prepend(GnostrTimelineView *self,
                                  const char *display,
                                  const char *handle,
                                  const char *ts,
                                  const char *content,
                                  guint depth) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  ensure_list_model(self);
  TimelineItem *item = timeline_item_new(display, handle, ts, content, depth);
  g_list_store_insert(self->list_model, 0, item);
  g_object_unref(item);
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
  }
}

GtkWidget *gnostr_timeline_view_get_scrolled_window(GnostrTimelineView *self) {
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_VIEW(self), NULL);
  return self->root_scroller;
}
