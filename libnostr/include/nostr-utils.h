#ifndef __NOSTR_UTILS_H__
#define __NOSTR_UTILS_H__

/* GLib-friendly transitional header for utils */

#include "utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provide alias names in the nostr_* namespace for consistency. */
/**
 * nostr_memhash:
 * @data: (transfer none): buffer
 * @len: length
 *
 * Returns: hash value
 */
#define nostr_memhash                 memhash
/**
 * nostr_named_lock:
 * @name: (transfer none): lock name
 */
#define nostr_named_lock              named_lock
/**
 * nostr_similar:
 * @a: (transfer none)
 * @b: (transfer none)
 *
 * Returns: %TRUE if similar
 */
#define nostr_similar                 similar
/**
 * nostr_escape_string:
 * @s: (transfer none): string to escape
 *
 * Returns: (transfer full): newly-allocated escaped string
 */
#define nostr_escape_string           escape_string
/**
 * nostr_pointer_values_equal:
 * @a: (transfer none): pointer value
 * @b: (transfer none): pointer value
 *
 * Returns: %TRUE if equal
 */
#define nostr_pointer_values_equal    are_pointer_values_equal
/**
 * nostr_normalize_url:
 * @in: (transfer none): input URL
 *
 * Returns: (transfer full): normalized URL or %NULL
 */
#define nostr_normalize_url           normalize_url
/**
 * nostr_normalize_ok_message:
 * @in: (transfer none): OK message
 *
 * Returns: (transfer full): normalized message or %NULL
 */
#define nostr_normalize_ok_message    normalize_ok_message
/**
 * nostr_hex2bin:
 * @hex: (transfer none): hex input
 * @out: (out caller-allocates) (transfer none): binary buffer
 * @out_len: (out): length written
 *
 * Returns: 0 on success
 */
#define nostr_hex2bin                 hex2bin
/**
 * nostr_sub_id_to_serial:
 * @sub_id: (transfer none)
 *
 * Returns: serial number or -1 on error
 */
#define nostr_sub_id_to_serial        sub_id_to_serial

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_UTILS_H__ */
