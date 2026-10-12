#ifndef GH_APP_OUTBOX_H
#define GH_APP_OUTBOX_H

#include "gh-account-relays.h"
#include "gh-contact-directory.h"
#include "gh-inbox-resolver.h"
#include "gh-relay-publish.h"
#include "gh-store.h"

G_BEGIN_DECLS

/* The send side the account store makes a durable outbox (G06, gh-outbox.h)
 * with, once per open store: the recipient 10050 resolver and the NIP-17
 * sealer/publisher. (gh-outbox.h and the conversation model share one
 * GhMessageStatus since G13; see gh-send-ui.h for the UI side.) Main context
 * only. */

typedef struct {
  GhAccountController *accounts;             /* required */
  GhAccountRelays *account_relays;           /* required: own 10050 (self-copies) */
  GSettings *settings;                       /* required when inboxes is NULL */
  /* NULL: the cached GhContactDirectory (G10) over gnostr relays, which
   * each gh_app_outbox_create() store backs while its outbox runs. */
  GhInboxResolver *inboxes;
  GhConversationStore *conversations;        /* nullable: the directory's accepted contacts */
  const GhRelayPublishTransport *transport;  /* NULL: gnostr relays (with NIP-42) */
  gpointer transport_data;
  /* nostrc-qp24.13: run a GhMlsService beside each outbox (gh-app-services.c
   * passes GH_FEATURE_ENCRYPTED_GROUPS; builds without the MLS service
   * ignore it). */
  gboolean encrypted_groups;
} GhAppOutboxConfig;

typedef struct _GhAppOutbox GhAppOutbox;

GhAppOutbox *gh_app_outbox_new(const GhAppOutboxConfig *config);
void gh_app_outbox_free(GhAppOutbox *self);

/* A GhAccountStoreOutboxFunc (gh-account-store.h): a new GhOutbox over
 * store; user_data is the GhAppOutbox. */
GObject *gh_app_outbox_create(GhStore *store, gpointer user_data, GError **error);
/* gh_outbox_prune() on a gh_app_outbox_create() object (NULL: nothing to
 * do): the expiry purge deleted entries of disappearing messages (G07). */
void gh_app_outbox_prune(GObject *outbox);
/* The conversation model that defines the directory's accepted contacts
 * (when the directory is the resolver); NULL detaches. */
void gh_app_outbox_set_conversations(GhAppOutbox *self, GhConversationStore *conversations);
/* The contact directory made by gh_app_outbox_new(), or NULL when the
 * config supplied inboxes. Borrowed. Conversation titles may use its cached
 * display names (gh_contact_directory_dup_conversation_title()). */
GhContactDirectory *gh_app_outbox_get_directory(GhAppOutbox *self);

/* G20a: the open store's NIP-29 groups (a GhNip29Service,
 * gh-nip29-service.h), made beside each gh_app_outbox_create() outbox when
 * the build has them and disposed with it; NULL otherwise. Borrowed. */
GObject *gh_app_outbox_get_nip29_service(GhAppOutbox *self);

/* qp24.13: the open store's encrypted groups (a GhMlsService,
 * gh-mls-service.h), made beside each gh_app_outbox_create() outbox when the
 * build has them and encrypted_groups is set, and disposed with it; NULL
 * otherwise. Borrowed. */
GObject *gh_app_outbox_get_mls_service(GhAppOutbox *self);

/* W26 slice B (nostrc-191r): the open store's reaction store, made beside
 * each gh_app_outbox_create() outbox and disposed with it. Holds reaction
 * summaries for all backends (MLS, NIP-17, NIP-29). NULL when no store
 * is open. Borrowed. */
typedef struct _GhReactionStore GhReactionStore;
GhReactionStore *gh_app_outbox_get_reactions(GhAppOutbox *self);

/* A NIP-09 deletion request received in a NIP-17 room (groundhog/
 * bubble-menu): called for each message it names, with the room, the
 * message's rumor id and the request's author (the verified seal sender).
 * The callee deletes the message only if that author wrote it in that room
 * (gh_store_conversations_delete_message() with the sender). NULL: none. */
typedef void (*GhAppOutboxDeleteFunc)(const gchar *room_id, const gchar *rumor_id,
                                      const gchar *author_pubkey, gpointer user_data);
void gh_app_outbox_set_delete_func(GhAppOutbox *self, GhAppOutboxDeleteFunc func,
                                   gpointer user_data);

/* Delete for Me / Delete for Everyone from a bubble's menu (groundhog/
 * bubble-menu), for a NIP-17 conversation of @stored's account. For
 * everyone (own messages only), a NIP-09 deletion request (a kind-5 rumor
 * naming the message) goes first, gift-wrapped to every member and the
 * self-copy through @outbox (a gh_app_outbox_create() object) like any
 * message; then the message is deleted here, from the store (its outbox
 * entry with it, seen keys kept) and the model, and the outbox lets go of
 * an entry it was still sending. */
typedef struct _GhStoreConversations GhStoreConversations;
gboolean gh_app_outbox_delete_message(GhStoreConversations *stored, GObject *outbox,
                                      GhConversation *conversation, GhMessage *message,
                                      gboolean for_everyone, GError **error);
/* The same for a message of an encrypted (MLS) or NIP-29 group of @self's
 * open store (groundhog/deletion): gh_mls_service_delete_message() or
 * gh_nip29_service_delete_message(); for everyone only for the account's
 * own message. NOT_SUPPORTED when the group's service is not running. */
gboolean gh_app_outbox_delete_group_message(GhAppOutbox *self, GhConversation *conversation,
                                            GhMessage *message, gboolean for_everyone,
                                            GError **error);
/* A received NIP-09 request (GhAppOutboxDeleteFunc): the message @rumor_id
 * in @room_id is deleted as with Delete for Me only if @author_pubkey wrote
 * it; anyone else's is left alone. *out_deleted (nullable) says whether one
 * was. @outbox (nullable) lets go of a deleted own message's entry. */
gboolean gh_app_outbox_honour_deletion(GhStoreConversations *stored, GObject *outbox,
                                       const gchar *room_id, const gchar *rumor_id,
                                       const gchar *author_pubkey, gboolean *out_deleted,
                                       GError **error);
/* The DM inbox whose Welcome sink the MLS service takes (borrowed; NULL
 * detaches). Set it before the store opens. */
void gh_app_outbox_set_inbox(GhAppOutbox *self, GObject *inbox);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhAppOutbox, gh_app_outbox_free)

G_END_DECLS
#endif
