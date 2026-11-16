#ifndef FIPSD_NOSTR_EVENTS_H
#define FIPSD_NOSTR_EVENTS_H
#include <fips/identity/local.h>
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <stddef.h>
#include <stdint.h>

#define FIPSD_NOSTR_IDENTIFIER "fips-overlay-v1"
#define FIPSD_NOSTR_SKEW_MS UINT64_C(60000)
/* Host safety ceilings, not limits claimed by the upstream wire schema. */
#define FIPSD_NOSTR_EVENT_MAX 65536
#define FIPSD_NOSTR_BODY_MAX 16384
#define FIPSD_NOSTR_ENDPOINTS_MAX 32
#define FIPSD_NOSTR_RELAYS_MAX 16
#define FIPSD_NOSTR_STUN_MAX 8
#define FIPSD_NOSTR_ADDR_MAX 256
#define FIPSD_NOSTR_URL_MAX 1024
#define FIPSD_NOSTR_SESSION_MAX 128
#define FIPSD_NOSTR_REASON_MAX 256
#define FIPSD_NOSTR_CANDIDATES_MAX 32

typedef enum {
    FIPSD_NOSTR_OK, FIPSD_NOSTR_INVALID, FIPSD_NOSTR_LIMIT,
    FIPSD_NOSTR_CRYPTO, FIPSD_NOSTR_STALE, FIPSD_NOSTR_FUTURE
} fipsd_nostr_status;
typedef enum { FIPSD_NOSTR_FRESH, FIPSD_NOSTR_SKEW } fipsd_nostr_freshness;

/* Pure event boundary: explicit-length JSON, bounded preflight before libnostr
 * allocation, strict signed shape, canonical id and signature verification.
 * On any failure, output arguments remain untouched. Event outputs are owned
 * by the caller (nostr_event_free); serialized strings use free(). No I/O. */
fipsd_nostr_status fipsd_nostr_event_parse(const char *json, size_t len, NostrEvent **out);
/* Bounded, caller-owned replay set. Insert ONLY after successful admission.
 * Full is not eviction: callers choose an explicit generation/expiry boundary. */
typedef struct { uint8_t (*ids)[32]; size_t count, capacity; } fipsd_nostr_seen;
bool fipsd_nostr_seen_contains(const fipsd_nostr_seen *seen, const uint8_t id[32]);
bool fipsd_nostr_seen_insert(fipsd_nostr_seen *seen, const uint8_t id[32]);
bool fipsd_nostr_event_id(const NostrEvent *event, uint8_t id[32]);
/* Serialize in upstream v0.44 field order. Signature bytes are randomized by
 * libnostr, so differential rebuilds compare the unsigned projection + id. */
char *fipsd_nostr_event_json(const NostrEvent *event, bool include_signature);
#endif
