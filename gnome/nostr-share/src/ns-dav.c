/* ns-dav.c - Hand calendar/contact files to nostr-dav
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-dav.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <string.h>

#define NS_DAV_DEFAULT_URL "http://127.0.0.1:7680/"

gchar *
ns_dav_extract_uid(const gchar *body)
{
  if (body == NULL)
    return NULL;
  g_auto(GStrv) lines = g_strsplit(body, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    gchar *l = g_strchomp(lines[i]);
    if (g_ascii_strncasecmp(l, "UID", 3) != 0 || (l[3] != ':' && l[3] != ';'))
      continue;
    const gchar *colon = strchr(l, ':');
    if (colon == NULL)
      continue;
    GString *s = g_string_new(NULL);
    for (const gchar *p = colon + 1; *p && s->len < 200; p++) {
      if (g_ascii_isalnum(*p) || strchr("-_.@", *p))
        g_string_append_c(s, *p);
      else
        g_string_append_c(s, '_');
    }
    if (s->len == 0 || g_str_equal(s->str, ".") || g_str_equal(s->str, "..")) {
      g_string_free(s, TRUE);
      return NULL;
    }
    return g_string_free(s, FALSE);
  }
  return NULL;
}

gboolean
ns_dav_check_single(NsInputClass cls, const gchar *body, GError **error)
{
  g_autoptr(GPtrArray) uids = g_ptr_array_new_with_free_func(g_free);
  guint cards = 0;
  g_auto(GStrv) lines = g_strsplit(body ? body : "", "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    gchar *l = g_strchomp(lines[i]);
    if (g_ascii_strcasecmp(l, "BEGIN:VCARD") == 0)
      cards++;
    if (g_ascii_strncasecmp(l, "UID", 3) == 0 && (l[3] == ':' || l[3] == ';')) {
      const gchar *v = strchr(l, ':');
      if (v == NULL)
        continue;
      gboolean seen = FALSE;
      for (guint j = 0; j < uids->len; j++)
        if (g_str_equal(g_ptr_array_index(uids, j), v + 1))
          seen = TRUE;
      if (!seen)
        g_ptr_array_add(uids, g_strdup(v + 1));
    }
  }
  /* Recurrence overrides share one UID, so distinct UIDs are the test. */
  if (cls == NS_CLASS_CALENDAR && uids->len > 1) {
    g_set_error(error, NS_ERROR, NS_ERROR_DAV,
                "this calendar file holds %u separate events; nostr-dav stores "
                "one per resource — export and share them one at a time",
                uids->len);
    return FALSE;
  }
  if (cls == NS_CLASS_CONTACT && cards > 1) {
    g_set_error(error, NS_ERROR, NS_ERROR_DAV,
                "this vCard file holds %u contacts; share them one at a time",
                cards);
    return FALSE;
  }
  return TRUE;
}

gchar *
ns_dav_href(NsInputClass cls, const gchar *uid)
{
  return cls == NS_CLASS_CALENDAR
    ? g_strdup_printf("calendars/nostr/%s.ics", uid)
    : g_strdup_printf("contacts/nostr/%s.vcf", uid);
}

/* Parse `nostr-dav --show-credentials` (documented in its README):
 *   Server:   http://127.0.0.1:7680/
 *   Username: nostr
 *   Password: <token>                                                  */
static gboolean
dav_credentials(gchar **server, gchar **user, gchar **password, GError **error)
{
  GError *local = NULL;
  const gchar *argv[] = { "nostr-dav", "--show-credentials", NULL };
  g_autoptr(GSubprocess) p = g_subprocess_newv(argv,
                                               G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                               G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                               &local);
  if (p == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_DAV,
                "calendar/contact sharing needs nostr-dav (%s)", local->message);
    g_clear_error(&local);
    return FALSE;
  }
  g_autofree gchar *out = NULL, *err = NULL;
  if (!g_subprocess_communicate_utf8(p, NULL, NULL, &out, &err, &local) ||
      !g_subprocess_get_successful(p)) {
    g_set_error(error, NS_ERROR, NS_ERROR_DAV, "nostr-dav --show-credentials failed: %s",
                local ? local->message : (err ? g_strstrip(err) : "unknown"));
    g_clear_error(&local);
    return FALSE;
  }
  g_auto(GStrv) lines = g_strsplit(out, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    gchar *l = lines[i];
    if (g_str_has_prefix(l, "Server:"))
      *server = g_strstrip(g_strdup(l + 7));
    else if (g_str_has_prefix(l, "Username:"))
      *user = g_strstrip(g_strdup(l + 9));
    else if (g_str_has_prefix(l, "Password:"))
      *password = g_strstrip(g_strdup(l + 9));
  }
  if (out != NULL)
    memset(out, 0, strlen(out));
  if (*user == NULL || *password == NULL) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_DAV,
                        "could not parse nostr-dav --show-credentials output");
    return FALSE;
  }
  return TRUE;
}

gboolean
ns_dav_stage(const gchar *dav_url_override, NsInputClass cls, GBytes *body,
             gboolean dry_run, gchar **out_url, GError **error)
{
  gsize len = 0;
  const gchar *data = g_bytes_get_data(body, &len);
  g_autofree gchar *text = g_strndup(data, len);
  if (!ns_dav_check_single(cls, text, error))
    return FALSE;
  g_autofree gchar *uid = ns_dav_extract_uid(text);
  if (uid == NULL)
    uid = g_uuid_string_random();
  g_autofree gchar *href = ns_dav_href(cls, uid);

  g_autofree gchar *server = NULL, *user = NULL, *password = NULL;
  if (dry_run) {
    server = g_strdup(dav_url_override ? dav_url_override : NS_DAV_DEFAULT_URL);
  } else {
    if (!dav_credentials(&server, &user, &password, error))
      return FALSE;
    if (dav_url_override != NULL) {
      g_free(server);
      server = g_strdup(dav_url_override);
    }
  }
  if (server == NULL)
    server = g_strdup(NS_DAV_DEFAULT_URL);
  const gchar *sep = g_str_has_suffix(server, "/") ? "" : "/";
  gchar *url = g_strdup_printf("%s%s%s", server, sep, href);

  if (dry_run) {
    *out_url = url;
    return TRUE;
  }

  g_autoptr(SoupSession) session = soup_session_new();
  g_autoptr(SoupMessage) msg = soup_message_new("PUT", url);
  if (msg == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_DAV, "invalid nostr-dav URL %s", url);
    g_free(url);
    return FALSE;
  }
  g_autofree gchar *userpass = g_strdup_printf("%s:%s", user, password);
  g_autofree gchar *b64 = g_base64_encode((const guchar *)userpass, strlen(userpass));
  g_autofree gchar *auth = g_strdup_printf("Basic %s", b64);
  memset(password, 0, strlen(password));
  memset(userpass, 0, strlen(userpass));
  SoupMessageHeaders *h = soup_message_get_request_headers(msg);
  soup_message_headers_replace(h, "Authorization", auth);
  soup_message_set_request_body_from_bytes(
    msg, cls == NS_CLASS_CALENDAR ? "text/calendar; charset=utf-8"
                                  : "text/vcard; charset=utf-8", body);

  GError *local = NULL;
  g_autoptr(GBytes) resp = soup_session_send_and_read(session, msg, NULL, &local);
  memset(auth, 0, strlen(auth));
  if (resp == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_DAV,
                "nostr-dav is not reachable at %s (%s); start it with "
                "`systemctl --user start nostr-dav`", server, local->message);
    g_clear_error(&local);
    g_free(url);
    return FALSE;
  }
  guint status = soup_message_get_status(msg);
  if (status < 200 || status >= 300) {
    g_set_error(error, NS_ERROR, NS_ERROR_DAV, "nostr-dav refused %s: HTTP %u %s",
                href, status, soup_message_get_reason_phrase(msg));
    g_free(url);
    return FALSE;
  }
  *out_url = url;
  return TRUE;
}
