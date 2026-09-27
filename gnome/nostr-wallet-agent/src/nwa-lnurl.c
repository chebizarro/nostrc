/* nwa-lnurl.c - see nwa-lnurl.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-lnurl.h"
#include "nwa-error.h"

#include <string.h>

void
nwa_lnurl_pay_clear(NwaLnurlPay *p)
{
  if (!p) return;
  g_free(p->callback);
  g_free(p->metadata);
  g_free(p->description);
  g_free(p->long_description);
  g_free(p->identifier);
  g_free(p->domain);
  memset(p, 0, sizeof *p);
}

static gboolean
host_is_loopback(const gchar *host)
{
  return g_strcmp0(host, "127.0.0.1") == 0 || g_strcmp0(host, "localhost") == 0 ||
         g_strcmp0(host, "::1") == 0;
}

gboolean
nwa_lnurl_url_allowed(const gchar *url)
{
  g_autoptr(GUri) u = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  if (!u || !g_uri_get_host(u) || !*g_uri_get_host(u) || g_uri_get_userinfo(u)) return FALSE;
  const gchar *scheme = g_uri_get_scheme(u);
  const gchar *host = g_uri_get_host(u);
  if (g_ascii_strcasecmp(scheme, "https") == 0) return TRUE;
  if (g_ascii_strcasecmp(scheme, "http") != 0) return FALSE;
  if (g_str_has_suffix(host, ".onion")) return TRUE;
#ifdef NWA_ORIGIN_BRIDGE_ENV
  /* test builds: in-process servers on loopback */
  if (host_is_loopback(host)) return TRUE;
#else
  (void)host_is_loopback;
#endif
  return FALSE;
}

gchar *
nwa_lnurl_target_url(const gchar *s, GError **error)
{
  if (!s || !*s) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "empty LNURL");
    return NULL;
  }
  gchar *url = NULL;
  const gchar *at = strchr(s, '@');
  if (g_ascii_strncasecmp(s, "lnurl1", 6) == 0) {
    g_autofree gchar *hrp = NULL;
    g_autoptr(GBytes) data = NULL;
    if (!nwa_bech32_decode(s, &hrp, &data, error)) return NULL;
    gsize n = 0;
    const gchar *d = g_bytes_get_data(data, &n);
    if (g_strcmp0(hrp, "lnurl") != 0 || n == 0 || !g_utf8_validate(d, (gssize)n, NULL) || memchr(d, 0, n)) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "not an LNURL");
      return NULL;
    }
    url = g_strndup(d, n);
  } else if (at && at != s && at[1] && !strchr(at + 1, '@')) {
    /* LUD-16: username a-z0-9-_.+ ; host is a domain (optionally :port) */
    g_autofree gchar *user = g_ascii_strdown(s, (gssize)(at - s));
    g_autofree gchar *host = g_ascii_strdown(at + 1, -1);
    for (const gchar *p = user; *p; p++)
      if (!(g_ascii_isalnum(*p) || strchr("-_.+", *p))) {
        g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid Lightning address");
        return NULL;
      }
    for (const gchar *p = host; *p; p++)
      if (!(g_ascii_isalnum(*p) || strchr("-.:", *p))) {
        g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid Lightning address");
        return NULL;
      }
    const gchar *scheme = "https";
    g_autofree gchar *bare = g_strdup(host);
    gchar *colon = strchr(bare, ':');
    if (colon) *colon = '\0';
    if (g_str_has_suffix(bare, ".onion")) scheme = "http";
#ifdef NWA_ORIGIN_BRIDGE_ENV
    if (host_is_loopback(bare)) scheme = "http";
#endif
    url = g_strdup_printf("%s://%s/.well-known/lnurlp/%s", scheme, host, user);
  } else {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "neither an LNURL nor a Lightning address");
    return NULL;
  }
  if (!nwa_lnurl_url_allowed(url)) {
    g_set_error(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "LNURL must use https: %s", url);
    g_free(url);
    return NULL;
  }
  return url;
}

static guint64
member_u64(JsonObject *o, const gchar *k, gboolean *ok)
{
  JsonNode *n = json_object_get_member(o, k);
  if (!n || !JSON_NODE_HOLDS_VALUE(n)) { *ok = FALSE; return 0; }
  GType t = json_node_get_value_type(n);
  gint64 v = t == G_TYPE_INT64 ? json_node_get_int(n)
           : t == G_TYPE_DOUBLE ? (gint64)json_node_get_double(n) : -1;
  if (v < 0) { *ok = FALSE; return 0; }
  return (guint64)v;
}

gboolean
nwa_lnurl_parse_pay(JsonNode *root, const gchar *url, const gchar *address, NwaLnurlPay *out,
                    GError **error)
{
  memset(out, 0, sizeof *out);
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  const gchar *tag = o ? json_object_get_string_member_with_default(o, "tag", NULL) : NULL;
  if (g_strcmp0(tag, "withdrawRequest") == 0) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_UNSUPPORTED,
                        "this is an LNURL-withdraw link; receiving through LNURL is not supported");
    return FALSE;
  }
  if (g_strcmp0(tag, "payRequest") != 0) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_UNSUPPORTED, "not an LNURL-pay request");
    return FALSE;
  }
  const gchar *cb = json_object_get_string_member_with_default(o, "callback", NULL);
  const gchar *md = json_object_get_string_member_with_default(o, "metadata", NULL);
  gboolean ok = TRUE;
  guint64 min = member_u64(o, "minSendable", &ok);
  guint64 max = member_u64(o, "maxSendable", &ok);
  if (!cb || !md || !ok || min < 1 || min > max) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "LNURL-pay request lacks callback, metadata or a valid amount range");
    return FALSE;
  }
  if (!nwa_lnurl_url_allowed(cb)) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "LNURL-pay callback must use https");
    return FALSE;
  }
  /* metadata: a JSON array of [mime, value] pairs; text/plain required */
  g_autoptr(JsonParser) mp = json_parser_new();
  if (!json_parser_load_from_data(mp, md, -1, NULL) || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(mp))) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "LNURL-pay metadata is not a JSON array");
    return FALSE;
  }
  JsonArray *a = json_node_get_array(json_parser_get_root(mp));
  for (guint i = 0; i < json_array_get_length(a); i++) {
    JsonNode *e = json_array_get_element(a, i);
    if (!JSON_NODE_HOLDS_ARRAY(e) || json_array_get_length(json_node_get_array(e)) < 2) continue;
    JsonArray *pair = json_node_get_array(e);
    JsonNode *kn = json_array_get_element(pair, 0), *vn = json_array_get_element(pair, 1);
    if (!JSON_NODE_HOLDS_VALUE(kn) || json_node_get_value_type(kn) != G_TYPE_STRING ||
        !JSON_NODE_HOLDS_VALUE(vn) || json_node_get_value_type(vn) != G_TYPE_STRING)
      continue;
    const gchar *k = json_node_get_string(kn), *v = json_node_get_string(vn);
    if (g_str_equal(k, "text/plain") && !out->description) out->description = g_strdup(v);
    else if (g_str_equal(k, "text/long-desc") && !out->long_description) out->long_description = g_strdup(v);
    else if ((g_str_equal(k, "text/identifier") || g_str_equal(k, "text/email")) && !out->identifier)
      out->identifier = g_strdup(v);
  }
  if (!out->description) {
    nwa_lnurl_pay_clear(out);
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "LNURL-pay metadata has no text/plain");
    return FALSE;
  }
  if (address && out->identifier && g_ascii_strcasecmp(address, out->identifier) != 0) {
    nwa_lnurl_pay_clear(out);
    g_set_error(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                "the server answered for another address than %s", address);
    return FALSE;
  }
  out->callback = g_strdup(cb);
  out->metadata = g_strdup(md);
  out->min_msat = min;
  out->max_msat = max;
  gboolean cok = TRUE;
  guint64 ca = json_object_has_member(o, "commentAllowed") ? member_u64(o, "commentAllowed", &cok) : 0;
  out->comment_allowed = cok ? (guint)MIN(ca, 2000) : 0;
  g_autoptr(GUri) u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  out->domain = g_strdup(u ? g_uri_get_host(u) : "?");
  return TRUE;
}

gchar *
nwa_lnurl_callback_url(const NwaLnurlPay *p, guint64 amount_msat, const gchar *comment)
{
  GString *s = g_string_new(p->callback);
  g_string_append_c(s, strchr(p->callback, '?') ? '&' : '?');
  g_string_append_printf(s, "amount=%" G_GUINT64_FORMAT, amount_msat);
  if (p->comment_allowed && comment && *comment) {
    g_autofree gchar *cut = g_utf8_substring(comment, 0, MIN((glong)p->comment_allowed, g_utf8_strlen(comment, -1)));
    g_autofree gchar *esc = g_uri_escape_string(cut, NULL, FALSE);
    g_string_append_printf(s, "&comment=%s", esc);
  }
  return g_string_free(s, FALSE);
}

gboolean
nwa_lnurl_check_invoice(const NwaLnurlPay *p, guint64 amount_msat, const NwaBolt11 *inv, GError **error)
{
  if (inv->amount_msat != amount_msat) {
    g_set_error(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                "the recipient's invoice is for %" G_GUINT64_FORMAT " msat, not the %" G_GUINT64_FORMAT
                " msat you approved", inv->amount_msat, amount_msat);
    return FALSE;
  }
  g_autofree gchar *want = g_compute_checksum_for_string(G_CHECKSUM_SHA256, p->metadata, -1);
  if (!inv->description_hash || g_ascii_strcasecmp(inv->description_hash, want) != 0) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "the recipient's invoice does not commit to the payment details you approved");
    return FALSE;
  }
  return TRUE;
}

/* ---- HTTP ---- */

typedef struct {
  SoupMessage *msg;
  guint8      *buf;
  gsize        got;
} Fetch;

static void
fetch_free(Fetch *f)
{
  g_clear_object(&f->msg);
  g_free(f->buf);
  g_free(f);
}

static void
on_body(GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  Fetch *f = g_task_get_task_data(task);
  GError *err = NULL;
  if (!g_input_stream_read_all_finish(G_INPUT_STREAM(src), res, &f->got, &err)) {
    g_task_return_error(task, err);
    g_object_unref(task);
    return;
  }
  g_input_stream_close(G_INPUT_STREAM(src), NULL, NULL);
  if (f->got > NWA_LNURL_MAX_BODY) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "LNURL answer is too large");
    g_object_unref(task);
    return;
  }
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, (const gchar *)f->buf, (gssize)f->got, NULL) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "LNURL answer is not a JSON object");
    g_object_unref(task);
    return;
  }
  JsonObject *o = json_node_get_object(json_parser_get_root(p));
  if (g_strcmp0(json_object_get_string_member_with_default(o, "status", NULL), "ERROR") == 0) {
    const gchar *why = json_object_get_string_member_with_default(o, "reason", "no reason given");
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_WALLET, "[LNURL] %s", why);
    g_object_unref(task);
    return;
  }
  g_task_return_pointer(task, json_node_copy(json_parser_get_root(p)), (GDestroyNotify)json_node_unref);
  g_object_unref(task);
}

static void
on_sent(GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  Fetch *f = g_task_get_task_data(task);
  GError *err = NULL;
  GInputStream *in = soup_session_send_finish(SOUP_SESSION(src), res, &err);
  if (!in) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_RELAY, "LNURL server unreachable: %s", err->message);
    g_error_free(err);
    g_object_unref(task);
    return;
  }
  guint status = soup_message_get_status(f->msg);
  if (status != SOUP_STATUS_OK) {
    g_object_unref(in);
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_WALLET, "[LNURL] the server answered HTTP %u%s", status,
                            SOUP_STATUS_IS_REDIRECTION(status) ? " (redirects are not followed)" : "");
    g_object_unref(task);
    return;
  }
  f->buf = g_malloc(NWA_LNURL_MAX_BODY + 1);
  g_input_stream_read_all_async(in, f->buf, NWA_LNURL_MAX_BODY + 1, G_PRIORITY_DEFAULT,
                                g_task_get_cancellable(task), on_body, task);
  g_object_unref(in);
}

void
nwa_lnurl_fetch_json_async(SoupSession *session, const gchar *url, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (!nwa_lnurl_url_allowed(url)) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "LNURL must use https");
    g_object_unref(task);
    return;
  }
  Fetch *f = g_new0(Fetch, 1);
  f->msg = soup_message_new(SOUP_METHOD_GET, url);
  g_task_set_task_data(task, f, (GDestroyNotify)fetch_free);
  if (!f->msg) {
    g_task_return_new_error(task, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid LNURL");
    g_object_unref(task);
    return;
  }
  soup_message_add_flags(f->msg, SOUP_MESSAGE_NO_REDIRECT);
  soup_message_headers_replace(soup_message_get_request_headers(f->msg), "Accept", "application/json");
  soup_session_send_async(session, f->msg, G_PRIORITY_DEFAULT, cancellable, on_sent, task);
}

JsonNode *
nwa_lnurl_fetch_json_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}
