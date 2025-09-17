#ifndef GOA_NOSTR_PROVIDER_H
#define GOA_NOSTR_PROVIDER_H

#include <glib-object.h>
#include <goabackend/goabackend.h>

G_BEGIN_DECLS

#define GOA_TYPE_NOSTR_PROVIDER (goa_nostr_provider_get_type())
G_DECLARE_FINAL_TYPE(GoaNostrProvider, goa_nostr_provider, GOA, NOSTR_PROVIDER, GoaProvider)

G_END_DECLS

#endif /* GOA_NOSTR_PROVIDER_H */
