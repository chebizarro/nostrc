#ifndef NIP45_H
#define NIP45_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include "nostr-storage.h"
#include "nostr-filter.h"

/* NIP-45 COUNT: ask storage driver for count of events matching filters. */
static inline int nostr_nip45_count(NostrStorage *st,
                                    const NostrFilter *filters, size_t nfilters,
                                    uint64_t *out_count) {
  if (!st || !st->vt || !st->vt->count || !out_count) return -EINVAL;
  return st->vt->count(st, filters, nfilters, out_count);
}

#ifdef __cplusplus
}
#endif

#endif /* NIP45_H */
