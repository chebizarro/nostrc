#ifndef GH_MLS_KEY_PACKAGES_H
#define GH_MLS_KEY_PACKAGES_H

#include "gh-account-controller.h"

G_BEGIN_DECLS

/*
 * GhMlsKeyPackageLookup (nostrc-qp24.13): finds the KeyPackage (kind 30443,
 * MIP-00) to invite one person with. GTK-free, main context only.
 *
 * Where (Marmot transports/nostr.md "KeyPackage publication": KeyPackages
 * live on the author's kind-10002 write-capable set, nostrc-0bdg; privacy
 * charter §2.2 "Discovery relays: whose KeyPackages you fetch", §4.3
 * "Contact directory"):
 *  1. one URL-scoped REQ {kinds:[10002], authors:[person]} to the discovery
 *     relays the caller gives (the discovery-relays setting), then
 *  2. when a kind-10002 by that person came back, one REQ
 *     {kinds:[30443], authors:[person]} to its write-capable relays --
 *     `r` entries marked "write" or unmarked, never read-only ones (at most
 *     GH_MLS_KEY_PACKAGE_MAX_WRITE_RELAYS; a discovery relay among them is
 *     asked again). Only KeyPackages from phase 2 count: no 10002, no
 *     KeyPackage ("hasn't set up encrypted groups").
 * Nothing else is contacted, never the account's own relays as such, never
 * kind 10051, and never a relay in `exclude` (compared by
 * gh_mls_relay_key()) in either phase: the service passes the group's
 * relays, which must not learn whom an inviter looks up. Each
 * phase is one fresh scope with GhAuthPolicy's CONTACT_DIRECTORY identity:
 * an ephemeral key if a relay demands AUTH, never the account (R1). A phase
 * ends when every relay sent EOSE or failed; the deadline only bounds a
 * silent relay.
 *
 * Which: every signed kind 30443 authored by the person goes to
 * marmot_select_key_package_event_for_profile() for both adopted and MDK 0.8
 * formats (newest valid event per addressable slot). Adopted is preferred;
 * the legacy candidate is also returned for a legacy group's Add.
 *
 * Consent is the caller's: never look up someone who is only a message
 * request (charter PD-8, PT-8). The lookup is bound to the account generation
 * at its start: an account switch or the cancellable ends it with
 * G_IO_ERROR_CANCELLED. Errors: G_IO_ERROR_NOT_FOUND (a relay answered but no
 * valid KeyPackage exists: "hasn't set up encrypted groups"),
 * G_IO_ERROR_HOST_UNREACHABLE (no relay answered), G_IO_ERROR_INVALID_ARGUMENT
 * (no usable discovery relay or a bad pubkey).
 */

#define GH_MLS_KEY_PACKAGE_MAX_WRITE_RELAYS 8

typedef struct {
  gchar *pubkey;      /* lowercase hex */
  gchar *event_json;  /* the selected signed kind 30443 */
  gchar *event_id;
  gboolean adopted;          /* event_json uses the adopted profile */
  gchar *legacy_event_json;  /* nullable, independently validated MDK 0.8 event */
  guint sources;      /* relays asked, both phases */
  guint answered;     /* relays that sent EOSE */
} GhMlsKeyPackage;

void gh_mls_key_package_free(GhMlsKeyPackage *key_package);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhMlsKeyPackage, gh_mls_key_package_free)

/* deadline: seconds per phase (0: 15). exclude: nullable. */
void gh_mls_key_package_lookup_async(GhAccountController *accounts,
                                     const gchar *const *discovery_relays,
                                     const gchar *const *exclude,
                                     const gchar *pubkey, guint deadline,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
GhMlsKeyPackage *gh_mls_key_package_lookup_finish(GAsyncResult *result, GError **error);

/* The older KeyPackage kind (MIP-00 before kind 30443): verification
 * evidence only, never an invitation. */
#define GH_MLS_KIND_LEGACY_KEY_PACKAGE 443

/* A relay URL in a form two spellings of one relay share (lowercase
 * scheme and host, no default port, no trailing slash), for comparing
 * relay lists. Transfer full; NULL for NULL. */
gchar *gh_mls_relay_key(const gchar *url);

/* Verification evidence (nostrc-6ukh), not an invitation: every signed
 * KeyPackage event (kind 30443 and the older 443; never kind 10051, which
 * the adopted spec dropped) the person authored, for
 * marmot_key_package_event_matches_member() to judge against a group
 * member's leaf. Same two phases, privacy and cancellation as above:
 * relays (the discovery relays) in phase 1, the person's kind-10002 write
 * relays (marked "write" or unmarked) in phase 2. No relay in `exclude`
 * (compared by gh_mls_relay_key()) is asked in either phase: Verify passes
 * the group's relays, which must never learn whom a member looks up (W24
 * review A1). The caller verifies each event; this only drops events by
 * another author. finish: transfer full, g_ptr_array_unref(), each string
 * owned by the array (it may be empty: relays answered, nothing found);
 * G_IO_ERROR_HOST_UNREACHABLE when no relay answered at all (no verdict),
 * G_IO_ERROR_INVALID_ARGUMENT (no usable relay in phase 1) / _CANCELLED as
 * above. */
void gh_mls_key_package_evidence_lookup_async(GhAccountController *accounts,
                                              const gchar *const *relays,
                                              const gchar *const *exclude,
                                              const gchar *pubkey, guint deadline,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);
GPtrArray *gh_mls_key_package_evidence_lookup_finish(GAsyncResult *result, GError **error);

G_END_DECLS
#endif
