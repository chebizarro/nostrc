/* ns-git.h - Describe a local git repository as a NIP-34 announcement
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NS_GIT_H
#define NS_GIT_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

gboolean ns_git_is_repo(const gchar *dir);

/* Pure: turn remote URLs into public clone/web URL lists. Local paths
 * and file:// remotes are dropped (they would leak the user's filesystem
 * layout and are useless to anyone else); credentials in https URLs are
 * stripped. */
void ns_git_public_urls(const gchar *const *remotes, GPtrArray *clone_out,
                        GPtrArray *web_out);

/* Build the kind-30617 tags + content for @dir via nips/nip34, adding
 * the ["r", <root-commit>, "euc"] tag. Runs `git` (read-only). */
gboolean ns_git_announcement(const gchar *dir,
                             const gchar *const *relays,
                             JsonArray  **out_tags,
                             gchar      **out_content,
                             gchar      **out_repo_id,
                             GError     **error);

G_END_DECLS

#endif /* NS_GIT_H */
