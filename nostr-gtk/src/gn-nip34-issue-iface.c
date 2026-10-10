/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <nostr-gtk-1.0/gn-nip34-issue-iface.h>

G_DEFINE_QUARK(gn-issue-publish-error-quark, gn_issue_publish_error)

G_DEFINE_INTERFACE(GnIssuePublisher, gn_issue_publisher, G_TYPE_OBJECT)
G_DEFINE_INTERFACE(GnIssueUploader, gn_issue_uploader, G_TYPE_OBJECT)
G_DEFINE_INTERFACE(GnIssueRepoResolver, gn_issue_repo_resolver, G_TYPE_OBJECT)

static void gn_issue_publisher_default_init(GnIssuePublisherInterface *iface) { (void)iface; }
static void gn_issue_uploader_default_init(GnIssueUploaderInterface *iface) { (void)iface; }
static void gn_issue_repo_resolver_default_init(GnIssueRepoResolverInterface *iface) { (void)iface; }

void
gn_issue_publisher_sign_and_publish_async(GnIssuePublisher *self, const gchar *unsigned_json,
                                          const gchar *const *relays, GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GN_IS_ISSUE_PUBLISHER(self));
  g_return_if_fail(unsigned_json != NULL);
  GN_ISSUE_PUBLISHER_GET_IFACE(self)->sign_and_publish_async(self, unsigned_json, relays,
                                                             cancellable, callback, user_data);
}

gboolean
gn_issue_publisher_sign_and_publish_finish(GnIssuePublisher *self, GAsyncResult *result,
                                           gchar **event_id, gchar **message, GError **error)
{
  g_return_val_if_fail(GN_IS_ISSUE_PUBLISHER(self), FALSE);
  if (event_id)
    *event_id = NULL;
  if (message)
    *message = NULL;
  return GN_ISSUE_PUBLISHER_GET_IFACE(self)->sign_and_publish_finish(self, result, event_id,
                                                                     message, error);
}

gchar *
gn_issue_publisher_normalize_relay(GnIssuePublisher *self, const gchar *url, GError **error)
{
  g_return_val_if_fail(GN_IS_ISSUE_PUBLISHER(self), NULL);
  GnIssuePublisherInterface *iface = GN_ISSUE_PUBLISHER_GET_IFACE(self);
  if (iface->normalize_relay)
    return iface->normalize_relay(self, url, error);
  return gn_issue_normalize_relay_url(url, error);
}

const gchar *
gn_issue_uploader_describe_destination(GnIssueUploader *self)
{
  g_return_val_if_fail(GN_IS_ISSUE_UPLOADER(self), NULL);
  return GN_ISSUE_UPLOADER_GET_IFACE(self)->describe_destination(self);
}

void
gn_issue_uploader_upload_async(GnIssueUploader *self, GFile *file, const gchar *content_type,
                               GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_return_if_fail(GN_IS_ISSUE_UPLOADER(self));
  g_return_if_fail(G_IS_FILE(file));
  GN_ISSUE_UPLOADER_GET_IFACE(self)->upload_async(self, file, content_type, cancellable,
                                                  callback, user_data);
}

gchar *
gn_issue_uploader_upload_finish(GnIssueUploader *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(GN_IS_ISSUE_UPLOADER(self), NULL);
  return GN_ISSUE_UPLOADER_GET_IFACE(self)->upload_finish(self, result, error);
}

void
gn_issue_repo_resolver_resolve_async(GnIssueRepoResolver *self, const GnIssueTarget *hint,
                                     GCancellable *cancellable, GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail(GN_IS_ISSUE_REPO_RESOLVER(self));
  g_return_if_fail(hint != NULL);
  GN_ISSUE_REPO_RESOLVER_GET_IFACE(self)->resolve_async(self, hint, cancellable, callback,
                                                        user_data);
}

GnIssueTarget *
gn_issue_repo_resolver_resolve_finish(GnIssueRepoResolver *self, GAsyncResult *result,
                                      GError **error)
{
  g_return_val_if_fail(GN_IS_ISSUE_REPO_RESOLVER(self), NULL);
  return GN_ISSUE_REPO_RESOLVER_GET_IFACE(self)->resolve_finish(self, result, error);
}
