#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include <jansson.h>
#include "libnostr_store.h"
#include "storage_ndb.h"

/* Direct nostrdb access for subscription API */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wpedantic"
#  pragma clang diagnostic ignored "-Wzero-length-array"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "nostrdb.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* Backend impl structure from ndb_backend.h */
struct ln_ndb_impl {
  void *db;
};

static ln_store *g_store = NULL;

/* Global subscription callback - must be set before init */
static storage_ndb_notify_fn g_sub_cb = NULL;
static void *g_sub_cb_ctx = NULL;

/* Diagnostic counters */
static guint64 g_ingest_count = 0;
static guint64 g_ingest_bytes = 0;

guint64 storage_ndb_get_ingest_count(void) { return g_ingest_count; }
guint64 storage_ndb_get_ingest_bytes(void) { return g_ingest_bytes; }

/* Get raw nostrdb handle from our store */
static struct ndb *get_ndb(void)
{
  if (!g_store) return NULL;
  struct ln_ndb_impl *impl = (struct ln_ndb_impl *)ln_store_get_backend_handle(g_store);
  return impl ? (struct ndb *)impl->db : NULL;
}

int storage_ndb_init(const char *dbdir, const char *opts_json)
{
  fprintf(stderr, "[storage_ndb_init] ENTER: dbdir=%s opts=%s\n", dbdir, opts_json);
  fflush(stderr);
  if (g_store) {
    fprintf(stderr, "[storage_ndb_init] Already initialized, returning success\n");
    fflush(stderr);
    return 1; /* already initialized */
  }
  ln_store *s = NULL;
  fprintf(stderr, "[storage_ndb_init] Calling ln_store_open...\n");
  fflush(stderr);
  int rc = ln_store_open("nostrdb", dbdir ? dbdir : ".ndb-demo",
                         opts_json ? opts_json : "{\"mapsize\":1073741824,\"ingester_threads\":1}",
                         &s);
  fprintf(stderr, "[storage_ndb_init] ln_store_open returned rc=%d (LN_OK=%d)\n", rc, LN_OK);
  fflush(stderr);
  if (rc != LN_OK) return 0;
  g_store = s;
  fprintf(stderr, "[storage_ndb_init] SUCCESS, g_store=%p\n", (void*)g_store);
  fflush(stderr);
  return 1;
}

void storage_ndb_shutdown(void)
{
  if (g_store) {
    ln_store_close(g_store);
    g_store = NULL;
  }
}

int storage_ndb_ingest_ldjson(const char *buf, size_t len)
{
  if (!g_store) {
    fprintf(stderr, "[storage_ndb_ingest_ldjson] ERROR: g_store is NULL!\n");
    fflush(stderr);
    return LN_ERR_INGEST;
  }
  if (!buf) {
    fprintf(stderr, "[storage_ndb_ingest_ldjson] ERROR: buf is NULL!\n");
    fflush(stderr);
    return LN_ERR_INGEST;
  }
  int rc = ln_store_ingest_ldjson(g_store, buf, len, NULL);
  if (rc != 0) {
    fprintf(stderr, "[storage_ndb_ingest_ldjson] ln_store_ingest_ldjson returned rc=%d for %.80s\n", rc, buf);
    fflush(stderr);
  }
  return rc;
}

/* Helper: Add "tags":[] if missing from JSON.
 * Many relays omit the tags field, but nostrdb requires it.
 * Returns newly allocated string that caller must free, or NULL on error. */
static char *ensure_tags_field(const char *json)
{
  if (!json) return NULL;
  
  /* If tags field already exists, just duplicate */
  if (strstr(json, "\"tags\"")) {
    return strdup(json);
  }
  
  /* Find insertion point after "kind" field */
  const char *kind_pos = strstr(json, "\"kind\"");
  if (!kind_pos) {
    /* No kind field? Just duplicate as-is */
    return strdup(json);
  }
  
  /* Find the comma after the kind value */
  const char *comma_after_kind = strchr(kind_pos, ',');
  if (!comma_after_kind) {
    /* No comma after kind? Just duplicate as-is */
    return strdup(json);
  }
  
  /* Build new JSON with tags inserted */
  size_t prefix_len = comma_after_kind - json + 1;
  size_t suffix_len = strlen(comma_after_kind + 1);
  const char *tags_field = "\"tags\":[],";
  size_t tags_len = strlen(tags_field);
  
  char *result = malloc(prefix_len + tags_len + suffix_len + 1);
  if (!result) return NULL;
  
  memcpy(result, json, prefix_len);
  memcpy(result + prefix_len, tags_field, tags_len);
  memcpy(result + prefix_len + tags_len, comma_after_kind + 1, suffix_len);
  result[prefix_len + tags_len + suffix_len] = '\0';
  
  return result;
}

int storage_ndb_ingest_event_json(const char *json, const char *relay_opt)
{
  if (!g_store || !json) return LN_ERR_INGEST;

  /* CRITICAL FIX: Add tags field if missing */
  char *fixed_json = ensure_tags_field(json);
  if (!fixed_json) return LN_ERR_INGEST;

  /* Ensure explicit length; some backends may not accept -1 */
  size_t len = strlen(fixed_json);
  int rc = ln_store_ingest_event_json(g_store, fixed_json, (int)len, relay_opt);

  /* Log ingest failures only */
  if (rc != 0) {
    fprintf(stderr, "[storage_ndb_ingest] FAILED rc=%d len=%zu json_head=%.100s\n", rc, len, fixed_json);
    fflush(stderr);
  } else {
    /* Track successful ingestions */
    g_ingest_count++;
    g_ingest_bytes += len;
  }

  free(fixed_json);
  return rc;
}

int storage_ndb_begin_query(void **txn_out)
{
  if (!g_store || !txn_out) return LN_ERR_DB_TXN;
  return ln_store_begin_query(g_store, txn_out);
}

int storage_ndb_end_query(void *txn)
{
  if (!g_store || !txn) return LN_ERR_DB_TXN;
  return ln_store_end_query(g_store, txn);
}

int storage_ndb_begin_query_retry(void **txn_out, int attempts, int sleep_ms)
{
  if (!txn_out) return LN_ERR_DB_TXN;
  int rc = 0;
  void *txn = NULL;
  if (attempts <= 0) attempts = 1;
  if (sleep_ms <= 0) sleep_ms = 1;
  for (int i = 0; i < attempts; i++) {
    rc = storage_ndb_begin_query(&txn);
    if (rc == 0 && txn) { *txn_out = txn; return 0; }
    /* Exponential backoff capped at ~512ms between attempts */
    int backoff_ms = sleep_ms << (i / 50); /* increase every 50 attempts */
    if (backoff_ms > 512) backoff_ms = 512;
    usleep((useconds_t)backoff_ms * 1000);
  }
  *txn_out = NULL;
  return rc != 0 ? rc : LN_ERR_DB_TXN;
}

int storage_ndb_query(void *txn, const char *filters_json, char ***out_arr, int *out_count)
{
  if (!g_store || !txn || !filters_json || !out_arr || !out_count) return LN_ERR_QUERY;
  void *results = NULL; int count = 0;
  int rc = ln_store_query(g_store, txn, filters_json, &results, &count);
  if (rc != LN_OK) return rc;
  *out_arr = (char**)results; *out_count = count;
  return LN_OK;
}

int storage_ndb_text_search(void *txn, const char *q, const char *config_json, char ***out_arr, int *out_count)
{
  if (!g_store || !txn || !q || !out_arr || !out_count) return LN_ERR_TEXTSEARCH;
  void *results = NULL; int count = 0;
  int rc = ln_store_text_search(g_store, txn, q, config_json, &results, &count);
  if (rc != LN_OK) return rc;
  *out_arr = (char**)results; *out_count = count;
  return LN_OK;
}

int storage_ndb_get_note_by_id(void *txn, const unsigned char id32[32], char **json_out, int *json_len)
{
  if (!g_store || !txn || !id32 || !json_out) return LN_ERR_QUERY;
  const char *json = NULL; int len = 0;
  int rc = ln_store_get_note_by_id(g_store, txn, id32, &json, &len);
  if (rc != LN_OK) return rc;
  *json_out = (char*)json; if (json_len) *json_len = len;
  return LN_OK;
}

int storage_ndb_get_profile_by_pubkey(void *txn, const unsigned char pk32[32], char **json_out, int *json_len)
{
  if (!g_store || !txn || !pk32 || !json_out) return LN_ERR_QUERY;
  const char *json = NULL; int len = 0;
  int rc = ln_store_get_profile_by_pubkey(g_store, txn, pk32, &json, &len);
  if (rc != LN_OK) return rc;
  *json_out = (char*)json; if (json_len) *json_len = len;
  return LN_OK;
}

int storage_ndb_stat_json(char **json_out)
{
  if (!g_store || !json_out) return LN_ERR_QUERY;
  return ln_store_stat_json(g_store, json_out);
}

void storage_ndb_free_results(char **arr, int n)
{
  if (!arr) return; for (int i = 0; i < n; i++) free(arr[i]); free(arr);
}

/* Convenience: fetch a note by hex id with internal transaction and retries. */
int storage_ndb_get_note_by_id_nontxn(const char *id_hex, char **json_out, int *json_len)
{
  if (!id_hex || !json_out) return LN_ERR_QUERY;

  /* Convert hex to binary */
  unsigned char id32[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(id_hex + i*2, "%2x", &byte) != 1) return LN_ERR_QUERY;
    id32[i] = (unsigned char)byte;
  }

  /* Try to get note with internal transaction management */
  void *txn = NULL;
  int rc = storage_ndb_begin_query(&txn);

  if (rc != 0 || !txn) {
    return rc != 0 ? rc : LN_ERR_DB_TXN;
  }

  char *json_ptr = NULL;
  rc = storage_ndb_get_note_by_id(txn, id32, &json_ptr, json_len);

  if (rc == 0 && json_ptr && json_len && *json_len > 0) {
    /* Copy the JSON before ending transaction */
    *json_out = malloc(*json_len + 1);
    if (*json_out) {
      memcpy(*json_out, json_ptr, *json_len);
      (*json_out)[*json_len] = '\0';
    }
  }

  storage_ndb_end_query(txn);
  return rc;
}

/* ============== Subscription API Implementation ============== */

void storage_ndb_set_notify_callback(storage_ndb_notify_fn fn, void *ctx)
{
  g_sub_cb = fn;
  g_sub_cb_ctx = ctx;
}

void storage_ndb_get_notify_callback(storage_ndb_notify_fn *fn_out, void **ctx_out)
{
  if (fn_out) *fn_out = g_sub_cb;
  if (ctx_out) *ctx_out = g_sub_cb_ctx;
}

uint64_t storage_ndb_subscribe(const char *filter_json)
{
  struct ndb *ndb = get_ndb();
  if (!ndb || !filter_json) return 0;

  struct ndb_filter filter;
  if (!ndb_filter_init(&filter)) return 0;

  unsigned char *tmpbuf = (unsigned char *)malloc(4096);
  if (!tmpbuf) {
    ndb_filter_destroy(&filter);
    return 0;
  }

  int len = (int)strlen(filter_json);
  if (!ndb_filter_from_json(filter_json, len, &filter, tmpbuf, 4096)) {
    ndb_filter_destroy(&filter);
    free(tmpbuf);
    return 0;
  }

  uint64_t subid = ndb_subscribe(ndb, &filter, 1);

  ndb_filter_destroy(&filter);
  free(tmpbuf);

  return subid;
}

void storage_ndb_unsubscribe(uint64_t subid)
{
  struct ndb *ndb = get_ndb();
  if (!ndb || subid == 0) return;
  ndb_unsubscribe(ndb, subid);
}

int storage_ndb_poll_notes(uint64_t subid, uint64_t *note_keys, int capacity)
{
  struct ndb *ndb = get_ndb();
  if (!ndb || subid == 0 || !note_keys || capacity <= 0) return 0;
  return ndb_poll_for_notes(ndb, subid, note_keys, capacity);
}

void storage_ndb_invalidate_txn_cache(void)
{
  /* Call the ndb_backend function to invalidate thread-local transaction cache.
   * This function is defined in libnostr's ndb_backend.c when LIBNOSTR_WITH_NOSTRDB=ON.
   * When the backend isn't compiled in, we provide a local no-op stub. */
#ifdef LIBNOSTR_WITH_NOSTRDB
  extern void ln_ndb_invalidate_txn_cache_ext(void);
  ln_ndb_invalidate_txn_cache_ext();
#else
  /* No-op: libnostr ndb backend not compiled in */
  (void)0;
#endif
}

/* ============== Direct Note Access API Implementation ============== */

storage_ndb_note *storage_ndb_get_note_ptr(void *txn, uint64_t note_key)
{
  if (!txn || note_key == 0) return NULL;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;
  size_t note_size = 0;
  return ndb_get_note_by_key(ntxn, note_key, &note_size);
}

uint64_t storage_ndb_get_note_key_by_id(void *txn, const unsigned char id32[32],
                                         storage_ndb_note **note_out)
{
  if (!txn || !id32) return 0;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;
  size_t note_len = 0;
  uint64_t key = 0;
  struct ndb_note *note = ndb_get_note_by_id(ntxn, id32, &note_len, &key);
  if (note_out) *note_out = note;
  return note ? key : 0;
}

const unsigned char *storage_ndb_note_id(storage_ndb_note *note)
{
  if (!note) return NULL;
  return ndb_note_id(note);
}

const unsigned char *storage_ndb_note_pubkey(storage_ndb_note *note)
{
  if (!note) return NULL;
  return ndb_note_pubkey(note);
}

const char *storage_ndb_note_content(storage_ndb_note *note)
{
  if (!note) return NULL;
  return ndb_note_content(note);
}

uint32_t storage_ndb_note_content_length(storage_ndb_note *note)
{
  if (!note) return 0;
  return ndb_note_content_length(note);
}

uint32_t storage_ndb_note_created_at(storage_ndb_note *note)
{
  if (!note) return 0;
  return ndb_note_created_at(note);
}

uint32_t storage_ndb_note_kind(storage_ndb_note *note)
{
  if (!note) return 0;
  return ndb_note_kind(note);
}

void storage_ndb_hex_encode(const unsigned char *bin32, char *hex65)
{
  if (!bin32 || !hex65) return;
  static const char hex_chars[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    hex65[i*2] = hex_chars[(bin32[i] >> 4) & 0x0f];
    hex65[i*2 + 1] = hex_chars[bin32[i] & 0x0f];
  }
  hex65[64] = '\0';
}

/* Serialize note tags to JSON array string (for NIP-92 imeta parsing).
 * Returns newly allocated JSON string or NULL if no tags.
 * Caller must g_free() the result. */
char *storage_ndb_note_tags_json(storage_ndb_note *note)
{
  if (!note) return NULL;

  struct ndb_tags *tags = ndb_note_tags(note);
  if (!tags || ndb_tags_count(tags) == 0) return NULL;

  GString *json = g_string_new("[");
  struct ndb_iterator iter;
  ndb_tags_iterate_start(note, &iter);

  gboolean first_tag = TRUE;
  while (ndb_tags_iterate_next(&iter)) {
    if (!iter.tag) continue; /* Skip invalid tags */
    
    if (!first_tag) g_string_append_c(json, ',');
    first_tag = FALSE;

    g_string_append_c(json, '[');
    struct ndb_tag *tag = iter.tag;
    int nelem = ndb_tag_count(tag);
    
    /* Safety check: limit tag element count to prevent excessive processing */
    if (nelem > 100) {
      g_warning("Tag with excessive element count (%d), skipping", nelem);
      g_string_append(json, "]");
      continue;
    }
    
    for (int i = 0; i < nelem; i++) {
      if (i > 0) g_string_append_c(json, ',');
      struct ndb_str str = ndb_tag_str(note, tag, i);
      
      if (str.flag == NDB_PACKED_STR && str.str) {
        /* Validate string pointer before use */
        if (!g_utf8_validate(str.str, -1, NULL)) {
          /* Invalid UTF-8, escape as hex */
          g_string_append(json, "\"[invalid]\"");
          continue;
        }
        gchar *escaped = g_strescape(str.str, NULL);
        if (escaped) {
          g_string_append_printf(json, "\"%s\"", escaped);
          g_free(escaped);
        } else {
          g_string_append(json, "\"\"");
        }
      } else if (str.flag == NDB_PACKED_ID && str.id) {
        /* Handle ID type - convert to hex */
        char hex[65];
        storage_ndb_hex_encode(str.id, hex);
        g_string_append_printf(json, "\"%s\"", hex);
      } else {
        g_string_append(json, "\"\"");
      }
    }
    g_string_append_c(json, ']');
  }

  g_string_append_c(json, ']');
  return g_string_free(json, FALSE);
}

/* ============== NIP-25 Reaction Count API ============== */

/* Count reactions (kind 7) for a given event.
 * Uses a NIP-01 filter query to find all kind 7 events that reference the given event ID. */
guint storage_ndb_count_reactions(const char *event_id_hex)
{
  if (!event_id_hex || strlen(event_id_hex) != 64) return 0;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return 0;

  /* Build filter: {"kinds":[7],"#e":["<event_id>"]} */
  gchar *filter_json = g_strdup_printf("{\"kinds\":[7],\"#e\":[\"%s\"]}", event_id_hex);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  storage_ndb_end_query(txn);

  if (rc != 0) return 0;

  guint reaction_count = (guint)count;

  /* Free the results - we only need the count */
  if (results) {
    storage_ndb_free_results(results, count);
  }

  return reaction_count;
}

/* Check if a specific user has reacted to an event.
 * Uses a NIP-01 filter query to find reactions from the given user. */
gboolean storage_ndb_user_has_reacted(const char *event_id_hex, const char *user_pubkey_hex)
{
  if (!event_id_hex || strlen(event_id_hex) != 64) return FALSE;
  if (!user_pubkey_hex || strlen(user_pubkey_hex) != 64) return FALSE;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return FALSE;

  /* Build filter: {"kinds":[7],"authors":["<pubkey>"],"#e":["<event_id>"],"limit":1} */
  gchar *filter_json = g_strdup_printf(
    "{\"kinds\":[7],\"authors\":[\"%s\"],\"#e\":[\"%s\"],\"limit\":1}",
    user_pubkey_hex, event_id_hex);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  storage_ndb_end_query(txn);

  if (rc != 0) return FALSE;

  gboolean has_reacted = (count > 0);

  /* Free the results */
  if (results) {
    storage_ndb_free_results(results, count);
  }

  return has_reacted;
}

/* NIP-25: Get reaction breakdown for an event (emoji -> count).
 * Returns a GHashTable with emoji strings as keys and count as GUINT_TO_POINTER values.
 * Also populates reactor_pubkeys array if non-NULL.
 * Caller must free the returned hash table with g_hash_table_unref(). */
GHashTable *storage_ndb_get_reaction_breakdown(const char *event_id_hex, GPtrArray **reactor_pubkeys)
{
  GHashTable *breakdown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (!event_id_hex || strlen(event_id_hex) != 64) return breakdown;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return breakdown;

  /* Build filter: {"kinds":[7],"#e":["<event_id>"]} */
  gchar *filter_json = g_strdup_printf("{\"kinds\":[7],\"#e\":[\"%s\"]}", event_id_hex);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  if (rc != 0 || count == 0) {
    storage_ndb_end_query(txn);
    if (results) storage_ndb_free_results(results, count);
    return breakdown;
  }

  /* Initialize reactor_pubkeys array if requested */
  if (reactor_pubkeys) {
    *reactor_pubkeys = g_ptr_array_new_with_free_func(g_free);
  }

  /* Parse each reaction event to extract content (emoji) and pubkey */
  for (int i = 0; i < count; i++) {
    if (!results[i]) continue;

    /* Parse the JSON to extract content and pubkey */
    json_error_t err;
    json_t *event = json_loads(results[i], 0, &err);
    if (!event) continue;

    const char *content = json_string_value(json_object_get(event, "content"));
    const char *pubkey = json_string_value(json_object_get(event, "pubkey"));

    /* Default to "+" if content is empty */
    if (!content || !*content) content = "+";

    /* Increment count for this emoji */
    gpointer existing = g_hash_table_lookup(breakdown, content);
    guint emoji_count = existing ? GPOINTER_TO_UINT(existing) + 1 : 1;
    g_hash_table_insert(breakdown, g_strdup(content), GUINT_TO_POINTER(emoji_count));

    /* Track reactor pubkey if requested */
    if (reactor_pubkeys && pubkey && strlen(pubkey) == 64) {
      g_ptr_array_add(*reactor_pubkeys, g_strdup(pubkey));
    }

    json_decref(event);
  }

  storage_ndb_end_query(txn);
  if (results) storage_ndb_free_results(results, count);

  return breakdown;
}

/* ============== NIP-40 Expiration Timestamp API ============== */

/* Get expiration timestamp from note tags.
 * Returns 0 if no expiration tag is present, otherwise returns the Unix timestamp.
 * This function iterates through the note's tags looking for an "expiration" tag. */
gint64 storage_ndb_note_get_expiration(storage_ndb_note *note)
{
  if (!note) return 0;

  struct ndb_tags *tags = ndb_note_tags(note);
  if (!tags || ndb_tags_count(tags) == 0) return 0;

  struct ndb_iterator iter;
  ndb_tags_iterate_start(note, &iter);

  while (ndb_tags_iterate_next(&iter)) {
    struct ndb_tag *tag = iter.tag;
    int nelem = ndb_tag_count(tag);
    if (nelem < 2) continue;

    struct ndb_str key = ndb_tag_str(note, tag, 0);
    if (!key.str) continue;

    /* Check for "expiration" tag (NIP-40) */
    if (strcmp(key.str, "expiration") == 0) {
      struct ndb_str value = ndb_tag_str(note, tag, 1);
      if (value.str) {
        gint64 expiration = g_ascii_strtoll(value.str, NULL, 10);
        return expiration;
      }
    }
  }

  return 0;
}

/* Check if an event is expired (NIP-40).
 * Returns TRUE if the event has an expiration tag and the timestamp has passed.
 * Returns FALSE if the event has no expiration tag or is not yet expired. */
gboolean storage_ndb_note_is_expired(storage_ndb_note *note)
{
  if (!note) return FALSE;

  gint64 expiration = storage_ndb_note_get_expiration(note);
  if (expiration == 0) return FALSE;  /* No expiration tag */

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  return (expiration < now);
}

/* Check if an event is expired given its note_key.
 * Convenience function that handles transaction management internally.
 * Returns TRUE if expired, FALSE otherwise. */
gboolean storage_ndb_is_event_expired(uint64_t note_key)
{
  if (note_key == 0) return FALSE;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return FALSE;

  storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
  gboolean expired = storage_ndb_note_is_expired(note);

  storage_ndb_end_query(txn);
  return expired;
}
