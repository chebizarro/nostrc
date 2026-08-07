#include "ui/gnostr-bug-report-assembly.h"

#include "nip34.h"
#include "nostr-event.h"
#include "nostr-tag.h"

#include <glib.h>
#include <string.h>

static NostrTag *
find_tag(const NostrEvent *event, const char *key, guint occurrence)
{
  guint seen = 0;
  for (size_t i = 0; event && event->tags && i < event->tags->count; i++) {
    NostrTag *tag = event->tags->data[i];
    const char *tag_key = tag && tag->size > 0 ? string_array_get(tag, 0) : NULL;
    if (g_strcmp0(tag_key, key) == 0 && seen++ == occurrence)
      return tag;
  }
  return NULL;
}

static void
test_issue_assembly(void)
{
  g_autoptr(GPtrArray) logs = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) attachments = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(logs, g_strdup("https://blossom.example/crash"));
  g_ptr_array_add(attachments, g_strdup("https://blossom.example/image"));

  const char *maintainers[] = {"maintainer-1", NULL};
  NostrEvent *event = gnostr_bug_report_build_issue_event(
      "owner", "nostrc", "Feature request", "Please add this.",
      "feature, ui, feature", maintainers, logs, attachments,
      "App: gnostr 0.1.0\nOS: TestOS", "abc123, def456");

  g_assert_nonnull(event);
  g_assert_cmpint(nostr_event_get_kind(event), ==, NIP34_KIND_ISSUE);

  NostrTag *topic0 = find_tag(event, "t", 0);
  NostrTag *topic1 = find_tag(event, "t", 1);
  NostrTag *topic2 = find_tag(event, "t", 2);
  g_assert_cmpstr(string_array_get(topic0, 1), ==, "feature");
  g_assert_cmpstr(string_array_get(topic1, 1), ==, "ui");
  g_assert_null(topic2);

  NostrTag *namespace_tag = find_tag(event, "L", 0);
  NostrTag *label0 = find_tag(event, "l", 0);
  g_assert_cmpstr(string_array_get(namespace_tag, 1), ==,
                  NIP34_ISSUE_LABEL_NAMESPACE);
  g_assert_cmpuint(label0->size, ==, 3);
  g_assert_cmpstr(string_array_get(label0, 1), ==, "feature");
  g_assert_cmpstr(string_array_get(label0, 2), ==,
                  NIP34_ISSUE_LABEL_NAMESPACE);
  g_assert_cmpstr(string_array_get(find_tag(event, "p", 0), 1), ==, "owner");
  g_assert_cmpstr(string_array_get(find_tag(event, "p", 1), 1), ==,
                  "maintainer-1");

  const char *content = nostr_event_get_content(event);
  g_assert_nonnull(strstr(content, "## Crash logs"));
  g_assert_nonnull(strstr(content, "https://blossom.example/crash"));
  g_assert_nonnull(strstr(content, "## Attachments"));
  g_assert_nonnull(strstr(content, "https://blossom.example/image"));
  g_assert_nonnull(strstr(content, "## System info"));
  g_assert_nonnull(strstr(content, "```text\nApp: gnostr 0.1.0"));
  g_assert_nonnull(strstr(content, "## Related commits"));
  g_assert_nonnull(strstr(content, "`abc123`"));
  g_assert_null(find_tag(event, "r", 0));

  nostr_event_free(event);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gnostr/bug-report/assembly", test_issue_assembly);
  return g_test_run();
}
