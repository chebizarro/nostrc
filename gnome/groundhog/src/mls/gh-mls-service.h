#ifndef GH_MLS_SERVICE_H
#define GH_MLS_SERVICE_H

#include <marmot/marmot.h>
#if defined(__has_include)
#if __has_include(<marmot/marmot-version.h>)
#include <marmot/marmot-version.h>
#endif
#endif

#include "gh-account-relays.h"
#include "gh-conversation-store.h"
#include "gh-dm-inbox.h"
#include "gh-inbox-resolver.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhMlsService: one account's Marmot (MLS) encrypted groups (nostrc-qp24.13
 * part 1; privacy charter §2.2, §3.9 D5, §4.3, §4.4, §7.9-§7.10; Marmot
 * MIP-00..03 and transports/nostr.md). GTK-free, main context only, over the
 * account's open GhStore: libmarmot runs on GhStoreMarmot (gh-store-marmot.h),
 * so every piece of MLS state -- group secrets, sender ratchets, KeyPackage
 * private keys, pending Commits, the Welcome outbox -- lives only in the
 * encrypted store, in the same transactions as the messages and the outbox.
 * An ephemeral store is refused (KC-4: MLS is disabled without durable state).
 *
 * KeyPackages (MIP-00, kind 30443). While the account is active and online
 * the service keeps one KeyPackage of the account published: made by
 * libmarmot (its private init key stored first, in the store), signed by the
 * account's signer, and published to the account's own kind-10002 write
 * relays and kind-10050 inbox relays (charter §4.3 "own list publish":
 * GhAuthPolicy OWN_LIST_PUBLISH, account AUTH only on challenge). Every
 * KeyPackage reuses the account's addressable `d` slot, so a new one replaces
 * the old one on relays. It is rotated when it is older than the lifetime
 * (default GH_MLS_KEY_PACKAGE_LIFETIME) and after a Welcome consumed it.
 *
 * Invitations need consent (charter PD-8, PT-8). A KeyPackage is looked up
 * (gh-mls-key-packages.h: discovery relays, then the person's 10002 write
 * relays; ephemeral AUTH only) only for someone the account accepted: a
 * peer of an accepted, non-request NIP-17 conversation, or whoever
 * GhMlsServiceConfig.may_look_up allows. Never for a message request.
 *
 * Groups. A new group is made with the account as its only member and
 * admin, then everyone invited joins through ONE Add Commit: staged with
 * gh-mls-commits (T-mls: the pending Commit and its sealed kind 445 commit
 * together), published byte for byte to the group relays, merged on the first
 * relay OK (MIP-03: never before), and only then are the Welcomes sent. The
 * same lifecycle carries adds, removals and metadata changes. A restart
 * republishes a pending Commit exactly as sealed (gh_mls_commit_resume()).
 *
 * Welcomes (MIP-02) leave through libmarmot's Welcome outbox: each is sealed
 * by the account's signer, gift-wrapped (NIP-59, a fresh ephemeral key) to
 * the invitee alone, stored signed (outbox role WELCOME_WRAP, T-seal) and
 * published to the invitee's kind-10050 inbox relays only (GhInboxResolver;
 * RECIPIENT_WRAP: ephemeral AUTH only), republished byte for byte until one
 * relay accepted it, and only then marked sent. Received Welcomes come from
 * the account's own inbox (GhDmInbox's Welcome sink): each is stored as a
 * pending invitation (with its wrap id seen, one transaction) and joined only
 * when the user accepts it (gh_mls_service_accept_invite()).
 *
 * Group messages (MIP-03, kind 445). Per joined group one live REQ
 * {kinds:[445], #h:[nostr group id], since} on its own connection to exactly
 * the group's relays (GhAuthPolicy MLS_ROUTING: ephemeral AUTH only; a stable
 * per-group Tor isolation label), event driven, never polled. Every event
 * goes through marmot_process_message() (the relay path: the envelope's id
 * and signature are verified first) inside one store transaction with its
 * admission to the conversation model (T-admit), so a ratchet step and its
 * message commit together. Events of a later epoch wait (bounded) for the
 * Commit that makes them readable; duplicates are dropped by the scope, by
 * libmarmot's processed markers and by the seen set. The read cursor (the
 * REQ's since, minus an overlap) moves only for events libmarmot accepted
 * and the store kept, only while every group relay has answered, never past
 * now and never past an event held or dropped unread. Held events are kept
 * once per id (oldest dropped first when full) across network flaps and
 * retried as a fixpoint after every Commit (a whole backlog at once); one
 * still unreadable after GH_MLS_SERVICE_JUNK_AFTER_COMMITS new Commits is
 * junk. Nothing from before the account joined is held. A joined group is
 * read from its Welcome's time. A sent message is one
 * transaction -- the outgoing message row, marmot_create_message() (the
 * sender ratchet step) and its sealed kind 445 -- committed before anything
 * is published (libmarmot 0.8.0 review N1), then published to the group
 * relays; its status is honest (SENT once one group relay accepted it) and a
 * restart republishes it byte for byte. libmarmot signs every kind 445 with a
 * fresh ephemeral key: the account key never appears on a group relay.
 *
 * Rooms. Joined groups are rooms of the GhConversationStore (backend MLS,
 * room id gh_message_mls_room_id(), durable through gh-store-mls.h), listed
 * even before their first message and titled with the group's name.
 *
 * Account proof (libmarmot >= 0.10.0, GH_MLS_SERVICE_ACCOUNT_PROOF). Every
 * member leaf carries the account's signature over the leaf's MLS key. At
 * each start (libmarmot's instance key is not stored) the service asks the
 * account's signer to sign libmarmot's local-only kind:450 template (never
 * published), checks it is exactly that event by the account, and hands it
 * to marmot_set_account_proof(). Until then the identity state is WAITING
 * ("Waiting for approval"), no KeyPackage is made and no group created
 * (GH_MLS_SERVICE_ERROR_NOT_ENROLLED); a switch cancels the request, and a
 * declined one is asked again only at the next start or generation. An
 * invitee (or a group member) whose app cannot prove its account is
 * GH_MLS_SERVICE_ERROR_NEEDS_UPDATE.
 *
 * Leaving. marmot_leave_group() marks the group inactive locally and the
 * service stops reading it; an MLS member cannot remove itself from the tree
 * (no self-remove proposal in libmarmot yet), so the others keep counting it
 * until an admin removes it. A member an admin removed can no longer decrypt
 * anything of the following epochs.
 *
 * Generation. The service runs only while its store's account is the active
 * account: a switch cancels every subscription, lookup, signer request and
 * publish at once; nothing of the old generation is recorded afterwards.
 * Transports are the defaults (gh_relay_scope_new(), gh_relay_publish_new()),
 * so the network session's dispatcher (G09) routes everything, Tor included.
 * The store, model, relays, resolver and inbox are borrowed: dispose the
 * service before closing the store.
 */

/* libmarmot >= 0.10.0 binds every member leaf to its account with a proof
 * the account signs (nostrc-7vyi): the service enrolls it (see "Account
 * proof" above) before any KeyPackage or group. Older libmarmot has no
 * proof: the identity state is NOT_REQUIRED. */
#if defined(MARMOT_VERSION_MAJOR) && \
    (MARMOT_VERSION_MAJOR > 0 || MARMOT_VERSION_MINOR >= 10)
#define GH_MLS_SERVICE_ACCOUNT_PROOF 1
#else
#define GH_MLS_SERVICE_ACCOUNT_PROOF 0
#endif

/* Default KeyPackage rotation age (28 days). */
#define GH_MLS_KEY_PACKAGE_LIFETIME ((gint64)28 * 24 * 3600)
/* Kind-445 events of a later epoch held for a Commit, per group. */
#define GH_MLS_SERVICE_MAX_HELD 256
/* Overlap subtracted from a group's read cursor (seconds). */
#define GH_MLS_SERVICE_CURSOR_OVERLAP 600
/* A held event still unreadable after this many applied Commits is junk. */
#define GH_MLS_SERVICE_JUNK_AFTER_COMMITS 3
/* People invited at once (one Add Commit). */
#define GH_MLS_SERVICE_MAX_INVITEES 32

typedef enum {
  GH_MLS_KEY_PACKAGE_NONE,        /* not published (inactive, offline or not yet) */
  GH_MLS_KEY_PACKAGE_NO_RELAYS,   /* the account has no own write or inbox relay */
  GH_MLS_KEY_PACKAGE_PUBLISHING,  /* made, being signed or published */
  GH_MLS_KEY_PACKAGE_PUBLISHED,   /* a relay accepted the current one */
  GH_MLS_KEY_PACKAGE_FAILED       /* the signer declined, or no relay accepted it */
} GhMlsKeyPackageState;

GType gh_mls_key_package_state_get_type(void);
#define GH_TYPE_MLS_KEY_PACKAGE_STATE (gh_mls_key_package_state_get_type())

typedef enum {
  GH_MLS_READ_IDLE,         /* not read (inactive, offline or left) */
  GH_MLS_READ_SYNCING,      /* subscribed, history until EOSE */
  GH_MLS_READ_LIVE,         /* every group relay sent EOSE */
  GH_MLS_READ_DISCONNECTED  /* every group relay dropped or refused; it reconnects */
} GhMlsReadState;

GType gh_mls_read_state_get_type(void);
#define GH_TYPE_MLS_READ_STATE (gh_mls_read_state_get_type())

#define GH_MLS_SERVICE_ERROR (gh_mls_service_error_quark())
GQuark gh_mls_service_error_quark(void);
typedef enum {
  GH_MLS_SERVICE_ERROR_NO_CONSENT = 1, /* a message request, or not a contact (PT-8) */
  GH_MLS_SERVICE_ERROR_NO_KEY_PACKAGE, /* the person hasn't set up encrypted groups */
  GH_MLS_SERVICE_ERROR_NOT_ADMIN,      /* the change needs a group admin */
  GH_MLS_SERVICE_ERROR_BUSY,           /* another change of the group is pending */
  GH_MLS_SERVICE_ERROR_REFUSED,        /* every group relay refused the Commit */
  GH_MLS_SERVICE_ERROR_SUPERSEDED,     /* another member's change won the epoch */
  GH_MLS_SERVICE_ERROR_NO_RELAYS,      /* no group relay (or none usable) */
  GH_MLS_SERVICE_ERROR_INACTIVE,       /* the store's account is not the active one */
  GH_MLS_SERVICE_ERROR_NOT_ENROLLED,   /* the signer has not approved this device's proof */
  GH_MLS_SERVICE_ERROR_NEEDS_UPDATE    /* someone's app cannot prove their account yet */
} GhMlsServiceError;

typedef enum {
  GH_MLS_IDENTITY_NOT_REQUIRED, /* libmarmot < 0.10.0: no account proof */
  GH_MLS_IDENTITY_NONE,         /* not asked yet (inactive, offline) */
  GH_MLS_IDENTITY_WAITING,      /* the signer is asking the user */
  GH_MLS_IDENTITY_ENROLLED,     /* the proof is set for this start */
  GH_MLS_IDENTITY_DECLINED,     /* the user declined; asked again at the next start */
  GH_MLS_IDENTITY_FAILED        /* the signer failed or returned something else */
} GhMlsIdentityState;

GType gh_mls_identity_state_get_type(void);
#define GH_TYPE_MLS_IDENTITY_STATE (gh_mls_identity_state_get_type())

#define GH_TYPE_MLS_GROUP (gh_mls_group_get_type())
G_DECLARE_FINAL_TYPE(GhMlsGroup, gh_mls_group, GH, MLS_GROUP, GObject)

/* One joined (or left) group of the account. Read-only properties, notified
 * on change: "group-id" (hex MLS group id), "room-id", "name",
 * "description", "epoch", "active" (FALSE once left), "read-state",
 * "is-admin" (the account is a GroupData admin), "pending-commit" (a change
 * of the account's is published but not merged yet) and
 * "unsent-welcomes" (Welcomes of merged Adds not yet accepted by an
 * invitee's inbox relay) and "unreadable" (kind-445 events received this
 * session that cannot be decrypted yet: of an epoch the account has not
 * reached, or, after a removal, never; charter §7.15 state 13 "Unable to
 * decrypt yet"). Signal "members-changed": the member list or the admins may
 * differ. */
const gchar *gh_mls_group_get_group_id(GhMlsGroup *self);
const gchar *gh_mls_group_get_room_id(GhMlsGroup *self);
const gchar *gh_mls_group_get_name(GhMlsGroup *self);
const gchar *gh_mls_group_get_description(GhMlsGroup *self);
guint64 gh_mls_group_get_epoch(GhMlsGroup *self);
gboolean gh_mls_group_get_active(GhMlsGroup *self);
GhMlsReadState gh_mls_group_get_read_state(GhMlsGroup *self);
gboolean gh_mls_group_get_is_admin(GhMlsGroup *self);
gboolean gh_mls_group_get_pending_commit(GhMlsGroup *self);
guint gh_mls_group_get_unsent_welcomes(GhMlsGroup *self);
guint gh_mls_group_get_unreadable(GhMlsGroup *self);
/* The read cursor (unix seconds; 0: none): the group's next REQ asks from
 * it minus GH_MLS_SERVICE_CURSOR_OVERLAP. For diagnostics and tests. */
gint64 gh_mls_group_get_cursor(GhMlsGroup *self);
/* The members' account keys (lowercase hex, sorted; the account included):
 * what every member can see (charter §2.2). Transfer full. */
GStrv gh_mls_group_dup_members(GhMlsGroup *self);
/* The GroupData admins (lowercase hex, sorted). Transfer full. */
GStrv gh_mls_group_dup_admins(GhMlsGroup *self);
/* The group relays (sorted). Transfer full. */
GStrv gh_mls_group_dup_relays(GhMlsGroup *self);

/* A pending invitation: a Welcome received and stored, not yet accepted. */
typedef struct {
  gchar *wrapper_id;   /* the gift wrap's id (hex): what accept/decline name */
  gchar *inviter;      /* hex: the seal's signer, who invited the account */
  gchar *group_name;   /* from the Welcome (the inviter's claim until joined) */
  guint member_count;  /* at invite time */
  GStrv relays;        /* the group relays the Welcome names */
} GhMlsInvite;

void gh_mls_invite_free(GhMlsInvite *invite);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhMlsInvite, gh_mls_invite_free)

/* Whether the account may look pubkey (hex) up to invite it (PT-8). */
typedef gboolean (*GhMlsConsentFunc)(const gchar *pubkey, gpointer data);

typedef struct {
  GhStore *store;                      /* the account's open store; borrowed */
  GhAccountController *accounts;       /* signer, generation */
  GhConversationStore *conversations;  /* bound to the store's account; borrowed */
  GhAccountRelays *account_relays;     /* own 10002 write + 10050: KeyPackage publish */
  GhInboxResolver *inboxes;            /* invitees' 10050: Welcome delivery */
  GSettings *settings;                 /* discovery-relays: KeyPackage lookups */
  GhDmInbox *inbox;                    /* nullable: where Welcomes arrive (its sink) */
  /* NULL: an accepted peer of a non-request NIP-17 room of conversations. */
  GhMlsConsentFunc may_look_up;
  gpointer consent_data;
  GNetworkMonitor *network;            /* NULL: g_network_monitor_get_default() */
  guint publish_deadline;              /* per-relay seconds; 0: the publish default */
  guint lookup_deadline;               /* per-phase seconds; 0: 15 */
  gint64 key_package_lifetime;         /* seconds; 0: GH_MLS_KEY_PACKAGE_LIFETIME */
} GhMlsServiceConfig;

#define GH_TYPE_MLS_SERVICE (gh_mls_service_get_type())
G_DECLARE_FINAL_TYPE(GhMlsService, gh_mls_service, GH, MLS_SERVICE, GObject)

/* Opens libmarmot on the store, lists the stored rooms and joined groups,
 * resumes pending Commits, Welcomes and sends, and starts once the account
 * is active and online. The service is a GListModel of GhMlsGroup, oldest
 * first. Signals: "invite-received" (gchar *wrapper_id), "group-added"
 * (GhMlsGroup). */
GhMlsService *gh_mls_service_new(const GhMlsServiceConfig *config, GError **error);
const gchar *gh_mls_service_get_account(GhMlsService *self);
/* libmarmot, for tests and diagnostics (borrowed; one thread). */
Marmot *gh_mls_service_get_marmot(GhMlsService *self);

GhMlsKeyPackageState gh_mls_service_get_key_package_state(GhMlsService *self);
/* The account-proof enrollment ("identity-state", notified). */
GhMlsIdentityState gh_mls_service_get_identity_state(GhMlsService *self);
/* The id of the KeyPackage event last accepted by a relay, or NULL. */
const gchar *gh_mls_service_get_key_package_id(GhMlsService *self);
/* Rotates the KeyPackage now (e.g. the user asked); FALSE with
 * GH_MLS_SERVICE_ERROR_INACTIVE when the service is not running. */
gboolean gh_mls_service_rotate_key_package(GhMlsService *self, GError **error);

/* The group of a hex MLS group id or of a room id, or NULL. Transfer none. */
GhMlsGroup *gh_mls_service_lookup(GhMlsService *self, const gchar *group_id_or_room_id);

/* Creates a group named name (description nullable) on relays (1 to 16
 * ws(s) URLs) and invites invitees (1 to GH_MLS_SERVICE_MAX_INVITEES hex
 * pubkeys): consent, KeyPackage lookups, the group, one Add Commit
 * (published, merged), then the Welcomes. Completes once the Add was merged
 * (the group, with the Welcomes on their way) or failed. */
void gh_mls_service_create_group_async(GhMlsService *self, const gchar *name,
                                       const gchar *description,
                                       const gchar *const *relays,
                                       const gchar *const *invitees,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback, gpointer user_data);
GhMlsGroup *gh_mls_service_create_group_finish(GhMlsService *self, GAsyncResult *result,
                                               GError **error);

/* Adds, removes, renames: one Commit each, completing once it was merged
 * (TRUE) or failed. Only admins (GH_MLS_SERVICE_ERROR_NOT_ADMIN). A Commit
 * still unanswered by every relay when the round ends stays pending and is
 * republished (it completes then). */
void gh_mls_service_add_members_async(GhMlsService *self, GhMlsGroup *group,
                                      const gchar *const *invitees,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback, gpointer user_data);
void gh_mls_service_remove_members_async(GhMlsService *self, GhMlsGroup *group,
                                         const gchar *const *members,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data);
/* name and description: NULL keeps each. */
void gh_mls_service_update_metadata_async(GhMlsService *self, GhMlsGroup *group,
                                          const gchar *name, const gchar *description,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data);
/* Replaces the GroupData admins (charter §7.10 owner/admin) with admins:
 * 1 to 1000 hex pubkeys of current members. */
void gh_mls_service_set_admins_async(GhMlsService *self, GhMlsGroup *group,
                                     const gchar *const *admins, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_mls_service_change_finish(GhMlsService *self, GAsyncResult *result,
                                      GError **error);

/* Leaves (locally; see above): the group turns inactive and is not read any
 * more; its room and history stay. */
gboolean gh_mls_service_leave(GhMlsService *self, GhMlsGroup *group, GError **error);

/* Sends a chat message (kind 9 inner event) to an active group: stored and
 * ratcheted in one transaction, listed at once, then published. The listed
 * message (transfer full) carries its GhMessageStatus. */
GhMessage *gh_mls_service_send(GhMlsService *self, GhMlsGroup *group, const gchar *text,
                               GError **error);

/* Pending invitations, oldest first (GhMlsInvite). Transfer full. */
GPtrArray *gh_mls_service_list_invites(GhMlsService *self, GError **error);
/* Joins the group of a pending invitation: its room is listed and read.
 * Transfer none. */
GhMlsGroup *gh_mls_service_accept_invite(GhMlsService *self, const gchar *wrapper_id,
                                         GError **error);
gboolean gh_mls_service_decline_invite(GhMlsService *self, const gchar *wrapper_id,
                                       GError **error);

G_END_DECLS
#endif
