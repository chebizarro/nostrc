#ifndef GH_RELAY_LIST_SETUP_H
#define GH_RELAY_LIST_SETUP_H

#include "gh-inbox-setup.h"

G_BEGIN_DECLS

/*
 * The account's kind-10002 relay list, as far as encrypted groups need it
 * (nostrc-0bdg; Marmot transports/nostr.md "KeyPackage publication":
 * inviters find a person's KeyPackages only on its 10002 write-capable
 * relays). GTK-free; onboarding drives it, only after the user agreed
 * (PD-13). Never a list over the user's own:
 *
 *  - CREATE: own-list discovery (GhAccountRelays) finished with EVERY
 *    discovery relay answering and none holding a 10002. The new list names
 *    the chosen relays as "write" relays.
 *  - ADD_WRITE: the account's 10002 exists but names no write-capable relay
 *    (only "read" entries, or none Groundhog can use). The chosen relays are
 *    added to it as "write" relays ("read" entries of them become unmarked);
 *    every other tag and the content are kept as they are.
 *
 * Before anything is signed, each publish target is asked
 * {kinds:[10002], authors:[account]} on its own connection (own list
 * discovery, charter §4.3: an ephemeral key only if a relay demands AUTH,
 * never the account). Publishing goes ahead only if every target answered
 * (EOSE) and none holds a list that is not the one being extended (CREATE:
 * none at all; ADD_WRITE: none but that one or older): a target that holds
 * one makes it SKIPPED, a target that failed or did not answer within
 * GH_RELAY_LIST_SETUP_CHECK_S makes it FAILED ("couldn't confirm"). Then
 * the account's signer signs it and it is published to the targets under
 * "own list publish". An account switch ends it (FAILED).
 */

typedef enum {
  GH_RELAY_LIST_OFFER_NONE,      /* nothing to offer (or not known yet) */
  GH_RELAY_LIST_OFFER_CREATE,    /* no relay list anywhere discovery looked */
  GH_RELAY_LIST_OFFER_ADD_WRITE  /* a relay list without any write-capable relay */
} GhRelayListOffer;

/* What config's account could be offered now (config->offer_relay_list,
 * an active account, its GhAccountRelays of that generation COMPLETE with
 * every discovery relay answering). */
GhRelayListOffer gh_relay_list_offer(const GhInboxSetupConfig *config);

typedef enum {
  GH_RELAY_LIST_SETUP_IDLE,
  GH_RELAY_LIST_SETUP_CHECKING,   /* asking every target for an existing list */
  GH_RELAY_LIST_SETUP_SIGNING,
  GH_RELAY_LIST_SETUP_PUBLISHING,
  GH_RELAY_LIST_SETUP_DONE,       /* at least one relay accepted it */
  GH_RELAY_LIST_SETUP_FAILED,     /* declined, unconfirmed, or no relay accepted it */
  GH_RELAY_LIST_SETUP_SKIPPED     /* a target holds the user's list: left as it is */
} GhRelayListSetupState;

/* A target's answer to the pre-publish check, at most this long. */
#define GH_RELAY_LIST_SETUP_CHECK_S 15

#define GH_TYPE_RELAY_LIST_SETUP (gh_relay_list_setup_get_type())
G_DECLARE_FINAL_TYPE(GhRelayListSetup, gh_relay_list_setup, GH, RELAY_LIST_SETUP, GObject)

/* config is copied (accounts, account_relays referenced); the scope and
 * publish transports in it are used for the check and the publication. */
GhRelayListSetup *gh_relay_list_setup_new(const GhInboxSetupConfig *config);
/* write_relays: the relays the list names (normalized wss:// URLs);
 * targets: where to check and publish (write_relays are added). FALSE with
 * G_IO_ERROR_EXISTS when nothing is to be offered (gh_relay_list_offer()),
 * G_IO_ERROR_INVALID_ARGUMENT for no relay, or G_IO_ERROR_PENDING when it
 * already ran. One use per object; "changed" after every update. */
gboolean gh_relay_list_setup_start(GhRelayListSetup *self, const gchar *const *write_relays,
                                   const gchar *const *targets, GError **error);
void gh_relay_list_setup_cancel(GhRelayListSetup *self);
GhRelayListSetupState gh_relay_list_setup_get_state(GhRelayListSetup *self);
GhRelayListOffer gh_relay_list_setup_get_mode(GhRelayListSetup *self);
/* Relays that accepted the list. */
guint gh_relay_list_setup_get_n_accepted(GhRelayListSetup *self);
/* Why it FAILED (NULL otherwise). */
const GError *gh_relay_list_setup_get_error(GhRelayListSetup *self);

/* GhInboxSetup's relay list (gh-inbox-setup.h): what it would offer, and
 * the GhRelayListSetup it runs (NULL before; transfer none). */
GhRelayListOffer gh_inbox_setup_get_relay_list_offer(GhInboxSetup *self);
GhRelayListSetup *gh_inbox_setup_get_relay_list(GhInboxSetup *self);

G_END_DECLS
#endif
