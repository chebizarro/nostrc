#ifndef GH_APP_OUTBOX_H
#define GH_APP_OUTBOX_H

#include "gh-account-relays.h"
#include "gh-inbox-resolver.h"
#include "gh-relay-publish.h"
#include "gh-store.h"

G_BEGIN_DECLS

/* The send side the account store makes a durable outbox (G06, gh-outbox.h)
 * with, once per open store: the recipient 10050 resolver and the NIP-17
 * sealer/publisher. Kept in its own translation unit because gh-outbox.h's
 * GhMessageStatus and the conversation model's interim one (gh-message.h)
 * cannot share one until G12/G13 merge them. Main context only. */

typedef struct {
  GhAccountController *accounts;             /* required */
  GhAccountRelays *account_relays;           /* required: own 10050 (self-copies) */
  GSettings *settings;                       /* required when inboxes is NULL */
  GhInboxResolver *inboxes;                  /* NULL: a GhInboxLookup over gnostr relays */
  const GhRelayPublishTransport *transport;  /* NULL: gnostr relays (with NIP-42) */
  gpointer transport_data;
} GhAppOutboxConfig;

typedef struct _GhAppOutbox GhAppOutbox;

GhAppOutbox *gh_app_outbox_new(const GhAppOutboxConfig *config);
void gh_app_outbox_free(GhAppOutbox *self);

/* A GhAccountStoreOutboxFunc (gh-account-store.h): a new GhOutbox over
 * store; user_data is the GhAppOutbox. */
GObject *gh_app_outbox_create(GhStore *store, gpointer user_data, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhAppOutbox, gh_app_outbox_free)

G_END_DECLS
#endif
