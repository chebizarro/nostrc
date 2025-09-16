#ifndef GOA_NOSTR_PROVIDER_H
#define GOA_NOSTR_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

define_type_id (GoaNostrProvider)

typedef struct _GoaNostrProvider GoaNostrProvider;
typedef struct _GoaNostrProviderClass GoaNostrProviderClass;

struct _GoaNostrProvider {
  GObject parent_instance;
};

struct _GoaNostrProviderClass {
  GObjectClass parent_class;
};

GType goa_nostr_provider_get_type(void);

G_END_DECLS

#endif /* GOA_NOSTR_PROVIDER_H */
