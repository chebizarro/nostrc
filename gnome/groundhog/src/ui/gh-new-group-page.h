#ifndef GH_NEW_GROUP_PAGE_H
#define GH_NEW_GROUP_PAGE_H

#include <adwaita.h>
#include "gh-nip29-service.h"

G_BEGIN_DECLS

/*
 * GhNewGroupPage (data/ui/gh-new-group-page.blp; privacy charter §7.9;
 * item G20b): creates a NIP-29 relay group on a relay the user names (the
 * relay part of New Group), as a sidebar panel pushed over New Message
 * (gh_window_push_flow()). Its own navigation view holds the steps; Escape
 * goes one step back, and Back (new-group.close) or Escape on the first
 * step returns to New Message. The first field of the step showing takes
 * focus. The page leaves when the service goes. With an encrypted page added
 * (gh_new_group_page_add_encrypted_page(), qp24.13 part 2) it opens on the
 * chooser, "Encrypted Group" or "Relay Group", each saying what it protects;
 * without one it opens on the relay form and encrypted groups are not
 * mentioned. Create asks gh_nip29_service_create_group() (9007 through the
 * durable outbox, then the chosen name, about and flags as 9002 once the
 * relay created it) and shows the relay's answer (gh_group_create_copy()):
 * a relay that does not let people create groups says so, quoted. The
 * group is "created, waiting for its details" until the relay's own 39000
 * arrives. Nothing contacts the network before Create. Open Group emits
 * "open-group" (GhNip29Room), then "finished"; the encrypted page's own
 * "open-group" (after its handlers) is followed by "finished" too. The
 * owner closes the panels on "finished".
 *
 * Actions (widget actions): new-group.create, new-group.open, new-group.back,
 * new-group.choose-relay, new-group.choose-encrypted and new-group.close.
 */
#define GH_TYPE_NEW_GROUP_PAGE (gh_new_group_page_get_type())
G_DECLARE_FINAL_TYPE(GhNewGroupPage, gh_new_group_page, GH, NEW_GROUP_PAGE,
                     AdwNavigationPage)

GhNewGroupPage *gh_new_group_page_new(GhNip29Service *service);
/* Back: pops the page (and anything over it) from its navigation view. */
void gh_new_group_page_close(GhNewGroupPage *self);
/* The group asked for, once Create was chosen; NULL before. */
GhNip29Room *gh_new_group_page_get_room(GhNewGroupPage *self);
const gchar *gh_new_group_page_get_status_title(GhNewGroupPage *self);
/* The encrypted-group page (e.g. GhMlsNewGroupPage), once: the dialog then
 * opens on the chooser. page's tag becomes "encrypted"; its "open-group"
 * signal, when it has one, finishes New Group. */
void gh_new_group_page_add_encrypted_page(GhNewGroupPage *self, AdwNavigationPage *page);
AdwNavigationPage *gh_new_group_page_get_encrypted_page(GhNewGroupPage *self);

G_END_DECLS
#endif
