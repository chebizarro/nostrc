#ifndef GH_NIP29_GROUP_LIST_H
#define GH_NIP29_GROUP_LIST_H

#include "gh-account-relays.h"
#include "gh-nip29-service.h"
#include "gh-relay-publish.h"

G_BEGIN_DECLS

/*
 * GhNip29GroupList (nostrc-90jof): the account's NIP-51 "simple groups" list
 * (kind 10009), so that the NIP-29 groups joined on one device, or before
 * this store existed, can be restored on another. GTK-free, main context
 * only; one per open store, beside its GhNip29Service (gh-app-outbox.c).
 *
 * Privacy (charter §2.2, §4.3, amended 2026-10-11 for nostrc-90jof). Group
 * membership is part of the social graph (§1.1), so Groundhog writes its
 * entries as NIP-51 *private* items: the JSON array of tags
 * (["group", <id>, <relay URL>, <name>] and ["r", <relay URL>]) NIP-44
 * encrypted by the account's signer to the account itself, in the content.
 * Public tags another client put in the list are kept as they are (that
 * client, and the user, chose to make them public); a group left here is
 * removed from either part. A list whose private part cannot be decrypted is
 * never replaced (fail closed: it might name groups Groundhog cannot see).
 *
 * Where. Read and written on the same set: the account's kind-10002
 * write-capable relays (GhAccountRelays) and the discovery-relays setting,
 * at most GH_NIP29_GROUP_LIST_MAX_RELAYS, nothing else. Reading is one
 * URL-scoped live REQ {kinds:[10009], authors:[account]} under GhAuthPolicy
 * OWN_LIST_DISCOVERY (a throwaway key if a relay demands AUTH); publishing
 * is GhRelayPublish under OWN_LIST_PUBLISH (account AUTH, on challenge only).
 * Nothing starts before own-list discovery settled, and nothing is contacted
 * for an inactive account or another generation.
 *
 * Merge. The newest list wins (NIP-01: created_at, then the lowest id). The
 * list Groundhog publishes is that list with this device's changes: a group
 * that is MEMBER or PENDING here is added if missing (so a group joined here
 * is never dropped by a list another device wrote); a group that became
 * LEFT, REMOVED, DENIED or CLOSED here, or was forgotten here, is removed
 * unless the list is newer than that change (another device joined it again
 * later); every other entry (groups joined elsewhere and not restored here,
 * groups in a passing state here) stays. Groundhog publishes only when that
 * differs from the newest list, only after every source answered (EOSE) for
 * this generation and the newest list was read in full, so a list it did not
 * see is never overwritten, and dates its list after the one it replaces. A
 * signature that took long enough for a newer list to arrive is dropped, and
 * the merge runs again. After a refusal (signer or relays) it tries again on
 * the next change (a group, a list, the relays).
 *
 * Restore. A group of the newest list with no record in this store is
 * "restorable" (offered once the list is read, even if some source did not
 * answer). Nothing is joined on its own: the UI asks first, and
 * gh_nip29_group_list_restore() joins the chosen ones through
 * gh_nip29_service_join() (each its own relay, account AUTH, the honest join
 * states). gh_nip29_group_list_dismiss() stops offering the groups offered
 * now for the rest of this session.
 *
 * Emits "changed" when the state or the restorable groups change.
 */

#define GH_NIP29_GROUP_LIST_KIND 10009
#define GH_NIP29_GROUP_LIST_MAX_RELAYS 16

typedef enum {
  GH_NIP29_GROUP_LIST_INACTIVE,    /* no active account, or no relay to use */
  GH_NIP29_GROUP_LIST_WAITING,     /* own-list discovery has not settled yet */
  GH_NIP29_GROUP_LIST_READING,     /* a source has neither answered nor failed */
  GH_NIP29_GROUP_LIST_PARTIAL,     /* a source failed: offers, never publishes */
  GH_NIP29_GROUP_LIST_UNREADABLE,  /* the newest list's private part did not decrypt */
  GH_NIP29_GROUP_LIST_SYNCED,      /* every source answered; nothing to publish */
  GH_NIP29_GROUP_LIST_PUBLISHING,  /* encrypting, signing or publishing */
  GH_NIP29_GROUP_LIST_FAILED       /* the last publish was refused or not accepted */
} GhNip29GroupListState;

typedef struct {
  gchar *relay_url; /* normalized */
  gchar *group_id;
  gchar *name;      /* the list's name for it, or NULL */
} GhNip29ListedGroup;

void gh_nip29_listed_group_free(GhNip29ListedGroup *group);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip29ListedGroup, gh_nip29_listed_group_free)

typedef struct {
  GhAccountController *accounts;   /* required */
  GhAccountRelays *account_relays; /* required: own 10002 write relays */
  GSettings *settings;             /* required: discovery-relays */
  GhNip29Service *service;         /* required: the store's groups; borrowed weakly */
  /* NULL: gnostr relays with NIP-42 (the application). */
  const GhRelayTransport *scope_transport;
  const GhRelayAuthTransport *scope_auth_transport;
  gpointer scope_transport_data;
  const GhRelayPublishTransport *publish_transport;
  const GhRelayPublishAuthTransport *publish_auth_transport;
  gpointer publish_transport_data;
  guint publish_deadline;          /* per-relay seconds; 0: the publish default */
} GhNip29GroupListConfig;

#define GH_TYPE_NIP29_GROUP_LIST (gh_nip29_group_list_get_type())
G_DECLARE_FINAL_TYPE(GhNip29GroupList, gh_nip29_group_list, GH, NIP29_GROUP_LIST, GObject)

GhNip29GroupList *gh_nip29_group_list_new(const GhNip29GroupListConfig *config);
GhNip29GroupListState gh_nip29_group_list_get_state(GhNip29GroupList *self);
/* The groups to offer (GhNip29ListedGroup, in list order). Transfer full. */
GPtrArray *gh_nip29_group_list_dup_restorable(GhNip29GroupList *self);
/* Joins each of groups (GhNip29ListedGroup) through gh_nip29_service_join();
 * returns how many requests were started. The first error is reported, the
 * others are still tried. */
guint gh_nip29_group_list_restore(GhNip29GroupList *self, GPtrArray *groups, GError **error);
/* "Not Now": the groups restorable now are not offered again this session. */
void gh_nip29_group_list_dismiss(GhNip29GroupList *self);
/* The newest signed kind 10009 known (seen or published), or NULL. */
const gchar *gh_nip29_group_list_get_event_json(GhNip29GroupList *self);

/* ---- the list's tags (pure; exposed for tests) ---------------------------------- */

/* Tags (GStrv each) from a NIP-51 private-items plaintext: a JSON array of
 * arrays of strings. NULL (G_IO_ERROR_INVALID_DATA) for anything else. */
GPtrArray *gh_nip29_group_list_parse_private(const gchar *plaintext, GError **error);
/* The plaintext for tags (GStrv each). Transfer full. */
gchar *gh_nip29_group_list_format_private(GPtrArray *tags);

G_END_DECLS
#endif
