#include "nostr-negentropy-ndb.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/*
 * Build-time guard: the actual implementation is only compiled when the
 * nostrdb dependency is available and this target defines NIP77_HAVE_NOSTRDB.
 * Otherwise, we compile a small stub so IDEs don't error when linting.
 */
#ifdef NIP77_HAVE_NOSTRDB

/* Include nostrdb public API; include dirs are provided by the 'nostrdb' target. */
#include <nostrdb.h>

/*
 * In-memory materialization strategy:
 * - On begin_iter(): start a read txn, run a broad query that matches all notes,
 *   copy out (created_at, id) into a growable array, then sort by
 *   (created_at ASC, id ASC). Next()/end_iter() iterate and clean up.
 */

struct ndb_ctx {
  char *db_path;
  struct ndb *db;              /* owned */
  struct ndb_txn txn;          /* live during iteration */
  struct ndb_filter filter;    /* empty filter -> match all */
  int filter_inited;

  /* Materialized and sorted items */
  NostrIndexItem *items;
  size_t items_len;
  size_t items_cap;
  size_t it; /* iteration cursor */
};

static int cmp_index_item(const void *a, const void *b) {
  const NostrIndexItem *ia = (const NostrIndexItem *)a;
  const NostrIndexItem *ib = (const NostrIndexItem *)b;
  if (ia->created_at < ib->created_at) return -1;
  if (ia->created_at > ib->created_at) return 1;
  return memcmp(ia->id.bytes, ib->id.bytes, 32);
}

static int ensure_items_cap(struct ndb_ctx *c, size_t need) {
  if (need <= c->items_cap) return 0;
  size_t ncap = c->items_cap ? c->items_cap * 2 : 1024;
  while (ncap < need) ncap *= 2;
  void *tmp = realloc(c->items, ncap * sizeof(*c->items));
  if (!tmp) return -1;
  c->items = (NostrIndexItem *)tmp; c->items_cap = ncap; return 0;
}

static void items_reset(struct ndb_ctx *c) {
  if (!c) return;
  free(c->items); c->items = NULL; c->items_len = c->items_cap = 0; c->it = 0;
}

static int ds_begin(void *ctx) {
  struct ndb_ctx *c = (struct ndb_ctx *)ctx;
  if (!c || !c->db) return -1;

  /* Start read transaction */
  if (ndb_begin_query(c->db, &c->txn) != 0) {
    return -1;
  }

  /* Build an empty filter that matches everything */
  memset(&c->filter, 0, sizeof(c->filter));
  if (ndb_filter_init(&c->filter) != 0) {
    (void)ndb_end_query(&c->txn);
    return -1;
  }
  if (ndb_filter_end(&c->filter) != 0) {
    ndb_filter_destroy(&c->filter);
    (void)ndb_end_query(&c->txn);
    return -1;
  }
  c->filter_inited = 1;

  /* Query results: try to pull as many as we reasonably can in one shot. */
  struct ndb_query_result *results = NULL;
  int cap = 2048;
  results = (struct ndb_query_result *)malloc((size_t)cap * sizeof(*results));
  if (!results) {
    ndb_filter_destroy(&c->filter); c->filter_inited = 0;
    (void)ndb_end_query(&c->txn);
    return -1;
  }
  int count = 0;
  int rc = ndb_query(&c->txn, &c->filter, 1, results, cap, &count);
  if (rc != 0) {
    free(results);
    ndb_filter_destroy(&c->filter); c->filter_inited = 0;
    (void)ndb_end_query(&c->txn);
    return -1;
  }

  /* Materialize and sort */
  if (count > 0) {
    if (ensure_items_cap(c, (size_t)count) != 0) {
      free(results);
      ndb_filter_destroy(&c->filter); c->filter_inited = 0;
      (void)ndb_end_query(&c->txn);
      return -1;
    }
    for (int i = 0; i < count; ++i) {
      struct ndb_note *note = results[i].note;
      if (!note) continue;
      NostrIndexItem *dst = &c->items[c->items_len++];
      dst->created_at = (uint64_t)ndb_note_created_at(note);
      unsigned char *id = ndb_note_id(note);
      if (id) memcpy(dst->id.bytes, id, 32); else memset(dst->id.bytes, 0, 32);
    }
    qsort(c->items, c->items_len, sizeof(*c->items), cmp_index_item);
  }

  free(results);
  c->it = 0;
  return 0;
}

static int ds_next(void *ctx, NostrIndexItem *out) {
  struct ndb_ctx *c = (struct ndb_ctx *)ctx;
  if (!c || !out) return -1;
  if (c->it >= c->items_len) return 1; /* done */
  *out = c->items[c->it++];
  return 0;
}

static void ds_end(void *ctx) {
  struct ndb_ctx *c = (struct ndb_ctx *)ctx;
  if (!c) return;
  items_reset(c);
  if (c->filter_inited) {
    ndb_filter_destroy(&c->filter);
    c->filter_inited = 0;
  }
  (void)ndb_end_query(&c->txn);
}

int nostr_ndb_make_datasource(const char *db_path, NostrNegDataSource *out) {
  if (!out || !db_path) return -1;

  struct ndb_ctx *ctx = (struct ndb_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) return -1;

  /* Save DB path for potential future use */
  size_t dlen = strlen(db_path);
  ctx->db_path = (char *)malloc(dlen + 1);
  if (!ctx->db_path) { free(ctx); return -1; }
  memcpy(ctx->db_path, db_path, dlen + 1);

  /* Ensure directory exists */
  if (mkdir(db_path, 0700) != 0) {
    if (errno != EEXIST) {
      fprintf(stderr, "nostr_ndb_make_datasource: mkdir('%s') failed: %s\n", db_path, strerror(errno));
      free(ctx->db_path); free(ctx);
      return -1;
    }
  }

  /* Initialize/open the database */
  struct ndb_config cfg;
  ndb_default_config(&cfg);
  /* Use safe test-friendly config */
  ndb_config_set_flags(&cfg, NDB_FLAG_NO_FULLTEXT | NDB_FLAG_NO_NOTE_BLOCKS | NDB_FLAG_NO_STATS);
  /* Set a modest mapsize (e.g., 64MB) suitable for tests */
  ndb_config_set_mapsize(&cfg, 64ull * 1024ull * 1024ull);
  /* Keep defaults; allow missing optional subsystems. */
  if (ndb_init(&ctx->db, db_path, &cfg) != 0) {
    fprintf(stderr, "nostr_ndb_make_datasource: ndb_init('%s') failed (see NostrDB logs if any)\n", db_path);
    free(ctx->db_path); free(ctx);
    return -1;
  }

  out->ctx = ctx;
  out->begin_iter = ds_begin;
  out->next = ds_next;
  out->end_iter = ds_end;
  return 0;
}

#else /* !NIP77_HAVE_NOSTRDB */

int nostr_ndb_make_datasource(const char *db_path, NostrNegDataSource *out) {
  (void)db_path; (void)out;
  return -1; /* nostrdb backend not available at build time */
}

#endif /* NIP77_HAVE_NOSTRDB */
