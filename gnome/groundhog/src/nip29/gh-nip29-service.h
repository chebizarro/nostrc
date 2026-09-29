#ifndef GH_NIP29_SERVICE_H
#define GH_NIP29_SERVICE_H

#include "gh-conversation-store.h"
#include "gh-nip29-group.h"
#include "gh-nip29-outbox.h"
#include "gh-relay-scope.h"

G_BEGIN_DECLS

/*
 * GhNip29Service: one account's NIP-29 relay groups (privacy charter §4.3,
 * §4.4, §7.10, §8.2 G20a; plan W5), GTK-free, over that account's open
 * GhStore. The UI (G20b) binds GhNip29Room objects; this layer has none.
 *
 * Identity. A group is its (normalized relay URL, group id) pair
 * (gh-nip29-group.h): the same id on two relays is two groups, two rooms and
 * two conversations (room id "<relay URL>\x1f<group id>"), and nothing one
 * relay says reaches the other.
 *
 * Relays. Per relay URL with subscribed groups, one live GhRelayScope (its
 * own connection) with one REQ for all of that relay's groups: the
 * relay-signed 39000-39003 of each (#d), each group's messages, kinds 9-12
 * (#h; since the group's sync cursor minus 10 minutes, or its newest 200 the
 * first time), and the 9000/9001 moderation events naming the account
 * (#h, #p). EOSE marks the backfill complete (the group's first one marks the
 * joined history read). NIP-42: GhAuthPolicy purpose GROUP, so a relay that
 * demands AUTH (a private group) gets one signed as the account, on
 * challenge only (§4.4 R1, R6). A relay's scope is rebuilt when its groups
 * change; nothing is opened for a group that is not joined or being joined,
 * and no relay but the group's is ever contacted for it.
 *
 * Trust. Group state is only what the relay's own key signed: the key comes
 * from the relay's NIP-11 document ("self", legacy "pubkey"; gh-nip11.h
 * explains why not trust-on-first-use), is pinned with the group, and is
 * fetched again (once per session) only when a snapshot arrives signed by
 * another key; a changed key rebuilds the group state. Until the key is known
 * snapshots are held back, not admitted. Snapshots dated more than 10 minutes
 * ahead are refused (qp24.12.2). A 9000/9001 about the account counts only
 * when the relay's key or a listed admin signed it; the latest one decides
 * (NIP-29 "checking your own membership").
 *
 * Honest join states (GhNip29JoinState): a join request (9021) is REQUESTING
 * while it is signed and sent; the relay's answer makes it MEMBER (OK true, a
 * "duplicate:" already-a-member answer, a 9000 for the account, or a 39002
 * that lists it), PENDING (the relay holds it for review; a later 9000 or
 * 39002 listing admits it), DENIED (refused for good, with the relay's
 * reason), CLOSED (refused by a closed group, which needs an invite code) or
 * NOT_SENT (the signer declined, sign-in failed, or the relay stayed
 * unreachable). A 39002 that does not list the account never means "not a
 * member": relays may publish a subset (GH_NIP29_MEMBERS_PARTIAL, "Member
 * list may be incomplete"). Leaving (9022) is LEAVING, then LEFT; a 9001 from
 * an admin is REMOVED.
 *
 * Writes. Chat messages, join/leave requests and admin operations (9000,
 * 9001, 9002, 9005, 9009) are GhNip29Op of the durable GhNip29Outbox: stored
 * before the signer is asked, signed by the account signer, stored signed,
 * published to exactly the group's relay, retried on transient failures, and
 * reported with the relay's own outcome (gh-nip29-outbox.h). Each carries a
 * `previous` tag from the group's timeline ring (gh-nip29-template.h). Admin
 * operations are refused locally only when gh_nip29_group_check_permission()
 * says DENIED_*; UNKNOWN ones are attempted and the relay decides (§7.10).
 * Nothing changes locally on an admin operation: the relay's next snapshot
 * is the truth.
 *
 * Messages. Group events are admitted into the GhConversationStore as NIP-29
 * rooms (gh-conversation.h) through the encrypted store's NIP-29 delegate
 * (gh-store-nip29.h): T-admit, dedup on the event id, restore on restart. A
 * sent chat message is listed at once (its local echo) with its honest
 * GhMessageStatus. Joined groups are listed even before their first message.
 *
 * Persistence. Every group, its join state, the last admitted snapshots, the
 * timeline ring and the sync cursor are kept in nip29_groups (encrypted); a
 * restart restores them, resubscribes and resumes queued operations.
 *
 * Generation. The service runs only while its store's account is the active
 * account: a switch cancels every subscription, signature and publish at
 * once and nothing of the old generation is recorded afterwards; reselecting
 * resumes. Main context only. The store and the model are borrowed: dispose
 * the service before closing the store.
 */

typedef enum {
  GH_NIP29_JOIN_NONE,        /* not joined, never asked */
  GH_NIP29_JOIN_REQUESTING,  /* the join request is being signed or sent */
  GH_NIP29_JOIN_PENDING,     /* the relay holds the request for an admin */
  GH_NIP29_JOIN_MEMBER,      /* the relay admitted the account */
  GH_NIP29_JOIN_DENIED,      /* the relay refused the request for good */
  GH_NIP29_JOIN_CLOSED,      /* refused by a closed group: an invite code is needed */
  GH_NIP29_JOIN_NOT_SENT,    /* the request could not be sent; it can be retried */
  GH_NIP29_JOIN_LEAVING,     /* the leave request is being signed or sent */
  GH_NIP29_JOIN_LEFT,        /* the relay accepted the leave request */
  GH_NIP29_JOIN_REMOVED      /* a 9001 removed the account */
} GhNip29JoinState;

GType gh_nip29_join_state_get_type(void);
#define GH_TYPE_NIP29_JOIN_STATE (gh_nip29_join_state_get_type())

typedef enum {
  GH_NIP29_READ_IDLE,          /* not subscribed (not joined, inactive or offline) */
  GH_NIP29_READ_SYNCING,       /* subscribed; history until the relay's EOSE */
  GH_NIP29_READ_LIVE,          /* EOSE received: live */
  GH_NIP29_READ_AUTH_REQUIRED, /* the relay wants a sign-in that did not happen */
  GH_NIP29_READ_REFUSED,       /* the relay closed the subscription; see detail */
  GH_NIP29_READ_DISCONNECTED   /* the connection dropped; it reconnects */
} GhNip29ReadState;

GType gh_nip29_read_state_get_type(void);
#define GH_TYPE_NIP29_READ_STATE (gh_nip29_read_state_get_type())

typedef enum {
  GH_NIP29_RELAY_KEY_UNKNOWN,     /* not fetched yet: group state is held back */
  GH_NIP29_RELAY_KEY_PINNED,      /* the relay's NIP-11 key verifies group state */
  GH_NIP29_RELAY_KEY_UNAVAILABLE  /* no NIP-11 key: group state cannot be verified */
} GhNip29RelayKeyState;

GType gh_nip29_relay_key_state_get_type(void);
#define GH_TYPE_NIP29_RELAY_KEY_STATE (gh_nip29_relay_key_state_get_type())

/* At most this many groups are subscribed per relay (one REQ). */
#define GH_NIP29_SERVICE_MAX_GROUPS_PER_RELAY 16
/* Messages asked for the first time a group is subscribed. */
#define GH_NIP29_SERVICE_INITIAL_HISTORY 200
/* The overlap subtracted from a group's sync cursor. */
#define GH_NIP29_SERVICE_CURSOR_OVERLAP 600

#define GH_TYPE_NIP29_ROOM (gh_nip29_room_get_type())
G_DECLARE_FINAL_TYPE(GhNip29Room, gh_nip29_room, GH, NIP29_ROOM, GObject)

/* One group of the account. Read-only properties, notified on change:
 * "relay-url", "group-id", "room-id" (its conversation's), "name" (the
 * relay-signed name, or NULL), "join-state", "read-state",
 * "relay-key-state", "members-state" (a GhNip29MemberList: UNAVAILABLE
 * without a 39002, PARTIAL with one; either way the UI says "Member list may
 * be incomplete", never "0 members"), "is-closed", "is-private",
 * "is-restricted" (from the admitted 39000) and "detail" (the relay's reason
 * for the last refusal or closed subscription, or a local reason). */
const gchar *gh_nip29_room_get_relay_url(GhNip29Room *self);
const gchar *gh_nip29_room_get_group_id(GhNip29Room *self);
const gchar *gh_nip29_room_get_room_id(GhNip29Room *self);
const gchar *gh_nip29_room_get_name(GhNip29Room *self);
GhNip29JoinState gh_nip29_room_get_join_state(GhNip29Room *self);
GhNip29ReadState gh_nip29_room_get_read_state(GhNip29Room *self);
GhNip29RelayKeyState gh_nip29_room_get_relay_key_state(GhNip29Room *self);
GhNip29MemberList gh_nip29_room_get_members_state(GhNip29Room *self);
gboolean gh_nip29_room_get_is_closed(GhNip29Room *self);
gboolean gh_nip29_room_get_is_private(GhNip29Room *self);
gboolean gh_nip29_room_get_is_restricted(GhNip29Room *self);
const gchar *gh_nip29_room_get_detail(GhNip29Room *self);
/* The relay-verified group state (borrowed; NULL while the relay key is not
 * known), for members, admins, roles and metadata. */
const GhNip29Group *gh_nip29_room_get_group(GhNip29Room *self);
/* The account's authorization for permission (gh-nip29-group.h), with the
 * relay's role policy when one was set. UNKNOWN_NO_ADMINS while no 39001 is
 * known. */
GhNip29Authz gh_nip29_room_check_permission(GhNip29Room *self, nostr_permission_t permission);

typedef struct {
  GhStore *store;                      /* the account's open store; borrowed */
  GhAccountController *accounts;       /* signer, generation */
  GhConversationStore *conversations;  /* bound to the store's account; borrowed */
  GNetworkMonitor *network;            /* NULL: g_network_monitor_get_default() */
  /* NULL: gnostr relays with NIP-42 (the application). Tests pass their own
   * transports; each authenticates only with its auth half. */
  const GhRelayTransport *scope_transport;
  const GhRelayAuthTransport *scope_auth_transport;
  gpointer scope_transport_data;
  const GhRelayPublishTransport *publish_transport;
  const GhRelayPublishAuthTransport *publish_auth_transport;
  gpointer publish_transport_data;
  guint publish_deadline;              /* per-relay seconds; 0: the publish default */
  /* network-mode for the relays' NIP-11 documents (GhNetHttp, gh-nip11.h),
   * fetched only for groups the user joined or opened; NULL: "system". */
  GSettings *settings;
} GhNip29ServiceConfig;

#define GH_TYPE_NIP29_SERVICE (gh_nip29_service_get_type())
G_DECLARE_FINAL_TYPE(GhNip29Service, gh_nip29_service, GH, NIP29_SERVICE, GObject)

/* Restores the stored groups (and their rooms in the model), resumes queued
 * operations and subscribes once the account is active and online. The
 * service is a GListModel of GhNip29Room, oldest first. */
GhNip29Service *gh_nip29_service_new(const GhNip29ServiceConfig *config, GError **error);
GhNip29Outbox *gh_nip29_service_get_outbox(GhNip29Service *self);

/* The group (relay_url is normalized first), or NULL. Transfer full. */
GhNip29Room *gh_nip29_service_lookup(GhNip29Service *self, const gchar *relay_url,
                                     const gchar *group_id);
/* The room whose conversation is room_id (gh_conversation_get_room_id()). */
GhNip29Room *gh_nip29_service_lookup_room(GhNip29Service *self, const gchar *room_id);

/* Asks to join (kind 9021, with invite_code when non-empty): stores the
 * group, lists its room, subscribes to its relay and sends the request
 * through the outbox. A group that is already MEMBER, REQUESTING or PENDING
 * is returned as it is (nothing is sent). Errors: G_IO_ERROR_PERMISSION_DENIED
 * (the store's account is not active), GH_NIP29_ERROR (a bad relay URL or
 * group id), G_IO_ERROR_TOO_MANY_OPEN_FILES (the relay already has
 * GH_NIP29_SERVICE_MAX_GROUPS_PER_RELAY groups) or the store's. Transfer
 * full. */
GhNip29Room *gh_nip29_service_join(GhNip29Service *self, const gchar *relay_url,
                                   const gchar *group_id, const gchar *reason,
                                   const gchar *invite_code, GError **error);
/* Asks to leave (kind 9022). G_IO_ERROR_INVALID_ARGUMENT when not joined. */
GhNip29Op *gh_nip29_service_leave(GhNip29Service *self, GhNip29Room *room,
                                  const gchar *reason, GError **error);
/* Sends a chat message (kind 9); it is listed at once. Refused
 * (G_IO_ERROR_NOT_CONNECTED) unless the room is MEMBER, PENDING or
 * REQUESTING: the relay decides whether a non-member may write. */
GhNip29Op *gh_nip29_service_send(GhNip29Service *self, GhNip29Room *room,
                                 const gchar *text, GError **error);

/* Admin operations, refused with G_IO_ERROR_PERMISSION_DENIED only when the
 * account's authorization for them is DENIED_* (gh_nip29_room_check_permission). */
GhNip29Op *gh_nip29_service_put_user(GhNip29Service *self, GhNip29Room *room,
                                     const gchar *pubkey, const gchar *const *roles,
                                     const gchar *reason, GError **error);
GhNip29Op *gh_nip29_service_remove_user(GhNip29Service *self, GhNip29Room *room,
                                        const gchar *pubkey, const gchar *reason,
                                        GError **error);
/* metadata: start from gh_nip29_group_dup_metadata() (unknown tags survive). */
GhNip29Op *gh_nip29_service_edit_metadata(GhNip29Service *self, GhNip29Room *room,
                                          const GhNip29Metadata *metadata,
                                          const gchar *reason, GError **error);
GhNip29Op *gh_nip29_service_delete_event(GhNip29Service *self, GhNip29Room *room,
                                         const gchar *event_id, const gchar *reason,
                                         GError **error);
GhNip29Op *gh_nip29_service_create_invite(GhNip29Service *self, GhNip29Room *room,
                                          const gchar *code, const gchar *reason,
                                          GError **error);

/* The relay's role capabilities, when known (e.g. from its implementation);
 * NULL clears. Takes ownership of policy. */
void gh_nip29_service_set_role_policy(GhNip29Service *self, const gchar *relay_url,
                                      GhNip29RolePolicy *policy);

/* Lists up to limit (1 to 1000) older stored messages of a group room of the
 * model; see gh_store_nip29_load_older(). */
gboolean gh_nip29_service_load_older(GhNip29Service *self, GhConversation *conversation,
                                     guint limit, guint *out_loaded, GError **error);

/* Forgets a group that is not joined (NONE, DENIED, CLOSED, NOT_SENT, LEFT or
 * REMOVED): its record, its conversation (a tombstone keeps backfill out,
 * charter ST-9) and its room. G_IO_ERROR_INVALID_ARGUMENT otherwise. */
gboolean gh_nip29_service_forget(GhNip29Service *self, GhNip29Room *room, GError **error);

G_END_DECLS
#endif
