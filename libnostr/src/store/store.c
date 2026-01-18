#include <stdlib.h>
#include "libnostr_store.h"
#include "store_int.h"

int ln_store_ingest_event_json(ln_store *s, const char *json, int len, const char *relay)
{
  if (!s || !s->ops || !s->ops->ingest_event_json) return LN_ERR_INGEST;
  return s->ops->ingest_event_json(s, json, len, relay);
}

int ln_store_ingest_ldjson(ln_store *s, const char *ldjson, size_t len, const char *relay)
{
  if (!s || !s->ops || !s->ops->ingest_ldjson) return LN_ERR_INGEST;
  return s->ops->ingest_ldjson(s, ldjson, len, relay);
}

int ln_store_begin_query(ln_store *s, void **txn)
{
  if (!txn) return LN_ERR_DB_TXN;
  if (!s || !s->ops || !s->ops->begin_query) return LN_ERR_DB_TXN;
  return s->ops->begin_query(s, txn);
}

int ln_store_end_query(ln_store *s, void *txn)
{
  if (!s || !s->ops || !s->ops->end_query) return LN_ERR_DB_TXN;
  return s->ops->end_query(s, txn);
}

int ln_store_query(ln_store *s, void *txn, const char *filters_json, void **results, int *count)
{
  if (!s || !s->ops || !s->ops->query) return LN_ERR_QUERY;
  return s->ops->query(s, txn, filters_json, results, count);
}

int ln_store_text_search(ln_store *s, void *txn, const char *query, const char *config_json, void **results, int *count)
{
  if (!s || !s->ops || !s->ops->text_search) return LN_ERR_TEXTSEARCH;
  return s->ops->text_search(s, txn, query, config_json, results, count);
}

int ln_store_get_note_by_id(ln_store *s, void *txn, const unsigned char id[32], const char **json, int *json_len)
{
  if (!s || !s->ops || !s->ops->get_note_by_id) return LN_ERR_QUERY;
  return s->ops->get_note_by_id(s, txn, id, json, json_len);
}

int ln_store_get_profile_by_pubkey(ln_store *s, void *txn, const unsigned char pk[32], const char **json, int *json_len)
{
  if (!s || !s->ops || !s->ops->get_profile_by_pubkey) return LN_ERR_QUERY;
  return s->ops->get_profile_by_pubkey(s, txn, pk, json, json_len);
}

int ln_store_stat_json(ln_store *s, char **json_out)
{
  if (!json_out) return LN_ERR_QUERY;
  if (!s || !s->ops || !s->ops->stat_json) return LN_ERR_QUERY;
  return s->ops->stat_json(s, json_out);
}

void *ln_store_get_backend_handle(ln_store *s)
{
  if (!s || !s->impl) return NULL;
  /* For nostrdb backend, impl is struct ln_ndb_impl which has db pointer.
   * We return the impl and let the caller cast/access as needed. */
  return s->impl;
}
