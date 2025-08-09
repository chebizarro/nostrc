#ifndef __NOSTR_CORE_H__
#define __NOSTR_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-config.h"

/* Core, dependency-light public API surface. For now, include existing public headers.
 * As we refactor, these will be reorganized into hyphenated headers per module. */
#include "kinds.h"
#include "connection.h"
#include "envelope.h"
#include "nostr-envelope.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-connection.h"
#include "json.h"
#include "nostr-json.h"
#include "nostr-relay.h"
#include "nostr-relay-store.h"
#include "nostr-subscription.h"
#include "nostr-tag.h"
#include "timestamp.h"
#include "nostr-timestamp.h"
#include "nostr-keys.h"
#include "utils.h"
#include "nostr-simple-pool.h"
#include "nostr-pointer.h"
#include "nostr-utils.h"
#include "nostr-event-extra.h"

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_CORE_H__ */
