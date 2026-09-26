/* nsp-engine.c — see nsp-engine.h. */
#include "nsp-engine.h"

#include <string.h>

#include "nd-event.h"
#include "nsp-item.h"
#include "nsp-query.h"
#include "nsp-text.h"

#define DEADLINE_MARGIN_US (20 * 1000)   /* relay ops end just before we answer */
#define MIN_PHASE_US (60 * 1000)         /* don't start a follow-up REQ with less */
#define MAX_AUTHOR_LOOKUP 20

struct _NspEngine {
  gint rc;
  gboolean disposed;
  char *socket_path;
  guint deadline_ms, debounce_ms;
  gboolean resolve_nip05;
  NspAvatars *avatars;
  GHashTable *items;    /* uri -> NspItem* (owned) */
  GHashTable *profiles; /* pubkey -> uri of the newest kind 0 (owned str) */
  GHashTable *verified; /* pubkey -> NIP-05 address resolved to it this session */
  NspNip05 *nip05;
  guint gen, seq;
  GPtrArray *inflight;  /* SearchData* (borrowed) */
  /* circuit breaker for a relay that never answers */
  gboolean relay_ever_eose;
  guint consecutive_timeouts;
  gint64 circuit_until;
};

typedef struct SearchData SearchData;
typedef void (*ReplyFn)(SearchData *d, NspRelayReply *r);

struct SearchData {
  gint rc;
  NspEngine *e; /* strong ref */
  GTask *task;
  NspQuery *q;
  gint64 start, deadline;
  GCancellable *cancel;
  guint deadline_id;
  guint pending;
  guint seq;
  gboolean finished;
  GPtrArray *found; /* uri strings (owned), insertion order */
  GHashTable *found_set;
  char *resolved_pk;
  NspSearchStats stats;
};

static void engine_unref(NspEngine *e) {
  if (--e->rc > 0) return;
  g_free(e->socket_path);
  g_hash_table_destroy(e->items);
  g_hash_table_destroy(e->profiles);
  g_hash_table_destroy(e->verified);
  g_ptr_array_unref(e->inflight);
  g_free(e);
}

NspEngine *nsp_engine_new(const NspEngineOptions *o) {
  NspEngine *e = g_new0(NspEngine, 1);
  e->rc = 1;
  e->socket_path = o && o->socket_path ? g_strdup(o->socket_path) : nsp_relay_default_socket_path();
  e->deadline_ms = o && o->deadline_ms ? o->deadline_ms : NSP_DEADLINE_MS_DEFAULT;
  e->debounce_ms = o && o->nip05_debounce_ms ? o->nip05_debounce_ms : NSP_NIP05_DEBOUNCE_MS_DEFAULT;
  if (e->debounce_ms == G_MAXUINT) e->debounce_ms = 0;
  e->resolve_nip05 = o ? o->resolve_nip05 : FALSE;
  e->avatars = o ? o->avatars : NULL;
  e->items = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)nsp_item_free);
  e->profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  e->verified = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  e->nip05 = nsp_nip05_new();
  e->inflight = g_ptr_array_new();
  return e;
}

NspNip05 *nsp_engine_get_nip05(NspEngine *e) { return e->nip05; }

static void search_finish(SearchData *d);

void nsp_engine_cancel_all(NspEngine *e) {
  while (e->inflight->len > 0) search_finish(e->inflight->pdata[0]);
}

void nsp_engine_free(NspEngine *e) {
  if (!e) return;
  nsp_engine_cancel_all(e);
  e->disposed = TRUE;
  g_clear_pointer(&e->nip05, nsp_nip05_free);
  engine_unref(e);
}

/* ---- store ---------------------------------------------------------- */

static NspItem *store_item(NspEngine *e, NspItem *it /* owned */) {
  NspItem *old = g_hash_table_lookup(e->items, it->uri);
  if (old && !old->bare && (it->bare || old->created_at >= it->created_at)) {
    nsp_item_free(it);
    return old;
  }
  if (old) it->gen = old->gen;
  g_hash_table_replace(e->items, it->uri, it);
  if (!it->bare && it->kind == 0)
    g_hash_table_replace(e->profiles, g_strdup(it->pubkey_hex), g_strdup(it->uri));
  return it;
}

static NspItem *store_event(NspEngine *e, const NdEvent *ev) {
  NspItem *it = nsp_item_new_from_event(ev);
  return it ? store_item(e, it) : NULL;
}

static NspItem *profile_of(NspEngine *e, const char *pk) {
  const char *uri = pk ? g_hash_table_lookup(e->profiles, pk) : NULL;
  return uri ? g_hash_table_lookup(e->items, uri) : NULL;
}

static void prune_store(NspEngine *e) {
  if (g_hash_table_size(e->items) <= NSP_STORE_MAX) return;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, e->items);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    NspItem *item = v;
    if (item->gen + 2 >= e->gen) continue;
    if (!item->bare && item->kind == 0) {
      const char *cur = g_hash_table_lookup(e->profiles, item->pubkey_hex);
      if (g_strcmp0(cur, item->uri) == 0) g_hash_table_remove(e->profiles, item->pubkey_hex);
    }
    g_hash_table_iter_remove(&it);
  }
}

/* ---- search plumbing ------------------------------------------------ */

static SearchData *sd_ref(SearchData *d) {
  d->rc++;
  return d;
}

static void sd_unref(SearchData *d) {
  if (--d->rc > 0) return;
  g_clear_object(&d->task);
  nsp_query_free(d->q);
  g_clear_object(&d->cancel);
  g_ptr_array_unref(d->found);
  g_hash_table_destroy(d->found_set);
  g_free(d->resolved_pk);
  engine_unref(d->e);
  g_free(d);
}

static void op_begin(SearchData *d) {
  d->pending++;
  sd_ref(d);
}

static void op_end(SearchData *d) {
  d->pending--;
  if (d->pending == 0 && !d->finished) search_finish(d);
  sd_unref(d);
}

static gboolean live(SearchData *d) {
  return !d->finished && !d->e->disposed;
}

static gboolean time_left(SearchData *d, gint64 min_us) {
  return d->deadline - DEADLINE_MARGIN_US - g_get_monotonic_time() >= min_us;
}

static void found_add(SearchData *d, NspItem *it) {
  if (!it || g_hash_table_contains(d->found_set, it->uri)) return;
  char *uri = g_strdup(it->uri);
  g_ptr_array_add(d->found, uri);
  g_hash_table_add(d->found_set, uri);
}

static void record_health(NspEngine *e, const NspRelayReply *r) {
  switch (r->status) {
  case NSP_RELAY_EOSE:
    e->relay_ever_eose = TRUE;
    /* fall through */
  case NSP_RELAY_CLOSED:
    e->consecutive_timeouts = 0;
    e->circuit_until = 0;
    break;
  case NSP_RELAY_TIMEOUT:
    if (r->events->len > 0) break;
    if (++e->consecutive_timeouts >= (e->relay_ever_eose ? 3u : 1u)) {
      e->circuit_until = g_get_monotonic_time() + (gint64)NSP_CIRCUIT_OPEN_S * G_USEC_PER_SEC;
      g_message("nostr-search-provider: session relay does not answer REQs (no EOSE); "
                "skipping it for %d s", NSP_CIRCUIT_OPEN_S);
    }
    break;
  default:
    break;
  }
}

typedef struct {
  SearchData *d;
  ReplyFn fn;
} ReplyCtx;

static void on_reply(GObject *src, GAsyncResult *res, gpointer user_data) {
  ReplyCtx *c = user_data;
  SearchData *d = c->d;
  g_autoptr(NspRelayReply) r = nsp_relay_query_finish(res);
  if (!d->e->disposed) record_health(d->e, r);
  if (d->stats.requests == 1 && d->stats.primary == (NspRelayStatus)-1) d->stats.primary = r->status;
  if (live(d)) c->fn(d, r);
  g_free(c);
  op_end(d);
}

static void issue(SearchData *d, JsonNode *filters, guint max_events, ReplyFn fn) {
  if (!filters) return;
  if (!live(d)) {
    json_node_unref(filters);
    return;
  }
  d->stats.requests++;
  op_begin(d);
  ReplyCtx *c = g_new0(ReplyCtx, 1);
  c->d = d;
  c->fn = fn;
  nsp_relay_query_async(d->e->socket_path, filters, d->deadline - DEADLINE_MARGIN_US, max_events,
                        d->cancel, on_reply, c);
  json_node_unref(filters);
}

/* ---- phases ---------------------------------------------------------- */

static void on_authors(SearchData *d, NspRelayReply *r) {
  for (guint i = 0; i < r->events->len; i++) {
    NdEvent *ev = r->events->pdata[i];
    if (ev->kind == 0) store_event(d->e, ev);
  }
}

static void fetch_missing_authors(SearchData *d) {
  if (!time_left(d, MIN_PHASE_US)) return;
  g_autoptr(GPtrArray) pks = g_ptr_array_new();
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < d->found->len && pks->len < MAX_AUTHOR_LOOKUP; i++) {
    NspItem *it = g_hash_table_lookup(d->e->items, d->found->pdata[i]);
    if (!it || nsp_item_is_profile(it) || !it->pubkey_hex) continue;
    if (profile_of(d->e, it->pubkey_hex) || !g_hash_table_add(seen, it->pubkey_hex)) continue;
    g_ptr_array_add(pks, it->pubkey_hex);
  }
  if (pks->len == 0) return;
  g_ptr_array_add(pks, NULL);
  issue(d, nsp_filters_profiles((const char *const *)pks->pdata), MAX_AUTHOR_LOOKUP, on_authors);
}

static void add_bare(SearchData *d, const NdTarget *t, const char *nip05) {
  NspItem *it = nsp_item_new_bare(t, nip05);
  if (it) found_add(d, store_item(d->e, it));
}

static void on_identifier(SearchData *d, NspRelayReply *r) {
  NspQuery *q = d->q;
  for (guint i = 0; i < r->events->len; i++) {
    NdEvent *ev = r->events->pdata[i];
    gboolean match = q->type == NSP_QUERY_HEX
                         ? (g_strcmp0(ev->id_hex, q->hex) == 0 ||
                            (ev->kind == 0 && g_strcmp0(ev->pubkey_hex, q->hex) == 0))
                         : nd_event_matches_target(ev, q->target);
    NspItem *it = match ? store_event(d->e, ev) : NULL;
    found_add(d, it);
  }
  if (d->found->len == 0 && q->target) add_bare(d, q->target, NULL);
  fetch_missing_authors(d);
}

static void on_nip05_profile(SearchData *d, NspRelayReply *r) {
  for (guint i = 0; i < r->events->len; i++) {
    NdEvent *ev = r->events->pdata[i];
    if (ev->kind == 0 && g_strcmp0(ev->pubkey_hex, d->resolved_pk) == 0)
      found_add(d, store_event(d->e, ev));
  }
  if (!profile_of(d->e, d->resolved_pk)) {
    NdTarget t = {.entity = ND_ENTITY_PROFILE, .pubkey_hex = d->resolved_pk, .kind = 0};
    add_bare(d, &t, d->q->nip05);
  } else {
    found_add(d, profile_of(d->e, d->resolved_pk));
  }
}

static void on_nip05_claim(SearchData *d, NspRelayReply *r) {
  for (guint i = 0; i < r->events->len; i++) {
    NdEvent *ev = r->events->pdata[i];
    if (ev->kind != 0) continue;
    NspItem *it = store_event(d->e, ev);
    if (it && g_strcmp0(it->nip05, d->q->nip05) == 0) found_add(d, it);
  }
}

static void use_resolved(SearchData *d, const char *pk) {
  g_free(d->resolved_pk);
  d->resolved_pk = g_strdup(pk);
  g_hash_table_replace(d->e->verified, g_strdup(pk), g_strdup(d->q->nip05));
  if (!d->stats.circuit_open && time_left(d, MIN_PHASE_US)) {
    const char *pks[] = {pk, NULL};
    issue(d, nsp_filters_profiles(pks), 1, on_nip05_profile);
  } else {
    NspRelayReply empty = {.events = g_ptr_array_new()};
    on_nip05_profile(d, &empty);
    g_ptr_array_unref(empty.events);
  }
}

static void on_resolved(GObject *src, GAsyncResult *res, gpointer user_data) {
  SearchData *d = user_data;
  g_autoptr(GError) err = NULL;
  g_autofree char *pk = nsp_nip05_resolve_finish(res, &err);
  if (pk && !d->e->disposed)
    g_hash_table_replace(d->e->verified, g_strdup(pk), g_strdup(d->q->nip05));
  if (pk && live(d)) use_resolved(d, pk);
  op_end(d);
}

static gboolean nip05_debounce_cb(gpointer user_data) {
  SearchData *d = user_data;
  /* Only resolve what the user stopped typing at: a newer search means
   * this address was an intermediate keystroke (privacy + load). */
  if (live(d) && d->seq == d->e->seq && d->e->nip05) {
    op_begin(d);
    nsp_nip05_resolve_async(d->e->nip05, d->q->nip05_local, d->q->nip05_domain, on_resolved, d);
  }
  op_end(d);
  return G_SOURCE_REMOVE;
}

static void text_admit(SearchData *d, NspRelayReply *r, gboolean client_match) {
  for (guint i = 0; i < r->events->len; i++) {
    NdEvent *ev = r->events->pdata[i];
    if (ev->kind != 0 && ev->kind != 1 && ev->kind != 30023) continue;
    NspItem *it = store_event(d->e, ev);
    if (!it) continue;
    if (!client_match || nsp_text_match_all(it->haystack, (const char *const *)d->q->words))
      found_add(d, it);
  }
}

static void on_text_scan(SearchData *d, NspRelayReply *r) {
  text_admit(d, r, TRUE);
  fetch_missing_authors(d);
}

static void on_text(SearchData *d, NspRelayReply *r) {
  if (r->status == NSP_RELAY_CLOSED && r->closed_reason &&
      g_str_has_prefix(r->closed_reason, "unsupported")) {
    d->stats.fallback_scan = TRUE;
    if (time_left(d, MIN_PHASE_US))
      issue(d, nsp_filters_text_scan(), NSP_SCAN_PROFILE_LIMIT + NSP_SCAN_NOTE_LIMIT, on_text_scan);
    return;
  }
  text_admit(d, r, FALSE);
  fetch_missing_authors(d);
}

/* ---- ranking / completion -------------------------------------------- */

typedef struct {
  NspItem *it;
  guint idx;
  gboolean verified;
} Ranked;

static gint rank_cmp(gconstpointer a, gconstpointer b) {
  const Ranked *x = a, *y = b;
  gboolean px = nsp_item_is_profile(x->it), py = nsp_item_is_profile(y->it);
  if (px != py) return px ? -1 : 1;
  if (px) {
    if (x->verified != y->verified) return x->verified ? -1 : 1;
    if (x->it->bare != y->it->bare) return x->it->bare ? 1 : -1;
  } else if (x->it->created_at != y->it->created_at) {
    return x->it->created_at > y->it->created_at ? -1 : 1;
  }
  return x->idx < y->idx ? -1 : x->idx > y->idx;
}

static gboolean is_verified(NspEngine *e, const NspItem *it) {
  if (!it->nip05 || !it->pubkey_hex) return FALSE;
  return g_strcmp0(g_hash_table_lookup(e->verified, it->pubkey_hex), it->nip05) == 0;
}

static void search_finish(SearchData *d) {
  if (d->finished) return;
  d->finished = TRUE;
  NspEngine *e = d->e;
  if (d->deadline_id) g_source_remove(d->deadline_id);
  d->deadline_id = 0;
  g_cancellable_cancel(d->cancel); /* end outstanding relay ops now */
  g_ptr_array_remove(e->inflight, d);

  GArray *ranked = g_array_new(FALSE, FALSE, sizeof(Ranked));
  for (guint i = 0; i < d->found->len; i++) {
    NspItem *it = g_hash_table_lookup(e->items, d->found->pdata[i]);
    if (!it) continue;
    Ranked r = {it, i, is_verified(e, it)};
    g_array_append_val(ranked, r);
  }
  if (!d->stats.narrowed) g_array_sort(ranked, rank_cmp);
  guint n = MIN(ranked->len, NSP_MAX_RESULTS);
  char **ids = g_new0(char *, n + 1);
  e->gen++;
  for (guint i = 0; i < n; i++) {
    NspItem *it = g_array_index(ranked, Ranked, i).it;
    ids[i] = g_strdup(it->uri);
    it->gen = e->gen;
    NspItem *author = nsp_item_is_profile(it) ? NULL : profile_of(e, it->pubkey_hex);
    if (author) author->gen = e->gen;
  }
  g_array_unref(ranked);
  prune_store(e);

  d->stats.elapsed_us = g_get_monotonic_time() - d->start;
  if (d->stats.primary == (NspRelayStatus)-1) d->stats.primary = NSP_RELAY_UNAVAILABLE;
  NspSearchStats *st = g_new(NspSearchStats, 1);
  *st = d->stats;
  g_task_set_task_data(d->task, st, g_free);
  g_task_return_pointer(d->task, ids, (GDestroyNotify)g_strfreev);
  g_debug("nostr-search-provider: %s search: %u result(s), %u REQ(s), %" G_GINT64_FORMAT
          " ms%s%s%s",
          nsp_query_type_name(d->q->type), n, d->stats.requests, d->stats.elapsed_us / 1000,
          d->stats.deadline_hit ? ", deadline" : "", d->stats.fallback_scan ? ", scan" : "",
          d->stats.circuit_open ? ", relay skipped" : "");
  sd_unref(d); /* the "search" ref */
}

static gboolean deadline_cb(gpointer user_data) {
  SearchData *d = user_data;
  d->deadline_id = 0;
  d->stats.deadline_hit = TRUE;
  search_finish(d);
  return G_SOURCE_REMOVE;
}

static void start(SearchData *d, const char *const *previous) {
  NspEngine *e = d->e;
  NspQuery *q = d->q;

  if (q->type == NSP_QUERY_NONE) return;

  if (previous && q->type == NSP_QUERY_TEXT) {
    for (const char *const *p = previous; *p; p++) {
      NspItem *it = g_hash_table_lookup(e->items, *p);
      if (it && nsp_text_match_all(it->haystack, (const char *const *)q->words)) found_add(d, it);
    }
    if (d->found->len > 0) {
      d->stats.narrowed = TRUE;
      return;
    }
  }

  gboolean relay_ok = g_get_monotonic_time() >= e->circuit_until;
  d->stats.circuit_open = !relay_ok;

  switch (q->type) {
  case NSP_QUERY_PROFILE:
  case NSP_QUERY_EVENT:
  case NSP_QUERY_ADDRESS:
  case NSP_QUERY_HEX:
    if (relay_ok)
      issue(d, nsp_query_filters(q), 4, on_identifier);
    else if (q->target)
      add_bare(d, q->target, NULL);
    break;
  case NSP_QUERY_NIP05: {
    char pk[65];
    gboolean negative = FALSE;
    if (nsp_nip05_lookup_cached(e->nip05, q->nip05, pk, &negative)) {
      use_resolved(d, pk);
      break;
    }
    if (relay_ok) issue(d, nsp_query_filters(q), NSP_NIP05_CLAIM_LIMIT, on_nip05_claim);
    if (e->resolve_nip05 && !negative) {
      op_begin(d);
      if (e->debounce_ms)
        g_timeout_add(e->debounce_ms, nip05_debounce_cb, d);
      else
        nip05_debounce_cb(d);
    }
    break;
  }
  case NSP_QUERY_TEXT:
    if (relay_ok) issue(d, nsp_query_filters(q), NSP_TEXT_SEARCH_LIMIT, on_text);
    break;
  default:
    break;
  }
}

void nsp_engine_search_async(NspEngine *e, const char *const *terms,
                             const char *const *previous, GAsyncReadyCallback callback,
                             gpointer user_data) {
  SearchData *d = g_new0(SearchData, 1);
  d->rc = 1; /* the "search" ref, dropped by search_finish */
  e->rc++;
  d->e = e;
  d->task = g_task_new(NULL, NULL, callback, user_data);
  g_task_set_source_tag(d->task, nsp_engine_search_async);
  d->q = nsp_query_classify(terms);
  d->start = g_get_monotonic_time();
  d->deadline = d->start + (gint64)e->deadline_ms * 1000;
  d->cancel = g_cancellable_new();
  d->found = g_ptr_array_new_with_free_func(g_free);
  d->found_set = g_hash_table_new(g_str_hash, g_str_equal);
  d->seq = ++e->seq;
  d->stats.primary = (NspRelayStatus)-1;
  g_ptr_array_add(e->inflight, d);

  op_begin(d); /* guard: nothing may finish the search while start() runs */
  start(d, previous);
  if (!d->finished && d->pending > 1)
    d->deadline_id = g_timeout_add((guint)e->deadline_ms, deadline_cb, d);
  op_end(d);
}

char **nsp_engine_search_finish(GAsyncResult *res, NspSearchStats *stats_out) {
  GTask *task = G_TASK(res);
  if (stats_out) {
    NspSearchStats *st = g_task_get_task_data(task);
    if (st) *stats_out = *st;
  }
  char **ids = g_task_propagate_pointer(task, NULL);
  return ids ? ids : g_new0(char *, 1);
}

/* ---- metas ------------------------------------------------------------ */

static GIcon *fallback_icon(const NspItem *it) {
  return g_themed_icon_new_with_default_fallbacks(nsp_item_is_profile(it) ? "avatar-default-symbolic"
                                                                          : "text-x-generic-symbolic");
}

GVariant *nsp_engine_result_metas(NspEngine *e, const char *const *ids) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("aa{sv}"));
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  for (const char *const *id = ids; id && *id; id++) {
    NspItem *it = g_hash_table_lookup(e->items, *id);
    if (!it) continue;
    NspItem *author = nsp_item_is_profile(it) ? NULL : profile_of(e, it->pubkey_hex);
    g_autofree char *name = NULL, *desc = NULL;
    nsp_item_meta_text(it, author, is_verified(e, it), now, &name, &desc);
    const char *picture = nsp_item_is_profile(it) ? it->picture : author ? author->picture : NULL;
    g_autoptr(GIcon) icon = picture ? nsp_avatars_lookup(e->avatars, picture) : NULL;
    if (!icon) icon = fallback_icon(it);

    g_variant_builder_open(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "id", g_variant_new_string(it->uri));
    g_variant_builder_add(&b, "{sv}", "name", g_variant_new_string(name));
    g_variant_builder_add(&b, "{sv}", "description", g_variant_new_string(desc));
    GVariant *ser = g_icon_serialize(icon);
    if (ser) {
      g_variant_builder_add(&b, "{sv}", "icon", ser);
      g_variant_unref(ser);
    }
    g_variant_builder_add(&b, "{sv}", "clipboardText", g_variant_new_string(it->uri));
    g_variant_builder_close(&b);
  }
  return g_variant_builder_end(&b);
}
