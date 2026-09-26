/*
 * nsp-nip05 — NIP-05 identifier → pubkey for the search provider.
 *
 * Reuses nostr-homed's syntactic validator, .well-known URL builder,
 * response parser and positive/negative TTL cache
 * (gnome/nostr-homed/src/nip05/nip05_validate.c, nip05_cache.c; compiled
 * in, not linked, so this package does not pull in the login broker).
 * nip05_client.c is NOT reused: it fork/execs a helper that drops root
 * privileges, which is meaningless for a per-user session service. The
 * transport is nsp-http instead, with the same policy as that helper:
 * https only, NO redirects (NIP-05 requires ignoring them), 64 KiB body
 * cap, non-public peers refused at connect time.
 *
 * Concurrent look-ups of one address share a single request, and a
 * request keeps running after the search that started it hit its
 * deadline, so the answer lands in the cache for the next keystroke.
 */
#ifndef NSP_NIP05_H
#define NSP_NIP05_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NSP_NIP05_TIMEOUT_S 5
#define NSP_NIP05_MAX_BODY (64 * 1024)

typedef struct _NspNip05 NspNip05;

NspNip05 *nsp_nip05_new(void);
void nsp_nip05_free(NspNip05 *r);

/* Cache only. Returns TRUE with @pk_out (65 bytes) filled on a positive
 * hit; sets *@negative when a recent failure suppresses re-resolution. */
gboolean nsp_nip05_lookup_cached(NspNip05 *r, const char *address, char pk_out[65],
                                 gboolean *negative);

/* Seed the cache (tests, and nothing else). */
void nsp_nip05_cache_put(NspNip05 *r, const char *address, const char *pubkey_hex);

/* Resolve @local@@domain over HTTPS. Completes with a pubkey (hex) or an
 * error. Result is cached either way. */
void nsp_nip05_resolve_async(NspNip05 *r, const char *local, const char *domain,
                             GAsyncReadyCallback callback, gpointer user_data);
char *nsp_nip05_resolve_finish(GAsyncResult *res, GError **error);

G_END_DECLS

#endif /* NSP_NIP05_H */
