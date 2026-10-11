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
 * they are: an issue attachment is public, not encrypted. The upload is
 * authorized by a throwaway key unless the account consented to the server
 * (as for message attachments). Finishes with <server>/<sha256>.
 */

#define GH_TYPE_ISSUE_UPLOADER (gh_issue_uploader_get_type())
G_DECLARE_FINAL_TYPE(GhIssueUploader, gh_issue_uploader, GH, ISSUE_UPLOADER, GObject)

/* NULL when client is NULL or none of its servers has a public host: the
 * form then offers no upload. */
GnIssueUploader *gh_issue_uploader_new(GhBlossomClient *client);

/* The servers this uploader may contact, in order. */
const gchar *const *gh_issue_uploader_get_servers(GhIssueUploader *self);

G_END_DECLS
#endif
