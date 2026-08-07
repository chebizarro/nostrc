#include "gnostr-bug-report-assembly.h"

#include "../../../../nips/nip34/include/nip34.h"

static void
append_url_section(GString *content, const char *heading, const GPtrArray *urls)
{
  if (!urls || urls->len == 0)
    return;

  g_string_append_printf(content, "\n\n## %s\n", heading);
  for (guint i = 0; i < urls->len; i++) {
    const char *url = g_ptr_array_index((GPtrArray *)urls, i);
    if (url && *url)
      g_string_append_printf(content, "\n- %s", url);
  }
}

GStrv
gnostr_bug_report_parse_labels(const char *labels_csv)
{
  g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  if (labels_csv && *labels_csv) {
    g_auto(GStrv) parts = g_strsplit(labels_csv, ",", -1);
    for (guint i = 0; parts[i]; i++) {
      char *label = g_strstrip(parts[i]);
      if (!*label)
        continue;
      if (g_hash_table_add(seen, g_strdup(label)))
        g_ptr_array_add(labels, g_strdup(label));
    }
  }

  g_ptr_array_add(labels, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&labels), FALSE);
}

char *
gnostr_bug_report_assemble_content(const char *description,
                                   const GPtrArray *crash_log_urls,
                                   const GPtrArray *attachment_urls,
                                   const char *system_info,
                                   const char *related_commits)
{
  GString *content = g_string_new(description ? description : "");

  append_url_section(content, "Crash logs", crash_log_urls);
  append_url_section(content, "Attachments", attachment_urls);

  if (system_info && *system_info) {
    g_string_append(content, "\n\n## System info\n\n```text\n");
    g_string_append(content, system_info);
    if (content->len > 0 && content->str[content->len - 1] != '\n')
      g_string_append_c(content, '\n');
    g_string_append(content, "```");
  }

  if (related_commits && *related_commits) {
    g_auto(GStrv) commits = g_strsplit_set(related_commits, ", \t\r\n", -1);
    gboolean wrote_heading = FALSE;
    for (guint i = 0; commits[i]; i++) {
      char *commit = g_strstrip(commits[i]);
      if (!*commit)
        continue;
      if (!wrote_heading) {
        g_string_append(content, "\n\n## Related commits\n");
        wrote_heading = TRUE;
      }
      g_string_append_printf(content, "\n- `%s`", commit);
    }
  }

  return g_string_free(content, FALSE);
}

NostrEvent *
gnostr_bug_report_build_issue_event(const char *repo_owner_pubkey_hex,
                                    const char *repo_id,
                                    const char *subject,
                                    const char *description,
                                    const char *labels_csv,
                                    const char *const *maintainers,
                                    const GPtrArray *crash_log_urls,
                                    const GPtrArray *attachment_urls,
                                    const char *system_info,
                                    const char *related_commits)
{
  g_auto(GStrv) labels = gnostr_bug_report_parse_labels(labels_csv);
  g_autofree char *content =
      gnostr_bug_report_assemble_content(description, crash_log_urls,
                                         attachment_urls, system_info,
                                         related_commits);

  return nip34_create_issue(repo_owner_pubkey_hex, repo_id, subject, content,
                            (const char *const *)labels, maintainers);
}
