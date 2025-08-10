#ifndef __NOSTR_KEYS_H__
#define __NOSTR_KEYS_H__

/* Public header: standardized names for key utilities (no legacy macros). */

#include <stdbool.h>
#include "keys.h" /* underlying implementations */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical API */
char *nostr_key_generate_private(void);
char *nostr_key_get_public(const char *sk);
bool  nostr_key_is_valid_public_hex(const char *pk);
bool  nostr_key_is_valid_public(const char *pk);

/* No legacy aliases retained. */

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_KEYS_H__ */
