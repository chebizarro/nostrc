#ifndef NOSTR_SECURE_BUF_H
#define NOSTR_SECURE_BUF_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Secure buffer utilities for secret material.
 * - Best-effort mlock to keep out of swap.
 * - Explicit wipe on free.
 * - Constant-time memcmp.
 */

typedef struct nostr_secure_buf {
  void *ptr;
  size_t len;
  bool locked;
} nostr_secure_buf;

/* Allocate a secure buffer of size 'len'. Returns {NULL,0,false} on failure. */
nostr_secure_buf secure_alloc(size_t len);

/* Wipe and free a secure buffer. Safe on {NULL,0}. */
void secure_free(nostr_secure_buf *sb);

/* Explicitly wipe memory region. */
void secure_wipe(void *p, size_t n);

/* Constant-time compare. Returns 0 if equal, nonzero otherwise. */
int secure_memcmp_ct(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_SECURE_BUF_H */
