#ifndef NOSTRDB_STORAGE_H
#define NOSTRDB_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-storage.h"

NostrStorage* nostrdb_storage_new(void);

/* Relay-only nonblocking write interface; the ordinary storage vtable remains
 * synchronous for callers that need a confirmed put_event(). */
struct RelaydAsyncStorageOps;
const struct RelaydAsyncStorageOps *nostrdb_storage_async_ops(void);

#ifdef __cplusplus
}
#endif

#endif /* NOSTRDB_STORAGE_H */
