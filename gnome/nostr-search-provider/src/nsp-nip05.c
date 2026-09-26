/* nsp-nip05.c — see nsp-nip05.h. */
#include "nsp-nip05.h"

#include <string.h>
#include <time.h>

#include "nostr_nip05.h"
#include "nsp-http.h"

/* One HTTPS request per address; late waiters join it. A flight outlives
 * the resolver if it is freed mid-request (f->r becomes NULL). */
typedef struct {
  struct _NspNip05 *r;
  char *address;
  char *local;
  GPtrArray *waiters; /* GTask*, owned */
} Flight;

struct _NspNip05 {
  nh_nip05_cache *cache;
  SoupSession *session;
  GHashTable *flights; /* f->address (borrowed) -> Flight* */
};

static void flight_free(Flight *f) {
  g_free(f->address);
  g_free(f->local);
  if (f->waiters) g_ptr_array_unref(f->waiters);
  g_free(f);
}

/* Detach @f from its resolver and answer every waiter. */
static void flight_complete(Flight *f, const char *pk, nh_nip05_rc rc) {
  if (f->r) g_hash_table_remove(f->r->flights, f->address);
  f->r = NULL;
  g_autoptr(GPtrArray) waiters = g_steal_pointer(&f->waiters);
  for (guint i = 0; waiters && i < waiters->len; i++) {
    GTask *t = waiters->pdata[i];
    if (pk)
      g_task_return_pointer(t, g_strdup(pk), g_free);
    else
      g_task_return_new_error(t, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              "NIP-05 resolution failed (code %d)", (int)rc);
  }
}

NspNip05 *nsp_nip05_new(void) {
  NspNip05 *r = g_new0(NspNip05, 1);
  r->cache = nh_nip05_cache_new(NH_NIP05_CACHE_POSITIVE_TTL_DEFAULT_SEC);
  r->session = nsp_http_session_new(NSP_NIP05_TIMEOUT_S);
  r->flights = g_hash_table_new(g_str_hash, g_str_equal);
  return r;
}

void nsp_nip05_free(NspNip05 *r) {
  if (!r) return;
  GList *fl = g_hash_table_get_values(r->flights);
  for (GList *l = fl; l; l = l->next) flight_complete(l->data, NULL, NH_NIP05_ERR_INTERNAL);
  g_list_free(fl);
  soup_session_abort(r->session);
  g_clear_object(&r->session);
  g_hash_table_destroy(r->flights);
  nh_nip05_cache_free(r->cache);
  g_free(r);
}

gboolean nsp_nip05_lookup_cached(NspNip05 *r, const char *address, char pk_out[65],
                                 gboolean *negative) {
  if (negative) *negative = FALSE;
  nh_nip05_result res;
  memset(&res, 0, sizeof res);
  nh_nip05_rc rc = nh_nip05_cache_lookup(r->cache, address, (int64_t)time(NULL), &res);
  if (rc == NH_NIP05_OK) {
    memcpy(pk_out, res.pubkey_hex, 65);
    return TRUE;
  }
  /* ERR_INTERNAL is the cache's "miss" sentinel. */
  if (negative) *negative = rc != NH_NIP05_ERR_INTERNAL;
  return FALSE;
}

void nsp_nip05_cache_put(NspNip05 *r, const char *address, const char *pubkey_hex) {
  nh_nip05_result res;
  memset(&res, 0, sizeof res);
  g_strlcpy(res.pubkey_hex, pubkey_hex, sizeof res.pubkey_hex);
  nh_nip05_cache_put_positive(r->cache, address, (int64_t)time(NULL), &res);
}

static void on_http(GObject *src, GAsyncResult *res, gpointer user_data) {
  Flight *f = user_data;
  NspNip05 *r = f->r;
  g_autoptr(GError) err = NULL;
  if (!r) { /* resolver freed while in flight; waiters already answered */
    GBytes *b = nsp_http_get_finish(res, NULL, NULL);
    if (b) g_bytes_unref(b);
    flight_free(f);
    return;
  }
  g_autofree char *ct = NULL;
  g_autoptr(GBytes) body = nsp_http_get_finish(res, &ct, &err);
  nh_nip05_rc rc = NH_NIP05_ERR_TRANSPORT;
  nh_nip05_result out;
  memset(&out, 0, sizeof out);
  if (!body) {
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) rc = NH_NIP05_ERR_SSRF;
    g_debug("nostr-search-provider: NIP-05 %s: %s", f->address, err->message);
  } else {
    gsize len = 0;
    const char *data = g_bytes_get_data(body, &len);
    rc = nh_nip05_parse_wellknown(data, len, f->local, &out);
  }
  int64_t now = (int64_t)time(NULL);
  if (rc == NH_NIP05_OK)
    nh_nip05_cache_put_positive(r->cache, f->address, now, &out);
  else
    nh_nip05_cache_put_negative(r->cache, f->address, now, rc);
  flight_complete(f, rc == NH_NIP05_OK ? out.pubkey_hex : NULL, rc);
  flight_free(f);
}

void nsp_nip05_resolve_async(NspNip05 *r, const char *local, const char *domain,
                             GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_autofree char *address = g_strdup_printf("%s@%s", local, domain);
  char pk[65];
  gboolean negative = FALSE;
  if (nsp_nip05_lookup_cached(r, address, pk, &negative)) {
    g_task_return_pointer(task, g_strdup(pk), g_free);
    g_object_unref(task);
    return;
  }
  if (negative) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "NIP-05 recently failed; not retrying yet");
    g_object_unref(task);
    return;
  }
  Flight *existing = g_hash_table_lookup(r->flights, address);
  if (existing) {
    g_ptr_array_add(existing->waiters, task);
    return;
  }
  nh_nip05_address a;
  char url[NH_NIP05_ADDRESS_MAX + 128];
  if (nh_nip05_parse(address, &a) != 0 || nh_nip05_wellknown_url(&a, url, sizeof url) != 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid NIP-05 address");
    g_object_unref(task);
    return;
  }
  Flight *f = g_new0(Flight, 1);
  f->r = r;
  f->address = g_strdup(address);
  f->local = g_strdup(local);
  f->waiters = g_ptr_array_new_with_free_func(g_object_unref);
  g_ptr_array_add(f->waiters, task);
  g_hash_table_insert(r->flights, f->address, f);
  nsp_http_get_async(r->session, url, "application/json", NSP_NIP05_MAX_BODY, FALSE, NULL,
                     on_http, f);
}

char *nsp_nip05_resolve_finish(GAsyncResult *res, GError **error) {
  return g_task_propagate_pointer(G_TASK(res), error);
}
