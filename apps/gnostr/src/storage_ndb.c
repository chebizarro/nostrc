#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include "nostr_json.h"
#include "json.h"
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
#include "block.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* bolt11 parsing from nostrdb for NIP-57 zap amount extraction */
#include "bolt11/bolt11.h"
#include "bolt11/amount.h"

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
    /* nostrc-i26h: Force-close any TLS-cached transaction before destroying
     * the database. Without this, open read transactions pin LMDB pages,
     * preventing page reclamation and causing the database to balloon.
     *
     * The TLS cache normally keeps transactions open with refcount for
     * efficiency, but at shutdown we must forcefully end them. This call
     * affects only the calling thread (main thread in our case). */
#ifdef LIBNOSTR_WITH_NOSTRDB
    extern void ln_ndb_force_close_txn_cache(void);
    ln_ndb_force_close_txn_cache();
#endif

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

/* Convenience: fetch a note by key with internal transaction management.
 * Uses ndb_note_json() for serialization. Caller must free *json_out. */
int storage_ndb_get_note_json_by_key(uint64_t note_key, char **json_out, int *json_len)
{
  if (note_key == 0 || !json_out) return LN_ERR_QUERY;

  void *txn = NULL;
  int rc = storage_ndb_begin_query(&txn);
  if (rc != 0 || !txn) {
    return rc != 0 ? rc : LN_ERR_DB_TXN;
  }

  storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
  if (!note) {
    storage_ndb_end_query(txn);
    return LN_ERR_NOT_FOUND;
  }

  /* Allocate buffer and serialize note to JSON using ndb_note_json */
  int bufsize = 4096;
  char *buf = NULL;
  for (;;) {
    char *nb = (char *)realloc(buf, (size_t)bufsize);
    if (!nb) { free(buf); storage_ndb_end_query(txn); return LN_ERR_OOM; }
    buf = nb;
    int n = ndb_note_json(note, buf, bufsize);
    if (n > 0 && n < bufsize) {
      buf[n] = '\0';
      *json_out = buf;
      if (json_len) *json_len = n;
      storage_ndb_end_query(txn);
      return 0;
    }
    bufsize *= 2;
    if (bufsize > (16 * 1024 * 1024)) {
      free(buf);
      storage_ndb_end_query(txn);
      return LN_ERR_OOM;
    }
  }
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
  if (!ndb || !filter_json) {
    g_warning("storage_ndb_subscribe: ndb=%p filter_json=%p", (void*)ndb, (void*)filter_json);
    return 0;
  }

  struct ndb_filter filter;
  if (!ndb_filter_init(&filter)) {
    g_warning("storage_ndb_subscribe: ndb_filter_init failed");
    return 0;
  }

  unsigned char *tmpbuf = (unsigned char *)malloc(4096);
  if (!tmpbuf) {
    g_warning("storage_ndb_subscribe: malloc failed");
    ndb_filter_destroy(&filter);
    return 0;
  }

  /* ndb_filter_from_json expects a plain object {...}, but callers often pass
   * array-wrapped filters [{...}]. Handle both formats. */
  const char *json_ptr = filter_json;
  int len = (int)strlen(filter_json);

  /* Skip leading whitespace */
  while (len > 0 && (*json_ptr == ' ' || *json_ptr == '\t' || *json_ptr == '\n')) {
    json_ptr++;
    len--;
  }

  /* If wrapped in array, skip the outer brackets to get the first filter object */
  if (len > 2 && json_ptr[0] == '[') {
    json_ptr++;  /* skip '[' */
    len -= 2;    /* account for '[' and ']' */

    /* Skip whitespace after '[' */
    while (len > 0 && (*json_ptr == ' ' || *json_ptr == '\t' || *json_ptr == '\n')) {
      json_ptr++;
      len--;
    }

    /* Find the end of the first object (we only support single-filter arrays for now) */
    int brace_count = 0;
    int obj_len = 0;
    for (int i = 0; i < len; i++) {
      if (json_ptr[i] == '{') brace_count++;
      else if (json_ptr[i] == '}') brace_count--;
      obj_len++;
      if (brace_count == 0) break;
    }
    len = obj_len;
  }

  if (!ndb_filter_from_json(json_ptr, len, &filter, tmpbuf, 4096)) {
    g_warning("storage_ndb_subscribe: ndb_filter_from_json failed for: %.*s", len, json_ptr);
    ndb_filter_destroy(&filter);
    free(tmpbuf);
    return 0;
  }

  uint64_t subid = ndb_subscribe(ndb, &filter, 1);
  if (subid == 0) {
    g_warning("storage_ndb_subscribe: ndb_subscribe returned 0");
  }

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
  /* Guard against use-after-free: don't invalidate if store is already closed */
  if (!g_store) return;
  
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

      /* BUG FIX (nostrc-nip10-threading):
       * ndb_note_str sets str.str for BOTH packed inline strings (flag=NDB_PACKED_STR)
       * AND offset-based strings (flag=0 or other). The flag indicates storage type,
       * not string validity. Check str.str directly for string values. */
      if (str.flag == NDB_PACKED_ID && str.id) {
        /* Handle ID type - convert to hex */
        char hex[65];
        storage_ndb_hex_encode(str.id, hex);
        g_string_append_printf(json, "\"%s\"", hex);
      } else if (str.str) {
        /* String value (either packed inline or offset-based) */
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

/* Helper: convert a 64-char hex event ID to a 32-byte binary ID.
 * Returns TRUE on success, FALSE on malformed input. */
static gboolean
hex_to_id32(const char *hex, unsigned char out[32])
{
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + 2*i, "%02x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* Count reactions (kind 7) for a given event.
 * Uses pre-computed ndb_note_meta for O(1) lookup instead of filter queries. */
guint storage_ndb_count_reactions(const char *event_id_hex)
{
  if (!event_id_hex || strlen(event_id_hex) != 64) return 0;

  unsigned char id32[32];
  if (!hex_to_id32(event_id_hex, id32)) return 0;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return 0;

  guint reaction_count = 0;
  struct ndb_note_meta *meta = ndb_get_note_meta((struct ndb_txn *)txn, id32);
  if (meta) {
    struct ndb_note_meta_entry *entry =
        ndb_note_meta_find_entry(meta, NDB_NOTE_META_COUNTS, NULL);
    if (entry) {
      uint32_t *total = ndb_note_meta_counts_total_reactions(entry);
      if (total) reaction_count = (guint)*total;
    }
  }

  storage_ndb_end_query(txn);
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
 * Caller must free the returned hash table with g_hash_table_unref().
 *
 * Uses pre-computed ndb_note_meta for O(1) emoji→count lookup.
 * Falls back to filter query only when reactor_pubkeys are requested
 * (metadata stores counts, not individual reactor pubkeys). */
GHashTable *storage_ndb_get_reaction_breakdown(const char *event_id_hex, GPtrArray **reactor_pubkeys)
{
  GHashTable *breakdown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (!event_id_hex || strlen(event_id_hex) != 64) return breakdown;

  unsigned char id32[32];
  if (!hex_to_id32(event_id_hex, id32)) return breakdown;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return breakdown;

  struct ndb_note_meta *meta = ndb_get_note_meta((struct ndb_txn *)txn, id32);
  if (meta) {
    uint16_t n_entries = ndb_note_meta_entries_count(meta);
    for (int i = 0; i < n_entries; i++) {
      struct ndb_note_meta_entry *entry = ndb_note_meta_entry_at(meta, i);
      if (!entry) continue;
      uint16_t *type = ndb_note_meta_entry_type(entry);
      if (!type || *type != NDB_NOTE_META_REACTION) continue;

      uint32_t *cnt = ndb_note_meta_reaction_count(entry);
      union ndb_reaction_str *rstr = ndb_note_meta_reaction_str(entry);
      if (!cnt || !rstr || *cnt == 0) continue;

      char buf[128];
      const char *emoji = ndb_reaction_to_str(rstr, buf);
      if (!emoji || !*emoji) emoji = "+";

      g_hash_table_insert(breakdown, g_strdup(emoji), GUINT_TO_POINTER((guint)*cnt));
    }
  }

  storage_ndb_end_query(txn);

  /* Metadata only stores aggregate counts, not individual reactor pubkeys.
   * Fall back to filter query if caller needs the pubkey list. */
  if (reactor_pubkeys) {
    *reactor_pubkeys = g_ptr_array_new_with_free_func(g_free);

    void *txn2 = NULL;
    if (storage_ndb_begin_query_retry(&txn2, 3, 10) == 0 && txn2) {
      gchar *filter_json = g_strdup_printf("{\"kinds\":[7],\"#e\":[\"%s\"]}", event_id_hex);
      char **results = NULL;
      int count = 0;
      int rc = storage_ndb_query(txn2, filter_json, &results, &count);
      g_free(filter_json);

      if (rc == 0 && results) {
        for (int i = 0; i < count; i++) {
          if (!results[i] || !gnostr_json_is_valid(results[i])) continue;
          char *pubkey_val = gnostr_json_get_string(results[i], "pubkey", NULL);
          if (pubkey_val && strlen(pubkey_val) == 64)
            g_ptr_array_add(*reactor_pubkeys, pubkey_val);
          else
            g_free(pubkey_val);
        }
        storage_ndb_free_results(results, count);
      }
      storage_ndb_end_query(txn2);
    }
  }

  return breakdown;
}

/* ============== NIP-57 Zap Stats API ============== */

/* Count zap receipts (kind 9735) for a given event.
 * Uses a NIP-01 filter query to find all kind 9735 events that reference the given event ID. */
guint storage_ndb_count_zaps(const char *event_id_hex)
{
  if (!event_id_hex || strlen(event_id_hex) != 64) return 0;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return 0;

  /* Build filter: {"kinds":[9735],"#e":["<event_id>"]} */
  gchar *filter_json = g_strdup_printf("{\"kinds\":[9735],\"#e\":[\"%s\"]}", event_id_hex);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  storage_ndb_end_query(txn);

  if (rc != 0) return 0;

  guint zap_count = (guint)count;

  /* Free the results - we only need the count */
  if (results) {
    storage_ndb_free_results(results, count);
  }

  return zap_count;
}

/* Context and callback for finding bolt11 tag in zap receipts */
typedef struct {
  char *bolt11;
  gboolean found;
} StorageNdbBolt11Ctx;

static gboolean storage_ndb_find_bolt11_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  StorageNdbBolt11Ctx *ctx = (StorageNdbBolt11Ctx *)user_data;
  if (ctx->found) return FALSE;
  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  char *name = NULL;
  if ((name = gnostr_json_get_array_string(element_json, NULL, 0, NULL)) == NULL) return TRUE;

  if (g_strcmp0(name, "bolt11") == 0) {
    ctx->bolt11 = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    ctx->found = TRUE;
  }
  g_free(name);
  return !ctx->found;
}

/* Get zap statistics for an event - count and total amount in millisatoshis.
 * Parses bolt11 invoices from zap receipt tags to extract amounts. */
gboolean storage_ndb_get_zap_stats(const char *event_id_hex, guint *zap_count, gint64 *total_msat)
{
  if (zap_count) *zap_count = 0;
  if (total_msat) *total_msat = 0;

  if (!event_id_hex || strlen(event_id_hex) != 64) return FALSE;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return FALSE;

  /* Build filter: {"kinds":[9735],"#e":["<event_id>"]} */
  gchar *filter_json = g_strdup_printf("{\"kinds\":[9735],\"#e\":[\"%s\"]}", event_id_hex);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  storage_ndb_end_query(txn);

  if (rc != 0 || count == 0) {
    if (results) storage_ndb_free_results(results, count);
    return (rc == 0);  /* Return TRUE if query succeeded with 0 results */
  }

  if (zap_count) *zap_count = (guint)count;

  /* Parse each zap receipt to extract amount from bolt11 */
  gint64 total = 0;
  for (int i = 0; i < count; i++) {
    if (!results[i] || !gnostr_json_is_valid(results[i])) continue;

    /* Get raw tags array */
    char *tags_json = NULL;
    if ((tags_json = gnostr_json_get_raw(results[i], "tags", NULL)) == NULL || !tags_json) continue;

    /* Find bolt11 tag by iterating through tags */
    char *bolt11_value = NULL;
    size_t tags_len = 0;
    if ((tags_len = gnostr_json_get_array_length(tags_json, NULL, NULL)) >= 0) {
      for (size_t j = 0; j < tags_len && !bolt11_value; j++) {
        /* Each tag is an array like ["bolt11", "lnbc..."] */
        /* Get the raw tag element first */
        char *tag_raw = NULL;
        /* We need to get the j-th element of the tags array as raw JSON */
        /* Use array index access - tag is at position j */
        char *tag_name = NULL;
        char *tag_val = NULL;

        /* Try to get the tag name and value from nested array */
        /* tags_json is like [["bolt11","lnbc..."],["p","..."]] */
        /* We need to extract element j and parse it */
        /* Simpler: use the foreach callback approach */
      }
    }

    /* Use callback to find bolt11 */
    StorageNdbBolt11Ctx bctx = { .bolt11 = NULL, .found = FALSE };
    gnostr_json_array_foreach_root(tags_json, storage_ndb_find_bolt11_cb, &bctx);

    if (bctx.bolt11 && *bctx.bolt11) {
      /* Parse bolt11 invoice to get amount */
      char *fail = NULL;
      struct bolt11 *b11 = bolt11_decode_minimal(NULL, bctx.bolt11, &fail);
      if (b11) {
        if (b11->msat) {
          total += (gint64)b11->msat->millisatoshis;
        }
        tal_free(b11);
      } else if (fail) {
        g_debug("storage_ndb: failed to parse bolt11: %s", fail);
        free(fail);
      }
    }
    g_free(bctx.bolt11);
    g_free(tags_json);
  }

  if (total_msat) *total_msat = total;

  if (results) storage_ndb_free_results(results, count);

  return TRUE;
}

/* ============== Batch Reaction/Zap API (nostrc-qff) ============== */

/* Helper: extract the last "e" tag value from an event JSON's tags.
 * In NIP-25/NIP-57, the last "e" tag references the target event.
 * Returns newly allocated string or NULL. Caller must g_free(). */
typedef struct {
  gchar *last_e_value;
} StorageNdbExtractECtx;

static gboolean storage_ndb_extract_e_tag_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  StorageNdbExtractECtx *ctx = user_data;
  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  char *name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!name) return TRUE;

  if (g_strcmp0(name, "e") == 0) {
    g_free(ctx->last_e_value);
    ctx->last_e_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  }
  g_free(name);
  return TRUE; /* continue to find the LAST "e" tag */
}

static gchar *storage_ndb_extract_referenced_event_id(const char *event_json)
{
  if (!event_json || !gnostr_json_is_valid(event_json)) return NULL;

  char *tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
  if (!tags_json) return NULL;

  StorageNdbExtractECtx ctx = { .last_e_value = NULL };
  gnostr_json_array_foreach_root(tags_json, storage_ndb_extract_e_tag_cb, &ctx);

  g_free(tags_json);
  return ctx.last_e_value;
}

/* Build a JSON filter with multiple event IDs in the #e array.
 * kind: event kind (7 for reactions, 9735 for zaps)
 * event_ids: array of 64-char hex event IDs
 * n_ids: number of IDs
 * extra: optional extra filter JSON fragment (e.g. "\"authors\":[\"...\"],")
 * Returns newly allocated filter JSON string. */
static gchar *storage_ndb_build_batch_filter(int kind, const char * const *event_ids, guint n_ids,
                                              const char *extra)
{
  GString *filter = g_string_new("{\"kinds\":[");
  g_string_append_printf(filter, "%d],", kind);
  if (extra) g_string_append(filter, extra);
  g_string_append(filter, "\"#e\":[");
  for (guint i = 0; i < n_ids; i++) {
    if (i > 0) g_string_append_c(filter, ',');
    g_string_append_printf(filter, "\"%s\"", event_ids[i]);
  }
  g_string_append(filter, "]}");
  return g_string_free(filter, FALSE);
}

GHashTable *storage_ndb_count_reactions_batch(const char * const *event_ids, guint n_ids)
{
  GHashTable *counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (!event_ids || n_ids == 0) return counts;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return counts;

  gchar *filter_json = storage_ndb_build_batch_filter(7, event_ids, n_ids, NULL);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  storage_ndb_end_query(txn);

  if (rc != 0 || count == 0) {
    if (results) storage_ndb_free_results(results, count);
    return counts;
  }

  /* Group by referenced event ID */
  for (int i = 0; i < count; i++) {
    if (!results[i]) continue;
    gchar *ref_id = storage_ndb_extract_referenced_event_id(results[i]);
    if (!ref_id || strlen(ref_id) != 64) { g_free(ref_id); continue; }

    gpointer existing = g_hash_table_lookup(counts, ref_id);
    guint cur = existing ? GPOINTER_TO_UINT(existing) : 0;
    g_hash_table_insert(counts, ref_id, GUINT_TO_POINTER(cur + 1));
    /* ref_id ownership transferred to hash table */
  }

  if (results) storage_ndb_free_results(results, count);
  return counts;
}

GHashTable *storage_ndb_user_has_reacted_batch(const char * const *event_ids, guint n_ids,
                                                const char *user_pubkey_hex)
{
  GHashTable *reacted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (!event_ids || n_ids == 0 || !user_pubkey_hex || strlen(user_pubkey_hex) != 64)
    return reacted;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return reacted;

  /* Build filter with author constraint */
  gchar *author_frag = g_strdup_printf("\"authors\":[\"%s\"],", user_pubkey_hex);
  gchar *filter_json = storage_ndb_build_batch_filter(7, event_ids, n_ids, author_frag);
  g_free(author_frag);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  storage_ndb_end_query(txn);

  if (rc != 0 || count == 0) {
    if (results) storage_ndb_free_results(results, count);
    return reacted;
  }

  /* Mark each referenced event as reacted-to */
  for (int i = 0; i < count; i++) {
    if (!results[i]) continue;
    gchar *ref_id = storage_ndb_extract_referenced_event_id(results[i]);
    if (!ref_id || strlen(ref_id) != 64) { g_free(ref_id); continue; }

    g_hash_table_insert(reacted, ref_id, GINT_TO_POINTER(TRUE));
  }

  if (results) storage_ndb_free_results(results, count);
  return reacted;
}

GHashTable *storage_ndb_get_zap_stats_batch(const char * const *event_ids, guint n_ids)
{
  GHashTable *stats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (!event_ids || n_ids == 0) return stats;

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0 || !txn) return stats;

  gchar *filter_json = storage_ndb_build_batch_filter(9735, event_ids, n_ids, NULL);

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  g_free(filter_json);

  storage_ndb_end_query(txn);

  if (rc != 0 || count == 0) {
    if (results) storage_ndb_free_results(results, count);
    return stats;
  }

  /* Group by referenced event ID and accumulate amounts */
  for (int i = 0; i < count; i++) {
    if (!results[i] || !gnostr_json_is_valid(results[i])) continue;

    gchar *ref_id = storage_ndb_extract_referenced_event_id(results[i]);
    if (!ref_id || strlen(ref_id) != 64) { g_free(ref_id); continue; }

    /* Get or create stats entry for this event */
    StorageNdbZapStats *entry = g_hash_table_lookup(stats, ref_id);
    if (!entry) {
      entry = g_new0(StorageNdbZapStats, 1);
      g_hash_table_insert(stats, g_strdup(ref_id), entry);
    }
    entry->zap_count++;

    /* Parse bolt11 invoice for amount */
    char *tags_json = gnostr_json_get_raw(results[i], "tags", NULL);
    if (tags_json) {
      StorageNdbBolt11Ctx bctx = { .bolt11 = NULL, .found = FALSE };
      gnostr_json_array_foreach_root(tags_json, storage_ndb_find_bolt11_cb, &bctx);

      if (bctx.bolt11 && *bctx.bolt11) {
        char *fail = NULL;
        struct bolt11 *b11 = bolt11_decode_minimal(NULL, bctx.bolt11, &fail);
        if (b11) {
          if (b11->msat) {
            entry->total_msat += (gint64)b11->msat->millisatoshis;
          }
          tal_free(b11);
        } else if (fail) {
          g_debug("storage_ndb: batch zap bolt11 parse fail: %s", fail);
          free(fail);
        }
      }
      g_free(bctx.bolt11);
      g_free(tags_json);
    }

    g_free(ref_id);
  }

  if (results) storage_ndb_free_results(results, count);
  return stats;
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

/* ============== NIP-10 Thread Info API ============== */

/* Helper to validate and duplicate a relay URL string.
 * nostrc-7r5: Returns NULL if invalid, newly allocated string if valid. */
static char *dup_valid_relay_url_ndb(const char *relay) {
  if (!relay || !*relay) return NULL;
  /* Basic validation: must start with wss:// or ws:// */
  if (strncmp(relay, "wss://", 6) != 0 && strncmp(relay, "ws://", 5) != 0) {
    return NULL;
  }
  return g_strdup(relay);
}

/* Extract NIP-10 thread context with relay hints from note tags.
 * nostrc-7r5: Extended version that also extracts relay hints from e-tags.
 * Supports both preferred marker style and positional fallback.
 * Returns allocated strings via out parameters. Caller must g_free().
 * Pass NULL for outputs you don't need. */
void storage_ndb_note_get_nip10_thread_full(storage_ndb_note *note,
                                             char **root_id_out,
                                             char **reply_id_out,
                                             char **root_relay_hint_out,
                                             char **reply_relay_hint_out)
{
  if (root_id_out) *root_id_out = NULL;
  if (reply_id_out) *reply_id_out = NULL;
  if (root_relay_hint_out) *root_relay_hint_out = NULL;
  if (reply_relay_hint_out) *reply_relay_hint_out = NULL;
  if (!note) return;

  struct ndb_tags *tags = ndb_note_tags(note);
  if (!tags || ndb_tags_count(tags) == 0) return;

  char *found_root = NULL;
  char *found_root_relay = NULL;
  char *found_reply = NULL;
  char *found_reply_relay = NULL;
  const char *first_e_id = NULL;
  const char *first_e_relay = NULL;
  const char *last_e_id = NULL;
  const char *last_e_relay = NULL;
  char first_e_buf[65] = {0};
  char first_e_relay_buf[256] = {0};
  char last_e_buf[65] = {0};
  char last_e_relay_buf[256] = {0};

  struct ndb_iterator iter;
  ndb_tags_iterate_start(note, &iter);

  while (ndb_tags_iterate_next(&iter)) {
    struct ndb_tag *tag = iter.tag;
    int nelem = ndb_tag_count(tag);
    if (nelem < 2) continue;

    struct ndb_str key = ndb_tag_str(note, tag, 0);
    if (!key.str || strcmp(key.str, "e") != 0) continue;

    /* Get the event ID (may be packed ID or string) */
    char id_hex[65];
    struct ndb_str id_str = ndb_tag_str(note, tag, 1);
    if (id_str.flag == NDB_PACKED_ID && id_str.id) {
      storage_ndb_hex_encode(id_str.id, id_hex);
    } else if (id_str.flag != NDB_PACKED_ID && id_str.str && strlen(id_str.str) == 64) {
      strncpy(id_hex, id_str.str, 64);
      id_hex[64] = '\0';
    } else {
      continue; /* Invalid ID format */
    }

    /* nostrc-7r5: Get relay hint at position 2 */
    const char *relay_hint = NULL;
    if (nelem >= 3) {
      struct ndb_str relay_str = ndb_tag_str(note, tag, 2);
      if (relay_str.str && *relay_str.str) {
        relay_hint = relay_str.str;
      }
    }

    /* Check for marker at position 3 */
    const char *marker = NULL;
    if (nelem >= 4) {
      struct ndb_str marker_str = ndb_tag_str(note, tag, 3);
      /* BUG FIX (nostrc-nip10-threading):
       * ndb_note_str sets str.str for BOTH packed inline strings (flag=NDB_PACKED_STR)
       * AND offset-based strings (flag=0). The string "root"/"reply"/"mention" are
       * 4-7 chars and stored as offsets (flag=0), not inline. We must check str.str
       * regardless of flag value, not only when flag==NDB_PACKED_STR. */
      if (marker_str.str && *marker_str.str) {
        marker = marker_str.str;
        g_debug("[NIP10] e-tag index 3 marker: %s (flag=%d)", marker, marker_str.flag);
      }
    }

    if (marker && strcmp(marker, "root") == 0) {
      g_free(found_root);
      g_free(found_root_relay);
      found_root = g_strdup(id_hex);
      found_root_relay = dup_valid_relay_url_ndb(relay_hint);
      g_debug("[NIP10] Found root marker: %.16s... relay: %s", id_hex, relay_hint ? relay_hint : "(none)");
    } else if (marker && strcmp(marker, "reply") == 0) {
      g_free(found_reply);
      g_free(found_reply_relay);
      found_reply = g_strdup(id_hex);
      found_reply_relay = dup_valid_relay_url_ndb(relay_hint);
      g_debug("[NIP10] Found reply marker: %.16s... relay: %s", id_hex, relay_hint ? relay_hint : "(none)");
    } else if (marker && strcmp(marker, "mention") == 0) {
      /* Skip mentions - not part of reply chain */
      continue;
    } else {
      /* No marker - track for positional fallback */
      if (!first_e_id) {
        strncpy(first_e_buf, id_hex, 64);
        first_e_buf[64] = '\0';
        first_e_id = first_e_buf;
        if (relay_hint && strlen(relay_hint) < sizeof(first_e_relay_buf)) {
          strncpy(first_e_relay_buf, relay_hint, sizeof(first_e_relay_buf) - 1);
          first_e_relay_buf[sizeof(first_e_relay_buf) - 1] = '\0';
          first_e_relay = first_e_relay_buf;
        }
      }
      strncpy(last_e_buf, id_hex, 64);
      last_e_buf[64] = '\0';
      last_e_id = last_e_buf;
      if (relay_hint && strlen(relay_hint) < sizeof(last_e_relay_buf)) {
        strncpy(last_e_relay_buf, relay_hint, sizeof(last_e_relay_buf) - 1);
        last_e_relay_buf[sizeof(last_e_relay_buf) - 1] = '\0';
        last_e_relay = last_e_relay_buf;
      } else {
        last_e_relay = NULL;
      }
    }
  }

  /* NIP-10 positional fallback:
   * - First e-tag = root (thread root event)
   * - Last e-tag = reply target (event being replied to)
   * When there's only one e-tag (first == last), the event is a direct reply
   * to that event, so both root and reply should point to it.
   * nostrc-5b8: Fix single e-tag case where reply_id was incorrectly left NULL */
  if (!found_root && first_e_id) {
    found_root = g_strdup(first_e_id);
    if (!found_root_relay && first_e_relay) {
      found_root_relay = dup_valid_relay_url_ndb(first_e_relay);
    }
  }
  if (!found_reply && last_e_id) {
    /* Any e-tag (even if same as root) indicates this is a reply */
    found_reply = g_strdup(last_e_id);
    if (!found_reply_relay && last_e_relay) {
      found_reply_relay = dup_valid_relay_url_ndb(last_e_relay);
    }
  }

  /* NIP-10: When only "root" marker exists (no "reply" marker), this is a
   * direct reply to the root event. Set reply_id = root_id.
   * nostrc-mef: Fix root-only marker case */
  if (!found_reply && found_root) {
    found_reply = g_strdup(found_root);
    if (!found_reply_relay && found_root_relay) {
      found_reply_relay = g_strdup(found_root_relay);
    }
  }

  /* NIP-10: When only "reply" marker exists (no "root" marker), use the reply
   * target as the root. This happens when a client sets "reply" but not "root"
   * (e.g., replying to what the client considers the root, or incomplete tagging).
   * The reply target is always a valid ancestor, so use it as a fallback root. */
  if (!found_root && found_reply) {
    found_root = g_strdup(found_reply);
    if (!found_root_relay && found_reply_relay) {
      found_root_relay = g_strdup(found_reply_relay);
    }
    g_debug("[NIP10] No root marker, using reply target as root: %.16s...", found_root);
  }

  g_debug("[NIP10] Final result - root: %s (relay: %s), reply: %s (relay: %s)",
          found_root ? found_root : "(null)",
          found_root_relay ? found_root_relay : "(null)",
          found_reply ? found_reply : "(null)",
          found_reply_relay ? found_reply_relay : "(null)");

  if (root_id_out) *root_id_out = found_root;
  else g_free(found_root);

  if (reply_id_out) *reply_id_out = found_reply;
  else g_free(found_reply);

  if (root_relay_hint_out) *root_relay_hint_out = found_root_relay;
  else g_free(found_root_relay);

  if (reply_relay_hint_out) *reply_relay_hint_out = found_reply_relay;
  else g_free(found_reply_relay);
}

/* Extract NIP-10 thread context (root_id, reply_id) from note tags.
 * Supports both preferred marker style and positional fallback.
 * Returns allocated strings via out parameters. Caller must g_free().
 * Pass NULL for outputs you don't need. */
void storage_ndb_note_get_nip10_thread(storage_ndb_note *note, char **root_id_out, char **reply_id_out)
{
  /* Delegate to the full version, ignoring relay hints */
  storage_ndb_note_get_nip10_thread_full(note, root_id_out, reply_id_out, NULL, NULL);
}

/* ============== Hashtag Extraction API ============== */

/* Extract hashtags ("t" tags) from note.
 * Returns NULL-terminated array of hashtag strings, or NULL if none.
 * Caller must g_strfreev() the result. */
char **storage_ndb_note_get_hashtags(storage_ndb_note *note)
{
  if (!note) return NULL;

  struct ndb_tags *tags = ndb_note_tags(note);
  if (!tags || ndb_tags_count(tags) == 0) return NULL;

  GPtrArray *hashtags = g_ptr_array_new();

  struct ndb_iterator iter;
  ndb_tags_iterate_start(note, &iter);

  while (ndb_tags_iterate_next(&iter)) {
    struct ndb_tag *tag = iter.tag;
    int nelem = ndb_tag_count(tag);
    if (nelem < 2) continue;

    /* Check if this is a "t" tag */
    struct ndb_str key = ndb_tag_str(note, tag, 0);
    if (!key.str || strcmp(key.str, "t") != 0) continue;

    /* Get the hashtag value */
    struct ndb_str val = ndb_tag_str(note, tag, 1);
    if (val.str && *val.str) {
      g_ptr_array_add(hashtags, g_strdup(val.str));
    }
  }

  if (hashtags->len == 0) {
    g_ptr_array_free(hashtags, TRUE);
    return NULL;
  }
  g_ptr_array_add(hashtags, NULL); /* NULL-terminate */
  return (char **)g_ptr_array_free(hashtags, FALSE);
}

/* nostrc-57j: Get relay URLs that a note was seen on */
char **storage_ndb_note_get_relays(void *txn, uint64_t note_key)
{
  if (!txn || note_key == 0) return NULL;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;

  struct ndb_note_relay_iterator iter;
  if (!ndb_note_relay_iterate_start(ntxn, &iter, note_key))
    return NULL;

  GPtrArray *relays = g_ptr_array_new();
  const char *relay;
  while ((relay = ndb_note_relay_iterate_next(&iter)) != NULL) {
    g_ptr_array_add(relays, g_strdup(relay));
  }
  ndb_note_relay_iterate_close(&iter);

  if (relays->len == 0) {
    g_ptr_array_free(relays, TRUE);
    return NULL;
  }
  g_ptr_array_add(relays, NULL); /* NULL-terminate */
  return (char **)g_ptr_array_free(relays, FALSE);
}

/* ============== Contact List / Following API ============== */

/* Context and callback for extracting p-tags from contact list */
typedef struct {
  GPtrArray *pubkeys;
} StorageNdbPTagCtx;

static gboolean storage_ndb_extract_p_tag_cb(gsize idx, const gchar *element_json, gpointer user_data)
{
  (void)idx;
  StorageNdbPTagCtx *ctx = (StorageNdbPTagCtx *)user_data;
  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  char *name = NULL;
  if ((name = gnostr_json_get_array_string(element_json, NULL, 0, NULL)) == NULL) return TRUE;

  if (g_strcmp0(name, "p") == 0) {
    char *pubkey = NULL;
    if ((pubkey = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) != NULL &&
        pubkey && strlen(pubkey) == 64) {
      g_ptr_array_add(ctx->pubkeys, g_strdup(pubkey));
    }
    g_free(pubkey);
  }
  g_free(name);
  return TRUE; /* continue iterating */
}

/* nostrc-f0ll: Get followed pubkeys from a user's contact list (kind 3).
 * @user_pubkey_hex: 64-char hex pubkey of the user whose contact list to fetch
 * Returns NULL-terminated array of pubkey hex strings, or NULL if none/error.
 * Caller must g_strfreev() the result. */
char **storage_ndb_get_followed_pubkeys(const char *user_pubkey_hex)
{
  if (!user_pubkey_hex || strlen(user_pubkey_hex) != 64) return NULL;

  /* Query for kind 3 (contact list) from this author, limit 1 (most recent) */
  gchar *filter = g_strdup_printf(
    "[{\"kinds\":[3],\"authors\":[\"%s\"],\"limit\":1}]",
    user_pubkey_hex);

  void *txn = NULL;
  int rc = storage_ndb_begin_query_retry(&txn, 3, 10);
  if (rc != 0 || !txn) {
    g_free(filter);
    return NULL;
  }

  char **results = NULL;
  int count = 0;
  rc = storage_ndb_query(txn, filter, &results, &count);
  g_free(filter);

  if (rc != 0 || count == 0 || !results) {
    storage_ndb_end_query(txn);
    if (results) storage_ndb_free_results(results, count);
    return NULL;
  }

  /* Parse the contact list JSON to extract p-tags.
   * Format: {..., "tags": [["p", "pubkey1"], ["p", "pubkey2", "relay"], ...], ...} */
  StorageNdbPTagCtx ctx = { .pubkeys = g_ptr_array_new() };

  /* Get tags array from the JSON result */
  char *tags_json = NULL;
  tags_json = gnostr_json_get_raw(results[0], "tags", NULL);
  if (tags_json) {
    gnostr_json_array_foreach_root(tags_json, storage_ndb_extract_p_tag_cb, &ctx);
    g_free(tags_json);
  }

  storage_ndb_free_results(results, count);
  storage_ndb_end_query(txn);

  if (ctx.pubkeys->len == 0) {
    g_ptr_array_free(ctx.pubkeys, TRUE);
    return NULL;
  }

  g_ptr_array_add(ctx.pubkeys, NULL); /* NULL-terminate */
  g_debug("[STORAGE_NDB] Found %u followed pubkeys for %.16s...",
          ctx.pubkeys->len - 1, user_pubkey_hex);
  return (char **)g_ptr_array_free(ctx.pubkeys, FALSE);
}

/* ============== Content Blocks API ============== */

storage_ndb_blocks *storage_ndb_get_blocks(void *txn, uint64_t note_key)
{
  struct ndb *ndb = get_ndb();
  if (!ndb || !txn || note_key == 0) return NULL;
  struct ndb_txn *ntxn = (struct ndb_txn *)txn;
  return ndb_get_blocks_by_key(ndb, ntxn, note_key);
}

storage_ndb_blocks *storage_ndb_parse_content_blocks(const char *content, int content_len)
{
  if (!content || content_len <= 0) return NULL;

  /* Match ndb_note_to_blocks: use malloc (ndb_blocks_free calls free()) */
  unsigned char *buf = malloc(2 << 18);  /* 512KB, same as nostrdb internal */
  if (!buf) return NULL;
  struct ndb_blocks *blocks = NULL;

  if (ndb_parse_content(buf, content_len, content, content_len, &blocks) != 1) {
    free(buf);
    return NULL;
  }

  /* blocks points into buf; set OWNED flag so ndb_blocks_free() calls free() */
  blocks->flags |= NDB_BLOCK_FLAG_OWNED;
  return blocks;
}

void storage_ndb_blocks_free(storage_ndb_blocks *blocks)
{
  if (blocks) {
    ndb_blocks_free(blocks);
  }
}
