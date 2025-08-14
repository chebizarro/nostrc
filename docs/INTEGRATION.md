# Integrating NostrdB with gnostr demo client

This guide explains how to use the NostrdB-backed `ln_store` in the gnostr demo client to ingest events and run queries.

## Prereqs

- Build libnostr with NostrdB backend enabled (CMake option `LIBNOSTR_WITH_NOSTRDB=ON`).
- Ensure the example `ndb_store_demo` builds and runs (see `docs/STORAGE.md`).

## Runtime options

`ln_store_open("nostrdb", path, opts_json, &store)` accepts a JSON string of options (see `docs/STORAGE.md`). Common:

- `{"mapsize":1073741824,"ingester_threads":1}`
- For demo/test data with placeholder sigs: add `"ingest_skip_validation":1`.

## Minimal wiring in gnostr

1) Open the store at app startup and keep a handle.

```c
#include "libnostr_store.h"

static ln_store *g_store = NULL;

bool storage_init(const char *dbdir, const char *opts) {
  int rc = ln_store_open("nostrdb", dbdir, opts, &g_store);
  return rc == LN_OK;
}

void storage_shutdown(void) {
  if (g_store) ln_store_close(g_store), g_store = NULL;
}
```

2) Ingest events from relays as NDJSON client-events (preferred) or single event JSON.

```c
// NDJSON (writer path): lines like ["EVENT",{...}]["EVENT",{...}]\n
int storage_ingest_ldjson(const char *buf, size_t len) {
  return ln_store_ingest_ldjson(g_store, buf, len, NULL);
}

int storage_ingest_event_json(const char *json, const char *relay) {
  return ln_store_ingest_event_json(g_store, json, -1, relay);
}
```

3) Query API: open a query txn once per UI interaction, run filters, free results, then end txn.

```c
int storage_query(const char *filters_json, char ***out_arr, int *out_count) {
  void *txn = NULL; int rc = ln_store_begin_query(g_store, &txn);
  if (rc != LN_OK) return rc;
  void *results = NULL; int count = 0;
  rc = ln_store_query(g_store, txn, filters_json, &results, &count);
  ln_store_end_query(g_store, txn);
  if (rc != LN_OK) return rc;
  *out_arr = (char**)results; *out_count = count; return LN_OK;
}
```

- Single filter: `{"kinds":[1],"limit":50}`
- Multiple filters: `[{"kinds":[1],"limit":50},{"kinds":[6],"limit":50}]`

4) Text search example.

```c
int storage_text_search(const char *q, const char *cfg_json, char ***out_arr, int *out_count) {
  void *txn = NULL; int rc = ln_store_begin_query(g_store, &txn);
  if (rc != LN_OK) return rc;
  void *results = NULL; int count = 0;
  rc = ln_store_text_search(g_store, txn, q, cfg_json, &results, &count);
  ln_store_end_query(g_store, txn);
  if (rc != LN_OK) return rc;
  *out_arr = (char**)results; *out_count = count; return LN_OK;
}
```

5) Freeing results.

```c
void storage_free_results(char **arr, int n) {
  if (!arr) return; for (int i = 0; i < n; i++) free(arr[i]); free(arr);
}
```

## Suggested integration points

- `apps/gnostr/` main/init: call `storage_init("~/ndb-demo", "{\"mapsize\":1073741824,\"ingester_threads\":1}")` early.
- Relay pipeline: when receiving `EVENT`, append to an NDJSON buffer and periodically call `storage_ingest_ldjson()` to batch.
- Timeline views: translate UI filters to the JSON filter format and call `storage_query()`.
- Search box: call `storage_text_search(q, "{\"limit\":128}")` and render JSON results.

## Notes

- The internal cap is currently 256 results per call; use pagination for large timelines.
- If you maintain legacy wrappers, consider migrating call sites directly to `ln_store_*` now, or straight to canonical nostr* APIs later per project policy.
