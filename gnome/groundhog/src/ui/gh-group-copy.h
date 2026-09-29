#ifndef GH_GROUP_COPY_H
#define GH_GROUP_COPY_H

#include "gh-nip29-service.h"

G_BEGIN_DECLS

/*
 * The NIP-29 relay-group UI's words and decisions (privacy charter §7.5,
 * §7.7, §7.9, §7.10, §7.15 #18; item G20b), GTK-free so they are tested
 * without a display against the local NIP-29 relay. The dialogs
 * (gh-group-join-dialog, gh-group-info-dialog, gh-new-group-dialog) and the
 * composer glue (gh-group-ui.c) show exactly these strings.
 *
 * Honesty rules (P4): a relay group is never called encrypted; its relay's
 * operators can read every message; "joined" is only said on the relay's
 * own evidence; a member list is never "complete" or "0 members" (relays
 * may publish a subset: "Member list may be incomplete"); a relay's refusal
 * is quoted as the relay's words, never as Groundhog's judgement.
 * Relay-supplied text is plain text for the caller (never markup).
 */

/* Parses what a user pastes to join a group: "host'group-id" (NIP-29's
 * group identifier; a bare host means wss://), "wss://host'group-id", a
 * NIP-29 naddr (optionally "nostr:"), each with an optional "?invite=CODE".
 * The relay URL is normalized (gh_nip29_normalize_relay_url()) and must be
 * wss://, or ws:// on a loopback address (the test relays): Groundhog never
 * talks to a group relay in plaintext. invite is NULL when there is none.
 * Errors are G_IO_ERROR_INVALID_ARGUMENT with a translated sentence for the
 * user. Contacts nothing. */
gboolean gh_group_parse_reference(const gchar *text, gchar **out_relay_url,
                                  gchar **out_group_id, gchar **out_invite_code,
                                  GError **error);

/* Normalizes a relay the user typed (a bare host means wss://; the same
 * rules as above). */
gchar *gh_group_parse_relay(const gchar *text, GError **error);

/* The relay's host for people ("groups.example.com", with a port that is
 * not the default); the URL itself when it has no host. */
gchar *gh_group_relay_host(const gchar *relay_url);

/* The group's shareable address: "host'group-id" for a wss:// relay on its
 * default path (what gh_group_parse_reference() reads back), else
 * "<relay URL>'group-id"; with "?invite=CODE" when invite_code is set. */
gchar *gh_group_format_address(const gchar *relay_url, const gchar *group_id,
                               const gchar *invite_code);

/* A relay's OK/CLOSED message as shown to people: the machine prefix
 * ("restricted: ", "blocked: ", ...) dropped, the rest trimmed; NULL when
 * nothing is left. */
gchar *gh_group_relay_reason(const gchar *message);

typedef enum {
  GH_GROUP_TONE_NEUTRAL,
  GH_GROUP_TONE_PROGRESS, /* waiting for the signer or the relay */
  GH_GROUP_TONE_SUCCESS,
  GH_GROUP_TONE_WARNING,
  GH_GROUP_TONE_ERROR
} GhGroupTone;

typedef struct {
  GhGroupTone tone;
  const gchar *icon_name; /* symbolic */
  gchar *title;
  gchar *description;     /* with the relay's reason, quoted, when it gave one */
  gboolean can_retry;     /* offer to send the request again */
  gboolean needs_code;    /* offer an invite code (a closed group) */
  gboolean can_open;      /* the conversation is there to open */
} GhGroupStateCopy;

void gh_group_state_copy_free(GhGroupStateCopy *copy);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhGroupStateCopy, gh_group_state_copy_free)

/* What a join means now (charter §7.10 "join states have distinct copy":
 * pending review, denied, already a member, closed), for the Join dialog and
 * the Membership row of Group Info. request is the room's last request
 * (gh_nip29_room_dup_request_op(); nullable): its WAITING_FOR_SIGNER says
 * so, and DUPLICATE (or already_member: the room was MEMBER before the user
 * asked) is "already a member". detail is the room's (the relay's reason);
 * host the relay's (gh_group_relay_host()). */
GhGroupStateCopy *gh_group_join_copy(GhNip29JoinState join, GhNip29Op *request,
                                     gboolean already_member, const gchar *detail,
                                     const gchar *host);

/* The New Group result (charter §7.9 "shown as pending until the
 * authoritative 39000 arrives"): CREATING, then MEMBER (created; has_state:
 * the relay's 39000 arrived), else not created, with the relay's reason. */
GhGroupStateCopy *gh_group_create_copy(GhNip29JoinState join, GhNip29Op *request,
                                       gboolean has_state, const gchar *detail,
                                       const gchar *host);

/* Why the composer can't send into this group (charter §7.7 "relay group
 * closed to you"), or NULL when it can: MEMBER sends; a request still being
 * answered or held for review sends only to a group that is not restricted
 * (the relay decides); a denied, closed, removed or left account is told why
 * the group is closed to it. */
gchar *gh_group_send_reason(GhNip29JoinState join, gboolean restricted);

/* gh_group_send_reason() for the group whose conversation is room_id, or
 * why there is nothing to send to (no service: groups unavailable; not in
 * the service: the group is not in the list). */
gchar *gh_group_room_send_reason(GhNip29Service *service, const gchar *room_id);

/* The outbox operation of the account's own group message event_id (a chat
 * message this device queued), or NULL. Transfer full. */
GhNip29Op *gh_group_find_message_op(GhNip29Service *service, const gchar *event_id);

/* What the group's relay said about op, as one plain sentence for the
 * delivery details ("The relay accepted it", the refusal quoted, ...). */
gchar *gh_group_op_outcome(GhNip29Op *op);

/* How an admin action shows (charter §7.10): ALLOWED (enabled), HIDDEN
 * (DENIED_*, no relay-signed admin list, or not a member) or RELAY_DECIDES
 * (listed as an admin whose role's rights are unknown: enabled, with the
 * footer "The relay decides whether you're allowed"). */
typedef enum {
  GH_GROUP_GATE_HIDDEN,
  GH_GROUP_GATE_ALLOWED,
  GH_GROUP_GATE_RELAY_DECIDES
} GhGroupGate;

GhGroupGate gh_group_gate(GhNip29JoinState join, GhNip29Authz authz);
/* gh_group_gate() for the room's account and permission. */
GhGroupGate gh_group_room_gate(GhNip29Room *room, nostr_permission_t permission);

/* The Messages row of Group Info: how reading the group goes. */
const gchar *gh_group_read_copy(GhNip29ReadState read);
/* The Group Details row: whether its state is verified by the relay's key. */
const gchar *gh_group_key_copy(GhNip29RelayKeyState state);
/* The Members group's description: always says the list may be incomplete
 * (never a count as if complete; nothing at all while unavailable). */
const gchar *gh_group_members_copy(GhNip29MemberList members);

G_END_DECLS
#endif
