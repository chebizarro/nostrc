#ifndef GH_ACCOUNT_RELAYS_H
#define GH_ACCOUNT_RELAYS_H

#include "gh-account-controller.h"
#include "gh-relay-scope.h"

G_BEGIN_DECLS

typedef enum {
  GH_ACCOUNT_RELAYS_INACTIVE,    /* no active Groundhog account */
  GH_ACCOUNT_RELAYS_NO_SOURCES,  /* discovery-relays has no usable ws(s) URL */
  GH_ACCOUNT_RELAYS_DISCOVERING, /* a source has neither sent EOSE nor failed */
  GH_ACCOUNT_RELAYS_COMPLETE,    /* every source settled; one answered and its REQ is open */
  GH_ACCOUNT_RELAYS_UNREACHABLE  /* no source has an open, answered REQ: each failed
                                  * before EOSE or the relay CLOSED it. Admitted
                                  * lists are kept; a re-issued REQ can recover. */
} GhAccountRelaysState;

#define GH_TYPE_ACCOUNT_RELAYS (gh_account_relays_get_type())
G_DECLARE_FINAL_TYPE(GhAccountRelays, gh_account_relays, GH, ACCOUNT_RELAYS, GObject)

/* Binds one URL-scoped live REQ (kinds 10002 and 10050, authored by the
 * active account) to the account controller's generation. The REQ goes only
 * to the Groundhog discovery-relays setting. A source that demands NIP-42
 * AUTH for it gets a throwaway key, never the account (GhAuthPolicy,
 * OWN_LIST_DISCOVERY); a custom transport never authenticates. An account
 * switch or loss cancels the scope before any state for the next account is
 * built, and a callback from an older scope or generation is discarded. Only
 * signed events authored by the active account update the lists; newest wins
 * (NIP-01).
 * Emits "changed" on the main context. transport NULL uses gnostr relays. */
GhAccountRelays *gh_account_relays_new(GhAccountController *accounts,
                                       GSettings *settings,
                                       const GhRelayTransport *transport,
                                       gpointer transport_data);

GhAccountRelaysState gh_account_relays_get_state(GhAccountRelays *self);
/* The account generation the current lists belong to; 0 when inactive. */
guint64 gh_account_relays_get_generation(GhAccountRelays *self);
/* NULL-terminated and borrowed; NULL until a list of that kind is admitted. */
const gchar *const *gh_account_relays_get_read_relays(GhAccountRelays *self);
const gchar *const *gh_account_relays_get_write_relays(GhAccountRelays *self);
const gchar *const *gh_account_relays_get_inbox_relays(GhAccountRelays *self);
/* Whether a kind 10002 by the account was admitted (in any form, even one
 * without a usable write relay): Groundhog never publishes over it
 * (nostrc-0bdg). */
gboolean gh_account_relays_has_relay_list(GhAccountRelays *self);

G_END_DECLS
#endif
