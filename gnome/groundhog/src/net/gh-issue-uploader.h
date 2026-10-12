/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GH_ISSUE_UPLOADER_H
#define GH_ISSUE_UPLOADER_H

#include <nostr-gtk-1.0/gn-nip34-issue-iface.h>
#include "gh-blossom-client.h"

G_BEGIN_DECLS

/*
 * GhIssueUploader: Groundhog's GnIssueUploader for the public NIP-34 issue
 * form (nostrc-8xfib.5). The only uploader the form may get (privacy rule
 * "issue-services" in tests/check_privacy.py).
 *
 * Files go through GhBlossomClient, so through GhNetHttp: the network mode,
 * Tor, the https-only rule, no redirects. The servers are the user's own
 * attachment servers (blossom-servers, empty by default), snapshotted when
 * the form opens, so the consent text names exactly the servers that may be
 * contacted; Groundhog never substitutes a built-in server. Only servers
 * with a public host are kept (the address goes into a public issue), and
 * the connection is public-only. The form uploads only after the user
 * confirmed the consent that lists every file and this destination.
 *
 * Each file is read into memory (at most the client's cap, 25 MiB), JPEG
 * and PNG metadata is stripped (charter D6), and the bytes are uploaded as
 * they are: an issue attachment is public, not encrypted. Its type comes
 * from its bytes only (gh_issue_uploader_sniff(); never the file name or
 * the form's hint) and is sent as the Content-Type, so the server serves
 * it inline. Finishes with <server>/<sha256>.<ext>: the extension the
 * server's BUD-02 descriptor names, else the sniffed type's
 * (gh_blossom_client_upload_public_async()).
 *
 * The upload is authorized by a throwaway key unless the account consented
 * to the server (the per-server consent of message attachments,
 * nostrc-dnsc). For a server without it, while the client has an account
 * signer, the form's upload consent offers "upload as your account" for
 * that server (unchecked); checking it records the consent through the
 * recorder (gh_issue_uploader_set_consent_recorder(): the account's kept
 * consents) before the first upload.
 */

#define GH_TYPE_ISSUE_UPLOADER (gh_issue_uploader_get_type())
G_DECLARE_FINAL_TYPE(GhIssueUploader, gh_issue_uploader, GH, ISSUE_UPLOADER, GObject)

/* NULL when client is NULL or none of its servers has a public host: the
 * form then offers no upload. */
GnIssueUploader *gh_issue_uploader_new(GhBlossomClient *client);

/* The servers this uploader may contact, in order. */
const gchar *const *gh_issue_uploader_get_servers(GhIssueUploader *self);

/* Records the account's consent to upload to server (normalized) as itself;
 * FALSE with error if it could not be kept. */
typedef gboolean (*GhIssueConsentFunc)(gpointer user_data, const gchar *server, GError **error);
/* Without a recorder a checked option sets the consent on the client only,
 * for as long as it lives. */
void gh_issue_uploader_set_consent_recorder(GhIssueUploader *self, GhIssueConsentFunc func,
                                            gpointer user_data);

/* The type of a public issue file, from its first bytes only (at most 64
 * KiB are read), from an allowlist: image/jpeg, image/png, image/gif,
 * image/webp, video/mp4, video/webm, application/pdf, and text/plain for
 * UTF-8 text without control characters (so HTML or SVG is served as
 * text). Anything else is application/octet-stream without an extension.
 * out_extension (nullable) receives the type's extension or NULL. */
const gchar *gh_issue_uploader_sniff(GBytes *bytes, const gchar **out_extension);

G_END_DECLS
#endif
