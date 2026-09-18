#ifndef NOSTR_NIP_CAS0010_OTEL_INTERNAL_H
#define NOSTR_NIP_CAS0010_OTEL_INTERNAL_H

#include "nostr/nip_cas0010/otel.h"

/* Returns the single tag named @name, requiring exactly one occurrence with
 * exactly two elements, and yields its value in @out_value (borrowed).
 * Returns NOSTR_OTEL_OK or NOSTR_OTEL_ERR_MALFORMED_EVENT. */
int nostr_otel_required_tag_value(const NostrTags *tags, const char *name,
                                  const char **out_value);

/* Returns the value of the first tag named @name, or NULL. */
const char *nostr_otel_optional_tag_value(const NostrTags *tags, const char *name);

/* ------------------------------------------------- replay (seen-id) cache */

/**
 * Bounded, TTL'd LRU over event ids. Internal to the module: created by the
 * consumer and owned by it (each _new has a matching _free).
 */
typedef struct NostrOtelSeenCache NostrOtelSeenCache;

/**
 * nostr_otel_seen_cache_new:
 * @max_entries: cap on remembered ids; 0 => NOSTR_OTEL_DEFAULT_SEEN_CACHE_SIZE
 * @ttl_seconds: how long an id is remembered; <= 0 => twice the default window
 *
 * Returns: (transfer full) (nullable): cache freed with
 * nostr_otel_seen_cache_free().
 */
NostrOtelSeenCache *nostr_otel_seen_cache_new(size_t max_entries, int64_t ttl_seconds);

/** @cache: (transfer full) (nullable) */
void nostr_otel_seen_cache_free(NostrOtelSeenCache *cache);

/** Returns: the number of remembered ids (diagnostics and tests). */
size_t nostr_otel_seen_cache_count(NostrOtelSeenCache *cache);

/**
 * nostr_otel_seen_cache_observe:
 * @cache: (transfer none)
 * @id: (transfer none): hex event id, at most 64 characters
 * @now: current unix time in seconds
 * @out_seen: (out): whether @id was already remembered and had not expired
 *
 * Records @id, evicting expired and least-recently-seen ids as needed, so the
 * cache never holds more than its configured maximum.
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_seen_cache_observe(NostrOtelSeenCache *cache, const char *id, int64_t now,
                                  bool *out_seen);

#endif /* NOSTR_NIP_CAS0010_OTEL_INTERNAL_H */
