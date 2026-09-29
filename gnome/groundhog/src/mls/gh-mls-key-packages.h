#ifndef GH_MLS_KEY_PACKAGES_H
#define GH_MLS_KEY_PACKAGES_H

#include "gh-account-controller.h"

G_BEGIN_DECLS

/*
 * GhMlsKeyPackageLookup (nostrc-qp24.13): finds the KeyPackage (kind 30443,
 * MIP-00) to invite one person with. GTK-free, main context only.
 *
 * Where (Marmot transports/nostr.md: KeyPackages live on the author's
 * kind-10002 write set; privacy charter §2.2 "Discovery relays: whose
 * KeyPackages you fetch", §4.3 "Contact directory"):
 *  1. one URL-scoped REQ {kinds:[10002, 30443], authors:[person]} to the
 *     discovery relays the caller gives (the discovery-relays setting), then
 *  2. when a kind-10002 by that person came back, one REQ
 *     {kinds:[30443], authors:[person]} to its write relays not asked yet
 *     (at most GH_MLS_KEY_PACKAGE_MAX_WRITE_RELAYS).
 * Nothing else is contacted, never the account's own relays as such. Each
 * phase is one fresh scope with GhAuthPolicy's CONTACT_DIRECTORY identity:
 * an ephemeral key if a relay demands AUTH, never the account (R1). A phase
 * ends when every relay sent EOSE or failed; the deadline only bounds a
 * silent relay.
 *
 * Which: every signed kind 30443 authored by the person goes to
 * marmot_select_key_package_event() (the addressable-slot rules: newest per
 * (pubkey, d) slot, full KeyPackage validation, then newest), and the
 * winner is the result.
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
  guint sources;      /* relays asked, both phases */
  guint answered;     /* relays that sent EOSE */
} GhMlsKeyPackage;

void gh_mls_key_package_free(GhMlsKeyPackage *key_package);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhMlsKeyPackage, gh_mls_key_package_free)

/* deadline: seconds per phase (0: 15). */
void gh_mls_key_package_lookup_async(GhAccountController *accounts,
                                     const gchar *const *discovery_relays,
                                     const gchar *pubkey, guint deadline,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
GhMlsKeyPackage *gh_mls_key_package_lookup_finish(GAsyncResult *result, GError **error);

G_END_DECLS
#endif
