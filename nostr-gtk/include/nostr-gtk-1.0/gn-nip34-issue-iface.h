/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host services injected into GnNip34IssueView (nostrc-8xfib.5). The library
 * never signs, uploads or fetches by itself: each app supplies these through
 * its own network stack (gnostr: pool/Blossom/signer; Groundhog: Grotto,
 * GhRelayPublish and GhNet). The view calls them only after user consent.
 */
#ifndef GN_NIP34_ISSUE_IFACE_H
#define GN_NIP34_ISSUE_IFACE_H

#include <gio/gio.h>
#include <nostr-gtk-1.0/gn-nip34-issue.h>

G_BEGIN_DECLS

/* Errors a GnIssuePublisher reports. NOT_SENT means nothing left the device,
 * so the view lets the user try again; any other error keeps the report
 * locked, because the signed event may already have reached a relay and a
 * retry would publish a duplicate issue. */
#define GN_ISSUE_PUBLISH_ERROR (gn_issue_publish_error_quark())
GQuark gn_issue_publish_error_quark(void);
typedef enum {
  GN_ISSUE_PUBLISH_ERROR_NOT_SENT,
  GN_ISSUE_PUBLISH_ERROR_UNCONFIRMED,
} GnIssuePublishError;

/* ---- Publisher (required) ---- */

#define GN_TYPE_ISSUE_PUBLISHER (gn_issue_publisher_get_type())
G_DECLARE_INTERFACE(GnIssuePublisher, gn_issue_publisher, GN, ISSUE_PUBLISHER, GObject)

struct _GnIssuePublisherInterface {
  GTypeInterface parent_iface;
  /* Sign exactly unsigned_json (the reviewed event) and publish it to relays. */
  void (*sign_and_publish_async)(GnIssuePublisher *self, const gchar *unsigned_json,
                                 const gchar *const *relays, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data);
  /* event_id and message (a translated status line) are nullable outputs. */
  gboolean (*sign_and_publish_finish)(GnIssuePublisher *self, GAsyncResult *result,
                                      gchar **event_id, gchar **message, GError **error);
  /* Optional: the host's relay URL rule. Default gn_issue_normalize_relay_url(). */
  gchar *(*normalize_relay)(GnIssuePublisher *self, const gchar *url, GError **error);
  /*< private >*/
  gpointer padding[8];
};

void gn_issue_publisher_sign_and_publish_async(GnIssuePublisher *self, const gchar *unsigned_json,
                                               const gchar *const *relays,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback, gpointer user_data);
gboolean gn_issue_publisher_sign_and_publish_finish(GnIssuePublisher *self, GAsyncResult *result,
                                                    gchar **event_id, gchar **message,
                                                    GError **error);
gchar *gn_issue_publisher_normalize_relay(GnIssuePublisher *self, const gchar *url,
                                          GError **error);

/* ---- Uploader (optional) ---- */

#define GN_TYPE_ISSUE_UPLOADER (gn_issue_uploader_get_type())
G_DECLARE_INTERFACE(GnIssueUploader, gn_issue_uploader, GN, ISSUE_UPLOADER, GObject)

struct _GnIssueUploaderInterface {
  GTypeInterface parent_iface;
  /* Where files go, shown in the consent text (e.g. a Blossom server URL). */
  const gchar *(*describe_destination)(GnIssueUploader *self);
  void (*upload_async)(GnIssueUploader *self, GFile *file, const gchar *content_type,
                       GCancellable *cancellable, GAsyncReadyCallback callback,
                       gpointer user_data);
  /* Returns the public URL of the uploaded file. */
  gchar *(*upload_finish)(GnIssueUploader *self, GAsyncResult *result, GError **error);
  /*< private >*/
  gpointer padding[8];
};

const gchar *gn_issue_uploader_describe_destination(GnIssueUploader *self);
void gn_issue_uploader_upload_async(GnIssueUploader *self, GFile *file,
                                    const gchar *content_type, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data);
gchar *gn_issue_uploader_upload_finish(GnIssueUploader *self, GAsyncResult *result,
                                       GError **error);

/* ---- Repository resolver (optional) ---- */

#define GN_TYPE_ISSUE_REPO_RESOLVER (gn_issue_repo_resolver_get_type())
G_DECLARE_INTERFACE(GnIssueRepoResolver, gn_issue_repo_resolver, GN, ISSUE_REPO_RESOLVER, GObject)

struct _GnIssueRepoResolverInterface {
  GTypeInterface parent_iface;
  /* Look up the kind-30617 announcement for hint; see
   * gn_issue_target_from_announcements(). */
  void (*resolve_async)(GnIssueRepoResolver *self, const GnIssueTarget *hint,
                        GCancellable *cancellable, GAsyncReadyCallback callback,
                        gpointer user_data);
  /* NULL without error: no announcement found. */
  GnIssueTarget *(*resolve_finish)(GnIssueRepoResolver *self, GAsyncResult *result,
                                   GError **error);
  /*< private >*/
  gpointer padding[8];
};

void gn_issue_repo_resolver_resolve_async(GnIssueRepoResolver *self, const GnIssueTarget *hint,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data);
GnIssueTarget *gn_issue_repo_resolver_resolve_finish(GnIssueRepoResolver *self,
                                                     GAsyncResult *result, GError **error);

G_END_DECLS
#endif /* GN_NIP34_ISSUE_IFACE_H */
