#ifndef NOSTR_GOBJECT_NOSTR_TYPES_H
#define NOSTR_GOBJECT_NOSTR_TYPES_H

#include <glib-object.h>

G_BEGIN_DECLS

/*
 * Forward declarations for GObject wrapper types (G-prefixed).
 * These are safe to include alongside core libnostr headers.
 *
 * Note: Core libnostr types (NostrEvent, NostrFilter, NostrRelay, etc.)
 * are declared in their own headers under libnostr/include/.
 * Do NOT forward-declare them here — struct tag mismatches cause
 * typedef redefinition errors.
 */

/* GObject wrapper forward declarations */
typedef struct _GNostrEvent GNostrEvent;
typedef struct _GNostrRelay GNostrRelay;
typedef struct _GNostrFilter GNostrFilter;
typedef struct _GNostrPool GNostrPool;
typedef struct _GNostrKeys GNostrKeys;
typedef struct _GNostrSubscription GNostrSubscription;

/* Autoptr cleanup functions for GObject wrappers */
/* Note: G_DEFINE_AUTOPTR_CLEANUP_FUNC is defined in each type's header */

G_END_DECLS
#endif /* NOSTR_GOBJECT_NOSTR_TYPES_H */
