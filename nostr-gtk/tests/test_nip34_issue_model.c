/* NIP-34 issue model (nostrc-8xfib.5). Merges the former gnostr
 * test_bug_report_assembly.c and Groundhog's issue-draft cases. GTK-free. */
#include <nostr-gtk-1.0/gn-nip34-issue.h>
#include <nostr-gtk-1.0/gn-nip34-issue-iface.h>
#include <gio/gio.h>
#include <nip34.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

#define OWNER "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"
#define OTHER "1111111111111111111111111111111111111111111111111111111111111111"
#define PK "2222222222222222222222222222222222222222222222222222222222222222"
#define MAINT "3333333333333333333333333333333333333333333333333333333333333333"
#define COMMIT40 "ABCDEF0123456789ABCDEF0123456789ABCDEF01"
#define COMMIT64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define DIAG "Groundhog local diagnostics (schema 1; daily counts only)\n" \
             "day\tcomponent\tevent\tresult\tcount\n"

static const char *const GROUNDHOG_LABELS[] = { "bug", "groundhog", NULL };

static GnIssueTarget *
target(void)
{
  return gn_issue_target_new(OWNER, "nostrc", NULL, NULL);
}

static gboolean
has_tag(NostrEvent *event, const char *name, const char *value)
{
  const NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), name) == 0 &&
        g_strcmp0(nostr_tag_get(tag, 1), value) == 0)
      return TRUE;
  }
  return FALSE;
}

static guint
count_tags(NostrEvent *event, const char *name)
{
  guint n = 0;
  const NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; i < nostr_tags_size(tags); i++)
    if (g_strcmp0(nostr_tag_get(nostr_tags_get(tags, i), 0), name) == 0)
      n++;
  return n;
}

static GnNip34IssueFieldsSnapshot *
fields_new(const char *steps, const char *labels, const char *commits, const char *urls)
{
  GnNip34IssueFieldsSnapshot *fields = g_new0(GnNip34IssueFieldsSnapshot, 1);
  fields->steps = g_strdup(steps);
  fields->expected = g_strdup("It opens.");
  fields->actual = g_strdup("It crashes.");
  fields->labels = g_strdup(labels);
  fields->related_commits = g_strdup(commits);
  fields->attachment_urls = g_strdup(urls);
  return fields;
}

static GnIssueDraft *
draft(const char *title, const char *description, const GnNip34IssueFieldsSnapshot *fields,
      const char *diagnostics, GError **error)
{
  GnIssueDraftInput input = {
    .title = title, .description = description, .fields = fields,
    .diagnostics = diagnostics, .diagnostics_heading = "Local diagnostics",
    .required_labels = GROUNDHOG_LABELS,
  };
  return gn_issue_draft_new(&input, error);
}

static void
rejected(const char *title, const char *description,
         const GnNip34IssueFieldsSnapshot *fields, const char *diagnostics)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GnIssueDraft) d = draft(title, description, fields, diagnostics, &error);
  g_assert_null(d);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static const char *const RICH_BODY =
  "Crash on open.\n\n## Steps to Reproduce\n\n1. Open\n2. Click"
  "\n\n## Expected Result\n\nIt opens.\n\n## Actual Result\n\nIt crashes."
  "\n\n## Attachments\n\n- https://example.org/shot.png\n- http://example.net/log.txt"
  "\n\n## Related commits\n\n- `abcdef0123456789abcdef0123456789abcdef01`\n- `" COMMIT64 "`";

/* Former Groundhog test_issue_draft. */
static void
test_bounded_draft(void)
{
  g_autoptr(GnNip34IssueFieldsSnapshot) fields = fields_new(
    "\n1. Open\n2. Click\n", " ui, crash ,bug,, ui",
    COMMIT40 ", " COMMIT64 "\nabcdef0123456789abcdef0123456789abcdef01",
    "https://example.org/shot.png, http://example.net/log.txt https://example.org/shot.png");
  g_autoptr(GError) error = NULL;
  g_autoptr(GnIssueDraft) d = draft("  A title ", "  Crash on open.  \n", fields, DIAG, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(d->title, ==, "A title");
  g_autofree char *expected = g_strconcat(RICH_BODY, "\n\n## Local diagnostics\n\n```text\n", DIAG, "```", NULL);
  g_assert_cmpstr(d->body, ==, expected);
  const char *labels[] = { "bug", "groundhog", "ui", "crash", NULL };
  g_assert_true(g_strv_equal((const char *const *)d->labels, labels));

  /* The unsigned event carries the assembled body; commits are body text, never e tags. */
  g_autoptr(GnIssueTarget) t = target();
  g_autofree char *json = gn_issue_draft_to_unsigned_json(d, t, PK);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(event), ==, NIP34_KIND_ISSUE);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, PK);
  g_assert_cmpstr(nostr_event_get_content(event), ==, expected);
  g_assert_true(has_tag(event, "l", "ui"));
  g_assert_true(has_tag(event, "t", "crash"));
  g_assert_true(has_tag(event, "a", "30617:" OWNER ":nostrc"));
  g_assert_cmpuint(count_tags(event, "e"), ==, 0);
  g_assert_cmpuint(nostr_tags_size(nostr_event_get_tags(event)), ==, 13);
  nostr_event_free(event);

  /* Same input, same canonical draft; any edit differs. */
  g_autoptr(GnIssueDraft) again = draft("A title", "Crash on open.", fields, DIAG, NULL);
  g_assert_true(gn_issue_draft_equal(d, again));
  g_autoptr(GnIssueDraft) copy = gn_issue_draft_copy(d);
  g_assert_true(gn_issue_draft_equal(d, copy));
  g_autoptr(GnIssueDraft) without = draft("A title", "Crash on open.", fields, NULL, NULL);
  g_assert_false(gn_issue_draft_equal(d, without));
  g_assert_null(strstr(without->body, "diagnostics"));

  g_autoptr(GnIssueDraft) plain = draft("T", "Body", NULL, NULL, NULL);
  g_assert_cmpstr(plain->body, ==, "Body");
  g_assert_true(g_strv_equal((const char *const *)plain->labels, GROUNDHOG_LABELS));

  rejected("", "Body", NULL, NULL);
  rejected("T", " \n ", NULL, NULL);
  g_autofree char *title640 = g_strnfill(640, 't');
  g_autofree char *title641 = g_strnfill(641, 't');
  g_autoptr(GnIssueDraft) long_title = draft(title640, "Body", NULL, NULL, NULL);
  g_assert_nonnull(long_title);
  rejected(title641, "Body", NULL, NULL);

  g_autoptr(GnNip34IssueFieldsSnapshot) spaced = fields_new(NULL, "two words", NULL, NULL);
  rejected("T", "Body", spaced, NULL);
  g_autoptr(GnNip34IssueFieldsSnapshot) many = fields_new(NULL, "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q", NULL, NULL);
  rejected("T", "Body", many, NULL);
  g_autoptr(GnNip34IssueFieldsSnapshot) sixteen = fields_new(NULL, "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p", NULL, NULL);
  g_autoptr(GnIssueDraft) fits16 = draft("T", "Body", sixteen, NULL, NULL);
  g_assert_nonnull(fits16);
  g_autoptr(GnNip34IssueFieldsSnapshot) short_commit = fields_new(NULL, NULL, "abc1234", NULL);
  rejected("T", "Body", short_commit, NULL);
  g_autoptr(GnNip34IssueFieldsSnapshot) file_url = fields_new(NULL, NULL, NULL, "file:///home/me/log.txt");
  rejected("T", "Body", file_url, NULL);
  g_autoptr(GnNip34IssueFieldsSnapshot) credentials = fields_new(NULL, NULL, NULL, "https://me:pw@example.org/x");
  rejected("T", "Body", credentials, NULL);
  g_autofree char *diag8001 = g_strnfill(8001, 'd');
  rejected("T", "Body", NULL, diag8001);

  /* The assembled body is limited, not its parts, and nothing is trimmed. */
  g_autofree char *body16000 = g_strnfill(16000, 'b');
  g_autoptr(GnIssueDraft) full = draft("T", body16000, NULL, NULL, NULL);
  g_assert_cmpuint(strlen(full->body), ==, 16000);
  g_autofree char *body16001 = g_strnfill(16001, 'b');
  rejected("T", body16001, NULL, NULL);
  g_autofree char *body15000 = g_strnfill(15000, 'b');
  g_autofree char *diag1000 = g_strnfill(1000, 'd');
  g_autoptr(GnIssueDraft) fits = draft("T", body15000, NULL, NULL, NULL);
  g_assert_nonnull(fits);
  rejected("T", body15000, NULL, diag1000);
}

/* Former gnostr test_bug_report_assembly: uploaded URLs, system info and
 * maintainers. Commit IDs are now validated (was free text). */
static void
test_gnostr_assembly(void)
{
  const char *crash[] = { "https://blossom.example/crash", NULL };
  const char *attachments[] = { "https://blossom.example/image", NULL };
  const char *maintainers[] = { MAINT, NULL };
  g_autoptr(GnIssueTarget) t = gn_issue_target_new(OWNER, "nostrc", maintainers, NULL);
  g_autoptr(GnNip34IssueFieldsSnapshot) fields = fields_new(NULL, "feature, ui, feature", COMMIT40, NULL);
  GnIssueDraftInput input = {
    .title = "Feature request", .description = "Please add this.", .fields = fields,
    .diagnostics = "App: gnostr 0.1.0\nOS: TestOS", .diagnostics_heading = "System info",
    .crash_log_urls = crash, .attachment_urls = attachments,
  };
  g_autoptr(GError) error = NULL;
  g_autoptr(GnIssueDraft) d = gn_issue_draft_new(&input, &error);
  g_assert_no_error(error);
  const char *labels[] = { "feature", "ui", NULL };
  g_assert_true(g_strv_equal((const char *const *)d->labels, labels));

  NostrEvent *event = gn_issue_draft_build_event(d, t, NULL);
  g_assert_nonnull(event);
  g_assert_cmpuint(count_tags(event, "t"), ==, 2);
  g_assert_true(has_tag(event, "L", NIP34_ISSUE_LABEL_NAMESPACE));
  g_assert_true(has_tag(event, "p", OWNER));
  g_assert_true(has_tag(event, "p", MAINT));
  g_assert_cmpuint(count_tags(event, "r"), ==, 0);
  const char *content = nostr_event_get_content(event);
  g_assert_nonnull(strstr(content, "## Crash logs\n\n- https://blossom.example/crash"));
  g_assert_nonnull(strstr(content, "## Attachments\n\n- https://blossom.example/image"));
  g_assert_nonnull(strstr(content, "## System info\n\n```text\nApp: gnostr 0.1.0"));
  g_assert_nonnull(strstr(content, "## Related commits"));
  /* Section order: crash logs, attachments, commits, then diagnostics. */
  g_assert_true(strstr(content, "## Crash logs") < strstr(content, "## Attachments"));
  g_assert_true(strstr(content, "## Related commits") < strstr(content, "## System info"));
  nostr_event_free(event);
}

static char *
announcement(const char *author, const char *d, gint64 created_at, const char *relay,
             const char *maintainer)
{
  const char *relays[] = { relay, NULL };
  const char *maintainers[] = { maintainer, NULL };
  NostrEvent *event = nip34_create_repo_announcement(d, "nostrc", NULL, NULL, NULL,
                                                     relay ? relays : NULL,
                                                     maintainer ? maintainers : NULL);
  nostr_event_set_pubkey(event, author);
  nostr_event_set_created_at(event, created_at);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}

static void
test_announcement_newest_wins(void)
{
  g_autoptr(GnIssueTarget) hint = target();
  g_autofree char *older = announcement(OWNER, "nostrc", 100, "wss://old.example", MAINT);
  g_autofree char *newer = announcement(OWNER, "nostrc", 200, "wss://new.example", MAINT);
  g_autofree char *forged = announcement(OTHER, "nostrc", 300, "wss://forged.example", OTHER);
  g_autofree char *other_repo = announcement(OWNER, "other", 400, "wss://other.example", NULL);
  const char *events[] = { older, forged, newer, other_repo, "not json", NULL };
  g_autoptr(GnIssueTarget) t = gn_issue_target_from_announcements(hint, events, -1);
  g_assert_nonnull(t);
  g_assert_cmpstr(t->owner_hex, ==, OWNER);
  g_assert_cmpstr(t->repo_id, ==, "nostrc");
  g_assert_cmpstr(t->relays[0], ==, "wss://new.example");
  g_assert_null(t->relays[1]);
  g_assert_cmpstr(t->maintainers[0], ==, MAINT);
  g_assert_false(gn_issue_target_equal(t, hint));
  g_autofree char *address = gn_issue_target_get_address(t);
  g_assert_cmpstr(address, ==, "30617:" OWNER ":nostrc");

  const char *none[] = { forged, other_repo };
  g_assert_null(gn_issue_target_from_announcements(hint, none, 2));
  g_assert_null(gn_issue_target_from_announcements(hint, NULL, 0));
}

static void
test_open_status(void)
{
  const char *maintainers[] = { MAINT, OWNER, NULL };
  g_autoptr(GnIssueTarget) t = gn_issue_target_new(OWNER, "nostrc", maintainers, NULL);
  const char *issue = "4444444444444444444444444444444444444444444444444444444444444444";
  NostrEvent *status = gn_issue_build_open_status(issue, t, PK);
  g_assert_nonnull(status);
  g_assert_cmpint(nostr_event_get_kind(status), ==, NIP34_STATUS_OPEN);
  g_assert_cmpstr(nostr_event_get_pubkey(status), ==, PK);
  g_assert_true(has_tag(status, "e", issue));
  g_assert_true(has_tag(status, "a", "30617:" OWNER ":nostrc"));
  g_assert_true(has_tag(status, "p", OWNER));
  g_assert_true(has_tag(status, "p", MAINT));
  g_assert_true(has_tag(status, "p", PK));
  g_assert_cmpuint(count_tags(status, "p"), ==, 3);
  nostr_event_free(status);
}

static void
test_relay_rule(void)
{
  g_autofree char *ok = gn_issue_normalize_relay_url(" wss://relay.example ", NULL);
  g_assert_cmpstr(ok, ==, "wss://relay.example");
  const char *bad[] = { "ws://relay.example", "https://relay.example", "wss://u:p@relay.example",
                        "wss://", "", "relay.example", NULL };
  for (guint i = 0; bad[i]; i++) {
    g_autoptr(GError) error = NULL;
    g_assert_null(gn_issue_normalize_relay_url(bad[i], &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
}

/* The portable sources never reach a network stack, pixbuf or app code: the
 * services are injected (plan §3, check_privacy no-gdk-pixbuf/libsoup). */
static void
test_portable_sources_clean(void)
{
  const char *sources[] = {
    "src/gn-nip34-issue.c", "src/gn-nip34-issue-iface.c", "src/gn-nip34-issue-view.c",
    "src/gn-nip34-issue-fields.c", "include/nostr-gtk-1.0/gn-nip34-issue.h",
    "include/nostr-gtk-1.0/gn-nip34-issue-iface.h", "include/nostr-gtk-1.0/gn-nip34-issue-view.h",
    NULL };
  const char *forbidden[] = { "gdk_pixbuf", "gdk-pixbuf", "soup", "apps/gnostr", "gnostr-main-window",
                              "storage_ndb", "nostr_pool", "blossom", NULL };
  for (guint i = 0; sources[i]; i++) {
    g_autofree char *path = g_build_filename(NOSTR_GTK_SOURCE_DIR, sources[i], NULL);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_autofree char *lower = g_ascii_strdown(text, -1);
    for (guint j = 0; forbidden[j]; j++)
      if (strstr(lower, forbidden[j]) && !(g_str_equal(forbidden[j], "blossom") &&
                                           strstr(sources[i], "-iface.h")))
        g_error("%s mentions %s", sources[i], forbidden[j]);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-gtk/nip34-issue/bounded-draft", test_bounded_draft);
  g_test_add_func("/nostr-gtk/nip34-issue/gnostr-assembly", test_gnostr_assembly);
  g_test_add_func("/nostr-gtk/nip34-issue/announcement-newest-wins", test_announcement_newest_wins);
  g_test_add_func("/nostr-gtk/nip34-issue/open-status", test_open_status);
  g_test_add_func("/nostr-gtk/nip34-issue/relay-rule", test_relay_rule);
  g_test_add_func("/nostr-gtk/nip34-issue/portable-sources-clean", test_portable_sources_clean);
  return g_test_run();
}
