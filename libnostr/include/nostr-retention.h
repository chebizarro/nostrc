#ifndef NOSTR_RETENTION_H
#define NOSTR_RETENTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
  uint32_t ttl_days;
  uint32_t size_gb;
} NostrRetentionPolicy;

/* Apply a retention policy hint; actual deletion is storage-driver-specific. */
int nostr_retention_apply(const NostrRetentionPolicy *policy);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_RETENTION_H */
