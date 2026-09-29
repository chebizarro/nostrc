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

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhAppOutbox, gh_app_outbox_free)

G_END_DECLS
#endif
