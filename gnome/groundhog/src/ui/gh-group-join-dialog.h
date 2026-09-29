#ifndef GH_GROUP_JOIN_DIALOG_H
#define GH_GROUP_JOIN_DIALOG_H

#include <adwaita.h>
#include "gh-nip29-service.h"

G_BEGIN_DECLS

/*
 * GhGroupJoinDialog (data/ui/gh-group-join-dialog.blp; privacy charter §7.9,
 * §7.10, §7.15 #18; item G20b): joins a NIP-29 relay group by its address.
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
 *    announced (polite). Open Group emits "open-group" (GhNip29Room) and
 *    closes; an invite code or Try Again asks again.
 * Relay text is never markup.
 *
 * Actions (widget actions): join.join, join.open, join.retry.
 */
#define GH_TYPE_GROUP_JOIN_DIALOG (gh_group_join_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhGroupJoinDialog, gh_group_join_dialog, GH, GROUP_JOIN_DIALOG, AdwDialog)

GhGroupJoinDialog *gh_group_join_dialog_new(GhNip29Service *service);
/* Prefills the address (e.g. a link the user opened). */
void gh_group_join_dialog_set_address(GhGroupJoinDialog *self, const gchar *address);
/* The group asked for, once Join was chosen; NULL before. */
GhNip29Room *gh_group_join_dialog_get_room(GhGroupJoinDialog *self);
/* What the status page says now (tests). */
const gchar *gh_group_join_dialog_get_status_title(GhGroupJoinDialog *self);

G_END_DECLS
#endif
