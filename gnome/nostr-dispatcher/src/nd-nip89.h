/* nd-nip89 — hook for NIP-89 (kind 31990) handler discovery. */
#ifndef ND_NIP89_H
#define ND_NIP89_H

#include <glib.h>

G_BEGIN_DECLS

/* Return a desktop id (or, later, a web-handler URL template) able to
 * handle @kind, discovered via NIP-89 recommendations, or NULL.
 * Currently a stub that always returns NULL — see nd-nip89.c. */
char *nd_nip89_discover(guint32 kind);

G_END_DECLS

#endif /* ND_NIP89_H */
