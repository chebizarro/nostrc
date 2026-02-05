/*
 * nip34.h - NIP-34 Git Repository Event Utilities
 *
 * NIP-34 defines event kinds for git-related activities:
 * - 30617: Repository announcements (addressable)
 * - 1617: Patches
 * - 1621: Issues
 * - 1622: Issue/patch replies
 *
 * Repository announcements (kind 30617) contain:
 * - "d" - unique repository identifier
 * - "name" - repository name
 * - "description" - repository description
 * - "clone" - git clone URL(s)
 * - "web" - web URL(s) for browsing
 * - "relays" - recommended relays for this repo
 * - "maintainers" - list of maintainer pubkeys
 * - "r" - references (e.g., HEAD commit)
 * - "t" - topics/tags
 */

#ifndef NIP34_H
#define NIP34_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind numbers for NIP-34 events */
#define NOSTR_KIND_GIT_REPO     30617
#define NOSTR_KIND_GIT_PATCH    1617
#define NOSTR_KIND_GIT_ISSUE    1621
#define NOSTR_KIND_GIT_REPLY    1622

/*
 * GnostrRepoMeta:
 * Structure containing parsed NIP-34 repository metadata.
 * All strings are owned by the structure and freed with gnostr_repo_meta_free().
 */
typedef struct {
  gchar *d_tag;              /* Unique identifier (required) */
  gchar *name;               /* Repository name */
  gchar *description;        /* Repository description */
  gchar **clone_urls;        /* NULL-terminated array of git clone URLs */
  gsize clone_urls_count;
  gchar **web_urls;          /* NULL-terminated array of web URLs */
  gsize web_urls_count;
  gchar **maintainers;       /* NULL-terminated array of maintainer pubkeys */
  gsize maintainers_count;
  gchar **relays;            /* NULL-terminated array of relay URLs */
  gsize relays_count;
  gchar **topics;            /* NULL-terminated array of topics/tags */
  gsize topics_count;
  gchar *head_commit;        /* HEAD commit reference (from "r" tag) */
  gchar *license;            /* License identifier */
} GnostrRepoMeta;

/*
 * GnostrPatchMeta:
 * Structure containing parsed NIP-34 patch metadata.
 */
typedef struct {
  gchar *title;              /* Patch title (from subject line) */
  gchar *description;        /* Patch description */
  gchar *repo_a_tag;         /* Reference to repository ("a" tag) */
  gchar *commit_id;          /* Commit ID this patch applies to */
  gchar *parent_commit;      /* Parent commit */
  gchar **hashtags;          /* NULL-terminated array of hashtags */
  gsize hashtags_count;
} GnostrPatchMeta;

/*
 * GnostrIssueMeta:
 * Structure containing parsed NIP-34 issue metadata.
 */
typedef struct {
  gchar *title;              /* Issue title */
  gchar *repo_a_tag;         /* Reference to repository ("a" tag) */
  gchar **labels;            /* NULL-terminated array of labels */
  gsize labels_count;
  gboolean is_open;          /* Issue status (open/closed) */
} GnostrIssueMeta;

/* Repository metadata functions */
GnostrRepoMeta *gnostr_repo_meta_new(void);
void gnostr_repo_meta_free(GnostrRepoMeta *meta);
GnostrRepoMeta *gnostr_repo_parse_tags(const char *tags_json);

/* Patch metadata functions */
GnostrPatchMeta *gnostr_patch_meta_new(void);
void gnostr_patch_meta_free(GnostrPatchMeta *meta);
GnostrPatchMeta *gnostr_patch_parse_tags(const char *tags_json, const char *content);

/* Issue metadata functions */
GnostrIssueMeta *gnostr_issue_meta_new(void);
void gnostr_issue_meta_free(GnostrIssueMeta *meta);
GnostrIssueMeta *gnostr_issue_parse_tags(const char *tags_json, const char *content);

/* Kind detection helpers */
gboolean gnostr_nip34_is_repo(int kind);
gboolean gnostr_nip34_is_patch(int kind);
gboolean gnostr_nip34_is_issue(int kind);
gboolean gnostr_nip34_is_reply(int kind);
gboolean gnostr_nip34_is_git_event(int kind);

G_END_DECLS

#endif /* NIP34_H */
