/**
 * gnostr NIP-57 Zaps Utility
 *
 * Lightning zaps implementation per NIP-57 specification.
 */

#include "zap.h"
#include "relays.h"
#include "json.h"
#include "utils.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

/* bolt11 parsing from nostrdb */
#include "bolt11/bolt11.h"
#include "bolt11/amount.h"
#include "ccan/tal/tal.h"

GQuark gnostr_zap_error_quark(void) {
  return g_quark_from_static_string("gnostr-zap-error");
}

/* ============== LNURL HTTP Resilience ============== */

/* Timeout for LNURL HTTP requests (seconds) */
#define LNURL_TIMEOUT_SECS        10
/* Maximum retry attempts (total attempts = 1 + LNURL_MAX_RETRIES) */
#define LNURL_MAX_RETRIES          2
/* Base delay for exponential backoff (milliseconds): 1s, 2s, 4s */
#define LNURL_BACKOFF_BASE_MS   1000

/* Circuit breaker: trip after this many consecutive failures */
#define CB_FAILURE_THRESHOLD       5
/* Circuit breaker: cooldown before half-open probe (seconds) */
#define CB_COOLDOWN_SECS          60

typedef enum {
  CB_CLOSED,
  CB_OPEN,
  CB_HALF_OPEN
} CircuitState;

typedef struct {
  CircuitState state;
  guint failure_count;
  gint64 last_failure_time; /* monotonic seconds */
} CircuitBreaker;

/* Global registry keyed by domain */
static GHashTable *circuit_breakers = NULL;

static CircuitBreaker *
get_circuit_breaker(const gchar *domain)
{
  if (!circuit_breakers) {
    circuit_breakers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  }
  CircuitBreaker *cb = g_hash_table_lookup(circuit_breakers, domain);
  if (!cb) {
    cb = g_new0(CircuitBreaker, 1);
    cb->state = CB_CLOSED;
    g_hash_table_insert(circuit_breakers, g_strdup(domain), cb);
  }
  return cb;
}

static gboolean
circuit_breaker_allow_request(const gchar *domain)
{
  CircuitBreaker *cb = get_circuit_breaker(domain);
  if (cb->state == CB_CLOSED) return TRUE;

  gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
  if (cb->state == CB_OPEN && (now - cb->last_failure_time) >= CB_COOLDOWN_SECS) {
    cb->state = CB_HALF_OPEN;
    return TRUE;
  }

  return cb->state == CB_HALF_OPEN;
}

static void
circuit_breaker_record_success(const gchar *domain)
{
  CircuitBreaker *cb = get_circuit_breaker(domain);
  cb->failure_count = 0;
  cb->state = CB_CLOSED;
}

static void
circuit_breaker_record_failure(const gchar *domain)
{
  CircuitBreaker *cb = get_circuit_breaker(domain);
  cb->failure_count++;
  cb->last_failure_time = g_get_monotonic_time() / G_USEC_PER_SEC;
  if (cb->failure_count >= CB_FAILURE_THRESHOLD) {
    cb->state = CB_OPEN;
    g_debug("zap: circuit breaker OPEN for %s (%u consecutive failures)",
            domain, cb->failure_count);
  }
}

static gchar *
extract_domain(const gchar *url)
{
  GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  if (!uri) return NULL;
  gchar *domain = g_strdup(g_uri_get_host(uri));
  g_uri_unref(uri);
  return domain;
}

/* Propagate user cancellation to per-request cancellable */
static void
on_user_cancelled(GCancellable *source, gpointer data)
{
  (void)source;
  g_cancellable_cancel(G_CANCELLABLE(data));
}

/* ============== Memory Management ============== */

void gnostr_lnurl_pay_info_free(GnostrLnurlPayInfo *info) {
  if (!info) return;
  g_free(info->callback);
  g_free(info->nostr_pubkey);
  g_free(info->metadata);
  g_free(info->comment_allowed);
  g_free(info);
}

void gnostr_zap_request_free(GnostrZapRequest *req) {
  if (!req) return;
  g_free(req->recipient_pubkey);
  g_free(req->event_id);
  g_free(req->lnurl);
  g_free(req->lud16);
  g_free(req->comment);
  g_strfreev(req->relays);
  g_free(req);
}

void gnostr_zap_receipt_free(GnostrZapReceipt *receipt) {
  if (!receipt) return;
  g_free(receipt->id);
  g_free(receipt->event_pubkey);
  g_free(receipt->bolt11);
  g_free(receipt->preimage);
  g_free(receipt->description);
  g_free(receipt->sender_pubkey);
  g_free(receipt->recipient_pubkey);
  g_free(receipt->event_id);
  g_free(receipt);
}

/* ============== LNURL Operations ============== */

gchar *gnostr_zap_lud16_to_lnurl(const gchar *lud16) {
  if (!lud16 || !*lud16) return NULL;

  /* Parse user@domain format */
  const gchar *at = strchr(lud16, '@');
  if (!at || at == lud16) return NULL;

  gchar *user = g_strndup(lud16, at - lud16);
  const gchar *domain = at + 1;

  if (!domain || !*domain) {
    g_free(user);
    return NULL;
  }

  /* Build the LNURL endpoint: https://domain/.well-known/lnurlp/user */
  gchar *url = g_strdup_printf("https://%s/.well-known/lnurlp/%s", domain, user);
  g_free(user);

  return url;
}

#ifdef HAVE_SOUP3

/* Context for LNURL info fetch with retry support */
typedef struct {
  GnostrLnurlInfoCallback callback;
  gpointer user_data;
  gchar *lud16;
  gchar *url;
  gchar *domain;
  guint attempt;
  guint timeout_source_id;
  GCancellable *request_cancellable;
  GCancellable *user_cancellable; /* not owned */
  gulong cancel_handler_id;
} LnurlFetchContext;

static void do_lnurl_fetch(LnurlFetchContext *ctx);

static void lnurl_fetch_ctx_free(LnurlFetchContext *ctx) {
  if (!ctx) return;
  if (ctx->timeout_source_id > 0) {
    g_source_remove(ctx->timeout_source_id);
  }
  if (ctx->cancel_handler_id && ctx->user_cancellable) {
    g_cancellable_disconnect(ctx->user_cancellable, ctx->cancel_handler_id);
  }
  g_clear_object(&ctx->request_cancellable);
  g_free(ctx->lud16);
  g_free(ctx->url);
  g_free(ctx->domain);
  g_free(ctx);
}

static gboolean lnurl_timeout_cb(gpointer user_data) {
  LnurlFetchContext *ctx = (LnurlFetchContext *)user_data;
  ctx->timeout_source_id = 0;
  g_debug("zap: LNURL request timed out for %s (attempt %u)", ctx->lud16, ctx->attempt + 1);
  g_cancellable_cancel(ctx->request_cancellable);
  return G_SOURCE_REMOVE;
}

static gboolean retry_lnurl_fetch_cb(gpointer user_data) {
  LnurlFetchContext *ctx = (LnurlFetchContext *)user_data;
  do_lnurl_fetch(ctx);
  return G_SOURCE_REMOVE;
}

static void on_lnurl_info_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  LnurlFetchContext *ctx = (LnurlFetchContext *)user_data;
  GError *error = NULL;

  /* Cancel timeout if still pending */
  if (ctx->timeout_source_id > 0) {
    g_source_remove(ctx->timeout_source_id);
    ctx->timeout_source_id = 0;
  }

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    gboolean user_cancelled = ctx->user_cancellable &&
                               g_cancellable_is_cancelled(ctx->user_cancellable);

    if (user_cancelled) {
      /* User-initiated cancellation - propagate without retry */
      if (ctx->callback) {
        ctx->callback(NULL, error, ctx->user_data);
      }
      g_clear_error(&error);
      lnurl_fetch_ctx_free(ctx);
      return;
    }

    /* Network error or timeout - maybe retry */
    circuit_breaker_record_failure(ctx->domain);

    if (ctx->attempt < LNURL_MAX_RETRIES) {
      ctx->attempt++;
      guint delay = LNURL_BACKOFF_BASE_MS * (1 << (ctx->attempt - 1));
      g_debug("zap: retrying LNURL fetch for %s (attempt %u/%u, delay %ums)",
              ctx->lud16, ctx->attempt + 1, LNURL_MAX_RETRIES + 1, delay);
      g_clear_error(&error);
      g_timeout_add(delay, retry_lnurl_fetch_cb, ctx);
      return;
    }

    /* Final failure after all retries */
    g_debug("zap: LNURL fetch failed for %s after %u attempts: %s",
            ctx->lud16, ctx->attempt + 1, error->message);

    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_TIMEOUT,
                                "LNURL endpoint timed out after %u attempts. Try again later.",
                                ctx->attempt + 1);
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    g_clear_error(&error);
    lnurl_fetch_ctx_free(ctx);
    return;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_HTTP_FAILED,
                                "Empty response from LNURL endpoint");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    if (bytes) g_bytes_unref(bytes);
    lnurl_fetch_ctx_free(ctx);
    return;
  }

  /* Convert to null-terminated string for JSON parsing */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);
  gchar *json_str = g_strndup(data, len);
  g_bytes_unref(bytes);

  if (!nostr_json_is_valid(json_str)) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                                "Failed to parse LNURL response");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    g_free(json_str);
    lnurl_fetch_ctx_free(ctx);
    return;
  }

  /* Check for error response */
  char *status = NULL;
  if (nostr_json_get_string(json_str, "status", &status) == 0 && status) {
    if (g_ascii_strcasecmp(status, "ERROR") == 0) {
      char *reason = NULL;
      nostr_json_get_string(json_str, "reason", &reason);
      if (ctx->callback) {
        GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_HTTP_FAILED,
                                  "LNURL error: %s", reason ? reason : "Unknown error");
        ctx->callback(NULL, err, ctx->user_data);
        g_error_free(err);
      }
      g_free(reason);
      g_free(status);
      g_free(json_str);
      lnurl_fetch_ctx_free(ctx);
      return;
    }
    g_free(status);
  }

  /* Parse LNURL pay info */
  GnostrLnurlPayInfo *info = g_new0(GnostrLnurlPayInfo, 1);

  nostr_json_get_string(json_str, "callback", &info->callback);

  int64_t min_val = 0, max_val = 0;
  if (nostr_json_get_int64(json_str, "minSendable", &min_val) == 0) {
    info->min_sendable = min_val;
  }
  if (nostr_json_get_int64(json_str, "maxSendable", &max_val) == 0) {
    info->max_sendable = max_val;
  }

  bool allows_nostr_val = false;
  if (nostr_json_get_bool(json_str, "allowsNostr", &allows_nostr_val) == 0) {
    info->allows_nostr = allows_nostr_val;
  }

  nostr_json_get_string(json_str, "nostrPubkey", &info->nostr_pubkey);
  nostr_json_get_string(json_str, "metadata", &info->metadata);

  int64_t comment_allowed_val = 0;
  if (nostr_json_get_int64(json_str, "commentAllowed", &comment_allowed_val) == 0) {
    info->comment_allowed = g_strdup_printf("%lld", (long long)comment_allowed_val);
  }

  g_free(json_str);

  /* Validate required fields */
  if (!info->callback) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVALID_LNURL,
                                "Missing callback URL in LNURL response");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    gnostr_lnurl_pay_info_free(info);
    lnurl_fetch_ctx_free(ctx);
    return;
  }

  circuit_breaker_record_success(ctx->domain);

  g_debug("zap: LNURL info fetched - callback=%s, allows_nostr=%d, nostr_pubkey=%.16s...",
          info->callback, info->allows_nostr, info->nostr_pubkey ? info->nostr_pubkey : "none");

  if (ctx->callback) {
    ctx->callback(info, NULL, ctx->user_data);
  } else {
    gnostr_lnurl_pay_info_free(info);
  }

  lnurl_fetch_ctx_free(ctx);
}

/* Send (or re-send on retry) the LNURL HTTP request */
static void
do_lnurl_fetch(LnurlFetchContext *ctx)
{
  /* Fresh per-request cancellable for this attempt */
  g_clear_object(&ctx->request_cancellable);
  ctx->request_cancellable = g_cancellable_new();

  /* Propagate user cancellation */
  if (ctx->cancel_handler_id && ctx->user_cancellable) {
    g_cancellable_disconnect(ctx->user_cancellable, ctx->cancel_handler_id);
    ctx->cancel_handler_id = 0;
  }
  if (ctx->user_cancellable) {
    ctx->cancel_handler_id = g_cancellable_connect(
        ctx->user_cancellable, G_CALLBACK(on_user_cancelled),
        ctx->request_cancellable, NULL);
  }

  SoupSession *session = gnostr_get_shared_soup_session();
  SoupMessage *msg = soup_message_new("GET", ctx->url);

  if (!msg) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVALID_LNURL,
                                "Failed to create HTTP request");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    lnurl_fetch_ctx_free(ctx);
    return;
  }

  soup_message_headers_append(soup_message_get_request_headers(msg), "Accept", "application/json");

  /* Start per-request timeout */
  ctx->timeout_source_id = g_timeout_add_seconds(LNURL_TIMEOUT_SECS, lnurl_timeout_cb, ctx);

  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT,
                                   ctx->request_cancellable,
                                   on_lnurl_info_http_done, ctx);
  g_object_unref(msg);
}

void gnostr_zap_fetch_lnurl_info_async(const gchar *lud16,
                                       GnostrLnurlInfoCallback callback,
                                       gpointer user_data,
                                       GCancellable *cancellable) {
  if (!lud16 || !*lud16) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVALID_LNURL,
                                "Invalid lightning address");
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    return;
  }

  gchar *url = gnostr_zap_lud16_to_lnurl(lud16);
  if (!url) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVALID_LNURL,
                                "Could not parse lightning address: %s", lud16);
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    return;
  }

  /* Check circuit breaker before making request */
  gchar *domain = extract_domain(url);
  if (domain && !circuit_breaker_allow_request(domain)) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_CIRCUIT_OPEN,
                                "LNURL endpoint for %s is temporarily unavailable. Try again later.",
                                lud16);
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    g_free(domain);
    g_free(url);
    return;
  }

  g_debug("zap: fetching LNURL info from %s", url);

  LnurlFetchContext *ctx = g_new0(LnurlFetchContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->lud16 = g_strdup(lud16);
  ctx->url = url; /* takes ownership */
  ctx->domain = domain; /* takes ownership */
  ctx->user_cancellable = cancellable;

  do_lnurl_fetch(ctx);
}

/* Context for invoice request with timeout */
typedef struct {
  GnostrZapInvoiceCallback callback;
  gpointer user_data;
  guint timeout_source_id;
  GCancellable *request_cancellable;
  GCancellable *user_cancellable; /* not owned */
  gulong cancel_handler_id;
} InvoiceRequestContext;

static void invoice_request_ctx_free(InvoiceRequestContext *ctx) {
  if (!ctx) return;
  if (ctx->timeout_source_id > 0) {
    g_source_remove(ctx->timeout_source_id);
  }
  if (ctx->cancel_handler_id && ctx->user_cancellable) {
    g_cancellable_disconnect(ctx->user_cancellable, ctx->cancel_handler_id);
  }
  g_clear_object(&ctx->request_cancellable);
  g_free(ctx);
}

static gboolean invoice_timeout_cb(gpointer user_data) {
  InvoiceRequestContext *ctx = (InvoiceRequestContext *)user_data;
  ctx->timeout_source_id = 0;
  g_debug("zap: invoice request timed out");
  g_cancellable_cancel(ctx->request_cancellable);
  return G_SOURCE_REMOVE;
}

static void on_invoice_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  InvoiceRequestContext *ctx = (InvoiceRequestContext *)user_data;
  GError *error = NULL;

  /* Cancel timeout if still pending */
  if (ctx->timeout_source_id > 0) {
    g_source_remove(ctx->timeout_source_id);
    ctx->timeout_source_id = 0;
  }

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    gboolean user_cancelled = ctx->user_cancellable &&
                               g_cancellable_is_cancelled(ctx->user_cancellable);

    if (user_cancelled) {
      if (ctx->callback) {
        ctx->callback(NULL, error, ctx->user_data);
      }
    } else {
      g_debug("zap: invoice request error: %s", error->message);
      if (ctx->callback) {
        GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_TIMEOUT,
                                  "Invoice request timed out. Try again.");
        ctx->callback(NULL, err, ctx->user_data);
        g_error_free(err);
      }
    }
    g_clear_error(&error);
    invoice_request_ctx_free(ctx);
    return;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVOICE_FAILED,
                                "Empty response from callback");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    if (bytes) g_bytes_unref(bytes);
    invoice_request_ctx_free(ctx);
    return;
  }

  /* Convert to null-terminated string for JSON parsing */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);
  gchar *json_str = g_strndup(data, len);
  g_bytes_unref(bytes);

  if (!nostr_json_is_valid(json_str)) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                                "Failed to parse invoice response");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    g_free(json_str);
    invoice_request_ctx_free(ctx);
    return;
  }

  /* Check for error response */
  char *status = NULL;
  if (nostr_json_get_string(json_str, "status", &status) == 0 && status) {
    if (g_ascii_strcasecmp(status, "ERROR") == 0) {
      char *reason = NULL;
      nostr_json_get_string(json_str, "reason", &reason);
      if (ctx->callback) {
        GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVOICE_FAILED,
                                  "Invoice error: %s", reason ? reason : "Unknown error");
        ctx->callback(NULL, err, ctx->user_data);
        g_error_free(err);
      }
      g_free(reason);
      g_free(status);
      g_free(json_str);
      invoice_request_ctx_free(ctx);
      return;
    }
    g_free(status);
  }

  /* Extract the invoice */
  char *bolt11 = NULL;
  if (nostr_json_get_string(json_str, "pr", &bolt11) != 0 || !bolt11) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVOICE_FAILED,
                                "No invoice in response");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    g_free(json_str);
    invoice_request_ctx_free(ctx);
    return;
  }

  g_debug("zap: received invoice: %.40s...", bolt11);

  if (ctx->callback) {
    ctx->callback(bolt11, NULL, ctx->user_data);
  }

  g_free(bolt11);
  g_free(json_str);
  invoice_request_ctx_free(ctx);
}

void gnostr_zap_request_invoice_async(const GnostrLnurlPayInfo *lnurl_info,
                                      const gchar *signed_zap_request_json,
                                      gint64 amount_msat,
                                      GnostrZapInvoiceCallback callback,
                                      gpointer user_data,
                                      GCancellable *cancellable) {
  if (!lnurl_info || !lnurl_info->callback) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVALID_LNURL,
                                "Missing LNURL callback");
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    return;
  }

  /* Validate amount */
  if (amount_msat < lnurl_info->min_sendable || amount_msat > lnurl_info->max_sendable) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_AMOUNT_OUT_OF_RANGE,
                                "Amount %lld msat out of range [%lld, %lld]",
                                (long long)amount_msat,
                                (long long)lnurl_info->min_sendable,
                                (long long)lnurl_info->max_sendable);
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    return;
  }

  /* Build the callback URL with query parameters */
  gchar *encoded_nostr = g_uri_escape_string(signed_zap_request_json, NULL, TRUE);

  /* Determine URL separator */
  gboolean has_query = strchr(lnurl_info->callback, '?') != NULL;
  gchar *url = g_strdup_printf("%s%camount=%lld&nostr=%s",
                               lnurl_info->callback,
                               has_query ? '&' : '?',
                               (long long)amount_msat,
                               encoded_nostr);
  g_free(encoded_nostr);

  g_debug("zap: requesting invoice from %s", lnurl_info->callback);

  InvoiceRequestContext *ctx = g_new0(InvoiceRequestContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->user_cancellable = cancellable;
  ctx->request_cancellable = g_cancellable_new();

  /* Propagate user cancellation */
  if (cancellable) {
    ctx->cancel_handler_id = g_cancellable_connect(
        cancellable, G_CALLBACK(on_user_cancelled),
        ctx->request_cancellable, NULL);
  }

  SoupSession *session = gnostr_get_shared_soup_session();

  SoupMessage *msg = soup_message_new("GET", url);
  g_free(url);

  if (!msg) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVOICE_FAILED,
                                "Failed to create HTTP request");
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    invoice_request_ctx_free(ctx);
    return;
  }

  soup_message_headers_append(soup_message_get_request_headers(msg), "Accept", "application/json");

  /* Start per-request timeout */
  ctx->timeout_source_id = g_timeout_add_seconds(LNURL_TIMEOUT_SECS, invoice_timeout_cb, ctx);

  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT,
                                   ctx->request_cancellable,
                                   on_invoice_http_done, ctx);

  g_object_unref(msg);
}

#else /* !HAVE_SOUP3 */

void gnostr_zap_fetch_lnurl_info_async(const gchar *lud16,
                                       GnostrLnurlInfoCallback callback,
                                       gpointer user_data,
                                       GCancellable *cancellable) {
  (void)lud16;
  (void)cancellable;

  if (callback) {
    GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_HTTP_FAILED,
                              "HTTP support not available (libsoup3 not linked)");
    callback(NULL, err, user_data);
    g_error_free(err);
  }
}

void gnostr_zap_request_invoice_async(const GnostrLnurlPayInfo *lnurl_info,
                                      const gchar *signed_zap_request_json,
                                      gint64 amount_msat,
                                      GnostrZapInvoiceCallback callback,
                                      gpointer user_data,
                                      GCancellable *cancellable) {
  (void)lnurl_info;
  (void)signed_zap_request_json;
  (void)amount_msat;
  (void)cancellable;

  if (callback) {
    GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_HTTP_FAILED,
                              "HTTP support not available (libsoup3 not linked)");
    callback(NULL, err, user_data);
    g_error_free(err);
  }
}

#endif /* HAVE_SOUP3 */

/* ============== Zap Request Creation ============== */

gchar *gnostr_zap_create_request_event(const GnostrZapRequest *req,
                                       const gchar *sender_pubkey) {
  if (!req || !req->recipient_pubkey || !sender_pubkey) {
    return NULL;
  }

  NostrJsonBuilder *builder = nostr_json_builder_new();
  nostr_json_builder_begin_object(builder);

  /* Kind 9734 - zap request */
  nostr_json_builder_set_key(builder, "kind");
  nostr_json_builder_add_int(builder, 9734);

  /* Content - optional comment */
  nostr_json_builder_set_key(builder, "content");
  nostr_json_builder_add_string(builder, req->comment ? req->comment : "");

  /* Pubkey - the sender's pubkey */
  nostr_json_builder_set_key(builder, "pubkey");
  nostr_json_builder_add_string(builder, sender_pubkey);

  /* Created at */
  nostr_json_builder_set_key(builder, "created_at");
  nostr_json_builder_add_int(builder, (int64_t)time(NULL));

  /* Tags */
  nostr_json_builder_set_key(builder, "tags");
  nostr_json_builder_begin_array(builder);

  /* relays tag - required */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "relays");
  if (req->relays) {
    for (int i = 0; req->relays[i]; i++) {
      nostr_json_builder_add_string(builder, req->relays[i]);
    }
  } else {
    /* Get relays from config (GSettings defaults if none configured) */
    GPtrArray *cfg_relays = gnostr_get_write_relay_urls();
    for (guint i = 0; i < cfg_relays->len; i++) {
      nostr_json_builder_add_string(builder, g_ptr_array_index(cfg_relays, i));
    }
    g_ptr_array_unref(cfg_relays);
  }
  nostr_json_builder_end_array(builder);

  /* amount tag - recommended */
  if (req->amount_msat > 0) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "amount");
    gchar *amount_str = g_strdup_printf("%lld", (long long)req->amount_msat);
    nostr_json_builder_add_string(builder, amount_str);
    g_free(amount_str);
    nostr_json_builder_end_array(builder);
  }

  /* lnurl tag - recommended (bech32 encoded) */
  if (req->lnurl) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "lnurl");
    nostr_json_builder_add_string(builder, req->lnurl);
    nostr_json_builder_end_array(builder);
  }

  /* p tag - required (recipient pubkey) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "p");
  nostr_json_builder_add_string(builder, req->recipient_pubkey);
  nostr_json_builder_end_array(builder);

  /* e tag - required if zapping an event */
  if (req->event_id) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "e");
    nostr_json_builder_add_string(builder, req->event_id);
    nostr_json_builder_end_array(builder);
  }

  /* k tag - optional, kind of target event */
  if (req->event_id && req->event_kind > 0) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "k");
    gchar *kind_str = g_strdup_printf("%d", req->event_kind);
    nostr_json_builder_add_string(builder, kind_str);
    g_free(kind_str);
    nostr_json_builder_end_array(builder);
  }

  nostr_json_builder_end_array(builder);  /* end tags */
  nostr_json_builder_end_object(builder); /* end event */

  gchar *result = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);

  return result;
}

/* ============== Zap Receipt Parsing ============== */

/* Context for parsing receipt tags */
typedef struct {
  GnostrZapReceipt *receipt;
} ZapReceiptTagCtx;

/* Callback for parsing receipt tags */
static bool parse_receipt_tag_cb(size_t idx, const char *element_json, void *user_data) {
  (void)idx;
  ZapReceiptTagCtx *ctx = (ZapReceiptTagCtx *)user_data;

  if (!nostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  char *tag_value = NULL;

  if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 ||
      nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0) {
    g_free(tag_name);
    g_free(tag_value);
    return true;
  }

  if (g_strcmp0(tag_name, "bolt11") == 0) {
    ctx->receipt->bolt11 = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "preimage") == 0) {
    ctx->receipt->preimage = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "description") == 0) {
    ctx->receipt->description = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "p") == 0) {
    ctx->receipt->recipient_pubkey = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "P") == 0) {
    ctx->receipt->sender_pubkey = tag_value;
    tag_value = NULL;
  } else if (g_strcmp0(tag_name, "e") == 0) {
    ctx->receipt->event_id = tag_value;
    tag_value = NULL;
  }

  g_free(tag_name);
  g_free(tag_value);
  return true;
}

/* Context for parsing zap request tags (from description) */
typedef struct {
  gint64 *amount_msat;
  gboolean found;
} ZapReqAmountCtx;

/* Callback for parsing zap request amount tag */
static bool parse_zap_req_amount_cb(size_t idx, const char *element_json, void *user_data) {
  (void)idx;
  ZapReqAmountCtx *ctx = (ZapReqAmountCtx *)user_data;

  if (ctx->found) return false;  /* Stop if already found */
  if (!nostr_json_is_array_str(element_json)) return true;

  char *tag_name = NULL;
  char *tag_value = NULL;

  if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 ||
      nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0) {
    g_free(tag_name);
    g_free(tag_value);
    return true;
  }

  if (g_strcmp0(tag_name, "amount") == 0) {
    *ctx->amount_msat = g_ascii_strtoll(tag_value, NULL, 10);
    ctx->found = TRUE;
    g_debug("zap: parsed zap request amount: %lld msat", (long long)*ctx->amount_msat);
  }

  g_free(tag_name);
  g_free(tag_value);
  return !ctx->found;
}

GnostrZapReceipt *gnostr_zap_parse_receipt(const gchar *event_json) {
  if (!event_json || !*event_json) {
    return NULL;
  }

  if (!nostr_json_is_valid(event_json)) {
    g_debug("zap: failed to parse receipt JSON");
    return NULL;
  }

  /* Verify kind 9735 */
  int64_t kind_val = 0;
  if (nostr_json_get_int64(event_json, "kind", &kind_val) != 0 || kind_val != 9735) {
    return NULL;
  }

  GnostrZapReceipt *receipt = g_new0(GnostrZapReceipt, 1);

  char *id_val = NULL;
  if (nostr_json_get_string(event_json, "id", &id_val) == 0 && id_val) {
    receipt->id = id_val;
  }

  /* Extract event pubkey for validation against expected_nostr_pubkey */
  char *pubkey_val = NULL;
  if (nostr_json_get_string(event_json, "pubkey", &pubkey_val) == 0 && pubkey_val) {
    receipt->event_pubkey = pubkey_val;
  }

  int64_t created_val = 0;
  if (nostr_json_get_int64(event_json, "created_at", &created_val) == 0) {
    receipt->created_at = created_val;
  }

  /* Parse tags */
  char *tags_json = NULL;
  if (nostr_json_get_raw(event_json, "tags", &tags_json) == 0 && tags_json) {
    ZapReceiptTagCtx ctx = { .receipt = receipt };
    nostr_json_array_foreach_root(tags_json, parse_receipt_tag_cb, &ctx);
    g_free(tags_json);
  }

  /* Parse amount from bolt11 invoice using nostrdb's bolt11 decoder */
  if (receipt->bolt11) {
    char *fail = NULL;
    struct bolt11 *b11 = bolt11_decode_minimal(NULL, receipt->bolt11, &fail);
    if (b11) {
      if (b11->msat) {
        receipt->amount_msat = (gint64)b11->msat->millisatoshis;
        g_debug("zap: parsed bolt11 amount: %lld msat", (long long)receipt->amount_msat);
      } else {
        g_debug("zap: bolt11 invoice has no amount specified");
        receipt->amount_msat = 0;
      }
      tal_free(b11);
    } else {
      g_debug("zap: failed to parse bolt11 invoice: %s", fail ? fail : "unknown error");
      if (fail) tal_free(fail);
      receipt->amount_msat = 0;
    }
  }

  /* Parse zap request amount from description for validation */
  if (receipt->description && nostr_json_is_valid(receipt->description)) {
    char *zap_tags_json = NULL;
    if (nostr_json_get_raw(receipt->description, "tags", &zap_tags_json) == 0 && zap_tags_json) {
      ZapReqAmountCtx ctx = { .amount_msat = &receipt->zap_request_amount_msat, .found = FALSE };
      nostr_json_array_foreach_root(zap_tags_json, parse_zap_req_amount_cb, &ctx);
      g_free(zap_tags_json);
    }
  }

  return receipt;
}

gboolean gnostr_zap_validate_receipt(const GnostrZapReceipt *receipt,
                                     const gchar *expected_nostr_pubkey,
                                     GError **error) {
  if (!receipt) {
    g_set_error(error, GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                "Receipt is NULL");
    return FALSE;
  }

  /* Must have bolt11 */
  if (!receipt->bolt11) {
    g_set_error(error, GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                "Receipt missing bolt11 tag");
    return FALSE;
  }

  /* Must have description (the zap request) */
  if (!receipt->description) {
    g_set_error(error, GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                "Receipt missing description tag");
    return FALSE;
  }

  /* NIP-57 Appendix F: Verify that the receipt's pubkey matches expected_nostr_pubkey
   * "The zap receipt event's pubkey MUST be the same as the recipient's lnurl
   * provider's nostrPubkey (retrieved in step 1 of the protocol flow)." */
  if (expected_nostr_pubkey && expected_nostr_pubkey[0] != '\0') {
    if (!receipt->event_pubkey) {
      g_set_error(error, GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                  "Receipt missing event pubkey for validation");
      return FALSE;
    }
    if (g_ascii_strcasecmp(receipt->event_pubkey, expected_nostr_pubkey) != 0) {
      g_set_error(error, GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                  "Receipt pubkey %s does not match expected %s",
                  receipt->event_pubkey, expected_nostr_pubkey);
      return FALSE;
    }
    g_debug("zap: receipt pubkey validation passed");
  }

  /* NIP-57 Appendix F: Verify that invoiceAmount in bolt11 matches zap request amount
   * "The invoiceAmount contained in the bolt11 tag of the zap receipt MUST equal
   * the amount tag of the zap request (if present)." */
  if (receipt->zap_request_amount_msat > 0) {
    if (receipt->amount_msat <= 0) {
      g_set_error(error, GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                  "Could not parse bolt11 amount for validation");
      return FALSE;
    }
    if (receipt->amount_msat != receipt->zap_request_amount_msat) {
      g_set_error(error, GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                  "Bolt11 amount %lld msat does not match zap request amount %lld msat",
                  (long long)receipt->amount_msat,
                  (long long)receipt->zap_request_amount_msat);
      return FALSE;
    }
    g_debug("zap: amount validation passed (%lld msat)", (long long)receipt->amount_msat);
  }

  return TRUE;
}

/* ============== Utility Functions ============== */

gchar *gnostr_zap_format_amount(gint64 amount_msat) {
  gint64 sats = amount_msat / 1000;

  if (sats >= 1000000) {
    return g_strdup_printf("%.1fM sats", sats / 1000000.0);
  } else if (sats >= 10000) {
    return g_strdup_printf("%.1fK sats", sats / 1000.0);
  } else if (sats >= 1000) {
    return g_strdup_printf("%'lld sats", (long long)sats);
  } else {
    return g_strdup_printf("%lld sats", (long long)sats);
  }
}
