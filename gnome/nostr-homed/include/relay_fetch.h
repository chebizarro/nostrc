#ifndef RELAY_FETCH_H
#define RELAY_FETCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fetch the latest replaceable manifest JSON for the given namespace.
 * @author_hex: 64-char hex pubkey to filter by (REQUIRED for security).
 * @namespace_name: namespace "d" tag value (nullable, reserved).
 */
int nh_fetch_latest_manifest_json(const char **relays, size_t num_relays,
                                  const char *author_hex,
                                  const char *namespace_name,
                                  char **out_json);

/**
 * Fetch profile-provided relays (kind 30078).
 * @author_hex: 64-char hex pubkey to filter by (REQUIRED).
 * Returns 0 and allocates an array of strings in *out_relays
 * (caller must free each string and the array).
 */
int nh_fetch_profile_relays(const char **relays, size_t num_relays,
                            const char *author_hex,
                            char ***out_relays, size_t *out_count);

/**
 * Fetch the latest secrets envelope JSON (kind 30079).
 * @author_hex: 64-char hex pubkey to filter by (REQUIRED).
 * @namespace_name: namespace "d" tag value (nullable → "personal").
 * Returns a newly allocated JSON string in out_json.
 */
int nh_fetch_latest_secrets_json(const char **relays, size_t num_relays,
                                 const char *author_hex,
                                 const char *namespace_name,
                                 char **out_json);

/**
 * Fetch the latest kind-0 (user metadata) event for the given pubkey and
 * return its VERIFIED, RE-SERIALIZED compact JSON representation. The
 * returned JSON is a full Nostr event object (id/pubkey/kind/created_at/
 * tags/content/sig); the signature is validated against the event id and
 * the event's pubkey is checked to equal @author_hex before delivery. A
 * kind-0 event whose signature does not verify, whose pubkey mismatches,
 * or whose kind is not 0 is silently skipped and the caller sees a
 * fetch-timeout instead — never an unverified event.
 *
 * @author_hex: 64-char lowercase-hex xonly pubkey (REQUIRED).
 * @out_event_json: on success, receives a newly-allocated compact JSON
 *                  string (caller owns and free()s).
 * Returns 0 on success, -1 on timeout / no verified event within the
 * fetch window / OOM.
 */
int nh_fetch_latest_kind0_verified(const char **relays, size_t num_relays,
                                   const char *author_hex,
                                   char **out_event_json);

#ifdef __cplusplus
}
#endif

#endif /* RELAY_FETCH_H */
