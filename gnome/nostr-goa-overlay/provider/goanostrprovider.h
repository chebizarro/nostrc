#ifndef GOA_NOSTR_PROVIDER_H
#define GOA_NOSTR_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _GoaNostrProvider GoaNostrProvider;
typedef struct _GoaNostrProviderClass GoaNostrProviderClass;

GType goa_nostr_provider_get_type(void);
#define GOA_TYPE_NOSTR_PROVIDER (goa_nostr_provider_get_type())

G_END_DECLS

#endif /* GOA_NOSTR_PROVIDER_H */
