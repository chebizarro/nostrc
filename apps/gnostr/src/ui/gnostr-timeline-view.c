#include "gnostr-timeline-view.h"
#include "gn-timeline-tabs.h"
#include "gnostr-main-window.h"
#include "note_card_row.h"
#include "gnostr-zap-dialog.h"
#include "gnostr-profile-provider.h"
#include "../model/gn-nostr-event-item.h"
#include "../storage_ndb.h"
#include "nostr-event.h"
#include "nostr-json.h"
#include "nostr/nip19/nip19.h"
#include "nostr_simple_pool.h"
#include "../util/relays.h"
#include "../util/utils.h"
#include "../util/bookmarks.h"
#include "../util/nip32_labels.h"
#include "../util/nip23.h"
#include "../util/nip71.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json.h>
#include <json-glib/json-glib.h>
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
  /* Decode bech32 npub to get raw pubkey bytes */
  uint8_t pubkey_bytes[32];
  int decode_result = nostr_nip19_decode_npub(npub, pubkey_bytes);
  g_free(npub);
  if (decode_result != 0) {
    return NULL;
  }
  /* Convert to hex string (64 chars + null) */
  char *hex = g_malloc0(65);
  for (int i = 0; i < 32; i++) {
    snprintf(hex + i * 2, 3, "%02x", pubkey_bytes[i]);
  }
  return hex;
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
      if (!rr) continue;
      /* Get the weak ref as GObject first, then validate before casting */
      GObject *obj = g_weak_ref_get(&rr->ref);
      if (!obj || !GNOSTR_IS_NOTE_CARD_ROW(obj)) { 
        if (obj) g_object_unref(obj); 
        continue; 
      }
      GnostrNoteCardRow *r = GNOSTR_NOTE_CARD_ROW(obj);
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
      if (!rr) continue;
      /* Get weak ref as GObject first, validate before casting */
      GObject *obj = g_weak_ref_get(&rr->ref);
      gboolean keep = TRUE;
      if (!obj || !GNOSTR_IS_NOTE_CARD_ROW(obj) || GTK_WIDGET(obj) == row) keep = FALSE;
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
        for (int i=0;i<32;i++) snprintf(&idhex[i*2], 3, "%02x", id32[i]);
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
  /* NIP-18 repost info */
  gboolean is_repost;
  gchar *reposter_pubkey;
  gchar *reposter_display_name;
  gint64 repost_created_at;
  /* NIP-18 quote repost info */
  gboolean has_quote;
  gchar *quoted_event_id;
  gchar *quoted_content;
  gchar *quoted_author;
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
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->handle, g_free);
  g_clear_pointer(&self->timestamp, g_free);
  g_clear_pointer(&self->content, g_free);
  g_clear_pointer(&self->id, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->pubkey, g_free);
  g_clear_pointer(&self->avatar_url, g_free);
  if (self->children) g_clear_object(&self->children);
  /* NIP-18 repost cleanup */
  g_clear_pointer(&self->reposter_pubkey, g_free);
  g_clear_pointer(&self->reposter_display_name, g_free);
  g_clear_pointer(&self->quoted_event_id, g_free);
  g_clear_pointer(&self->quoted_content, g_free);
  g_clear_pointer(&self->quoted_author, g_free);
  G_OBJECT_CLASS(timeline_item_parent_class)->dispose(obj);
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

/* Public wrappers for building trees from outside */
void gnostr_timeline_item_add_child(TimelineItem *parent, TimelineItem *child) {
  timeline_item_add_child(parent, child);
}

GListModel *gnostr_timeline_item_get_children(TimelineItem *item) {
  return timeline_item_get_children_model(item);
}

struct _GnostrTimelineView {
  GtkWidget parent_instance;
  GtkWidget *root_box;                /* vertical box containing tabs + scroller */
  GtkWidget *tabs;                    /* GnTimelineTabs widget */
  GtkWidget *root_scroller;
  GtkWidget *list_view;
  GtkSelectionModel *selection_model; /* owned */
  GListStore *list_model;             /* owned: TimelineItem (flat) or tree roots when tree is active */
  GtkTreeListModel *tree_model;       /* owned when tree roots set */
  GListStore *flattened_model;        /* owned: flattened timeline with threading */

  /* nostrc-lig9: NIP-65 reaction fetch tracking */
  GHashTable *reaction_nip65_fetched; /* pubkey_hex -> gboolean: authors we've fetched NIP-65 for */
  GCancellable *reaction_cancellable; /* cancellable for reaction fetch operations */

  /* nostrc-x8z3.1: Batched NIP-65 relay list fetching */
  GPtrArray *nip65_pending_authors;   /* Pending pubkeys for batch NIP-65 fetch */
  GHashTable *nip65_pending_events;   /* pubkey_hex -> event_id_hex for callback context */
  guint nip65_batch_timeout_id;       /* Debounce timeout for NIP-65 batch */

  /* nostrc-y62r: Scroll position tracking for viewport-aware loading */
  guint visible_range_start;          /* First visible item index */
  guint visible_range_end;            /* Last visible item index (exclusive) */
  gdouble last_scroll_value;          /* Previous scroll position for velocity calc */
  gint64 last_scroll_time;            /* Timestamp of last scroll for velocity calc */
  gdouble scroll_velocity;            /* Scroll velocity in pixels/ms */
  gboolean is_fast_scrolling;         /* TRUE if user is scrolling fast (>threshold) */
  guint scroll_idle_id;               /* Source ID for scroll idle timeout */
};

G_DEFINE_TYPE(GnostrTimelineView, gnostr_timeline_view, GTK_TYPE_WIDGET)

/* Signals */
enum {
  SIGNAL_TAB_FILTER_CHANGED,
  N_SIGNALS
};

static guint timeline_view_signals[N_SIGNALS];

/* Handler for tab-selected signal from GnTimelineTabs */
static void on_tabs_tab_selected(GnTimelineTabs *tabs, guint index, gpointer user_data) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(user_data);
  if (!self || !tabs) return;

  GnTimelineTabType type = gn_timeline_tabs_get_tab_type(tabs, index);
  const char *filter_value = gn_timeline_tabs_get_tab_filter_value(tabs, index);

  g_debug("timeline_view: tab selected index=%u type=%d filter='%s'",
          index, type, filter_value ? filter_value : "(null)");

  /* Emit signal so main window can update the model query */
  g_signal_emit(self, timeline_view_signals[SIGNAL_TAB_FILTER_CHANGED], 0,
                (guint)type, filter_value);
}

static void gnostr_timeline_view_dispose(GObject *obj) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(obj);
  g_warning("🔥🔥🔥 TIMELINE_VIEW DISPOSE STARTING: list_view=%p list_model=%p tree_model=%p",
            (void*)self->list_view, (void*)self->list_model, (void*)self->tree_model);

  /* nostrc-y62r: Cancel scroll idle timeout */
  if (self->scroll_idle_id > 0) {
    g_source_remove(self->scroll_idle_id);
    self->scroll_idle_id = 0;
  }

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

  /* nostrc-lig9: Cancel and clean up reaction fetch state */
  if (self->reaction_cancellable) {
    g_cancellable_cancel(self->reaction_cancellable);
    g_clear_object(&self->reaction_cancellable);
  }
  g_clear_pointer(&self->reaction_nip65_fetched, g_hash_table_unref);

  /* nostrc-x8z3.1: Clean up batched NIP-65 fetch state */
  if (self->nip65_batch_timeout_id > 0) {
    g_source_remove(self->nip65_batch_timeout_id);
    self->nip65_batch_timeout_id = 0;
  }
  g_clear_pointer(&self->nip65_pending_authors, g_ptr_array_unref);
  g_clear_pointer(&self->nip65_pending_events, g_hash_table_unref);

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
static void on_note_card_like_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gint event_kind, const char *reaction_content, gpointer user_data) {
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
static void on_note_card_comment_requested_relay(GnostrNoteCardRow *row, const char *id_hex, int kind, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_zap_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, const char *lud16, gpointer user_data) {
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
  gchar *display_name = NULL;
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
    uint8_t pk32[32];
    gboolean valid = TRUE;
    for (int i = 0; i < 32 && valid; i++) {
      unsigned int b;
      if (sscanf(pubkey_hex + i*2, "%2x", &b) != 1) valid = FALSE;
      else pk32[i] = (uint8_t)b;
    }
    if (valid) {
      char *npub = NULL;
      if (nostr_nip19_encode_npub(pk32, &npub) == 0 && npub) {
        display_name = g_strdup_printf("%.12s...", npub);
        free(npub);
      }
    }
  }

  gnostr_zap_dialog_set_recipient(dialog, pubkey_hex, display_name, lud16);
  g_free(display_name);

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
static void on_note_card_view_thread_requested_relay(GnostrNoteCardRow *row, const char *root_event_id, gpointer user_data) {
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
static void on_note_card_navigate_to_note_relay(GnostrNoteCardRow *row, const char *event_id, gpointer user_data) {
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
static void on_note_card_mute_user_requested_relay(GnostrNoteCardRow *row, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_mute_thread_requested_relay(GnostrNoteCardRow *row, const char *event_id_hex, gpointer user_data) {
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
static void on_note_card_show_toast_relay(GnostrNoteCardRow *row, const char *message, gpointer user_data) {
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

/* Handler for bookmark-toggled signal - update NIP-51 bookmark list */
static void on_note_card_bookmark_toggled_cb(GnostrNoteCardRow *row, const char *event_id, gboolean is_bookmarked, gpointer user_data) {
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
static void on_note_card_delete_note_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_report_note_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *pubkey_hex, gpointer user_data) {
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
static void on_note_card_label_note_requested_relay(GnostrNoteCardRow *row, const char *id_hex, const char *namespace, const char *label, const char *pubkey_hex, gpointer user_data) {
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
  if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
    GnostrNoteCardRow *card_row = GNOSTR_NOTE_CARD_ROW(row);
    if (gnostr_note_card_row_is_disposed(card_row)) return;
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

    if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
      gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, avatar_url);
      gtk_widget_set_visible(row, TRUE);

      /* NIP-05: Set verification identifier if available */
      if (nip05 && *nip05) {
        gchar *pubkey = NULL;
        g_object_get(event_item, "pubkey", &pubkey, NULL);
        if (pubkey && strlen(pubkey) == 64) {
          gnostr_note_card_row_set_nip05(GNOSTR_NOTE_CARD_ROW(row), nip05, pubkey);
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
static void on_note_card_search_hashtag(GnostrNoteCardRow *row, const char *hashtag, gpointer user_data) {
  (void)row;
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(user_data);
  if (!self || !hashtag || !*hashtag) return;

  g_debug("timeline_view: search-hashtag signal received for #%s", hashtag);

  /* Add a new hashtag tab */
  gnostr_timeline_view_add_hashtag_tab(self, hashtag);
}

static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f;
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(data);
  GtkWidget *row = GTK_WIDGET(gnostr_note_card_row_new());

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
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url);
}

static void on_item_notify_handle(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL;
  g_object_get(obj, "display-name", &display, "handle", &handle, "avatar-url", &avatar_url, NULL);
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url);
}

static void on_item_notify_avatar_url(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  gchar *url = NULL, *display = NULL, *handle = NULL;
  g_object_get(obj, "avatar-url", &url, "display-name", &display, "handle", &handle, NULL);
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row), display, handle, url);
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
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_like_count(GNOSTR_NOTE_CARD_ROW(row), like_count);
}

/* NIP-25: Notify handler for is_liked changes */
static void on_item_notify_is_liked(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);
  if (!GTK_IS_WIDGET(row)) return;
  if (!obj || !G_IS_OBJECT(obj)) return;
  gboolean is_liked = FALSE;
  g_object_get(obj, "is-liked", &is_liked, NULL);
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_liked(GNOSTR_NOTE_CARD_ROW(row), is_liked);
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
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_zap_stats(GNOSTR_NOTE_CARD_ROW(row), zap_count, total_msat);
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
  if (GNOSTR_IS_NOTE_CARD_ROW(row))
    gnostr_note_card_row_set_zap_stats(GNOSTR_NOTE_CARD_ROW(row), zap_count, total_msat);
}

/* Unbind cleanup: detach row from any inflight operations.
 * 
 * CRITICAL: We do NOT access gtk_list_item_get_item() here because during
 * fast scrolling, the underlying GObject may already be freed and its memory
 * reused. Even calling G_IS_OBJECT() on freed memory can corrupt the heap.
 * 
 * Signal disconnection is handled automatically by g_signal_connect_object()
 * which we use in factory_bind_cb - signals auto-disconnect when row is destroyed.
 */
static void factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *row = gtk_list_item_get_child(item);
  
  /* Only access the row widget, never the item's underlying object */
  if (GTK_IS_WIDGET(row)) {
    /* Detach this row from any inflight embed fetches */
    inflight_detach_row(row);
    
    /* CRITICAL: Prepare the row for unbinding BEFORE GTK disposes it.
     * This cancels all async operations and sets the disposed flag to prevent
     * callbacks from corrupting Pango state during the unbind/dispose process. */
    if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
      gnostr_note_card_row_prepare_for_unbind(GNOSTR_NOTE_CARD_ROW(row));
    }
  }
}

/* Context for hashtag extraction callback */
typedef struct {
  GPtrArray *hashtags;
} HashtagExtractContext;

/* Callback for extracting hashtags */
static bool extract_hashtag_callback(size_t index, const char *tag_json, void *user_data) {
  (void)index;
  HashtagExtractContext *ctx = (HashtagExtractContext *)user_data;
  if (!tag_json || !ctx) return true;

  if (!nostr_json_is_array_str(tag_json)) return true;

  char *tag_name = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 0, &tag_name) != 0 || !tag_name) {
    return true;
  }

  if (g_strcmp0(tag_name, "t") != 0) {
    free(tag_name);
    return true;
  }
  free(tag_name);

  char *hashtag = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 1, &hashtag) == 0 && hashtag && *hashtag) {
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
  if (!nostr_json_is_array_str(tags_json)) return NULL;

  HashtagExtractContext ctx = { .hashtags = g_ptr_array_new() };
  nostr_json_array_foreach_root(tags_json, extract_hashtag_callback, &ctx);

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
static bool extract_content_warning_callback(size_t index, const char *tag_json, void *user_data) {
  (void)index;
  ContentWarningContext *ctx = (ContentWarningContext *)user_data;
  if (!tag_json || !ctx || ctx->reason) return true; /* Stop if already found */

  if (!nostr_json_is_array_str(tag_json)) return true;

  char *tag_name = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 0, &tag_name) != 0 || !tag_name) {
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
  if (nostr_json_get_array_string(tag_json, NULL, 1, &reason_str) == 0 && reason_str) {
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
  if (!nostr_json_is_array_str(tags_json)) return NULL;

  ContentWarningContext ctx = { .reason = NULL };
  nostr_json_array_foreach_root(tags_json, extract_content_warning_callback, &ctx);

  return ctx.reason;
}

/* ============== nostrc-lig9: NIP-65 Reaction Fetching ============== */

/* Context for async reaction fetch operations */
typedef struct {
  GWeakRef view_ref;        /* weak ref to timeline view */
  gchar *event_id_hex;      /* event ID to fetch reactions for */
  gchar *author_pubkey_hex; /* post author's pubkey */
  GPtrArray *write_relays;  /* nostrc-0u5h: cached for COUNT fallback */
} ReactionFetchContext;

static void reaction_fetch_ctx_free(ReactionFetchContext *ctx) {
  if (!ctx) return;
  g_weak_ref_clear(&ctx->view_ref);
  g_free(ctx->event_id_hex);
  g_free(ctx->author_pubkey_hex);
  if (ctx->write_relays) g_ptr_array_unref(ctx->write_relays);
  g_free(ctx);
}

/* Forward declare for fallback */
static void on_reaction_query_done(GObject *source, GAsyncResult *res, gpointer user_data);

/* nostrc-0u5h: Issue regular subscription query as fallback when COUNT fails/unsupported */
static void reaction_count_fallback_query(GnostrTimelineView *self, ReactionFetchContext *ctx) {
  if (!ctx->write_relays || ctx->write_relays->len == 0) {
    g_object_unref(self);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  g_debug("timeline_view: COUNT fallback - querying %u relays for reactions to %.16s",
          ctx->write_relays->len, ctx->event_id_hex);

  /* Build filter for kind:7 reactions referencing this event */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { 7 };
  nostr_filter_set_kinds(filter, kinds, 1);

  NostrTag *e_tag = nostr_tag_new("e", ctx->event_id_hex, NULL);
  NostrTags *tags = nostr_tags_new(1, e_tag);
  nostr_filter_set_tags(filter, tags);
  nostr_filter_set_limit(filter, 100);

  /* Build URL array */
  const char **urls = g_new0(const char *, ctx->write_relays->len);
  for (guint i = 0; i < ctx->write_relays->len; i++) {
    urls[i] = g_ptr_array_index(ctx->write_relays, i);
  }

  /* Query relays for reaction events */
  gnostr_simple_pool_query_single_async(
    gnostr_get_shared_query_pool(),
    urls,
    ctx->write_relays->len,
    filter,
    self->reaction_cancellable,
    on_reaction_query_done,
    ctx  /* transfer ownership */
  );

  g_free(urls);
  nostr_filter_free(filter);
  g_object_unref(self);
}

/* nostrc-x8z3.2: Callback when NIP-45 COUNT query for reactions completes */
static void on_reaction_count_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  ReactionFetchContext *ctx = (ReactionFetchContext *)user_data;
  if (!ctx) return;

  GError *error = NULL;
  gint64 count = gnostr_simple_pool_count_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  /* nostrc-0u5h: On error (relay may not support NIP-45), fall back to regular query */
  if (error) {
    gboolean cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (!cancelled) {
      g_debug("timeline_view: COUNT error for %.16s, falling back to query: %s",
              ctx->event_id_hex, error->message);
    }
    g_error_free(error);

    if (cancelled) {
      reaction_fetch_ctx_free(ctx);
      return;
    }

    /* Fallback to regular subscription query */
    GnostrTimelineView *self = g_weak_ref_get(&ctx->view_ref);
    if (!self) {
      reaction_fetch_ctx_free(ctx);
      return;
    }
    reaction_count_fallback_query(self, ctx);
    return;
  }

  GnostrTimelineView *self = g_weak_ref_get(&ctx->view_ref);
  if (!self) {
    reaction_fetch_ctx_free(ctx);
    return;
  }

  /* nostrc-0u5h: count < 0 means COUNT unsupported, fall back to regular query */
  if (count < 0) {
    g_debug("timeline_view: COUNT unsupported (returned %lld) for %.16s, falling back to query",
            (long long)count, ctx->event_id_hex);
    reaction_count_fallback_query(self, ctx);
    return;
  }

  /* count == 0 means no reactions, no need to fallback */
  if (count == 0) {
    g_debug("timeline_view: COUNT returned 0 for %.16s", ctx->event_id_hex);
    g_object_unref(self);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  g_debug("timeline_view: COUNT returned %lld reactions for %.16s", (long long)count, ctx->event_id_hex);

  /* Update reaction count in model - find the event item and update it */
  if (self->list_model) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(self->list_model));
    for (guint i = 0; i < n_items; i++) {
      GObject *obj = g_list_model_get_item(G_LIST_MODEL(self->list_model), i);
      if (obj && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
        gchar *item_id = NULL;
        g_object_get(obj, "event-id", &item_id, NULL);
        if (item_id && g_strcmp0(item_id, ctx->event_id_hex) == 0) {
          guint old_count = gn_nostr_event_item_get_like_count(GN_NOSTR_EVENT_ITEM(obj));
          if ((guint)count > old_count) {
            g_debug("timeline_view: updating reaction count for %.16s: %u -> %lld",
                    ctx->event_id_hex, old_count, (long long)count);
            gn_nostr_event_item_set_like_count(GN_NOSTR_EVENT_ITEM(obj), (guint)count);
          }
        }
        g_free(item_id);
      }
      if (obj) g_object_unref(obj);
    }
  }

  g_object_unref(self);
  reaction_fetch_ctx_free(ctx);
}

/* Callback when reaction query from author's NIP-65 relays completes (fallback for non-NIP-45 relays) */
static void on_reaction_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  ReactionFetchContext *ctx = (ReactionFetchContext *)user_data;
  if (!ctx) return;

  GError *error = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("timeline_view: reaction query error: %s", error->message);
    }
    g_error_free(error);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  GnostrTimelineView *self = g_weak_ref_get(&ctx->view_ref);
  if (!self) {
    if (results) g_ptr_array_unref(results);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  if (!results || results->len == 0) {
    g_debug("timeline_view: no reactions found from author NIP-65 relays for %.16s", ctx->event_id_hex);
    if (results) g_ptr_array_unref(results);
    g_object_unref(self);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  g_debug("timeline_view: received %u reaction events from author NIP-65 relays", results->len);

  /* Store reactions in nostrdb - they will be picked up by local query next time */
  for (guint i = 0; i < results->len; i++) {
    const char *event_json = g_ptr_array_index(results, i);
    if (event_json && *event_json) {
      storage_ndb_ingest_event_json(event_json, NULL);
    }
  }

  /* Update reaction count in model - find the event item and update it */
  guint new_count = storage_ndb_count_reactions(ctx->event_id_hex);
  if (new_count > 0 && self->list_model) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(self->list_model));
    for (guint i = 0; i < n_items; i++) {
      GObject *obj = g_list_model_get_item(G_LIST_MODEL(self->list_model), i);
      if (obj && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
        gchar *item_id = NULL;
        g_object_get(obj, "event-id", &item_id, NULL);
        if (item_id && g_strcmp0(item_id, ctx->event_id_hex) == 0) {
          guint old_count = gn_nostr_event_item_get_like_count(GN_NOSTR_EVENT_ITEM(obj));
          if (new_count > old_count) {
            g_debug("timeline_view: updating reaction count for %.16s: %u -> %u",
                    ctx->event_id_hex, old_count, new_count);
            gn_nostr_event_item_set_like_count(GN_NOSTR_EVENT_ITEM(obj), new_count);
          }
        }
        g_free(item_id);
      }
      if (obj) g_object_unref(obj);
    }
  }

  g_ptr_array_unref(results);
  g_object_unref(self);
  reaction_fetch_ctx_free(ctx);
}

/* Callback when author's NIP-65 relay list is fetched */
static void on_author_nip65_for_reactions(GPtrArray *relays, gpointer user_data) {
  ReactionFetchContext *ctx = (ReactionFetchContext *)user_data;
  if (!ctx) return;

  GnostrTimelineView *self = g_weak_ref_get(&ctx->view_ref);
  if (!self) {
    if (relays) g_ptr_array_unref(relays);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  if (!relays || relays->len == 0) {
    g_debug("timeline_view: no NIP-65 relays for author %.16s", ctx->author_pubkey_hex);
    if (relays) g_ptr_array_unref(relays);
    g_object_unref(self);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  /* Get write relays from NIP-65 list (reactions are sent to author's write relays) */
  GPtrArray *write_relays = gnostr_nip65_get_write_relays(relays);
  g_ptr_array_unref(relays);

  if (!write_relays || write_relays->len == 0) {
    g_debug("timeline_view: no write relays in NIP-65 for author %.16s", ctx->author_pubkey_hex);
    if (write_relays) g_ptr_array_unref(write_relays);
    g_object_unref(self);
    reaction_fetch_ctx_free(ctx);
    return;
  }

  g_debug("timeline_view: querying %u author write relays for reactions to %.16s",
          write_relays->len, ctx->event_id_hex);

  /* Build filter for kind:7 reactions referencing this event */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { 7 };
  nostr_filter_set_kinds(filter, kinds, 1);

  /* Set #e tag filter using NostrTags API */
  NostrTag *e_tag = nostr_tag_new("e", ctx->event_id_hex, NULL);
  NostrTags *tags = nostr_tags_new(1, e_tag);
  nostr_filter_set_tags(filter, tags);  /* Takes ownership of tags */

  nostr_filter_set_limit(filter, 100);

  /* Build URL array */
  const char **urls = g_new0(const char *, write_relays->len);
  for (guint i = 0; i < write_relays->len; i++) {
    urls[i] = g_ptr_array_index(write_relays, i);
  }

  /* Query author's relays for reactions */
  gnostr_simple_pool_query_single_async(
    gnostr_get_shared_query_pool(),
    urls,
    write_relays->len,
    filter,
    self->reaction_cancellable,
    on_reaction_query_done,
    ctx  /* transfer ownership of ctx */
  );

  g_free(urls);
  g_ptr_array_unref(write_relays);
  nostr_filter_free(filter);
  g_object_unref(self);
  /* Note: ctx ownership transferred to on_reaction_query_done */
}

/* ============== nostrc-x8z3.1: Batched NIP-65 Fetching ============== */

/* Debounce delay for batching NIP-65 requests (milliseconds) */
#define NIP65_BATCH_DEBOUNCE_MS 50

/* Context for batched NIP-65 query */
typedef struct {
  GWeakRef view_ref;          /* weak ref to timeline view */
  GPtrArray *authors;         /* array of author pubkeys (owned, copied from pending) */
  GHashTable *author_events;  /* author_pubkey -> event_id mapping (owned, copied) */
} Nip65BatchContext;

static void nip65_batch_ctx_free(Nip65BatchContext *ctx) {
  if (!ctx) return;
  g_weak_ref_clear(&ctx->view_ref);
  if (ctx->authors) g_ptr_array_unref(ctx->authors);
  if (ctx->author_events) g_hash_table_unref(ctx->author_events);
  g_free(ctx);
}

/* Forward declaration */
static gboolean nip65_batch_dispatch(gpointer user_data);

/* Callback when batched NIP-65 query completes */
static void on_batch_nip65_query_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip65BatchContext *ctx = (Nip65BatchContext *)user_data;
  if (!ctx) return;

  GnostrTimelineView *self = g_weak_ref_get(&ctx->view_ref);
  if (!self) {
    nip65_batch_ctx_free(ctx);
    return;
  }

  GError *error = NULL;
  GPtrArray *results = gnostr_simple_pool_query_single_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("timeline_view: batched NIP-65 query error: %s", error->message);
    }
    g_error_free(error);
    g_object_unref(self);
    nip65_batch_ctx_free(ctx);
    return;
  }

  if (!results || results->len == 0) {
    g_debug("timeline_view: batched NIP-65 query returned no results for %u authors",
            ctx->authors ? ctx->authors->len : 0);
    if (results) g_ptr_array_unref(results);
    g_object_unref(self);
    nip65_batch_ctx_free(ctx);
    return;
  }

  g_debug("timeline_view: batched NIP-65 query returned %u results", results->len);

  /* Process each NIP-65 result and fetch reactions for the corresponding event */
  for (guint i = 0; i < results->len; i++) {
    const char *event_json = g_ptr_array_index(results, i);
    if (!event_json) continue;

    /* Extract author pubkey from the event JSON using json-glib */
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
      g_object_unref(parser);
      continue;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_node)) {
      g_object_unref(parser);
      continue;
    }

    JsonObject *root = json_node_get_object(root_node);
    const char *author_pubkey = NULL;
    if (json_object_has_member(root, "pubkey")) {
      author_pubkey = json_object_get_string_member(root, "pubkey");
    }

    if (!author_pubkey || strlen(author_pubkey) != 64) {
      g_object_unref(parser);
      continue;
    }

    /* Look up the event ID for this author */
    const char *event_id = g_hash_table_lookup(ctx->author_events, author_pubkey);
    if (!event_id) {
      g_object_unref(parser);
      continue;
    }

    /* Parse NIP-65 relay list */
    GPtrArray *relays = gnostr_nip65_parse_event(event_json, NULL);
    if (!relays || relays->len == 0) {
      g_object_unref(parser);
      if (relays) g_ptr_array_unref(relays);
      continue;
    }

    /* Get write relays for reaction query */
    GPtrArray *write_relays = gnostr_nip65_get_write_relays(relays);
    g_ptr_array_unref(relays);

    if (!write_relays || write_relays->len == 0) {
      g_object_unref(parser);
      if (write_relays) g_ptr_array_unref(write_relays);
      continue;
    }

    g_debug("timeline_view: using NIP-45 COUNT for reactions to %.16s (author %.16s, %u relays)",
            event_id, author_pubkey, write_relays->len);

    /* Create reaction fetch context - keep write_relays for fallback (nostrc-0u5h) */
    ReactionFetchContext *rctx = g_new0(ReactionFetchContext, 1);
    g_weak_ref_init(&rctx->view_ref, self);
    rctx->event_id_hex = g_strdup(event_id);
    rctx->author_pubkey_hex = g_strdup(author_pubkey);
    rctx->write_relays = write_relays;  /* Transfer ownership for COUNT fallback */

    /* Build filter for kind:7 reactions referencing this event */
    NostrFilter *filter = nostr_filter_new();
    int kinds[1] = { 7 };
    nostr_filter_set_kinds(filter, kinds, 1);

    NostrTag *e_tag = nostr_tag_new("e", event_id, NULL);
    NostrTags *tags = nostr_tags_new(1, e_tag);
    nostr_filter_set_tags(filter, tags);
    /* Note: No limit needed for COUNT - we just want the total */

    /* Build URL array */
    const char **urls = g_new0(const char *, write_relays->len);
    for (guint j = 0; j < write_relays->len; j++) {
      urls[j] = g_ptr_array_index(write_relays, j);
    }

    /* nostrc-x8z3.2: Use NIP-45 COUNT for efficiency - just get the count, not full events */
    gnostr_simple_pool_count_async(
      gnostr_get_shared_query_pool(),
      urls,
      write_relays->len,
      filter,
      self->reaction_cancellable,
      on_reaction_count_done,
      rctx
    );

    g_free(urls);
    /* Note: write_relays ownership transferred to rctx for fallback, freed in reaction_fetch_ctx_free */
    nostr_filter_free(filter);
    g_object_unref(parser);
  }

  g_ptr_array_unref(results);
  g_object_unref(self);
  nip65_batch_ctx_free(ctx);
}

/* Dispatch batched NIP-65 request after debounce timeout */
static gboolean nip65_batch_dispatch(gpointer user_data) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(user_data);
  if (!self) return G_SOURCE_REMOVE;

  self->nip65_batch_timeout_id = 0;

  if (!self->nip65_pending_authors || self->nip65_pending_authors->len == 0) {
    return G_SOURCE_REMOVE;
  }

  guint author_count = self->nip65_pending_authors->len;
  g_debug("timeline_view: dispatching batched NIP-65 fetch for %u authors", author_count);

  /* Create batch context with copies of pending data */
  Nip65BatchContext *ctx = g_new0(Nip65BatchContext, 1);
  g_weak_ref_init(&ctx->view_ref, self);
  ctx->authors = g_ptr_array_new_with_free_func(g_free);
  ctx->author_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  /* Copy pending authors and events */
  for (guint i = 0; i < self->nip65_pending_authors->len; i++) {
    const char *author = g_ptr_array_index(self->nip65_pending_authors, i);
    const char *event = g_hash_table_lookup(self->nip65_pending_events, author);
    if (author && event) {
      g_ptr_array_add(ctx->authors, g_strdup(author));
      g_hash_table_insert(ctx->author_events, g_strdup(author), g_strdup(event));
    }
  }

  /* Clear pending state */
  g_ptr_array_set_size(self->nip65_pending_authors, 0);
  g_hash_table_remove_all(self->nip65_pending_events);

  if (ctx->authors->len == 0) {
    nip65_batch_ctx_free(ctx);
    return G_SOURCE_REMOVE;
  }

  /* Build multi-author filter for kind 10002 */
  NostrFilter *filter = nostr_filter_new();
  int kinds[1] = { 10002 };
  nostr_filter_set_kinds(filter, kinds, 1);

  /* Set authors array */
  const char **authors = g_new0(const char *, ctx->authors->len);
  for (guint i = 0; i < ctx->authors->len; i++) {
    authors[i] = g_ptr_array_index(ctx->authors, i);
  }
  nostr_filter_set_authors(filter, authors, ctx->authors->len);
  g_free(authors);

  /* Get configured relays */
  GPtrArray *relay_arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(relay_arr);

  const char **urls = g_new0(const char *, relay_arr->len);
  for (guint i = 0; i < relay_arr->len; i++) {
    urls[i] = g_ptr_array_index(relay_arr, i);
  }

  /* Query all authors' NIP-65 in one request */
  gnostr_simple_pool_query_single_async(
    gnostr_get_shared_query_pool(),
    urls,
    relay_arr->len,
    filter,
    self->reaction_cancellable,
    on_batch_nip65_query_done,
    ctx
  );

  g_free(urls);
  g_ptr_array_unref(relay_arr);
  nostr_filter_free(filter);

  return G_SOURCE_REMOVE;
}

/* ============== End nostrc-x8z3.1 ============== */

/* Initiate async fetch of reactions from post author's NIP-65 relays.
 * nostrc-x8z3.1: Now batches requests using multi-author filter to reduce N subscriptions to 1. */
static void fetch_reactions_from_author_relays(GnostrTimelineView *self,
                                                const char *event_id_hex,
                                                const char *author_pubkey_hex) {
  if (!self || !event_id_hex || !author_pubkey_hex) return;
  if (strlen(event_id_hex) != 64 || strlen(author_pubkey_hex) != 64) return;

  /* Check if we've already initiated a fetch for this author */
  if (g_hash_table_contains(self->reaction_nip65_fetched, author_pubkey_hex)) {
    return;
  }

  /* Mark as fetched to prevent duplicate requests */
  g_hash_table_insert(self->reaction_nip65_fetched,
                      g_strdup(author_pubkey_hex),
                      GINT_TO_POINTER(1));

  g_debug("timeline_view: queueing NIP-65 fetch for author %.16s, event %.16s",
          author_pubkey_hex, event_id_hex);

  /* Queue author for batched fetch */
  g_ptr_array_add(self->nip65_pending_authors, g_strdup(author_pubkey_hex));
  g_hash_table_insert(self->nip65_pending_events,
                      g_strdup(author_pubkey_hex),
                      g_strdup(event_id_hex));

  /* Reset debounce timer */
  if (self->nip65_batch_timeout_id > 0) {
    g_source_remove(self->nip65_batch_timeout_id);
  }
  self->nip65_batch_timeout_id = g_timeout_add(NIP65_BATCH_DEBOUNCE_MS,
                                                nip65_batch_dispatch,
                                                self);
}

/* ============== End nostrc-lig9 ============== */

static void factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f;
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(data);
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
  if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
    /* CRITICAL: Prepare row for binding - resets disposed flag and creates fresh
     * cancellable. Must be called BEFORE populating the row with data.
     * nostrc-o7pp: Matches NoteCardFactory pattern. */
    gnostr_note_card_row_prepare_for_bind(GNOSTR_NOTE_CARD_ROW(row));

    /* Use pubkey prefix as fallback if no profile info available */
    gchar *display_fallback = NULL;
    if (!display && !handle && pubkey && strlen(pubkey) >= 8) {
      display_fallback = g_strdup_printf("%.8s...", pubkey);
    }

    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row),
                                     display ? display : display_fallback,
                                     handle, avatar_url);
    gnostr_note_card_row_set_timestamp(GNOSTR_NOTE_CARD_ROW(row), created_at, ts);

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

        gnostr_note_card_row_set_article_mode(GNOSTR_NOTE_CARD_ROW(row),
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
        gnostr_note_card_row_set_content_with_imeta(GNOSTR_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-71: Handle video events (kind 34235/34236) */
    else if (gnostr_video_is_video(event_kind) && tags_json) {
      GnostrVideoMeta *video_meta = gnostr_video_parse_tags(tags_json, event_kind);
      if (video_meta) {
        gnostr_note_card_row_set_video_mode(GNOSTR_NOTE_CARD_ROW(row),
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
        gnostr_note_card_row_set_content_with_imeta(GNOSTR_NOTE_CARD_ROW(row), content, tags_json);
      }
    } else if (tags_json) {
      gnostr_note_card_row_set_content_with_imeta(GNOSTR_NOTE_CARD_ROW(row), content, tags_json);

      /* NIP-36: Check for content-warning tag */
      gchar *content_warning = parse_content_warning_from_tags_json(tags_json);
      if (content_warning) {
        gnostr_note_card_row_set_content_warning(GNOSTR_NOTE_CARD_ROW(row), content_warning);
        g_free(content_warning);
      }

      /* Extract and set hashtags from "t" tags for regular notes */
      gchar **hashtags = parse_hashtags_from_tags_json(tags_json);
      if (hashtags) {
        gnostr_note_card_row_set_hashtags(GNOSTR_NOTE_CARD_ROW(row), (const char * const *)hashtags);
        g_strfreev(hashtags);
      }
    } else {
      gnostr_note_card_row_set_content(GNOSTR_NOTE_CARD_ROW(row), content);
    }

    /* Set hashtags from GnNostrEventItem if available (works even when tags_json is disabled) */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      const char * const *item_hashtags = gn_nostr_event_item_get_hashtags(GN_NOSTR_EVENT_ITEM(obj));
      if (item_hashtags && item_hashtags[0]) {
        gnostr_note_card_row_set_hashtags(GNOSTR_NOTE_CARD_ROW(row), item_hashtags);
      }
    }
    gnostr_note_card_row_set_depth(GNOSTR_NOTE_CARD_ROW(row), depth);
    gnostr_note_card_row_set_ids(GNOSTR_NOTE_CARD_ROW(row), id_hex, root_id, pubkey);

    /* Set NIP-10 thread info (reply indicator, view thread button) */
    gnostr_note_card_row_set_thread_info(GNOSTR_NOTE_CARD_ROW(row),
                                          root_id,
                                          parent_id,
                                          NULL, /* parent_author_name - will be resolved asynchronously if needed */
                                          is_reply);

    /* NIP-18: Set repost info if this is a TimelineItem with repost data */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, timeline_item_get_type())) {
      TimelineItem *ti = (TimelineItem *)obj;
      if (ti->is_repost) {
        gnostr_note_card_row_set_is_repost(GNOSTR_NOTE_CARD_ROW(row), TRUE);
        gnostr_note_card_row_set_repost_info(GNOSTR_NOTE_CARD_ROW(row),
                                              ti->reposter_pubkey,
                                              ti->reposter_display_name,
                                              ti->repost_created_at);
      }
      /* NIP-18: Set quote info if this item has a quote */
      if (ti->has_quote && ti->quoted_event_id) {
        gnostr_note_card_row_set_quote_info(GNOSTR_NOTE_CARD_ROW(row),
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
          gnostr_note_card_row_set_is_repost(GNOSTR_NOTE_CARD_ROW(row), TRUE);
          gnostr_note_card_row_set_repost_info(GNOSTR_NOTE_CARD_ROW(row),
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
                gnostr_note_card_row_set_content(GNOSTR_NOTE_CARD_ROW(row), orig_content);
              }

              /* Update timestamp to original note's time */
              if (orig_created_at > 0) {
                time_t t = (time_t)orig_created_at;
                struct tm *tm_info = localtime(&t);
                char orig_ts_buf[64];
                strftime(orig_ts_buf, sizeof(orig_ts_buf), "%Y-%m-%d %H:%M", tm_info);
                gnostr_note_card_row_set_timestamp(GNOSTR_NOTE_CARD_ROW(row), orig_created_at, orig_ts_buf);
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
                      if (nostr_json_is_valid(profile_json)) {
                        /* Profile is stored as event - need to parse content */
                        char *profile_content = NULL;
                        if (nostr_json_get_string(profile_json, "content", &profile_content) == 0 && profile_content) {
                          if (nostr_json_is_valid(profile_content)) {
                            char *orig_name = NULL;
                            char *orig_display = NULL;
                            char *orig_avatar = NULL;
                            char *orig_nip05_str = NULL;

                            nostr_json_get_string(profile_content, "display_name", &orig_display);
                            nostr_json_get_string(profile_content, "name", &orig_name);
                            nostr_json_get_string(profile_content, "picture", &orig_avatar);
                            nostr_json_get_string(profile_content, "nip05", &orig_nip05_str);

                            /* Update author display with original author */
                            gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row),
                                                             orig_display && *orig_display ? orig_display : orig_name,
                                                             orig_name,
                                                             orig_avatar);

                            /* Update IDs to use original note's pubkey for actions */
                            gnostr_note_card_row_set_ids(GNOSTR_NOTE_CARD_ROW(row),
                                                          reposted_id, root_id, (char*)orig_pubkey);

                            /* Update NIP-05 if available */
                            if (orig_nip05_str && *orig_nip05_str) {
                              gnostr_note_card_row_set_nip05(GNOSTR_NOTE_CARD_ROW(row),
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
            gchar *nostr_uri = g_strdup_printf("nostr:note1%s", reposted_id);
            g_signal_emit_by_name(row, "request-embed", nostr_uri);
            g_free(nostr_uri);
          }
          g_free(reposted_id);
        }
      }
    }

    /* NIP-57: Handle zap receipt events (kind 9735) */
    if (event_kind == 9735 && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      /* For zap receipts, mark them as such and set up the indicator */
      gnostr_note_card_row_set_is_zap_receipt(GNOSTR_NOTE_CARD_ROW(row), TRUE);

      /* Get zap stats from the event item if available */
      gint64 zap_total = gn_nostr_event_item_get_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj));

      /* Simple zap display - show "⚡ Zap" with amount if available */
      gnostr_note_card_row_set_zap_receipt_info(GNOSTR_NOTE_CARD_ROW(row),
                                                 pubkey,    /* sender is the event pubkey for now */
                                                 display,   /* sender name */
                                                 NULL,      /* recipient - parsed from tags if needed */
                                                 NULL,      /* recipient name */
                                                 NULL,      /* target event - parsed from tags if needed */
                                                 zap_total > 0 ? zap_total : 21000); /* Default 21 sats if unknown */
    }

    /* NIP-05: Set verification identifier for async verification badge */
    if (nip05 && *nip05 && pubkey && strlen(pubkey) == 64) {
      gnostr_note_card_row_set_nip05(GNOSTR_NOTE_CARD_ROW(row), nip05, pubkey);
    }

    /* NIP-51: Set bookmark state from local cache */
    if (id_hex && strlen(id_hex) == 64) {
      GnostrBookmarks *bookmarks = gnostr_bookmarks_get_default();
      if (bookmarks) {
        gboolean is_bookmarked = gnostr_bookmarks_is_bookmarked(bookmarks, id_hex);
        gnostr_note_card_row_set_bookmarked(GNOSTR_NOTE_CARD_ROW(row), is_bookmarked);
      }
    }

    /* NIP-09: Check if this is the current user's own note (enables delete option) */
    /* Also set login state for authentication-required buttons */
    gchar *user_pubkey = get_current_user_pubkey_hex();
    gboolean is_logged_in = (user_pubkey != NULL);
    gnostr_note_card_row_set_logged_in(GNOSTR_NOTE_CARD_ROW(row), is_logged_in);
    if (pubkey && strlen(pubkey) == 64 && user_pubkey) {
      gboolean is_own = (g_ascii_strcasecmp(pubkey, user_pubkey) == 0);
      gnostr_note_card_row_set_is_own_note(GNOSTR_NOTE_CARD_ROW(row), is_own);
    } else {
      gnostr_note_card_row_set_is_own_note(GNOSTR_NOTE_CARD_ROW(row), FALSE);
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
      guint zap_count = gn_nostr_event_item_get_zap_count(GN_NOSTR_EVENT_ITEM(obj));
      gint64 zap_total = gn_nostr_event_item_get_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj));

      /* nostrc-nke8: Skip expensive DB lookups and network fetches for off-screen items
       * Defer if: (1) fast scrolling OR (2) item position is outside visible range */
      guint item_position = gtk_list_item_get_position(item);
      gboolean is_visible = gnostr_timeline_view_is_item_visible(self, item_position);
      gboolean defer_metadata = self && (gnostr_timeline_view_is_fast_scrolling(self) || !is_visible);

      if (!defer_metadata) {
        /* If model doesn't have reaction data, fetch from local storage */
        if (like_count == 0 && id_hex && strlen(id_hex) == 64) {
          like_count = storage_ndb_count_reactions(id_hex);
          if (like_count > 0) {
            gn_nostr_event_item_set_like_count(GN_NOSTR_EVENT_ITEM(obj), like_count);
          }
        }

        /* Check if current user has liked this event (from local storage) */
        if (!is_liked && id_hex && strlen(id_hex) == 64 && user_pubkey) {
          is_liked = storage_ndb_user_has_reacted(id_hex, user_pubkey);
          if (is_liked) {
            gn_nostr_event_item_set_is_liked(GN_NOSTR_EVENT_ITEM(obj), is_liked);
          }
        }

        /* NIP-57: If model doesn't have zap data, fetch from local storage */
        if (zap_count == 0 && id_hex && strlen(id_hex) == 64) {
          guint fetched_count = 0;
          gint64 fetched_total = 0;
          if (storage_ndb_get_zap_stats(id_hex, &fetched_count, &fetched_total)) {
            if (fetched_count > 0) {
              gn_nostr_event_item_set_zap_count(GN_NOSTR_EVENT_ITEM(obj), fetched_count);
              gn_nostr_event_item_set_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj), fetched_total);
              zap_count = fetched_count;
              zap_total = fetched_total;
            }
          }
        }

        /* nostrc-lig9: Fetch reactions from post author's NIP-65 relays if we haven't already */
        if (self && id_hex && pubkey && strlen(id_hex) == 64 && strlen(pubkey) == 64) {
          fetch_reactions_from_author_relays(self, id_hex, pubkey);
        }

        gtk_widget_remove_css_class(row, "needs-metadata-refresh");
      } else {
        /* Mark for deferred refresh when scroll stops or item becomes visible */
        gtk_widget_add_css_class(row, "needs-metadata-refresh");
        g_debug("[SCROLL] Deferring metadata load for item position=%u (fast=%s visible=%s)",
                item_position,
                gnostr_timeline_view_is_fast_scrolling(self) ? "Y" : "N",
                is_visible ? "Y" : "N");
      }

      gnostr_note_card_row_set_like_count(GNOSTR_NOTE_CARD_ROW(row), like_count);
      gnostr_note_card_row_set_liked(GNOSTR_NOTE_CARD_ROW(row), is_liked);
      gnostr_note_card_row_set_zap_stats(GNOSTR_NOTE_CARD_ROW(row), zap_count, zap_total);
    }

    g_free(user_pubkey);

    /* Always show row - use fallback display if no profile */
    gtk_widget_set_visible(row, TRUE);

    /* Connect to profile change notification to update author when profile loads */
    if (!display && !handle) {
      /* Debug logging removed - too verbose */
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

  /* Debug logging removed - too verbose for per-item binding */
  g_free(display); g_free(handle); g_free(ts); g_free(content); g_free(root_id); g_free(id_hex);
  g_free(avatar_url); g_free(parent_id); g_free(parent_pubkey); g_free(nip05);

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

    /* NIP-25: Connect reaction count/state change handlers */
    g_signal_connect_object(obj, "notify::like-count",   G_CALLBACK(on_item_notify_like_count),   row, 0);
    g_signal_connect_object(obj, "notify::is-liked",     G_CALLBACK(on_item_notify_is_liked),     row, 0);

    /* NIP-57: Connect zap stats change handlers */
    g_signal_connect_object(obj, "notify::zap-count",      G_CALLBACK(on_item_notify_zap_count),      row, 0);
    g_signal_connect_object(obj, "notify::zap-total-msat", G_CALLBACK(on_item_notify_zap_total_msat), row, 0);
  }
}

static void setup_default_factory(GnostrTimelineView *self) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), self);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), self);
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

/* nostrc-y62r: Scroll position tracking */
#define FAST_SCROLL_THRESHOLD 2.0  /* pixels/ms - above this is "fast" scrolling */
#define SCROLL_IDLE_TIMEOUT_MS 150 /* ms of no scroll activity before marking idle */
#define ESTIMATED_ROW_HEIGHT 100   /* Estimated row height in pixels for range calculation */

/* Forward declaration for deferred metadata refresh */
static void refresh_visible_items_metadata(GnostrTimelineView *self);

static gboolean scroll_idle_timeout_cb(gpointer user_data) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(user_data);
  gboolean was_fast = self->is_fast_scrolling;
  self->is_fast_scrolling = FALSE;
  self->scroll_velocity = 0.0;
  self->scroll_idle_id = 0;
  g_debug("[SCROLL] Scroll idle - fast_scroll=FALSE");

  /* nostrc-nke8: When scroll stops after fast scrolling, refresh visible items */
  if (was_fast) {
    g_debug("[SCROLL] Triggering deferred metadata refresh for visible items");
    refresh_visible_items_metadata(self);
  }

  return G_SOURCE_REMOVE;
}

static void update_visible_range(GnostrTimelineView *self) {
  if (!self->root_scroller || !GTK_IS_SCROLLED_WINDOW(self->root_scroller))
    return;

  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
  if (!vadj) return;

  gdouble value = gtk_adjustment_get_value(vadj);
  gdouble page_size = gtk_adjustment_get_page_size(vadj);

  /* Get model item count */
  guint n_items = 0;
  if (self->selection_model) {
    GListModel *model = gtk_single_selection_get_model(GTK_SINGLE_SELECTION(self->selection_model));
    if (model) n_items = g_list_model_get_n_items(model);
  }

  if (n_items == 0) {
    self->visible_range_start = 0;
    self->visible_range_end = 0;
    return;
  }

  /* Estimate visible range based on scroll position and estimated row height */
  guint start_idx = (guint)(value / ESTIMATED_ROW_HEIGHT);
  guint visible_count = (guint)((page_size / ESTIMATED_ROW_HEIGHT) + 2); /* +2 for partial rows */

  self->visible_range_start = MIN(start_idx, n_items);
  self->visible_range_end = MIN(start_idx + visible_count, n_items);

  g_debug("[SCROLL] visible_range=[%u, %u) of %u items (value=%.0f page=%.0f)",
          self->visible_range_start, self->visible_range_end, n_items, value, page_size);
}

/* nostrc-nke8: Refresh metadata for visible items that were deferred during fast scroll */
static void refresh_visible_items_metadata(GnostrTimelineView *self) {
  if (!self || !self->selection_model) return;

  GListModel *model = gtk_single_selection_get_model(GTK_SINGLE_SELECTION(self->selection_model));
  if (!model) return;

  guint n_items = g_list_model_get_n_items(model);
  if (n_items == 0) return;

  /* Get current user pubkey for reaction checks */
  gchar *user_pubkey = get_current_user_pubkey_hex();

  /* Iterate visible range and load deferred metadata */
  guint refresh_count = 0;
  for (guint i = self->visible_range_start; i < self->visible_range_end && i < n_items; i++) {
    GObject *obj = g_list_model_get_item(model, i);
    if (!obj) continue;

    /* Only handle GnNostrEventItem */
    extern GType gn_nostr_event_item_get_type(void);
    if (!G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      g_object_unref(obj);
      continue;
    }

    /* Check if metadata was already loaded (like_count > 0 or zap_count > 0) */
    guint like_count = gn_nostr_event_item_get_like_count(GN_NOSTR_EVENT_ITEM(obj));
    guint zap_count = gn_nostr_event_item_get_zap_count(GN_NOSTR_EVENT_ITEM(obj));

    /* If metadata seems missing, fetch it */
    if (like_count == 0 || zap_count == 0) {
      gchar *id_hex = NULL;
      gchar *pubkey = NULL;
      g_object_get(obj, "event-id", &id_hex, "pubkey", &pubkey, NULL);

      if (id_hex && strlen(id_hex) == 64) {
        /* Fetch reaction count from local storage */
        if (like_count == 0) {
          like_count = storage_ndb_count_reactions(id_hex);
          if (like_count > 0) {
            gn_nostr_event_item_set_like_count(GN_NOSTR_EVENT_ITEM(obj), like_count);
          }
        }

        /* Check if user has liked */
        if (user_pubkey && !gn_nostr_event_item_get_is_liked(GN_NOSTR_EVENT_ITEM(obj))) {
          gboolean is_liked = storage_ndb_user_has_reacted(id_hex, user_pubkey);
          if (is_liked) {
            gn_nostr_event_item_set_is_liked(GN_NOSTR_EVENT_ITEM(obj), is_liked);
          }
        }

        /* Fetch zap stats from local storage */
        if (zap_count == 0) {
          guint fetched_count = 0;
          gint64 fetched_total = 0;
          if (storage_ndb_get_zap_stats(id_hex, &fetched_count, &fetched_total) && fetched_count > 0) {
            gn_nostr_event_item_set_zap_count(GN_NOSTR_EVENT_ITEM(obj), fetched_count);
            gn_nostr_event_item_set_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj), fetched_total);
          }
        }

        /* Trigger NIP-65 relay fetch for reactions */
        if (pubkey && strlen(pubkey) == 64) {
          fetch_reactions_from_author_relays(self, id_hex, pubkey);
        }

        refresh_count++;
      }

      g_free(id_hex);
      g_free(pubkey);
    }

    g_object_unref(obj);
  }

  g_free(user_pubkey);

  if (refresh_count > 0) {
    g_debug("[SCROLL] Refreshed metadata for %u deferred items in visible range [%u, %u)",
            refresh_count, self->visible_range_start, self->visible_range_end);
  }
}

static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data) {
  GnostrTimelineView *self = GNOSTR_TIMELINE_VIEW(user_data);

  gint64 now = g_get_monotonic_time();
  gdouble value = gtk_adjustment_get_value(adj);

  /* Calculate velocity */
  if (self->last_scroll_time > 0) {
    gint64 dt_us = now - self->last_scroll_time;
    if (dt_us > 0) {
      gdouble dt_ms = dt_us / 1000.0;
      gdouble dv = ABS(value - self->last_scroll_value);
      self->scroll_velocity = dv / dt_ms;
      self->is_fast_scrolling = (self->scroll_velocity > FAST_SCROLL_THRESHOLD);
    }
  }

  self->last_scroll_value = value;
  self->last_scroll_time = now;

  /* Update visible range */
  update_visible_range(self);

  /* Reset idle timeout */
  if (self->scroll_idle_id > 0) {
    g_source_remove(self->scroll_idle_id);
  }
  self->scroll_idle_id = g_timeout_add(SCROLL_IDLE_TIMEOUT_MS, scroll_idle_timeout_cb, self);

  g_debug("[SCROLL] value=%.0f velocity=%.2f px/ms fast=%s",
          value, self->scroll_velocity, self->is_fast_scrolling ? "YES" : "no");
}

static void gnostr_timeline_view_class_init(GnostrTimelineViewClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_timeline_view_dispose;
  gobj_class->finalize = gnostr_timeline_view_finalize;
  /* Ensure this widget can have children in templates by declaring a layout manager type */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
  /* Ensure GnTimelineTabs type is registered before loading template */
  g_type_ensure(GN_TYPE_TIMELINE_TABS);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, root_box);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, tabs);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, root_scroller);
  gtk_widget_class_bind_template_child(widget_class, GnostrTimelineView, list_view);

  /* Register signals */
  timeline_view_signals[SIGNAL_TAB_FILTER_CHANGED] =
    g_signal_new("tab-filter-changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_STRING);
}

static void gnostr_timeline_view_init(GnostrTimelineView *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->list_view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Timeline List", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->root_scroller),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Timeline Scroll", -1);
  /* Child widgets already have hexpand/vexpand in template */
  g_debug("timeline_view init: self=%p root_scroller=%p list_view=%p tabs=%p",
          (void*)self, (void*)self->root_scroller, (void*)self->list_view, (void*)self->tabs);
  setup_default_factory(self);

  /* Connect to tabs signals */
  if (self->tabs && GN_IS_TIMELINE_TABS(self->tabs)) {
    g_signal_connect(self->tabs, "tab-selected", G_CALLBACK(on_tabs_tab_selected), self);
  }

  /* nostrc-lig9: Initialize NIP-65 reaction fetch tracking */
  self->reaction_nip65_fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->reaction_cancellable = g_cancellable_new();

  /* nostrc-x8z3.1: Initialize batched NIP-65 fetch state */
  self->nip65_pending_authors = g_ptr_array_new_with_free_func(g_free);
  self->nip65_pending_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->nip65_batch_timeout_id = 0;

  /* nostrc-y62r: Connect scroll position tracking */
  if (self->root_scroller && GTK_IS_SCROLLED_WINDOW(self->root_scroller)) {
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->root_scroller));
    if (vadj) {
      g_signal_connect(vadj, "value-changed", G_CALLBACK(on_scroll_value_changed), self);
      g_debug("[SCROLL] Connected scroll tracking to vadj=%p", (void*)vadj);
    }
  }

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

GtkWidget *gnostr_timeline_view_get_list_view(GnostrTimelineView *self) {
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_VIEW(self), NULL);
  return self->list_view;
}

/* ============== Timeline Tabs Support (Phase 3) ============== */

GnTimelineTabs *gnostr_timeline_view_get_tabs(GnostrTimelineView *self) {
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_VIEW(self), NULL);
  return GN_TIMELINE_TABS(self->tabs);
}

void gnostr_timeline_view_set_tabs_visible(GnostrTimelineView *self, gboolean visible) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  if (self->tabs) {
    gtk_widget_set_visible(self->tabs, visible);
  }
}

void gnostr_timeline_view_add_hashtag_tab(GnostrTimelineView *self, const char *hashtag) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  g_return_if_fail(hashtag != NULL);

  if (!self->tabs) return;

  /* Show the tabs bar */
  gtk_widget_set_visible(self->tabs, TRUE);

  /* Format label with # prefix */
  char *label = g_strdup_printf("#%s", hashtag);
  guint index = gn_timeline_tabs_add_tab(GN_TIMELINE_TABS(self->tabs),
                                          GN_TIMELINE_TAB_HASHTAG,
                                          label,
                                          hashtag);
  g_free(label);

  /* Switch to the new tab */
  gn_timeline_tabs_set_selected(GN_TIMELINE_TABS(self->tabs), index);

  g_debug("timeline_view: added hashtag tab #%s at index %u", hashtag, index);
}

void gnostr_timeline_view_add_author_tab(GnostrTimelineView *self, const char *pubkey_hex, const char *display_name) {
  g_return_if_fail(GNOSTR_IS_TIMELINE_VIEW(self));
  g_return_if_fail(pubkey_hex != NULL);

  if (!self->tabs) return;

  /* Show the tabs bar */
  gtk_widget_set_visible(self->tabs, TRUE);

  /* Use display name or truncated pubkey as label */
  char *label;
  if (display_name && *display_name) {
    label = g_strdup(display_name);
  } else {
    label = g_strndup(pubkey_hex, 8);
  }

  guint index = gn_timeline_tabs_add_tab(GN_TIMELINE_TABS(self->tabs),
                                          GN_TIMELINE_TAB_AUTHOR,
                                          label,
                                          pubkey_hex);

  g_debug("timeline_view: added author tab '%s' at index %u", label, index);
  g_free(label);

  /* Switch to the new tab */
  gn_timeline_tabs_set_selected(GN_TIMELINE_TABS(self->tabs), index);
}

/* ============== nostrc-y62r: Scroll Position Tracking API ============== */

gboolean gnostr_timeline_view_get_visible_range(GnostrTimelineView *self,
                                                  guint *start,
                                                  guint *end) {
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_VIEW(self), FALSE);

  if (start) *start = self->visible_range_start;
  if (end) *end = self->visible_range_end;

  return (self->visible_range_end > self->visible_range_start);
}

gboolean gnostr_timeline_view_is_item_visible(GnostrTimelineView *self, guint index) {
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_VIEW(self), FALSE);

  return (index >= self->visible_range_start && index < self->visible_range_end);
}

gboolean gnostr_timeline_view_is_fast_scrolling(GnostrTimelineView *self) {
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_VIEW(self), FALSE);

  return self->is_fast_scrolling;
}

gdouble gnostr_timeline_view_get_scroll_velocity(GnostrTimelineView *self) {
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_VIEW(self), 0.0);

  return self->scroll_velocity;
}
