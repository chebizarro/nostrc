#ifndef GH_NIP05_H
#define GH_NIP05_H

#include "gh-net-http.h"

G_BEGIN_DECLS

/*
 * NIP-05 lookup (privacy charter PD-2, §7.9, G18): name@domain to a public
 * key, only when the user chose "Look up name@domain" in New Message, whose
 * row says that it connects to domain. It is one GET of
 * https://<domain>/.well-known/nostr.json?name=<name> through a
 * GhHttpTransport (by default GhNetHttp, in the configured network mode): no
 * redirect is followed (NIP-05), the answer is bounded to
 * GH_NIP05_MAX_DOCUMENT bytes, and only its "names" entry for the name is
 * read. The document's "relays" hints are ignored: where a message goes is
 * decided by the person's own signed inbox list (charter P1). Nothing is
 * cached or stored. In Tor mode the lookup goes through the Tor proxy
 * (GhNetHttp); a .onion domain is looked up only there, over http (the onion
 * address authenticates the service), and refused in any other mode (NT-8).
 */

#define GH_NIP05_MAX_DOCUMENT (64 * 1024)

#define GH_NIP05_ERROR (gh_nip05_error_quark())
GQuark gh_nip05_error_quark(void);

typedef enum {
  GH_NIP05_ERROR_ADDRESS,      /* not a NIP-05 address */
  GH_NIP05_ERROR_NOT_FOUND,    /* the domain does not list the name */
  GH_NIP05_ERROR_RESPONSE,     /* not a valid nostr.json document */
  GH_NIP05_ERROR_NETWORK_MODE  /* the network mode does not allow it */
} GhNip05Error;

/* The https URL looked up for address; NULL with GH_NIP05_ERROR_ADDRESS. */
gchar *gh_nip05_dup_url(const gchar *address, GError **error);
/* The lowercase hex pubkey that document lists for local (lower case). */
gchar *gh_nip05_parse_document(GBytes *document, const gchar *local, GError **error);

#define GH_TYPE_NIP05 (gh_nip05_get_type())
G_DECLARE_FINAL_TYPE(GhNip05, gh_nip05, GH, NIP05, GObject)

/* settings (nullable) supplies network-mode; transport NULL: a GhNetHttp. */
GhNip05 *gh_nip05_new(GSettings *settings, const GhHttpTransport *transport,
                      gpointer transport_data);

void gh_nip05_lookup_async(GhNip05 *self, const gchar *address, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data);
/* The lowercase hex pubkey; NULL with a GH_NIP05_ERROR, a G_IO_ERROR from
 * the transport, or G_IO_ERROR_CANCELLED. */
gchar *gh_nip05_lookup_finish(GhNip05 *self, GAsyncResult *result, GError **error);

G_END_DECLS
#endif
