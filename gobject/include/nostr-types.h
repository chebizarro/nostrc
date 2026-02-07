#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Forward declarations for core libnostr types (plain C structs) */
typedef struct _NostrEvent NostrEvent;
typedef struct _NostrFilter NostrFilter;
typedef struct _NostrTagList NostrTagList;
typedef struct _NostrRelay NostrRelay;
typedef struct _NostrPool NostrPool;
typedef struct _NostrSubscription NostrSubscription;
typedef struct _NostrKeys NostrKeys;

/* Forward declarations for GObject wrapper types (prefixed with G) */
typedef struct _GNostrEvent GNostrEvent;
typedef struct _GNostrRelay GNostrRelay;
typedef struct _GNostrFilter GNostrFilter;

/* Autoptr cleanup functions for GObject wrappers */
/* Note: G_DEFINE_AUTOPTR_CLEANUP_FUNC is defined in each type's header */

G_END_DECLS
