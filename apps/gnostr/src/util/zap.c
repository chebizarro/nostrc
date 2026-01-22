/**
 * gnostr NIP-57 Zaps Utility
 *
 * Lightning zaps implementation per NIP-57 specification.
 */

#include "zap.h"
#include "relays.h"
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <jansson.h>
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

/* Context for LNURL info fetch */
typedef struct {
  GnostrLnurlInfoCallback callback;
  gpointer user_data;
  gchar *lud16;
} LnurlFetchContext;

static void lnurl_fetch_ctx_free(LnurlFetchContext *ctx) {
  if (!ctx) return;
  g_free(ctx->lud16);
  g_free(ctx);
}

static void on_lnurl_info_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  LnurlFetchContext *ctx = (LnurlFetchContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("zap: LNURL fetch error for %s: %s", ctx->lud16, error->message);
    }
    if (ctx->callback) {
      ctx->callback(NULL, error, ctx->user_data);
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

  /* Parse JSON response */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);

  json_error_t json_err;
  json_t *root = json_loadb(data, len, 0, &json_err);
  g_bytes_unref(bytes);

  if (!root) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                                "Failed to parse LNURL response: %s", json_err.text);
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    lnurl_fetch_ctx_free(ctx);
    return;
  }

  /* Check for error response */
  json_t *status = json_object_get(root, "status");
  if (status && json_is_string(status) &&
      g_ascii_strcasecmp(json_string_value(status), "ERROR") == 0) {
    json_t *reason = json_object_get(root, "reason");
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_HTTP_FAILED,
                                "LNURL error: %s",
                                reason && json_is_string(reason) ?
                                json_string_value(reason) : "Unknown error");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    json_decref(root);
    lnurl_fetch_ctx_free(ctx);
    return;
  }

  /* Parse LNURL pay info */
  GnostrLnurlPayInfo *info = g_new0(GnostrLnurlPayInfo, 1);

  json_t *callback = json_object_get(root, "callback");
  if (callback && json_is_string(callback)) {
    info->callback = g_strdup(json_string_value(callback));
  }

  json_t *min_sendable = json_object_get(root, "minSendable");
  if (min_sendable && json_is_integer(min_sendable)) {
    info->min_sendable = json_integer_value(min_sendable);
  }

  json_t *max_sendable = json_object_get(root, "maxSendable");
  if (max_sendable && json_is_integer(max_sendable)) {
    info->max_sendable = json_integer_value(max_sendable);
  }

  json_t *allows_nostr = json_object_get(root, "allowsNostr");
  info->allows_nostr = allows_nostr && json_is_boolean(allows_nostr) && json_boolean_value(allows_nostr);

  json_t *nostr_pubkey = json_object_get(root, "nostrPubkey");
  if (nostr_pubkey && json_is_string(nostr_pubkey)) {
    info->nostr_pubkey = g_strdup(json_string_value(nostr_pubkey));
  }

  json_t *metadata = json_object_get(root, "metadata");
  if (metadata && json_is_string(metadata)) {
    info->metadata = g_strdup(json_string_value(metadata));
  }

  json_t *comment_allowed = json_object_get(root, "commentAllowed");
  if (comment_allowed && json_is_integer(comment_allowed)) {
    info->comment_allowed = g_strdup_printf("%lld", (long long)json_integer_value(comment_allowed));
  }

  json_decref(root);

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

  g_debug("zap: LNURL info fetched - callback=%s, allows_nostr=%d, nostr_pubkey=%.16s...",
          info->callback, info->allows_nostr, info->nostr_pubkey ? info->nostr_pubkey : "none");

  if (ctx->callback) {
    ctx->callback(info, NULL, ctx->user_data);
  } else {
    gnostr_lnurl_pay_info_free(info);
  }

  lnurl_fetch_ctx_free(ctx);
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

  g_debug("zap: fetching LNURL info from %s", url);

  LnurlFetchContext *ctx = g_new0(LnurlFetchContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->lud16 = g_strdup(lud16);

  SoupSession *session = soup_session_new();
  soup_session_set_timeout(session, 15);  /* 15 second timeout */

  SoupMessage *msg = soup_message_new("GET", url);
  g_free(url);

  if (!msg) {
    if (callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVALID_LNURL,
                                "Failed to create HTTP request");
      callback(NULL, err, user_data);
      g_error_free(err);
    }
    lnurl_fetch_ctx_free(ctx);
    g_object_unref(session);
    return;
  }

  soup_message_headers_append(soup_message_get_request_headers(msg), "Accept", "application/json");

  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, cancellable,
                                   on_lnurl_info_http_done, ctx);

  g_object_unref(msg);
  g_object_unref(session);
}

/* Context for invoice request */
typedef struct {
  GnostrZapInvoiceCallback callback;
  gpointer user_data;
} InvoiceRequestContext;

static void invoice_request_ctx_free(InvoiceRequestContext *ctx) {
  g_free(ctx);
}

static void on_invoice_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  InvoiceRequestContext *ctx = (InvoiceRequestContext *)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("zap: invoice request error: %s", error->message);
    }
    if (ctx->callback) {
      ctx->callback(NULL, error, ctx->user_data);
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

  /* Parse JSON response */
  gsize len = 0;
  const char *data = g_bytes_get_data(bytes, &len);

  json_error_t json_err;
  json_t *root = json_loadb(data, len, 0, &json_err);
  g_bytes_unref(bytes);

  if (!root) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_PARSE_FAILED,
                                "Failed to parse invoice response: %s", json_err.text);
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    invoice_request_ctx_free(ctx);
    return;
  }

  /* Check for error response */
  json_t *status = json_object_get(root, "status");
  if (status && json_is_string(status) &&
      g_ascii_strcasecmp(json_string_value(status), "ERROR") == 0) {
    json_t *reason = json_object_get(root, "reason");
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVOICE_FAILED,
                                "Invoice error: %s",
                                reason && json_is_string(reason) ?
                                json_string_value(reason) : "Unknown error");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    json_decref(root);
    invoice_request_ctx_free(ctx);
    return;
  }

  /* Extract the invoice */
  json_t *pr = json_object_get(root, "pr");
  if (!pr || !json_is_string(pr)) {
    if (ctx->callback) {
      GError *err = g_error_new(GNOSTR_ZAP_ERROR, GNOSTR_ZAP_ERROR_INVOICE_FAILED,
                                "No invoice in response");
      ctx->callback(NULL, err, ctx->user_data);
      g_error_free(err);
    }
    json_decref(root);
    invoice_request_ctx_free(ctx);
    return;
  }

  const gchar *bolt11 = json_string_value(pr);
  g_debug("zap: received invoice: %.40s...", bolt11);

  if (ctx->callback) {
    ctx->callback(bolt11, NULL, ctx->user_data);
  }

  json_decref(root);
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

  SoupSession *session = soup_session_new();
  soup_session_set_timeout(session, 30);  /* 30 second timeout for invoice */

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
    g_object_unref(session);
    return;
  }

  soup_message_headers_append(soup_message_get_request_headers(msg), "Accept", "application/json");

  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, cancellable,
                                   on_invoice_http_done, ctx);

  g_object_unref(msg);
  g_object_unref(session);
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

  json_t *event = json_object();

  /* Kind 9734 - zap request */
  json_object_set_new(event, "kind", json_integer(9734));

  /* Content - optional comment */
  json_object_set_new(event, "content", json_string(req->comment ? req->comment : ""));

  /* Pubkey - the sender's pubkey */
  json_object_set_new(event, "pubkey", json_string(sender_pubkey));

  /* Created at */
  json_object_set_new(event, "created_at", json_integer((json_int_t)time(NULL)));

  /* Tags */
  json_t *tags = json_array();

  /* relays tag - required */
  json_t *relays_tag = json_array();
  json_array_append_new(relays_tag, json_string("relays"));
  if (req->relays) {
    for (int i = 0; req->relays[i]; i++) {
      json_array_append_new(relays_tag, json_string(req->relays[i]));
    }
  } else {
    /* Get relays from config (GSettings defaults if none configured) */
    GPtrArray *cfg_relays = gnostr_get_write_relay_urls();
    for (guint i = 0; i < cfg_relays->len; i++) {
      json_array_append_new(relays_tag, json_string(g_ptr_array_index(cfg_relays, i)));
    }
    g_ptr_array_unref(cfg_relays);
  }
  json_array_append_new(tags, relays_tag);

  /* amount tag - recommended */
  if (req->amount_msat > 0) {
    json_t *amount_tag = json_array();
    json_array_append_new(amount_tag, json_string("amount"));
    gchar *amount_str = g_strdup_printf("%lld", (long long)req->amount_msat);
    json_array_append_new(amount_tag, json_string(amount_str));
    g_free(amount_str);
    json_array_append_new(tags, amount_tag);
  }

  /* lnurl tag - recommended (bech32 encoded) */
  if (req->lnurl) {
    json_t *lnurl_tag = json_array();
    json_array_append_new(lnurl_tag, json_string("lnurl"));
    json_array_append_new(lnurl_tag, json_string(req->lnurl));
    json_array_append_new(tags, lnurl_tag);
  }

  /* p tag - required (recipient pubkey) */
  json_t *p_tag = json_array();
  json_array_append_new(p_tag, json_string("p"));
  json_array_append_new(p_tag, json_string(req->recipient_pubkey));
  json_array_append_new(tags, p_tag);

  /* e tag - required if zapping an event */
  if (req->event_id) {
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(req->event_id));
    json_array_append_new(tags, e_tag);
  }

  /* k tag - optional, kind of target event */
  if (req->event_id && req->event_kind > 0) {
    json_t *k_tag = json_array();
    json_array_append_new(k_tag, json_string("k"));
    gchar *kind_str = g_strdup_printf("%d", req->event_kind);
    json_array_append_new(k_tag, json_string(kind_str));
    g_free(kind_str);
    json_array_append_new(tags, k_tag);
  }

  json_object_set_new(event, "tags", tags);

  gchar *result = json_dumps(event, JSON_COMPACT);
  json_decref(event);

  return result;
}

/* ============== Zap Receipt Parsing ============== */

GnostrZapReceipt *gnostr_zap_parse_receipt(const gchar *event_json) {
  if (!event_json || !*event_json) {
    return NULL;
  }

  json_error_t err;
  json_t *root = json_loads(event_json, 0, &err);
  if (!root) {
    g_debug("zap: failed to parse receipt JSON: %s", err.text);
    return NULL;
  }

  /* Verify kind 9735 */
  json_t *kind = json_object_get(root, "kind");
  if (!kind || !json_is_integer(kind) || json_integer_value(kind) != 9735) {
    json_decref(root);
    return NULL;
  }

  GnostrZapReceipt *receipt = g_new0(GnostrZapReceipt, 1);

  json_t *id = json_object_get(root, "id");
  if (id && json_is_string(id)) {
    receipt->id = g_strdup(json_string_value(id));
  }

  /* Extract event pubkey for validation against expected_nostr_pubkey */
  json_t *pubkey = json_object_get(root, "pubkey");
  if (pubkey && json_is_string(pubkey)) {
    receipt->event_pubkey = g_strdup(json_string_value(pubkey));
  }

  json_t *created_at = json_object_get(root, "created_at");
  if (created_at && json_is_integer(created_at)) {
    receipt->created_at = json_integer_value(created_at);
  }

  /* Parse tags */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      json_t *tag_name = json_array_get(tag, 0);
      json_t *tag_value = json_array_get(tag, 1);

      if (!json_is_string(tag_name) || !json_is_string(tag_value)) continue;

      const char *name = json_string_value(tag_name);
      const char *value = json_string_value(tag_value);

      if (g_strcmp0(name, "bolt11") == 0) {
        receipt->bolt11 = g_strdup(value);
      } else if (g_strcmp0(name, "preimage") == 0) {
        receipt->preimage = g_strdup(value);
      } else if (g_strcmp0(name, "description") == 0) {
        receipt->description = g_strdup(value);
      } else if (g_strcmp0(name, "p") == 0) {
        receipt->recipient_pubkey = g_strdup(value);
      } else if (g_strcmp0(name, "P") == 0) {
        receipt->sender_pubkey = g_strdup(value);
      } else if (g_strcmp0(name, "e") == 0) {
        receipt->event_id = g_strdup(value);
      }
    }
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
  if (receipt->description) {
    json_error_t desc_err;
    json_t *zap_req = json_loads(receipt->description, 0, &desc_err);
    if (zap_req) {
      json_t *zap_tags = json_object_get(zap_req, "tags");
      if (zap_tags && json_is_array(zap_tags)) {
        size_t j;
        json_t *ztag;
        json_array_foreach(zap_tags, j, ztag) {
          if (json_is_array(ztag) && json_array_size(ztag) >= 2) {
            json_t *ztag_name = json_array_get(ztag, 0);
            json_t *ztag_value = json_array_get(ztag, 1);
            if (json_is_string(ztag_name) && json_is_string(ztag_value)) {
              if (g_strcmp0(json_string_value(ztag_name), "amount") == 0) {
                receipt->zap_request_amount_msat = g_ascii_strtoll(json_string_value(ztag_value), NULL, 10);
                g_debug("zap: parsed zap request amount: %lld msat", (long long)receipt->zap_request_amount_msat);
                break;
              }
            }
          }
        }
      }
      json_decref(zap_req);
    }
  }

  json_decref(root);
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
