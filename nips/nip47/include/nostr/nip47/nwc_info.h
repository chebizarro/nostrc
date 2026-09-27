#ifndef NIPS_NIP47_NOSTR_NIP47_NWC_INFO_H
#define NIPS_NIP47_NOSTR_NIP47_NWC_INFO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build/parse helpers for NIP-47 Info (kind 13194), per NIP-47:
 *   content:  plaintext space-separated capabilities,
 *             e.g. "pay_invoice get_balance notifications"
 *   tags:     ["encryption", "nip44_v2 nip04"]
 *             ["notifications", "payment_received payment_sent"] (if any)
 */

/* Build an Info event JSON string.
 * @pubkey: optional hex pubkey to embed (may be NULL)
 * @created_at: timestamp to set (use 0 to auto-fill with current time)
 * @methods: supported capabilities (method names, optionally "notifications");
 *           tokens must be non-empty and contain no whitespace
 * @methods_count: number of items in @methods (>= 1)
 * @encryptions: supported encryption labels (e.g. "nip44_v2", "nip04"); emitted as one space-separated `encryption` tag per NIP-47
 * @enc_count: number of items in @encryptions
 * @notification_types: notification types sent (e.g. "payment_received"),
 *           or NULL; when non-empty a `notifications` tag is added and the
 *           "notifications" capability is appended to content if missing
 * @notif_count: number of items in @notification_types
 * @out_event_json: result JSON string (caller frees)
 * Returns 0 on success, -1 on invalid input.
 */
int nostr_nwc_info_build(const char *pubkey,
                         long long created_at,
                         const char **methods,
                         size_t methods_count,
                         const char **encryptions,
                         size_t enc_count,
                         const char **notification_types,
                         size_t notif_count,
                         char **out_event_json);

/* Parse an Info event JSON string.
 * Capabilities come from the plaintext content (the legacy nostrc
 * {"methods":[...]} content is still accepted); fails if there are none.
 * Encryption and notification types are split from their tags (a legacy
 * boolean notifications value yields no types). Duplicates are dropped.
 * Returned arrays are heap-allocated; caller must free each string and the
 * array itself. Every out-parameter is optional.
 */
int nostr_nwc_info_parse(const char *event_json,
                         char ***out_methods,
                         size_t *out_methods_count,
                         char ***out_encryptions,
                         size_t *out_enc_count,
                         char ***out_notification_types,
                         size_t *out_notif_count);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP47_NOSTR_NIP47_NWC_INFO_H */
