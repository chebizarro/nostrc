/* ns-git.c - Describe a local git repository as a NIP-34 announcement
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-git.h"
#include "ns-event.h"
#include "ns-kind.h"

#include <gio/gio.h>
#include <nip34.h>
#include "nostr-event.h"

#include <stdlib.h>
#include <string.h>

gboolean
ns_git_is_repo(const gchar *dir)
{
  g_autofree gchar *dotgit = g_build_filename(dir, ".git", NULL);
  if (g_file_test(dotgit, G_FILE_TEST_EXISTS))
    return TRUE;
  /* Bare repository. */
  g_autofree gchar *head = g_build_filename(dir, "HEAD", NULL);
  g_autofree gchar *objects = g_build_filename(dir, "objects", NULL);
  g_autofree gchar *refs = g_build_filename(dir, "refs", NULL);
  return g_file_test(head, G_FILE_TEST_IS_REGULAR) &&
         g_file_test(objects, G_FILE_TEST_IS_DIR) &&
         g_file_test(refs, G_FILE_TEST_IS_DIR);
}

static void
add_unique(GPtrArray *arr, gchar *s)
{
  for (guint i = 0; i < arr->len; i++)
    if (g_str_equal(g_ptr_array_index(arr, i), s)) {
      g_free(s);
      return;
    }
  g_ptr_array_add(arr, s);
}

void
ns_git_public_urls(const gchar *const *remotes, GPtrArray *clone_out,
                   GPtrArray *web_out)
{
  for (guint i = 0; remotes && remotes[i]; i++) {
    const gchar *r = remotes[i];
    if (*r == '\0' || *r == '/' || *r == '.' || *r == '~' ||
        g_str_has_prefix(r, "file://"))
      continue;

    if (g_str_has_prefix(r, "https://") || g_str_has_prefix(r, "http://")) {
      g_autoptr(GUri) uri = g_uri_parse(r, G_URI_FLAGS_NONE, NULL);
      if (uri == NULL || g_uri_get_host(uri) == NULL)
        continue;
      /* Drop user:token@ — tokens in remotes are common and secret. */
      g_autoptr(GUri) clean = g_uri_build(G_URI_FLAGS_NONE, g_uri_get_scheme(uri),
                                          NULL, g_uri_get_host(uri),
                                          g_uri_get_port(uri), g_uri_get_path(uri),
                                          NULL, NULL);
      g_autofree gchar *s = g_uri_to_string(clean);
      if (g_str_has_prefix(s, "https://")) {
        gchar *web = g_strdup(s);
        if (g_str_has_suffix(web, ".git"))
          web[strlen(web) - 4] = '\0';
        add_unique(web_out, web);
      }
      add_unique(clone_out, g_steal_pointer(&s));
      continue;
    }
    if (g_str_has_prefix(r, "ssh://") || g_str_has_prefix(r, "git://") ||
        (strchr(r, '@') != NULL && strchr(r, ':') != NULL && strstr(r, "://") == NULL)) {
      add_unique(clone_out, g_strdup(r));
      continue;
    }
  }
}

static gchar *
run_git(const gchar *dir, GError **error, ...)
{
  g_autoptr(GPtrArray) argv = g_ptr_array_new();
  g_ptr_array_add(argv, (gpointer)"git");
  g_ptr_array_add(argv, (gpointer)"-C");
  g_ptr_array_add(argv, (gpointer)dir);
  va_list ap;
  va_start(ap, error);
  for (const gchar *a = va_arg(ap, const gchar *); a; a = va_arg(ap, const gchar *))
    g_ptr_array_add(argv, (gpointer)a);
  va_end(ap);
  g_ptr_array_add(argv, NULL);

  g_autoptr(GSubprocess) p = g_subprocess_newv((const gchar *const *)argv->pdata,
                                               G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                               G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                               error);
  if (p == NULL)
    return NULL;
  gchar *out = NULL;
  if (!g_subprocess_communicate_utf8(p, NULL, NULL, &out, NULL, error))
    return NULL;
  if (!g_subprocess_get_successful(p)) {
    g_free(out);
    return g_strdup("");
  }
  return out;
}

gboolean
ns_git_announcement(const gchar *dir, const gchar *const *relays,
                    JsonArray **out_tags, gchar **out_content,
                    gchar **out_repo_id, GError **error)
{
  if (!ns_git_is_repo(dir)) {
    g_set_error(error, NS_ERROR, NS_ERROR_GIT, "%s is not a git repository", dir);
    return FALSE;
  }

  GError *local = NULL;
  g_autofree gchar *remotes_raw =
    run_git(dir, &local, "config", "--get-regexp", "^remote\\..*\\.url$", NULL);
  if (remotes_raw == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_GIT, "cannot run git: %s",
                local ? local->message : "unknown");
    g_clear_error(&local);
    return FALSE;
  }
  g_autoptr(GStrvBuilder) rb = g_strv_builder_new();
  g_auto(GStrv) lines = g_strsplit(remotes_raw, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    const gchar *sp = strchr(lines[i], ' ');
    if (sp != NULL && sp[1] != '\0')
      g_strv_builder_add(rb, sp + 1);
  }
  g_auto(GStrv) remotes = g_strv_builder_end(rb);

  g_autoptr(GPtrArray) clone = g_ptr_array_new_with_free_func(g_free);
  g_autoptr(GPtrArray) web = g_ptr_array_new_with_free_func(g_free);
  ns_git_public_urls((const gchar *const *)remotes, clone, web);
  if (remotes[0] == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_GIT,
                "%s has no git remote; push it to a public host and "
                "`git remote add origin <url>` so others can clone it", dir);
    return FALSE;
  }
  if (clone->len == 0) {
    g_set_error(error, NS_ERROR, NS_ERROR_GIT,
                "%s only has local remotes (paths / file://), which are not "
                "published; add a public https://, ssh:// or git@host:path "
                "remote", dir);
    return FALSE;
  }
  g_ptr_array_add(clone, NULL);
  g_ptr_array_add(web, NULL);

  g_autofree gchar *roots = run_git(dir, NULL, "rev-list", "--max-parents=0", "HEAD", NULL);
  g_autofree gchar *euc = NULL;
  if (roots != NULL) {
    g_auto(GStrv) rl = g_strsplit(g_strstrip(roots), "\n", -1);
    guint n = g_strv_length(rl);
    if (n > 0 && strlen(rl[n - 1]) == 40)
      euc = g_strdup(rl[n - 1]);   /* oldest root */
  }

  g_autofree gchar *abs = g_canonicalize_filename(dir, NULL);
  g_autofree gchar *name = g_path_get_basename(abs);
  if (g_str_has_suffix(name, ".git"))
    name[strlen(name) - 4] = '\0';
  g_autofree gchar *repo_id = ns_slugify(name);

  g_autofree gchar *desc = NULL;
  g_autofree gchar *desc_path = g_build_filename(abs, ".git", "description", NULL);
  if (!g_file_get_contents(desc_path, &desc, NULL, NULL)) {
    g_free(desc_path);
    desc_path = g_build_filename(abs, "description", NULL);
    (void)g_file_get_contents(desc_path, &desc, NULL, NULL);
  }
  if (desc != NULL) {
    g_strstrip(desc);
    if (*desc == '\0' || g_str_has_prefix(desc, "Unnamed repository"))
      g_clear_pointer(&desc, g_free);
  }

  NostrEvent *ev = nip34_create_repo_announcement(
    repo_id, name, desc, (const char *const *)clone->pdata,
    web->len > 1 ? (const char *const *)web->pdata : NULL,
    relays, NULL);
  if (ev == NULL) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_GIT,
                        "nip34_create_repo_announcement failed");
    return FALSE;
  }
  char *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);

  g_autoptr(JsonParser) parser = json_parser_new();
  gboolean ok = json != NULL && json_parser_load_from_data(parser, json, -1, NULL);
  free(json);
  JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_GIT,
                        "cannot serialise NIP-34 announcement");
    return FALSE;
  }
  JsonObject *obj = json_node_get_object(root);
  JsonNode *tn = json_object_get_member(obj, "tags");
  JsonArray *tags = (tn && JSON_NODE_HOLDS_ARRAY(tn))
    ? json_array_ref(json_node_get_array(tn)) : json_array_new();
  if (euc != NULL)
    ns_tags_add(tags, "r", euc, "euc", NULL);

  *out_tags = tags;
  *out_content = g_strdup(json_object_get_string_member_with_default(obj, "content", ""));
  if (out_repo_id) *out_repo_id = g_steal_pointer(&repo_id);
  return TRUE;
}
