#ifndef GN_CONCORD_INVITE_DIALOG_H
#define GN_CONCORD_INVITE_DIALOG_H

#include <gtk/gtk.h>

#include "../gn-concord-community-service.h"

G_BEGIN_DECLS

/* The creator's side of CORD-05: mint a shareable link, see the ones already
 * live, copy one, retire one.
 *
 * The window shows what a creator may act on and nothing more. A link's
 * unlock token and its signer secret stay in the service's Invite List: the
 * URL is the only shareable artifact, and it is already the token's only
 * written-down form outside that list (CORD-05 §2, §4).
 */

#define GN_TYPE_CONCORD_INVITE_DIALOG (gn_concord_invite_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnConcordInviteDialog, gn_concord_invite_dialog, GN,
                     CONCORD_INVITE_DIALOG, GtkWindow)

GnConcordInviteDialog *gn_concord_invite_dialog_new(
    GnConcordCommunityService *service, const char *community_id);

G_END_DECLS
#endif
