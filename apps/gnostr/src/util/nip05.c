#include "nip05.h"
#include "utils.h"
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif
#include <nostr-gobject-1.0/nostr_json.h>

/* Cache configuration */
#define NIP05_CACHE_TTL_SECONDS (60 * 60)  /* 1 hour cache validity */
#define NIP05_CACHE_MAX_ENTRIES 500        /* Max cached entries */

/* NIP-05 verification cache */
static GHashTable *nip05_cache = NULL;  /* key: char* identifier, value: GnostrNip05Result* */
static GMutex nip05_cache_mutex;
static gboolean nip05_cache_initialized = FALSE;

/* Context for async verification */
typedef struct {
  char *identifier;
  char *expected_pubkey;
  char *local_part;
  char *domain;
  GnostrNip05VerifyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} Nip05VerifyContext;

/* Forward declarations */
static void nip05_verify_ctx_free(Nip05VerifyContext *ctx);
static void ensure_nip05_cache(void);
static gboolean is_valid_domain(const char *domain);
static gboolean is_valid_local_part(const char *local);

const char *gnostr_nip05_status_to_string(GnostrNip05Status status) {
  switch (status) {
    case GNOSTR_NIP05_STATUS_UNKNOWN:   return "unknown";
    case GNOSTR_NIP05_STATUS_VERIFYING: return "verifying";
    case GNOSTR_NIP05_STATUS_VERIFIED:  return "verified";
    case GNOSTR_NIP05_STATUS_FAILED:    return "failed";
    case GNOSTR_NIP05_STATUS_INVALID:   return "invalid";
    default:                            return "unknown";
  }
}

void gnostr_nip05_result_free(GnostrNip05Result *result) {
  if (!result) return;
  g_free(result->identifier);
  g_free(result->pubkey_hex);
  if (result->relays) {
    g_strfreev(result->relays);
  }
  g_free(result);
}

static GnostrNip05Result *nip05_result_copy(const GnostrNip05Result *src) {
  if (!src) return NULL;
  GnostrNip05Result *dst = g_new0(GnostrNip05Result, 1);
  dst->status = src->status;
  dst->identifier = g_strdup(src->identifier);
  dst->pubkey_hex = g_strdup(src->pubkey_hex);
  dst->verified_at = src->verified_at;
  dst->expires_at = src->expires_at;
  if (src->relays) {
    dst->relays = g_strdupv(src->relays);
  }
  return dst;
}

static void nip05_verify_ctx_free(Nip05VerifyContext *ctx) {
  if (!ctx) return;
  g_free(ctx->identifier);
  g_free(ctx->expected_pubkey);
  g_free(ctx->local_part);
  g_free(ctx->domain);
  if (ctx->cancellable) g_object_unref(ctx->cancellable);
  g_free(ctx);
}

static void ensure_nip05_cache(void) {
  if (nip05_cache_initialized) return;
  g_mutex_init(&nip05_cache_mutex);
  nip05_cache = g_hash_table_new_full(
    g_str_hash, g_str_equal,
    g_free,
    (GDestroyNotify)gnostr_nip05_result_free
  );
  nip05_cache_initialized = TRUE;
  g_debug("nip05: cache initialized");
}

/* Validate local-part: alphanumeric, _, -, . allowed */
static gboolean is_valid_local_part(const char *local) {
  if (!local || !*local) return FALSE;
  for (const char *p = local; *p; p++) {
    char c = *p;
    if (!g_ascii_isalnum(c) && c != '_' && c != '-' && c != '.') {
      return FALSE;
    }
  }
  return TRUE;
}

/* Validate domain: basic check for valid hostname format */
static gboolean is_valid_domain(const char *domain) {
  if (!domain || !*domain) return FALSE;
  if (strlen(domain) > 253) return FALSE;

  /* Must contain at least one dot */
  if (!strchr(domain, '.')) return FALSE;

  /* Basic character validation */
  for (const char *p = domain; *p; p++) {
    char c = *p;
    if (!g_ascii_isalnum(c) && c != '-' && c != '.') {
      return FALSE;
    }
  }

  /* Cannot start or end with dot or hyphen */
  if (domain[0] == '.' || domain[0] == '-') return FALSE;
  size_t len = strlen(domain);
  if (domain[len - 1] == '.' || domain[len - 1] == '-') return FALSE;

  return TRUE;
}

gboolean gnostr_nip05_parse(const char *identifier, char **out_local, char **out_domain) {
  if (!identifier || !*identifier) return FALSE;

  /* Find the @ symbol */
  const char *at = strchr(identifier, '@');
  if (!at) return FALSE;

  /* Extract local-part */
  size_t local_len = at - identifier;
  if (local_len == 0) return FALSE;

  char *local = g_strndup(identifier, local_len);
  char *domain = g_strdup(at + 1);

  /* Validate parts */
  if (!is_valid_local_part(local) || !is_valid_domain(domain)) {
    g_free(local);
    g_free(domain);
    return FALSE;
  }

  if (out_local) *out_local = local;
  else g_free(local);

  if (out_domain) *out_domain = domain;
  else g_free(domain);

  return TRUE;
}

char *gnostr_nip05_get_display(const char *identifier) {
  if (!identifier || !*identifier) return NULL;

  char *local = NULL;
  char *domain = NULL;

  if (!gnostr_nip05_parse(identifier, &local, &domain)) {
    return g_strdup(identifier);
  }

  char *display = NULL;

  /* Special case: _@domain.com shows as @domain.com */
  if (g_strcmp0(local, "_") == 0) {
    display = g_strdup_printf("@%s", domain);
  } else {
    display = g_strdup(identifier);
  }

  g_free(local);
  g_free(domain);

  return display;
}

GnostrNip05Result *gnostr_nip05_cache_get(const char *identifier) {
  if (!identifier || !*identifier) return NULL;

  ensure_nip05_cache();

  g_mutex_lock(&nip05_cache_mutex);
  GnostrNip05Result *cached = g_hash_table_lookup(nip05_cache, identifier);
  GnostrNip05Result *result = NULL;

  if (cached) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (cached->expires_at > now) {
      /* Cache hit and still valid */
      result = nip05_result_copy(cached);
      g_debug("nip05: cache hit for %s (status=%s, expires_in=%llds)",
              identifier, gnostr_nip05_status_to_string(cached->status),
              (long long)(cached->expires_at - now));
    } else {
      /* Expired, remove from cache */
      g_hash_table_remove(nip05_cache, identifier);
      g_debug("nip05: cache expired for %s", identifier);
    }
  }

  g_mutex_unlock(&nip05_cache_mutex);
  return result;
}

void gnostr_nip05_cache_put(GnostrNip05Result *result) {
  if (!result || !result->identifier) return;

  ensure_nip05_cache();

  g_mutex_lock(&nip05_cache_mutex);

  /* Evict oldest entries if cache is full */
  while (g_hash_table_size(nip05_cache) >= NIP05_CACHE_MAX_ENTRIES) {
    GHashTableIter iter;
    gpointer key, value;
    const char *oldest_key = NULL;
    gint64 oldest_time = G_MAXINT64;

    g_hash_table_iter_init(&iter, nip05_cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      GnostrNip05Result *r = value;
      if (r->verified_at < oldest_time) {
        oldest_time = r->verified_at;
        oldest_key = key;
      }
    }

    if (oldest_key) {
      g_debug("nip05: evicting oldest cache entry %s", oldest_key);
      g_hash_table_remove(nip05_cache, oldest_key);
    } else {
      break;
    }
  }

  /* Store the result */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  result->verified_at = now;
  result->expires_at = now + NIP05_CACHE_TTL_SECONDS;

  g_hash_table_insert(nip05_cache, g_strdup(result->identifier), result);
  g_debug("nip05: cached result for %s (status=%s)",
          result->identifier, gnostr_nip05_status_to_string(result->status));

  g_mutex_unlock(&nip05_cache_mutex);
}

void gnostr_nip05_cache_cleanup(void) {
  if (!nip05_cache_initialized) return;

  g_mutex_lock(&nip05_cache_mutex);

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  GHashTableIter iter;
  gpointer key, value;
  GPtrArray *to_remove = g_ptr_array_new();

  g_hash_table_iter_init(&iter, nip05_cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnostrNip05Result *r = value;
    if (r->expires_at <= now) {
      g_ptr_array_add(to_remove, g_strdup(key));
    }
  }

  for (guint i = 0; i < to_remove->len; i++) {
    const char *k = g_ptr_array_index(to_remove, i);
    g_hash_table_remove(nip05_cache, k);
    g_debug("nip05: cleanup removed expired entry %s", k);
  }

  g_ptr_array_free(to_remove, TRUE);
  g_mutex_unlock(&nip05_cache_mutex);
}

GtkWidget *gnostr_nip05_create_badge(void) {
  GtkWidget *icon = gtk_image_new_from_icon_name("emblem-ok-symbolic");
  gtk_widget_add_css_class(icon, "nip05-verified-badge");
  gtk_widget_set_tooltip_text(icon, "NIP-05 Verified");
  return icon;
}

#ifdef HAVE_SOUP3

static void on_nip05_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip05VerifyContext *ctx = (Nip05VerifyContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  /* Create result */
  GnostrNip05Result *result = g_new0(GnostrNip05Result, 1);
  result->identifier = g_strdup(ctx->identifier);
  result->status = GNOSTR_NIP05_STATUS_FAILED;

  if (error) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("nip05: verification cancelled for %s", ctx->identifier);
    } else {
      g_debug("nip05: HTTP error for %s: %s", ctx->identifier, error->message);
    }
    g_clear_error(&error);
    goto done;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    g_debug("nip05: empty response for %s", ctx->identifier);
    if (bytes) g_bytes_unref(bytes);
    goto done;
  }

  /* Parse JSON response - convert bytes to null-terminated string */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);
  char *json_str = g_strndup(data, len);
  g_bytes_unref(bytes);

  if (!gnostr_json_is_valid(json_str)) {
    g_debug("nip05: JSON parse error for %s", ctx->identifier);
    g_free(json_str);
    goto done;
  }

  /* Look up the local-part in names object using facade */
  char *found_pubkey = NULL;
  found_pubkey = gnostr_json_get_string_at(json_str, "names", ctx->local_part, NULL);
  if (!found_pubkey) {
    g_debug("nip05: no entry for '%s' in names for %s", ctx->local_part, ctx->identifier);
    g_free(json_str);
    goto done;
  }

  if (strlen(found_pubkey) != 64) {
    g_debug("nip05: invalid pubkey format for %s", ctx->identifier);
    free(found_pubkey);
    g_free(json_str);
    goto done;
  }

  /* Verify pubkey matches (case-insensitive) */
  if (g_ascii_strcasecmp(found_pubkey, ctx->expected_pubkey) != 0) {
    g_debug("nip05: pubkey mismatch for %s (expected %s, got %s)",
            ctx->identifier, ctx->expected_pubkey, found_pubkey);
    free(found_pubkey);
    g_free(json_str);
    goto done;
  }

  /* Verification successful */
  result->status = GNOSTR_NIP05_STATUS_VERIFIED;
  result->pubkey_hex = g_strdup(found_pubkey);
  g_debug("nip05: verified %s -> %s", ctx->identifier, found_pubkey);

  /* Optionally extract relays using facade */
  GStrv relay_array = gnostr_json_get_string_array_at(json_str, "relays", found_pubkey, NULL);
  if (relay_array) {
    guint relay_count = g_strv_length(relay_array);
    guint valid = 0;
    if (relay_count > 0) {
      result->relays = g_new0(char *, relay_count + 1);
      for (guint i = 0; i < relay_count; i++) {
        const char *relay_url = relay_array[i];
        if (relay_url && (g_str_has_prefix(relay_url, "wss://") ||
                          g_str_has_prefix(relay_url, "ws://"))) {
          result->relays[valid++] = g_strdup(relay_url);
        }
      }
    }
    g_strfreev(relay_array);
    if (valid == 0) {
      g_free(result->relays);
      result->relays = NULL;
    }
  }

  free(found_pubkey);
  g_free(json_str);

done:
  /* Cache the result */
  gnostr_nip05_cache_put(nip05_result_copy(result));

  /* Invoke callback */
  if (ctx->callback) {
    ctx->callback(result, ctx->user_data);
  } else {
    gnostr_nip05_result_free(result);
  }

  nip05_verify_ctx_free(ctx);
}

void gnostr_nip05_verify_async(const char *identifier,
                               const char *expected_pubkey,
                               GnostrNip05VerifyCallback callback,
                               gpointer user_data,
                               GCancellable *cancellable) {
  if (!identifier || !expected_pubkey || strlen(expected_pubkey) != 64) {
    if (callback) {
      GnostrNip05Result *result = g_new0(GnostrNip05Result, 1);
      result->identifier = g_strdup(identifier ? identifier : "");
      result->status = GNOSTR_NIP05_STATUS_INVALID;
      callback(result, user_data);
    }
    return;
  }

  /* Check cache first */
  GnostrNip05Result *cached = gnostr_nip05_cache_get(identifier);
  if (cached) {
    /* Verify cached result matches expected pubkey */
    if (cached->status == GNOSTR_NIP05_STATUS_VERIFIED &&
        cached->pubkey_hex &&
        g_ascii_strcasecmp(cached->pubkey_hex, expected_pubkey) == 0) {
      if (callback) callback(cached, user_data);
      else gnostr_nip05_result_free(cached);
      return;
    }
    /* If cached but pubkey doesn't match, invalidate and re-verify */
    gnostr_nip05_result_free(cached);
  }

  /* Parse identifier */
  char *local = NULL;
  char *domain = NULL;
  if (!gnostr_nip05_parse(identifier, &local, &domain)) {
    if (callback) {
      GnostrNip05Result *result = g_new0(GnostrNip05Result, 1);
      result->identifier = g_strdup(identifier);
      result->status = GNOSTR_NIP05_STATUS_INVALID;
      callback(result, user_data);
    }
    return;
  }

  /* Build URL: https://domain/.well-known/nostr.json?name=local */
  char *encoded_local = g_uri_escape_string(local, NULL, TRUE);
  char *url = g_strdup_printf("https://%s/.well-known/nostr.json?name=%s", domain, encoded_local);
  g_free(encoded_local);

  g_debug("nip05: verifying %s via %s", identifier, url);

  /* Create context */
  Nip05VerifyContext *ctx = g_new0(Nip05VerifyContext, 1);
  ctx->identifier = g_strdup(identifier);
  ctx->expected_pubkey = g_strdup(expected_pubkey);
  ctx->local_part = local;  /* transfer ownership */
  ctx->domain = domain;     /* transfer ownership */
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

  /* nostrc-201: Use shared SoupSession to avoid TLS cleanup issues with multiple sessions */
  SoupSession *session = gnostr_get_shared_soup_session();

  SoupMessage *msg = soup_message_new("GET", url);
  g_free(url);

  if (!msg) {
    g_warning("nip05: failed to create HTTP message for %s", identifier);
    if (callback) {
      GnostrNip05Result *result = g_new0(GnostrNip05Result, 1);
      result->identifier = g_strdup(identifier);
      result->status = GNOSTR_NIP05_STATUS_FAILED;
      callback(result, user_data);
    }
    nip05_verify_ctx_free(ctx);
    return;
  }

  /* Set Accept header */
  soup_message_headers_append(soup_message_get_request_headers(msg), "Accept", "application/json");

  /* Start async request */
  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, ctx->cancellable, on_nip05_http_done, ctx);

  g_object_unref(msg);
  /* Note: Don't unref shared session - we don't own it */
}

#else /* !HAVE_SOUP3 */

void gnostr_nip05_verify_async(const char *identifier,
                               const char *expected_pubkey,
                               GnostrNip05VerifyCallback callback,
                               gpointer user_data,
                               GCancellable *cancellable) {
  (void)cancellable;

  /* Without libsoup, we cannot verify NIP-05 */
  if (callback) {
    GnostrNip05Result *result = g_new0(GnostrNip05Result, 1);
    result->identifier = g_strdup(identifier ? identifier : "");
    result->status = GNOSTR_NIP05_STATUS_UNKNOWN;
    callback(result, user_data);
  }
}

#endif /* HAVE_SOUP3 */
