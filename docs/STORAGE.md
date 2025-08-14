# NostrdB Storage Backend

This document describes how to use the NostrdB-backed `ln_store` implementation in libnostr.

## Opening the store

- API: `ln_store_open(ln_store **out, const char *path, const char *opts_json)`
- `path`: database directory.
- `opts_json` keys (all optional):
  - `mapsize` (size_t as integer): LMDB map size. Default ~8 GiB.
  - `flags` (int): `NDB_FLAG_*` bitmask; defaults to 0.
  - `ingester_threads` (int): worker threads; default 1.
  - `writer_scratch_buffer_size` (int): writer scratch size (bytes); default 2 MiB.
  - `ingest_skip_validation` (int): when >0, installs an ingest filter that skips ID/signature validation. Useful for demos/tests with placeholder events.

Example:
```c
const char *opts = "{\"mapsize\":1073741824,\"ingester_threads\":1}"; // 1 GiB
ln_store *store = NULL;
ln_store_open(&store, ".ndb-demo", opts);
```

## Ingest

- Single event JSON: `ln_store_ingest_event_json(store, json, -1, relay_opt)`
- LDJSON stream: `ln_store_ingest_ldjson(store, buf, len, relay_opt)`

## Queries

- Start/End txn: `ln_store_begin_query`, `ln_store_end_query`
- Filters: `filters_json` may be a single object or an array of objects. Up to 16 filters are accepted and composed as a filter group.
- Result: `char **results` of length `count`; each element is a heap-allocated NUL-terminated JSON string. You must free each string and then the array.

Example:
```c
void *txn = NULL; ln_store_begin_query(store, &txn);
const char *filters = "[{\"kinds\":[1],\"limit\":5},{\"kinds\":[6],\"limit\":5}]";
void *results = NULL; int count = 0;
int rc = ln_store_query(store, txn, filters, &results, &count);
// ... use results ...
for (int i=0;i<count;i++) free(((char**)results)[i]);
free(results);
ln_store_end_query(store, txn);
```

## Text search

- API: `ln_store_text_search(store, txn, query, config_json, &results, &count)`
- `config_json` (optional):
  - `limit` (int): 1..1024; default 128.
  - `order` (string): `"asc"` or `"desc"`; default descending.
- Returns `char **results` of JSON notes; see ownership notes above.

## Getters

- `ln_store_get_note_by_id(store, txn, id32, &json, &len)`
- `ln_store_get_profile_by_pubkey(store, txn, pk32, &json, &len)`
- Both return heap-allocated NUL-terminated JSON strings; caller frees.

## Stats

- `ln_store_stat_json(store, &json_out)` returns a minimal JSON string with totals; caller frees.

## Ownership/transfer

- All returned JSON blobs are heap-allocated and owned by the caller.
- Free policy:
  - Query/Text search: free each entry then the `char **` array.
  - Getters/Stats: free the single returned `char *`.

## Notes

- Capacity: queries currently cap at 256 results per call; consider paging.
- Minimal JSON parsing is used internally for options/config; provide simple flat JSON.
- No nostrdb types leak through the public API; all interaction is through strings and opaque handles.

## Demo

Build and run the included demo to verify ingestion and queries using the NostrdB backend:

```sh
cmake --build build -j --target ndb_store_demo
./build/ndb_store_demo ~/ndb-demo '{"mapsize":1073741824,"ingester_threads":1,"flags":2,"ingest_skip_validation":1}'
```

The demo ingests two sample events via the client-events (NDJSON) path and then runs:

- Single filter query: `{"kinds":[1],"limit":10}`
- Multi-filter query: `[{"kinds":[1],"limit":5},{"kinds":[6],"limit":5}]`
- Text search: query="hello", config `{"limit":16,"order":"desc"}`

Ownership: the demo frees each returned JSON string and then the array.
