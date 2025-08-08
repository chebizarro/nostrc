#ifndef __NOSTR_KEYS_H__
#define __NOSTR_KEYS_H__

/* Transitional header: standardized names for key utilities. */

#include <stdbool.h>
#include "keys.h" /* legacy declarations */

#ifdef __cplusplus
extern "C" {
#endif

#define nostr_key_generate_private      generate_private_key
#define nostr_key_get_public            get_public_key
#define nostr_key_is_valid_public_hex   is_valid_public_key_hex
#define nostr_key_is_valid_public       is_valid_public_key

#ifdef NOSTR_ENABLE_LEGACY_ALIASES
#  define generate_private_key          nostr_key_generate_private
#  define get_public_key                nostr_key_get_public
#  define is_valid_public_key_hex       nostr_key_is_valid_public_hex
#  define is_valid_public_key           nostr_key_is_valid_public
#endif

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_KEYS_H__ */
