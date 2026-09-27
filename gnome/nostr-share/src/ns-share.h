/* ns-share.h - nostr-share engine: inputs → plan → events → publish
 *
 * SPDX-License-Identifier: MIT
 *
 * Phases (each synchronous; the dialog runs the network ones in a
 * worker thread):
 *
 *   ns_share_new()        read + classify inputs, strip media metadata,
 *                         hash, group into posts (no network)
 *   ns_share_connect()    org.nostr.Signer → pubkey
 *   ns_share_resolve()    write relays (10002 / config / session relay)
 *                         and Blossom servers (10063 / config)
 *   ns_share_build()      unsigned event JSON per post (pure); media
 *                         URLs are predicted until uploaded
 *   ns_share_sign()       sign every post (used by --dry-run)
 *   ns_share_upload()     Blossom uploads (URLs may change → rebuild)
 *   ns_share_publish()    sign → publish / DAV PUT
 */
#ifndef NS_SHARE_H
#define NS_SHARE_H

#include <glib.h>

#include "ns-config.h"
#include "ns-event.h"
#include "ns-kind.h"
#include "ns-net.h"

G_BEGIN_DECLS

typedef struct {
  gchar        *path;           /* nullable for in-memory text */
  gchar        *display_name;
  NsInputClass  cls;
  GBytes       *bytes;          /* what would be uploaded / staged */
  gboolean      stripped;       /* container metadata removed */
  guint         n_meta_removed;
  gboolean      metadata_unverified;   /* media we could not clean */
  NsBlobMeta    blob;
  gboolean      uploaded;
} NsFile;

typedef struct {
  NsAction      action;
  NsInputClass  cls;
  GPtrArray    *files;          /* NsFile* */
  gchar        *text;           /* note/article body, or media caption */
  gchar        *title;          /* article title */
  gchar        *git_dir;
  gchar        *unsigned_json;  /* compact, what the signer receives */
  gchar        *signed_json;
  gchar        *result;         /* one-line outcome after publish */
} NsPost;

typedef struct {
  gint          forced_kind;
  const gchar  *to;
  const gchar  *title;
  gboolean      keep_metadata;
  gboolean      allow_empty;    /* dialog: start with an empty note */
  const gchar *const *texts;    /* -t TEXT (joined with blank lines) */
  const gchar *const *args;     /* FILE… | URL… | file:// URIs */
} NsShareOptions;

typedef struct {
  NsConfig           *cfg;
  NsNet               net;
  NsRecipient         to;
  gboolean            keep_metadata;
  gint64              created_at;
  gint                forced_kind;
  gboolean            allow_empty;

  /* Inputs, read once. */
  GPtrArray          *files;          /* NsFile* (owned) */
  gchar              *text;           /* free text: -t, or a lone text file */
  gchar              *text_path;      /* that lone text file, if any */
  gchar              *text_path_body; /* its content as read (to tell edits apart) */
  NsInputClass        text_class;     /* TEXT / MARKDOWN / URL */
  gchar              *title;          /* --title or derived */
  GPtrArray          *urls;           /* URL arguments */

  GPtrArray          *posts;          /* NsPost*, rebuilt by replan */

  NostrPublishSigner *signer;
  gchar              *pubkey_hex;
  gchar             **servers;
  gchar              *servers_source;
  NsTargets           targets;
  gboolean            resolved;
} NsShare;

/* Takes ownership of @cfg. */
NsShare *ns_share_new(NsConfig *cfg, const NsShareOptions *opts, GError **error);
void     ns_share_free(NsShare *share);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsShare, ns_share_free)

gboolean ns_share_needs_upload(const NsShare *share);
gboolean ns_share_needs_relays(const NsShare *share);

/* TRUE when some media could not be cleaned and the user has not
 * acknowledged it (keep_metadata). @why gets a human explanation. */
gboolean ns_share_metadata_blocked(const NsShare *share, GString *why);

/* Re-plan with a different kind (dialog picker; 0 = defaults) or free
 * text (dialog content field). Validated like --kind. */
gboolean ns_share_set_kind(NsShare *share, gint kind, GError **error);
gboolean ns_share_set_text(NsShare *share, const gchar *text, GError **error);

/* Primary post's class/kind (what the dialog's picker is about). */
NsInputClass ns_share_primary_class(const NsShare *share);
gint         ns_share_primary_kind(const NsShare *share);

gboolean ns_share_connect(NsShare *share, GError **error);
gboolean ns_share_resolve(NsShare *share, GError **error);
gboolean ns_share_build(NsShare *share, GError **error);
gboolean ns_share_sign(NsShare *share, GError **error);

/* All posts' event JSON (pretty), one per post, for display / --dry-run. */
gchar *ns_share_preview_json(const NsShare *share, gboolean signed_if_available);

typedef void (*NsProgressFunc)(const gchar *message, gpointer user_data);

/* Upload every not-yet-uploaded blob. *@urls_changed is set when a
 * server returned a URL different from the predicted one, i.e. the
 * event JSON the user reviewed is about to change. */
gboolean ns_share_upload(NsShare *share, gboolean *urls_changed,
                         NsProgressFunc progress, gpointer user_data,
                         GError **error);

/* Sign + publish every post (DAV posts are PUT to nostr-dav). Call after
 * ns_share_upload() + ns_share_build(). */
gboolean ns_share_publish(NsShare *share, NsProgressFunc progress,
                          gpointer user_data, GError **error);

/* After a successful publish, each local source (file, or git repository
 * directory) is tagged with the event id in the extended attribute
 * user.nostr.event (GIO "xattr::nostr.event"), which nostr-nautilus reads
 * for its "published" emblem. Best effort: FALSE (silently) where the
 * filesystem has no user xattrs or the file is not writable. */
#define NS_XATTR_EVENT "xattr::nostr.event"
gboolean ns_share_mark_published(const gchar *path, const gchar *event_id_hex);

/* Human-readable summaries for the CLI and dialog. */
gchar *ns_share_describe_targets(const NsShare *share);
gchar *ns_share_describe_servers(const NsShare *share);

G_END_DECLS

#endif /* NS_SHARE_H */
