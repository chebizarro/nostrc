#ifndef NOSTR_NIP77_NEGENTROPY_GLIB_H
#define NOSTR_NIP77_NEGENTROPY_GLIB_H

#include <stdbool.h>

#if defined(NOSTR_HAVE_GLIB)
#include <nostr/nip77/glib-export.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define NOSTR_TYPE_NEG_SESSION_G (nostr_neg_session_g_get_type())
NOSTR_NIP77_GLIB_API G_DECLARE_FINAL_TYPE(NostrNegSessionG, nostr_neg_session_g, NOSTR, NEG_SESSION_G, GObject)

/* Construction API mirroring the C layer (details in .c) */
NOSTR_NIP77_GLIB_API NostrNegSessionG *nostr_neg_session_g_new(void);

/* Signals:
 *  - "delta-ready" : void (GPtrArray *have, GPtrArray *need)
 */

G_END_DECLS
#endif /* NOSTR_HAVE_GLIB */

#endif /* NOSTR_NIP77_NEGENTROPY_GLIB_H */
