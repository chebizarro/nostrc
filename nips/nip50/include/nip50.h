#ifndef NIP50_H
#define NIP50_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include "nostr-storage.h"
#include "nostr-filter.h"

/* NIP-50 Search: delegate to storage driver if implemented; else ENOTSUP. */
static inline int nostr_nip50_search(NostrStorage *st,
                                     const char *q,
                                     const NostrFilter *scope,
                                     size_t limit,
                                     void **it_out) {
  if (!it_out) return -EINVAL;
  *it_out = NULL;
  if (!st || !st->vt || !st->vt->search) return -ENOTSUP;
  return st->vt->search(st, q, scope, limit, it_out);
}

#ifdef __cplusplus
}
#endif

#endif /* NIP50_H */
