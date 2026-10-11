#ifndef GH_GROUP_JOIN_PAGE_H
#define GH_GROUP_JOIN_PAGE_H

#include <adwaita.h>
#include "gh-nip29-service.h"

G_BEGIN_DECLS

/*
 * GhGroupJoinPage (data/ui/gh-group-join-page.blp; privacy charter §7.9,
 * §7.10, §7.15 #18; item G20b): joins a NIP-29 relay group by its address,
 * as a sidebar panel pushed over New Message (gh_window_push_flow()). Its
 * own navigation view has "entry" and "status"; Back (join.close) or Escape
 * on "entry" returns to New Message, from "status" to "entry". The address
 * takes focus when the page shows; the page leaves when the service goes.
 *
 *  - Entry: "host'group-id", "wss://host'group-id" or a NIP-29 naddr /
 *    nostr: link, each with an optional "?invite=" code, or the code typed
 *    separately (gh_group_parse_reference(): wss:// only, or loopback ws://).
 *    Before anything is sent it says that the group is not end-to-end
 *    encrypted and that joining tells the relay who asks. Nothing contacts
 *    the network until Join (charter P1).
 *  - Join: gh_nip29_service_join() (the service stores the group, sends the
 *    join request through the durable outbox and subscribes to its relay),
 *    then the relay's answer in the words of gh_group_join_copy(): waiting
 *    for the signer, asking, waiting for an admin (pending review), you're in,
 *    already a member (a "duplicate:" answer, or MEMBER before the user
 *    asked), declined, invite only (closed), not sent. Each change is
 *    announced (polite). Open Group emits "open-group" (GhNip29Room; its
 *    owner opens the group and closes the panels); an invite code or Try
 *    Again asks again.
 * Relay text is never markup.
 *
 * Actions (widget actions): join.join, join.open, join.retry, join.close.
 */
#define GH_TYPE_GROUP_JOIN_PAGE (gh_group_join_page_get_type())
G_DECLARE_FINAL_TYPE(GhGroupJoinPage, gh_group_join_page, GH, GROUP_JOIN_PAGE,
                     AdwNavigationPage)

GhGroupJoinPage *gh_group_join_page_new(GhNip29Service *service);
/* Back: pops the page (and anything over it) from its navigation view. */
void gh_group_join_page_close(GhGroupJoinPage *self);
/* Prefills the address (e.g. a link the user opened). */
void gh_group_join_page_set_address(GhGroupJoinPage *self, const gchar *address);
/* The group asked for, once Join was chosen; NULL before. */
GhNip29Room *gh_group_join_page_get_room(GhGroupJoinPage *self);
/* What the status page says now (tests). */
const gchar *gh_group_join_page_get_status_title(GhGroupJoinPage *self);

G_END_DECLS
#endif
