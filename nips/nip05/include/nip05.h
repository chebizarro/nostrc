#ifndef NOSTR_NIP05_H
#define NOSTR_NIP05_H

#include <stdbool.h>
#include <stddef.h>

/*
 * NIP-05 (Mapping Nostr Identifiers to Pubkeys)
 *
 * Functions return 0 on success and non-zero on error.
 * Any out_* pointer returned must be freed by the caller using free().
 */

/* Parse an identifier like "name@domain" or "_@domain" or just "domain".
 * - On success, out_name and out_domain are allocated (lower-cased).
 * - If no localpart is present, out_name is set to "_".
 */
int nostr_nip05_parse_identifier(const char *identifier, char **out_name, char **out_domain);

/* Fetch the raw nostr.json document (as a string) from a domain.
 * - Uses HTTPS by default. Honors env NIP05_TIMEOUT_MS. For tests,
 *   NIP05_ALLOW_INSECURE=1 disables TLS verification.
 */
int nostr_nip05_fetch_json(const char *domain, char **out_json, char **out_error);

/* Resolve identifier to pubkey and optional relays.
 * - Tries https://domain/.well-known/nostr.json?name=<name> first,
 *   falling back to fetching the full document if necessary.
 * - out_relays is an array of strings (URLs); count in out_relays_count.
 */
int nostr_nip05_lookup(const char *identifier,
                       char **out_hexpub,
                       char ***out_relays,
                       size_t *out_relays_count,
                       char **out_error);

/* Resolve from an already-fetched nostr.json string (no network).
 * name should be the local part (e.g. "_" for domain-only identifiers).
 */
int nostr_nip05_resolve_from_json(const char *name,
                                  const char *json,
                                  char **out_hexpub,
                                  char ***out_relays,
                                  size_t *out_relays_count,
                                  char **out_error);

/* Validate that identifier maps to the given hex pubkey. Sets *out_match=1/0. */
int nostr_nip05_validate(const char *identifier,
                         const char *hexpub,
                         int *out_match,
                         char **out_error);

#endif // NOSTR_NIP05_H
