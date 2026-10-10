/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Canonical NIP-34 issue model (nostrc-8xfib.5). Merges Groundhog's bounded
 * draft builder (byte limits, label/commit/URL validation, no trimming) with
 * gnostr's assembly (crash-log and attachment URL sections, maintainers, the
 * newest-wins kind-30617 lookup and the kind-1630 open status).
 */
#include <nostr-gtk-1.0/gn-nip34-issue.h>
#include "gn-portable-i18n-private.h"

#include <gio/gio.h>
#include <nip34.h>
#include <nostr-event.h>
#include <string.h>

/* ---- Small helpers ---- */

static GStrv
strv_or_empty(const gchar *const *values)
{
  return values ? g_strdupv((gchar **)values) : g_new0(gchar *, 1);
}

static gboolean
invalid(GError **error, const gchar *message)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
  return FALSE;
}

static gchar *
stripped(const gchar *text)
{
  return g_strstrip(g_strdup(text ? text : ""));
}

/* ---- Fields snapshot (struct shared with GnNip34IssueFields) ---- */

void
gn_nip34_issue_fields_snapshot_free(GnNip34IssueFieldsSnapshot *s)
{
  if (!s)
    return;
  g_free(s->steps);
  g_free(s->expected);
  g_free(s->actual);
  g_free(s->labels);
  g_free(s->related_commits);
  g_free(s->attachment_urls);
  g_free(s);
}

/* ---- Target ---- */

G_DEFINE_BOXED_TYPE(GnIssueTarget, gn_issue_target, gn_issue_target_copy, gn_issue_target_free)

GnIssueTarget *
gn_issue_target_new(const gchar *owner_hex, const gchar *repo_id,
                    const gchar *const *maintainers, const gchar *const *relays)
{
  g_return_val_if_fail(owner_hex && *owner_hex, NULL);
  g_return_val_if_fail(repo_id && *repo_id, NULL);
  GnIssueTarget *target = g_new0(GnIssueTarget, 1);
  target->owner_hex = g_ascii_strdown(owner_hex, -1);
  target->repo_id = g_strdup(repo_id);
  target->maintainers = strv_or_empty(maintainers);
  target->relays = strv_or_empty(relays);
  return target;
}

GnIssueTarget *
gn_issue_target_copy(const GnIssueTarget *target)
{
  if (!target)
    return NULL;
  return gn_issue_target_new(target->owner_hex, target->repo_id,
                             (const gchar *const *)target->maintainers,
                             (const gchar *const *)target->relays);
}

void
gn_issue_target_free(GnIssueTarget *target)
{
  if (!target)
    return;
  g_free(target->owner_hex);
  g_free(target->repo_id);
  g_strfreev(target->maintainers);
  g_strfreev(target->relays);
  g_free(target);
}

gboolean
gn_issue_target_equal(const GnIssueTarget *a, const GnIssueTarget *b)
{
  if (!a || !b)
    return a == b;
  return g_str_equal(a->owner_hex, b->owner_hex) && g_str_equal(a->repo_id, b->repo_id) &&
         g_strv_equal((const gchar *const *)a->maintainers, (const gchar *const *)b->maintainers) &&
         g_strv_equal((const gchar *const *)a->relays, (const gchar *const *)b->relays);
}

gchar *
gn_issue_target_get_address(const GnIssueTarget *target)
{
  g_return_val_if_fail(target != NULL, NULL);
  return g_strdup_printf("30617:%s:%s", target->owner_hex, target->repo_id);
}

static GnIssueTarget *
target_from_announcement(const GnIssueTarget *hint, const gchar *json, gint64 *created_at)
{
  if (!json || !*json)
    return NULL;
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_compact(event, json, NULL) != 1) {
    if (event)
      nostr_event_free(event);
    return NULL;
  }
  const char *author = nostr_event_get_pubkey(event);
  nip34_repository_t *repo = NULL;
  GnIssueTarget *target = NULL;
  if (nostr_event_get_kind(event) == NIP34_KIND_REPOSITORY && author &&
      g_ascii_strcasecmp(author, hint->owner_hex) == 0 &&
      nip34_parse_repository(event, &repo) == NIP34_OK && repo &&
      g_strcmp0(repo->id, hint->repo_id) == 0) {
    g_autoptr(GPtrArray) maintainers = g_ptr_array_new();
    g_autoptr(GPtrArray) relays = g_ptr_array_new();
    for (size_t i = 0; i < repo->maintainer_count; i++)
      if (repo->maintainers[i] && *repo->maintainers[i])
        g_ptr_array_add(maintainers, repo->maintainers[i]);
    for (size_t i = 0; i < repo->relay_count; i++)
      if (repo->relays[i] && *repo->relays[i])
        g_ptr_array_add(relays, repo->relays[i]);
    g_ptr_array_add(maintainers, NULL);
    g_ptr_array_add(relays, NULL);
    target = gn_issue_target_new(hint->owner_hex, hint->repo_id,
                                 (const gchar *const *)maintainers->pdata,
                                 (const gchar *const *)relays->pdata);
    *created_at = nostr_event_get_created_at(event);
  }
  nip34_repository_free(repo);
  nostr_event_free(event);
  return target;
}

GnIssueTarget *
gn_issue_target_from_announcements(const GnIssueTarget *hint, const gchar *const *events,
                                   gssize n_events)
{
  g_return_val_if_fail(hint != NULL, NULL);
  GnIssueTarget *newest = NULL;
  gint64 newest_at = 0;
  for (gssize i = 0; events && (n_events < 0 ? events[i] != NULL : i < n_events); i++) {
    gint64 created_at = 0;
    GnIssueTarget *candidate = target_from_announcement(hint, events[i], &created_at);
    if (!candidate)
      continue;
    if (!newest || created_at > newest_at) {
      gn_issue_target_free(newest);
      newest = candidate;
      newest_at = created_at;
    } else {
      gn_issue_target_free(candidate);
    }
  }
  return newest;
}

/* ---- Draft builder ---- */

static void
append_section(GString *body, const gchar *heading, const gchar *text)
{
  g_autofree gchar *copy = stripped(text);
  if (*copy)
    g_string_append_printf(body, "\n\n## %s\n\n%s", heading, copy);
}

static gboolean
label_ok(const gchar *label)
{
  if (strlen(label) > GN_ISSUE_LABEL_MAX_BYTES || !g_utf8_validate(label, -1, NULL))
    return FALSE;
  for (const gchar *p = label; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isspace(c) || g_unichar_iscntrl(c))
      return FALSE;
  }
  return TRUE;
}

static gboolean
parse_labels(const gchar *const *required, const gchar *csv, GPtrArray *labels, GError **error)
{
  for (guint i = 0; required && required[i]; i++)
    if (*required[i] && !g_ptr_array_find_with_equal_func(labels, required[i], g_str_equal, NULL))
      g_ptr_array_add(labels, g_strdup(required[i]));
  guint base = labels->len;
  g_auto(GStrv) parts = g_strsplit(csv ? csv : "", ",", -1);
  for (guint i = 0; parts[i]; i++) {
    gchar *label = g_strstrip(parts[i]);
    if (!*label || g_ptr_array_find_with_equal_func(labels, label, g_str_equal, NULL))
      continue;
    if (!label_ok(label))
      return invalid(error, _("Labels are separated by commas. Each label can be up to 64 bytes, without spaces."));
    if (labels->len == base + GN_ISSUE_MAX_USER_LABELS)
      return invalid(error, _("Add up to 16 labels."));
    g_ptr_array_add(labels, g_strdup(label));
  }
  return TRUE;
}

static gboolean
is_object_id(const gchar *id)
{
  gsize len = strlen(id);
  if (len != 40 && len != 64) /* SHA-1 or SHA-256 object format */
    return FALSE;
  for (gsize i = 0; i < len; i++)
    if (!g_ascii_isxdigit(id[i]))
      return FALSE;
  return TRUE;
}

static gboolean
append_commits(GString *body, const gchar *text, GError **error)
{
  g_auto(GStrv) parts = g_strsplit_set(text ? text : "", ", \t\r\n", -1);
  g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i])
      continue;
    if (!is_object_id(parts[i]))
      return invalid(error, _("Related commits must be full commit IDs (40 or 64 hexadecimal characters)."));
    gchar *id = g_ascii_strdown(parts[i], -1);
    if (g_ptr_array_find_with_equal_func(seen, id, g_str_equal, NULL)) {
      g_free(id);
      continue;
    }
    if (!seen->len)
      g_string_append(body, "\n\n## Related commits\n");
    g_string_append_printf(body, "\n- `%s`", id);
    g_ptr_array_add(seen, id);
  }
  return TRUE;
}

static gboolean
web_url_ok(const gchar *text)
{
  g_autoptr(GUri) uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);
  if (!uri)
    return FALSE;
  const gchar *scheme = g_uri_get_scheme(uri);
  const gchar *host = g_uri_get_host(uri);
  /* A reference for readers, never fetched. No credentials: a public report
   * must not carry userinfo. */
  return (g_ascii_strcasecmp(scheme, "https") == 0 || g_ascii_strcasecmp(scheme, "http") == 0) &&
         host && *host && !g_uri_get_userinfo(uri);
}

static gboolean
add_url(GPtrArray *urls, const gchar *url, GError **error)
{
  if (!url || !*url || g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
    return TRUE;
  if (!web_url_ok(url))
    return invalid(error, _("Attachment URLs must be web addresses without a user name or password."));
  g_ptr_array_add(urls, (gpointer)url);
  return TRUE;
}

static void
append_url_section(GString *body, const gchar *heading, GPtrArray *urls)
{
  for (guint i = 0; i < urls->len; i++) {
    if (!i)
      g_string_append_printf(body, "\n\n## %s\n", heading);
    g_string_append_printf(body, "\n- %s", (const gchar *)g_ptr_array_index(urls, i));
  }
}

G_DEFINE_BOXED_TYPE(GnIssueDraft, gn_issue_draft, gn_issue_draft_copy, gn_issue_draft_free)

GnIssueDraft *
gn_issue_draft_new(const GnIssueDraftInput *in, GError **error)
{
  g_return_val_if_fail(in != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  gn_portable_gettext_domain();
  g_autofree gchar *subject = stripped(in->title);
  g_autofree gchar *text = stripped(in->description);
  if (!*subject || !*text) {
    invalid(error, _("Enter a title and description."));
    return NULL;
  }
  if (strlen(subject) > GN_ISSUE_TITLE_MAX_BYTES || !g_utf8_validate(subject, -1, NULL)) {
    invalid(error, _("The title is too long. Shorten it to 640 bytes or fewer."));
    return NULL;
  }
  if (!g_utf8_validate(text, -1, NULL)) {
    invalid(error, _("The description is not valid text."));
    return NULL;
  }
  if (in->diagnostics && (strlen(in->diagnostics) > GN_ISSUE_DIAGNOSTICS_MAX_BYTES ||
                          !g_utf8_validate(in->diagnostics, -1, NULL))) {
    invalid(error, _("The diagnostics are larger than 8,000 bytes. Leave them out of this report."));
    return NULL;
  }
  const GnNip34IssueFieldsSnapshot *fields = in->fields;
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
  if (!parse_labels(in->required_labels, fields ? fields->labels : NULL, labels, error))
    return NULL;

  g_autoptr(GString) body = g_string_new(text);
  if (fields) {
    append_section(body, "Steps to Reproduce", fields->steps);
    append_section(body, "Expected Result", fields->expected);
    append_section(body, "Actual Result", fields->actual);
  }

  g_autoptr(GPtrArray) crash = g_ptr_array_new();
  for (guint i = 0; in->crash_log_urls && in->crash_log_urls[i]; i++)
    if (!add_url(crash, in->crash_log_urls[i], error))
      return NULL;
  append_url_section(body, "Crash logs", crash);

  g_autoptr(GPtrArray) attachments = g_ptr_array_new();
  g_auto(GStrv) manual = g_strsplit_set(fields && fields->attachment_urls ? fields->attachment_urls : "",
                                        ", \t\r\n", -1);
  for (guint i = 0; manual[i]; i++)
    if (!add_url(attachments, manual[i], error))
      return NULL;
  for (guint i = 0; in->attachment_urls && in->attachment_urls[i]; i++)
    if (!add_url(attachments, in->attachment_urls[i], error))
      return NULL;
  append_url_section(body, "Attachments", attachments);

  if (fields && !append_commits(body, fields->related_commits, error))
    return NULL;

  if (in->diagnostics && *in->diagnostics) {
    g_string_append_printf(body, "\n\n## %s\n\n```text\n",
                           in->diagnostics_heading && *in->diagnostics_heading
                             ? in->diagnostics_heading : "Diagnostics");
    g_string_append(body, in->diagnostics);
    if (body->str[body->len - 1] != '\n')
      g_string_append_c(body, '\n');
    g_string_append(body, "```");
  }
  /* Checked on the assembled body: never trim anything to make it fit. */
  if (body->len > GN_ISSUE_BODY_MAX_BYTES) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                _("The report is %" G_GSIZE_FORMAT " bytes; the limit is %d. Shorten it or leave out diagnostics."),
                body->len, GN_ISSUE_BODY_MAX_BYTES);
    return NULL;
  }
  GnIssueDraft *draft = g_new0(GnIssueDraft, 1);
  draft->title = g_steal_pointer(&subject);
  draft->body = g_string_free(g_steal_pointer(&body), FALSE);
  g_ptr_array_add(labels, NULL);
  draft->labels = (GStrv)g_ptr_array_free(g_steal_pointer(&labels), FALSE);
  return draft;
}

GnIssueDraft *
gn_issue_draft_copy(const GnIssueDraft *draft)
{
  if (!draft)
    return NULL;
  GnIssueDraft *copy = g_new0(GnIssueDraft, 1);
  copy->title = g_strdup(draft->title);
  copy->body = g_strdup(draft->body);
  copy->labels = g_strdupv(draft->labels);
  return copy;
}

void
gn_issue_draft_free(GnIssueDraft *draft)
{
  if (!draft)
    return;
  g_free(draft->title);
  g_free(draft->body);
  g_strfreev(draft->labels);
  g_free(draft);
}

gboolean
gn_issue_draft_equal(const GnIssueDraft *a, const GnIssueDraft *b)
{
  return a && b && g_str_equal(a->title, b->title) && g_str_equal(a->body, b->body) &&
         g_strv_equal((const gchar *const *)a->labels, (const gchar *const *)b->labels);
}

NostrEvent *
gn_issue_draft_build_event(const GnIssueDraft *draft, const GnIssueTarget *target,
                           const gchar *pubkey)
{
  g_return_val_if_fail(draft != NULL && target != NULL, NULL);
  NostrEvent *event = nip34_create_issue(target->owner_hex, target->repo_id, draft->title,
                                         draft->body, (const char *const *)draft->labels,
                                         (const char *const *)target->maintainers);
  if (event && pubkey && *pubkey)
    nostr_event_set_pubkey(event, pubkey);
  return event;
}

gchar *
gn_issue_draft_to_unsigned_json(const GnIssueDraft *draft, const GnIssueTarget *target,
                                const gchar *pubkey)
{
  NostrEvent *event = gn_issue_draft_build_event(draft, target, pubkey);
  if (!event)
    return NULL;
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

NostrEvent *
gn_issue_build_open_status(const gchar *issue_id, const GnIssueTarget *target,
                           const gchar *pubkey)
{
  g_return_val_if_fail(issue_id && *issue_id && target, NULL);
  g_autoptr(GPtrArray) participants = g_ptr_array_new();
  g_ptr_array_add(participants, target->owner_hex);
  for (guint i = 0; target->maintainers[i]; i++)
    if (*target->maintainers[i] &&
        !g_ptr_array_find_with_equal_func(participants, target->maintainers[i], g_str_equal, NULL))
      g_ptr_array_add(participants, target->maintainers[i]);
  if (pubkey && *pubkey &&
      !g_ptr_array_find_with_equal_func(participants, pubkey, g_str_equal, NULL))
    g_ptr_array_add(participants, (gpointer)pubkey);
  g_ptr_array_add(participants, NULL);
  g_autofree gchar *address = gn_issue_target_get_address(target);
  NostrEvent *status = nip34_create_status(issue_id, NIP34_STATUS_OPEN, "", address,
                                           (const char *const *)participants->pdata);
  if (status && pubkey && *pubkey)
    nostr_event_set_pubkey(status, pubkey);
  return status;
}

gchar *
gn_issue_normalize_relay_url(const gchar *url, GError **error)
{
  gn_portable_gettext_domain();
  g_autofree gchar *text = stripped(url);
  g_autoptr(GUri) uri = *text ? g_uri_parse(text, G_URI_FLAGS_NONE, NULL) : NULL;
  if (!uri || g_ascii_strcasecmp(g_uri_get_scheme(uri), "wss") != 0 ||
      !g_uri_get_host(uri) || !*g_uri_get_host(uri) || g_uri_get_userinfo(uri)) {
    invalid(error, _("Enter up to 16 secure WebSocket relay URLs (wss://), separated by spaces."));
    return NULL;
  }
  return g_steal_pointer(&text);
}
