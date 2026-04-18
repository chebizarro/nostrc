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

#ifdef __cplusplus
}
#endif

#endif /* RELAY_FETCH_H */
