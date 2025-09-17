#ifndef GOA_NOSTR_PROVIDER_H
#define GOA_NOSTR_PROVIDER_H

#define GOA_API_IS_SUBJECT_TO_CHANGE 1
#define GOA_BACKEND_API_IS_SUBJECT_TO_CHANGE 1
#include <glib-object.h>
#include <goabackend/goabackend.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(GoaNostrProvider, goa_nostr_provider, GOA, NOSTR_PROVIDER, GoaPasswordProvider)

G_END_DECLS

#endif /* GOA_NOSTR_PROVIDER_H */
