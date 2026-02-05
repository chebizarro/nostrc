/*
 * nip34.c - NIP-34 Git Repository Event Utilities
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nip34.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* Kind detection helpers */
gboolean gnostr_nip34_is_repo(int kind) {
  return kind == NOSTR_KIND_GIT_REPO;
}

gboolean gnostr_nip34_is_patch(int kind) {
  return kind == NOSTR_KIND_GIT_PATCH;
}

gboolean gnostr_nip34_is_issue(int kind) {
  return kind == NOSTR_KIND_GIT_ISSUE;
}

gboolean gnostr_nip34_is_reply(int kind) {
  return kind == NOSTR_KIND_GIT_REPLY;
}

gboolean gnostr_nip34_is_git_event(int kind) {
  return gnostr_nip34_is_repo(kind) ||
         gnostr_nip34_is_patch(kind) ||
         gnostr_nip34_is_issue(kind) ||
         gnostr_nip34_is_reply(kind);
}

/* Repository metadata */
GnostrRepoMeta *gnostr_repo_meta_new(void) {
  return g_new0(GnostrRepoMeta, 1);
}

void gnostr_repo_meta_free(GnostrRepoMeta *meta) {
  if (!meta) return;
  g_free(meta->d_tag);
  g_free(meta->name);
  g_free(meta->description);
  g_strfreev(meta->clone_urls);
  g_strfreev(meta->web_urls);
  g_strfreev(meta->maintainers);
  g_strfreev(meta->relays);
  g_strfreev(meta->topics);
  g_free(meta->head_commit);
  g_free(meta->license);
  g_free(meta);
}

/* Helper to append string to GPtrArray for building strv */
static void append_string(GPtrArray *arr, const char *str) {
  if (str && *str)
    g_ptr_array_add(arr, g_strdup(str));
}

/* Convert GPtrArray to NULL-terminated strv, freeing the array */
static gchar **finish_strv(GPtrArray *arr, gsize *out_count) {
  g_ptr_array_add(arr, NULL);  /* NULL terminator */
  if (out_count) *out_count = arr->len - 1;  /* Don't count NULL */
  return (gchar **)g_ptr_array_free(arr, FALSE);
}

GnostrRepoMeta *gnostr_repo_parse_tags(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_warning("[NIP34] Failed to parse tags JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrRepoMeta *meta = gnostr_repo_meta_new();
  GPtrArray *clone_urls = g_ptr_array_new();
  GPtrArray *web_urls = g_ptr_array_new();
  GPtrArray *maintainers = g_ptr_array_new();
  GPtrArray *relays = g_ptr_array_new();
  GPtrArray *topics = g_ptr_array_new();

  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (!tag) continue;

    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const char *key = json_array_get_string_element(tag, 0);
    const char *value = json_array_get_string_element(tag, 1);
    if (!key || !value) continue;

    if (g_strcmp0(key, "d") == 0 && !meta->d_tag) {
      meta->d_tag = g_strdup(value);
    } else if (g_strcmp0(key, "name") == 0 && !meta->name) {
      meta->name = g_strdup(value);
    } else if (g_strcmp0(key, "description") == 0 && !meta->description) {
      meta->description = g_strdup(value);
    } else if (g_strcmp0(key, "clone") == 0) {
      append_string(clone_urls, value);
    } else if (g_strcmp0(key, "web") == 0) {
      append_string(web_urls, value);
    } else if (g_strcmp0(key, "maintainers") == 0 || g_strcmp0(key, "p") == 0) {
      /* Maintainers can be in "maintainers" tag or "p" tags */
      append_string(maintainers, value);
    } else if (g_strcmp0(key, "relays") == 0 || g_strcmp0(key, "relay") == 0) {
      append_string(relays, value);
    } else if (g_strcmp0(key, "t") == 0) {
      append_string(topics, value);
    } else if (g_strcmp0(key, "r") == 0 && !meta->head_commit) {
      /* First "r" tag is typically HEAD reference */
      meta->head_commit = g_strdup(value);
    } else if (g_strcmp0(key, "license") == 0 && !meta->license) {
      meta->license = g_strdup(value);
    }
  }

  meta->clone_urls = finish_strv(clone_urls, &meta->clone_urls_count);
  meta->web_urls = finish_strv(web_urls, &meta->web_urls_count);
  meta->maintainers = finish_strv(maintainers, &meta->maintainers_count);
  meta->relays = finish_strv(relays, &meta->relays_count);
  meta->topics = finish_strv(topics, &meta->topics_count);

  g_object_unref(parser);
  return meta;
}

/* Patch metadata */
GnostrPatchMeta *gnostr_patch_meta_new(void) {
  return g_new0(GnostrPatchMeta, 1);
}

void gnostr_patch_meta_free(GnostrPatchMeta *meta) {
  if (!meta) return;
  g_free(meta->title);
  g_free(meta->description);
  g_free(meta->repo_a_tag);
  g_free(meta->commit_id);
  g_free(meta->parent_commit);
  g_strfreev(meta->hashtags);
  g_free(meta);
}

/* Extract title from git patch content (Subject: line or first line) */
static char *extract_patch_title(const char *content) {
  if (!content) return NULL;

  /* Look for "Subject: " line */
  const char *subject = strstr(content, "Subject: ");
  if (subject) {
    subject += 9;  /* Skip "Subject: " */
    /* Skip [PATCH ...] prefix if present */
    if (*subject == '[') {
      const char *bracket_end = strchr(subject, ']');
      if (bracket_end) subject = bracket_end + 1;
      while (*subject == ' ') subject++;
    }
    /* Find end of line */
    const char *eol = strchr(subject, '\n');
    if (eol) return g_strndup(subject, eol - subject);
    return g_strdup(subject);
  }

  /* Fallback: first non-empty line */
  while (*content == '\n' || *content == '\r') content++;
  const char *eol = strchr(content, '\n');
  if (eol) return g_strndup(content, eol - content);
  return g_strdup(content);
}

GnostrPatchMeta *gnostr_patch_parse_tags(const char *tags_json, const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrPatchMeta *meta = gnostr_patch_meta_new();
  GPtrArray *hashtags = g_ptr_array_new();

  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (!tag) continue;

    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const char *key = json_array_get_string_element(tag, 0);
    const char *value = json_array_get_string_element(tag, 1);
    if (!key || !value) continue;

    if (g_strcmp0(key, "a") == 0 && !meta->repo_a_tag) {
      meta->repo_a_tag = g_strdup(value);
    } else if (g_strcmp0(key, "commit") == 0 && !meta->commit_id) {
      meta->commit_id = g_strdup(value);
    } else if (g_strcmp0(key, "parent-commit") == 0 && !meta->parent_commit) {
      meta->parent_commit = g_strdup(value);
    } else if (g_strcmp0(key, "t") == 0) {
      append_string(hashtags, value);
    } else if (g_strcmp0(key, "subject") == 0 && !meta->title) {
      meta->title = g_strdup(value);
    } else if (g_strcmp0(key, "description") == 0 && !meta->description) {
      meta->description = g_strdup(value);
    }
  }

  meta->hashtags = finish_strv(hashtags, &meta->hashtags_count);

  /* Extract title from content if not in tags */
  if (!meta->title && content) {
    meta->title = extract_patch_title(content);
  }

  g_object_unref(parser);
  return meta;
}

/* Issue metadata */
GnostrIssueMeta *gnostr_issue_meta_new(void) {
  GnostrIssueMeta *meta = g_new0(GnostrIssueMeta, 1);
  meta->is_open = TRUE;  /* Default to open */
  return meta;
}

void gnostr_issue_meta_free(GnostrIssueMeta *meta) {
  if (!meta) return;
  g_free(meta->title);
  g_free(meta->repo_a_tag);
  g_strfreev(meta->labels);
  g_free(meta);
}

/* Extract title from issue content (first line) */
static char *extract_issue_title(const char *content) {
  if (!content) return NULL;
  while (*content == '\n' || *content == '\r' || *content == ' ') content++;
  const char *eol = strchr(content, '\n');
  if (eol) return g_strndup(content, eol - content);
  return g_strdup(content);
}

GnostrIssueMeta *gnostr_issue_parse_tags(const char *tags_json, const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrIssueMeta *meta = gnostr_issue_meta_new();
  GPtrArray *labels = g_ptr_array_new();

  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (!tag) continue;

    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const char *key = json_array_get_string_element(tag, 0);
    const char *value = json_array_get_string_element(tag, 1);
    if (!key || !value) continue;

    if (g_strcmp0(key, "a") == 0 && !meta->repo_a_tag) {
      meta->repo_a_tag = g_strdup(value);
    } else if (g_strcmp0(key, "subject") == 0 && !meta->title) {
      meta->title = g_strdup(value);
    } else if (g_strcmp0(key, "t") == 0 || g_strcmp0(key, "label") == 0) {
      append_string(labels, value);
    } else if (g_strcmp0(key, "status") == 0) {
      meta->is_open = (g_strcmp0(value, "closed") != 0);
    }
  }

  meta->labels = finish_strv(labels, &meta->labels_count);

  /* Extract title from content if not in tags */
  if (!meta->title && content) {
    meta->title = extract_issue_title(content);
  }

  g_object_unref(parser);
  return meta;
}
