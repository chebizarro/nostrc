#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Forward declarations for all types */
typedef struct _NostrEvent NostrEvent;
typedef struct _NostrFilter NostrFilter;
typedef struct _NostrTagList NostrTagList;
typedef struct _NostrRelay NostrRelay;
typedef struct _NostrPool NostrPool;
typedef struct _NostrSubscription NostrSubscription;
typedef struct _NostrKeys NostrKeys;

/* Autoptr cleanup functions - to be defined after type implementations */
/* Example pattern:
 * G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, g_object_unref)
 */

G_END_DECLS
