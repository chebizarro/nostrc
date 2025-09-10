#ifndef RELAY_FETCH_H
#define RELAY_FETCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fetch the latest replaceable manifest JSON for the given namespace across relays. */
int nh_fetch_latest_manifest_json(const char **relays, size_t num_relays,
                                  const char *namespace_name,
                                  char **out_json);

/* Fetch profile-provided relays (kind 30078). Returns 0 and allocates an array
 * of strings in *out_relays (caller must free each string and the array) and
 * sets *out_count. Returns -1 if none found or on error. */
int nh_fetch_profile_relays(const char **relays, size_t num_relays,
                            char ***out_relays, size_t *out_count);

/* Fetch the latest secrets envelope JSON (kind 30079). Returns a newly
 * allocated JSON string in out_json. */
int nh_fetch_latest_secrets_json(const char **relays, size_t num_relays,
                                 char **out_json);

#ifdef __cplusplus
}
#endif

#endif /* RELAY_FETCH_H */
