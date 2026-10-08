#ifndef GH_CONTACT_DIRECTORY_H
#define GH_CONTACT_DIRECTORY_H

#include "gh-account-controller.h"
#include "gh-clock.h"
#include "gh-conversation-store.h"
#include "gh-inbox-resolver.h"
#include "gh-relay-scope.h"
#include "gh-store.h"

G_BEGIN_DECLS

/*
 * GhContactDirectory (privacy charter G10, §4.3 "Contact directory", §4.5
 * S1/S2, PD-12, PT-8): the cached kind-10050 inbox relays and kind-0 display
 * name of the account's contacts, and the GhInboxResolver the send pipeline
 * (GhDmSender) and the durable outbox use.
 *
 * Contacts. A contact is accepted when it is a peer of a NIP-17 conversation
 * of the account that is not a message request (the conversation model set
 * with gh_contact_directory_set_conversations()). With own_profile enabled,
 * every listed signer identity is also accepted as the owner's own profile.
 * Only accepted contacts are ever refreshed, and only their kind 0 is ever
 * asked for (PT-8): a request's sender is looked up for nothing until it is
 * accepted. Replying to a request is sending, which accepts it.
 *
 * Cache. With the account's encrypted store bound
 * (gh_contact_directory_set_store()), the newest admitted signed event per
 * (contact, kind) and the time it was last asked for live in the store's
 * `directory` table, re-verified (signature, author, kind, id) when the store
 * is bound again after a restart; a row that fails is deleted. Admission is
 * strict: a valid NIP-01 signature, authored by exactly the contact asked
 * for, kind 10050 or (accepted contacts only) 0, at most 64 KiB, not dated
 * more than 15 minutes ahead, and newer than what is cached (created_at, then
 * the lower id; NIP-01). Kind-0 content is display-only: display_name (else
 * name) and the claimed NIP-05 address, both cleaned of control and
 * formatting characters and bounded; no picture is ever fetched (PD-2), and
 * NIP-05 is never verified here.
 *
 * Resolving (GhInboxResolver, S2). A cached 10050 younger than 24 h is the
 * answer, with no network (cached = TRUE). An older one is still the answer
 * for the send, and a background refresh of that contact follows after a
 * random U(5, 60) s; if it finds a changed list, the resolver's "changed"
 * signal lets the outbox add the new relays as targets of the same wrap.
 * Only a recipient without a cached 10050 (the first contact, which the user
 * chose explicitly) gets a one-shot lookup that the send waits for.
 *
 * Connections (§4.3, PD-12). Every lookup is one URL-scoped REQ, on a fresh
 * scope, to the discovery-relays setting only (never the account's own
 * relay lists as such), with GhAuthPolicy's CONTACT_DIRECTORY identity (an
 * ephemeral key if a relay demands AUTH, never the account). The user can
 * make their own inbox relays discovery relays in onboarding, whose confirm
 * page says those relays "will see whom you look up"; lookups then reach
 * them, still unauthenticated as the account. A REQ completes when every
 * source sent EOSE or failed; the deadline only bounds a silent source.
 *
 * Scheduling (S1, NT-12). Nothing is refreshed until a store is bound; the
 * first run starts U(2, 30) min after that. A run takes every accepted
 * contact whose 10050 or kind 0 was last asked for 24 h ago or more (or
 * never), in random order, in batches of at most 10 authors on fresh scopes
 * spaced U(10, 120) s apart; the next run is due when the next entry turns
 * stale, plus U(2, 30) min. Accepting a request refreshes that contact alone
 * after U(5, 60) s; a contact that newly appears accepted waits for a run no
 * sooner than U(2, 30) min away. Every delay and draw comes from the GhClock.
 *
 * Generation. Everything belongs to the active account's generation: a
 * switch cancels every lookup (pending resolves finish with
 * G_IO_ERROR_CANCELLED), drops the cache and the store, and stops the
 * schedule. Main context only.
 */

typedef struct {
  GhAccountController *accounts;              /* required */
  GSettings *settings;                        /* required: discovery-relays */
  GhClock *clock;                             /* NULL: the system clock */
  const GhRelayTransport *transport;          /* NULL: gnostr relays (with NIP-42) */
  const GhRelayAuthTransport *auth_transport; /* custom transport only; NULL: no AUTH */
  gpointer transport_data;
  /* The listed signer identities' kind 0 too (names and picture URLs for
   * the sidebar/account switcher): treated as accepted own profiles and
   * looked up within 5-60 s of store open when uncached. Default off. */
  gboolean own_profile;
} GhContactDirectoryConfig;

#define GH_TYPE_CONTACT_DIRECTORY (gh_contact_directory_get_type())
G_DECLARE_FINAL_TYPE(GhContactDirectory, gh_contact_directory, GH, CONTACT_DIRECTORY, GObject)

GhContactDirectory *gh_contact_directory_new(const GhContactDirectoryConfig *config);

/* The conversation model whose accepted NIP-17 rooms define the accepted
 * contacts (NULL: none). Borrowed with a reference. */
void gh_contact_directory_set_conversations(GhContactDirectory *self,
                                            GhConversationStore *conversations);
/* Binds the active account's open store (NULL unbinds): its cached entries
 * are restored and re-verified and the refresh schedule starts. The store is
 * borrowed until it is unbound or the account changes; unbind it before
 * closing it. Refused (G_IO_ERROR_PERMISSION_DENIED) for another account's
 * store or without an active account. */
gboolean gh_contact_directory_set_store(GhContactDirectory *self, GhStore *store,
                                        GError **error);
/* Failure bound for a silent source, in seconds (default 15, 1..120). */
void gh_contact_directory_set_deadline(GhContactDirectory *self, guint seconds);

/* Whether pubkey (hex) is an accepted contact of the account. */
gboolean gh_contact_directory_is_accepted(GhContactDirectory *self, const gchar *pubkey);
/* The cached kind-0 display name / claimed (unverified) NIP-05 address of an
 * accepted contact, cleaned for display; NULL when unknown or not accepted.
 * Borrowed until the next "profile-changed" for pubkey. */
const gchar *gh_contact_directory_get_display_name(GhContactDirectory *self,
                                                   const gchar *pubkey);
const gchar *gh_contact_directory_get_nip05(GhContactDirectory *self, const gchar *pubkey);
/* Cached picture URL only, never a lookup or an HTTP request. Transfer full. */
gchar *gh_contact_directory_dup_picture_uri(GhContactDirectory *self, const gchar *pubkey);
/* A title for an accepted conversation from its peers' cached display names,
 * ", "-joined, or NULL when it is a request, a note to self, or any peer has
 * no cached name (the conversation's own title applies then). */
gchar *gh_contact_directory_dup_conversation_title(GhContactDirectory *self,
                                                   GhConversation *conversation);

/* "profile-changed" (gchar *pubkey): a contact's display name, NIP-05 or
 * picture URL, or whether it may be shown, changed (a fetch, an accept, or a
 * store bound or unbound, which restores or drops the cache). */

G_END_DECLS
#endif
