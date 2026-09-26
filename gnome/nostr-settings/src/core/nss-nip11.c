/* nss-nip11.c — see nss-nip11.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-nip11.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

G_DEFINE_QUARK(nss-nip11-error-quark, nss_nip11_error)

void
nss_nip11_info_free(NssNip11Info *info)
{
  if (info == NULL)
    return;
  g_free(info->name);
  g_free(info->description);
  g_free(info->software);
  g_free(info->version);
  g_free(info->contact);
  g_free(info->pubkey);
  if (info->supported_nips)
    g_array_unref(info->supported_nips);
  g_free(info);
}

/* Valid UTF-8, no control characters, at most NSS_NIP11_MAX_STRING chars. */
static gchar *
clean_string(JsonObject *o, const gchar *member)
{
  if (!json_object_has_member(o, member))
    return NULL;
  JsonNode *n = json_object_get_member(o, member);
  if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
    return NULL;
  const gchar *s = json_node_get_string(n);
  if (s == NULL || !g_utf8_validate(s, -1, NULL))
    return NULL;
  GString *out = g_string_new(NULL);
  glong count = 0;
  for (const gchar *p = s; *p && count < NSS_NIP11_MAX_STRING; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (c == '\n' || c == '\t')
      c = ' ';
    if (g_unichar_iscntrl(c))
      continue;
    g_string_append_unichar(out, c);
    count++;
  }
  if (*s && count >= NSS_NIP11_MAX_STRING)
    g_string_append(out, "…");
  g_strstrip(out->str);
  if (out->str[0] == '\0') {
    g_string_free(out, TRUE);
    return NULL;
  }
  return g_string_free(out, FALSE);
}

static gint
cmp_int(gconstpointer a, gconstpointer b)
{
  gint x = *(const gint *)a, y = *(const gint *)b;
  return (x > y) - (x < y);
}

NssNip11Info *
nss_nip11_parse(const gchar *json, gssize len, GError **error)
{
  g_autoptr(JsonParser) p = json_parser_new();
  if (json == NULL || !json_parser_load_from_data(p, json, len, NULL) ||
      json_parser_get_root(p) == NULL ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
    g_set_error_literal(error, NSS_NIP11_ERROR, NSS_NIP11_ERROR_PARSE,
                        "the relay information document is not a JSON object");
    return NULL;
  }
  JsonObject *o = json_node_get_object(json_parser_get_root(p));
  NssNip11Info *info = g_new0(NssNip11Info, 1);
  info->name = clean_string(o, "name");
  info->description = clean_string(o, "description");
  info->software = clean_string(o, "software");
  info->version = clean_string(o, "version");
  info->contact = clean_string(o, "contact");
  info->pubkey = clean_string(o, "pubkey");
  info->supported_nips = g_array_new(FALSE, FALSE, sizeof(gint));
  if (json_object_has_member(o, "supported_nips") &&
      JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "supported_nips"))) {
    JsonArray *a = json_object_get_array_member(o, "supported_nips");
    for (guint i = 0; i < json_array_get_length(a); i++) {
      JsonNode *n = json_array_get_element(a, i);
      if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64)
        continue;
      gint64 v = json_node_get_int(n);
      if (v < 0 || v > 9999)
        continue;
      gint iv = (gint)v;
      gboolean dup = FALSE;
      for (guint j = 0; j < info->supported_nips->len; j++)
        dup |= g_array_index(info->supported_nips, gint, j) == iv;
      if (!dup && info->supported_nips->len < NSS_NIP11_MAX_NIPS)
        g_array_append_val(info->supported_nips, iv);
    }
    g_array_sort(info->supported_nips, cmp_int);
  }
  return info;
}

gchar *
nss_nip11_format_nips(const NssNip11Info *info)
{
  GString *s = g_string_new(NULL);
  for (guint i = 0; info && info->supported_nips && i < info->supported_nips->len; i++)
    g_string_append_printf(s, "%s%d", i ? ", " : "", g_array_index(info->supported_nips, gint, i));
  return g_string_free(s, FALSE);
}

gchar *
nss_nip11_http_url(const gchar *relay_url, GError **error)
{
  const gchar *rest = NULL, *scheme = NULL;
  if (relay_url && g_ascii_strncasecmp(relay_url, "wss://", 6) == 0) {
    scheme = "https://"; rest = relay_url + 6;
  } else if (relay_url && g_ascii_strncasecmp(relay_url, "ws://", 5) == 0) {
    scheme = "http://"; rest = relay_url + 5;
  }
  if (rest == NULL || *rest == '\0' || *rest == '/') {
    g_set_error(error, NSS_NIP11_ERROR, NSS_NIP11_ERROR_BAD_URL,
                "“%s” is not a ws:// or wss:// URL", relay_url ? relay_url : "");
    return NULL;
  }
  return g_strconcat(scheme, rest, NULL);
}

/* ── fetch ─────────────────────────────────────────────────────────────── */

typedef struct {
  SoupSession  *session;
  SoupMessage  *msg;
  GInputStream *stream;
  GByteArray   *body;
  GCancellable *cancel;      /* ours: timeout + caller cancel */
  gulong        caller_handler;
  GCancellable *caller;
  guint         timeout_id;
  gboolean      timed_out;
  guint8        buf[8192];
} Fetch;

static void
fetch_free(gpointer data)
{
  Fetch *f = data;
  if (f->timeout_id)
    g_source_remove(f->timeout_id);
  if (f->caller && f->caller_handler)
    g_cancellable_disconnect(f->caller, f->caller_handler);
  g_clear_object(&f->caller);
  g_clear_object(&f->stream);
  g_clear_object(&f->msg);
  g_clear_object(&f->session);
  g_clear_object(&f->cancel);
  if (f->body)
    g_byte_array_unref(f->body);
  g_free(f);
}

static gboolean
on_timeout(gpointer data)
{
  Fetch *f = data;
  f->timeout_id = 0;
  f->timed_out = TRUE;
  g_cancellable_cancel(f->cancel);
  return G_SOURCE_REMOVE;
}

static void
on_caller_cancel(GCancellable *c, gpointer data)
{
  (void)c;
  g_cancellable_cancel(((Fetch *)data)->cancel);
}

static void
fail(GTask *task, GError *err)
{
  Fetch *f = g_task_get_task_data(task);
  if (f->timed_out) {
    g_clear_error(&err);
    err = g_error_new_literal(NSS_NIP11_ERROR, NSS_NIP11_ERROR_TIMEOUT,
                              "the relay did not answer in time");
  }
  g_task_return_error(task, err);
  g_object_unref(task);
}

static void on_read(GObject *src, GAsyncResult *res, gpointer data);

static void
read_more(GTask *task)
{
  Fetch *f = g_task_get_task_data(task);
  g_input_stream_read_async(f->stream, f->buf, sizeof f->buf, G_PRIORITY_DEFAULT,
                            f->cancel, on_read, task);
}

static void
on_read(GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  Fetch *f = g_task_get_task_data(task);
  GError *err = NULL;
  gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &err);
  if (n < 0) {
    fail(task, err);
    return;
  }
  if (n > 0) {
    if (f->body->len + (gsize)n > NSS_NIP11_MAX_BYTES) {
      g_cancellable_cancel(f->cancel);
      g_task_return_new_error(task, NSS_NIP11_ERROR, NSS_NIP11_ERROR_TOO_LARGE,
                              "the relay information document exceeds %d KiB",
                              NSS_NIP11_MAX_BYTES / 1024);
      g_object_unref(task);
      return;
    }
    g_byte_array_append(f->body, f->buf, (guint)n);
    read_more(task);
    return;
  }
  NssNip11Info *info = nss_nip11_parse((const gchar *)f->body->data, f->body->len, &err);
  if (info == NULL)
    g_task_return_error(task, err);
  else
    g_task_return_pointer(task, info, (GDestroyNotify)nss_nip11_info_free);
  g_object_unref(task);
}

static void
on_sent(GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  Fetch *f = g_task_get_task_data(task);
  GError *err = NULL;
  f->stream = soup_session_send_finish(SOUP_SESSION(src), res, &err);
  if (f->stream == NULL) {
    fail(task, err);
    return;
  }
  guint status = soup_message_get_status(f->msg);
  if (status < 200 || status >= 300) {
    g_task_return_new_error(task, NSS_NIP11_ERROR, NSS_NIP11_ERROR_HTTP,
                            "HTTP %u %s", status,
                            soup_message_get_reason_phrase(f->msg)
                              ? soup_message_get_reason_phrase(f->msg) : "");
    g_object_unref(task);
    return;
  }
  goffset clen = soup_message_headers_get_content_length(
    soup_message_get_response_headers(f->msg));
  if (clen > NSS_NIP11_MAX_BYTES) {
    g_task_return_new_error(task, NSS_NIP11_ERROR, NSS_NIP11_ERROR_TOO_LARGE,
                            "the relay information document exceeds %d KiB",
                            NSS_NIP11_MAX_BYTES / 1024);
    g_object_unref(task);
    return;
  }
  read_more(task);
}

void
nss_nip11_fetch_async(gpointer soup_session, const gchar *relay_url, guint timeout_sec,
                      GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer user_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, nss_nip11_fetch_async);
  GError *err = NULL;
  g_autofree gchar *http = nss_nip11_http_url(relay_url, &err);
  if (http == NULL) {
    g_task_return_error(task, err);
    g_object_unref(task);
    return;
  }
  Fetch *f = g_new0(Fetch, 1);
  g_task_set_task_data(task, f, fetch_free);
  f->session = soup_session ? g_object_ref(SOUP_SESSION(soup_session)) : soup_session_new();
  f->body = g_byte_array_new();
  f->cancel = g_cancellable_new();
  if (cancellable) {
    f->caller = g_object_ref(cancellable);
    f->caller_handler = g_cancellable_connect(cancellable, G_CALLBACK(on_caller_cancel), f, NULL);
  }
  f->msg = soup_message_new(SOUP_METHOD_GET, http);
  if (f->msg == NULL) {
    g_task_return_new_error(task, NSS_NIP11_ERROR, NSS_NIP11_ERROR_BAD_URL,
                            "cannot request %s", http);
    g_object_unref(task);
    return;
  }
  soup_message_headers_replace(soup_message_get_request_headers(f->msg), "Accept",
                               "application/nostr+json");
  f->timeout_id = g_timeout_add_seconds(timeout_sec ? timeout_sec : 5, on_timeout, f);
  soup_session_send_async(f->session, f->msg, G_PRIORITY_DEFAULT, f->cancel, on_sent, task);
}

NssNip11Info *
nss_nip11_fetch_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}
