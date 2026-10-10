/* gnostr's GnNostrReferenceResolver over nostrdb (nostrc-8xfib.6): the NDB
 * note/profile lookups that used to live in the timeline factory's
 * repost/quote block, plus a relay fetch over the read relays. */
#ifndef GNOSTR_REFERENCE_RESOLVER_NDB_H
#define GNOSTR_REFERENCE_RESOLVER_NDB_H

#include <nostr-gtk-1.0/gn-nostr-reference-resolver.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_REFERENCE_RESOLVER_NDB (gnostr_reference_resolver_ndb_get_type())
G_DECLARE_FINAL_TYPE(GnostrReferenceResolverNdb, gnostr_reference_resolver_ndb,
                     GNOSTR, REFERENCE_RESOLVER_NDB, GObject)

/* The process-wide resolver (transfer none). */
GnNostrReferenceResolver *gnostr_reference_resolver_ndb_get_default(void);

/* Profile display_name, else name, from NDB; NULL when unknown. */
gchar *gnostr_reference_resolver_ndb_profile_field(const gchar *pubkey_hex,
                                                   const gchar *field);

G_END_DECLS
#endif
