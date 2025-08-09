#ifndef __NOSTR_UTILS_H__
#define __NOSTR_UTILS_H__

/* GLib-friendly transitional header for utils */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provide GI-friendly function prototypes in the nostr_* namespace. */

/**
 * nostr_memhash:
 * @data: (transfer none): buffer
 * @len: length
 *
 * Returns: hash value
 */
uint64_t nostr_memhash(const char *data, size_t len);

/**
 * nostr_named_lock:
 * @name: (transfer none): lock name
 * @critical_section: (scope call): function to run while holding lock
 * @arg: (closure): user data
 */
void nostr_named_lock(const char *name, void (*critical_section)(void *), void *arg);

/**
 * nostr_similar:
 * @a: (transfer none)
 * @a_len: length of @a
 * @b: (transfer none)
 * @b_len: length of @b
 *
 * Returns: %TRUE if similar
 */
bool nostr_similar(const int *a, size_t a_len, const int *b, size_t b_len);

/**
 * nostr_escape_string:
 * @s: (transfer none): string to escape
 *
 * Returns: (transfer full): newly-allocated escaped string
 */
char *nostr_escape_string(const char *s);

/**
 * nostr_pointer_values_equal:
 * @a: (transfer none): pointer value
 * @b: (transfer none): pointer value
 * @size: size in bytes
 *
 * Returns: %TRUE if equal
 */
bool nostr_pointer_values_equal(const void *a, const void *b, size_t size);

/**
 * nostr_normalize_url:
 * @in: (transfer none): input URL
 *
 * Returns: (transfer full): normalized URL or %NULL
 */
char *nostr_normalize_url(const char *in);

/**
 * nostr_normalize_ok_message:
 * @reason: (transfer none)
 * @prefix: (transfer none)
 *
 * Returns: (transfer full): normalized message or %NULL
 */
char *nostr_normalize_ok_message(const char *reason, const char *prefix);

/**
 * nostr_hex2bin:
 * @bin: (out caller-allocates): output buffer
 * @hex: (transfer none): hex input
 * @bin_len: size of @bin
 *
 * Returns: %TRUE on success
 */
bool nostr_hex2bin(unsigned char *bin, const char *hex, size_t bin_len);

/**
 * nostr_sub_id_to_serial:
 * @sub_id: (transfer none)
 *
 * Returns: serial number or -1 on error
 */
int64_t nostr_sub_id_to_serial(const char *sub_id);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_UTILS_H__ */
