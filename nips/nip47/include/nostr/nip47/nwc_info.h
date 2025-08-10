#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build/parse helpers for NIP-47 Info (kind 13194).
 * Content is a JSON string object with key "methods": ["..."]
 * Tags include one or more ["encryption", ENC] and optional ["notifications", "true"|"false"].
 */

/* Build an Info event JSON string.
 * @pubkey: optional hex pubkey to embed (may be NULL)
 * @created_at: timestamp to set (use 0 to auto-fill with current time)
 * @methods: array of supported method strings
 * @methods_count: number of items in @methods
 * @encryptions: array of supported encryption labels (e.g. "nip44-v2", "nip04")
 * @enc_count: number of items in @encryptions
 * @notifications: whether notifications are supported (adds a tag)
 * @out_event_json: result JSON string (caller frees)
 * Returns 0 on success.
 */
int nostr_nwc_info_build(const char *pubkey,
                         long long created_at,
                         const char **methods,
                         size_t methods_count,
                         const char **encryptions,
                         size_t enc_count,
                         int notifications,
                         char **out_event_json);

/* Parse an Info event JSON string.
 * Extracts content.methods, encryption tags, and notifications tag.
 * Returned arrays are heap-allocated; caller must free each string and the array itself.
 */
int nostr_nwc_info_parse(const char *event_json,
                         char ***out_methods,
                         size_t *out_methods_count,
                         char ***out_encryptions,
                         size_t *out_enc_count,
                         int *out_notifications);

#ifdef __cplusplus
}
#endif
