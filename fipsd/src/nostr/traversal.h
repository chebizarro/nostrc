#ifndef FIPSD_NOSTR_TRAVERSAL_H
#define FIPSD_NOSTR_TRAVERSAL_H
#include "nostr/events.h"
typedef struct { char protocol[17], ip[65]; uint16_t port; } fipsd_nostr_address;
typedef struct { uint64_t start_at_ms, interval_ms, duration_ms; } fipsd_nostr_punch;
typedef struct {
    bool answer, accepted, has_reflexive, has_stun, has_punch, has_reason, has_offer_received_at;
    char session_id[FIPSD_NOSTR_SESSION_MAX + 1], nonce[FIPSD_NOSTR_SESSION_MAX + 1];
    char sender_npub[64], recipient_npub[64], in_reply_to[FIPSD_NOSTR_SESSION_MAX + 1];
    uint64_t issued_at, expires_at, offer_received_at;
    fipsd_nostr_address reflexive, local[FIPSD_NOSTR_CANDIDATES_MAX];
    size_t local_count;
    char stun[FIPSD_NOSTR_URL_MAX + 1], reason[FIPSD_NOSTR_REASON_MAX + 1];
    fipsd_nostr_punch punch;
} fipsd_nostr_traversal;
char *fipsd_nostr_traversal_content(const fipsd_nostr_traversal *payload);
fipsd_nostr_status fipsd_nostr_traversal_parse_content(const char *json, size_t len,
    fipsd_nostr_traversal *out);
fipsd_nostr_status fipsd_nostr_traversal_freshness(uint64_t issued_at, uint64_t expires_at,
    uint64_t now_ms, uint64_t ttl_ms, fipsd_nostr_freshness *out);
fipsd_nostr_status fipsd_nostr_traversal_answer_for_offer(const fipsd_nostr_traversal *offer,
    const fipsd_nostr_traversal *answer, uint64_t now_ms, uint64_t ttl_ms,
    fipsd_nostr_freshness *out);
/* Uses the identity's ONLY key for the seal, and a fresh CSPRNG key per wrap.
 * NIP-59 kind-1059 output is re-kinded to ephemeral FIPS 21059, given NIP-40
 * expiration, then re-signed with the SAME one-time key. No RNG injection in
 * production; deterministic property tests exercise the pure payload surface. */
fipsd_nostr_status fipsd_nostr_traversal_build(const fips_identity *identity,
    const char *recipient_pubkey_hex, const fipsd_nostr_traversal *payload,
    uint64_t created_at_secs, uint64_t ttl_secs, NostrEvent **out);
/* Authenticates outer event, recipient, seal, rumor id/author/kind and payload
 * identities before freshness admission. Outer NIP-40 expires without grace;
 * only the authenticated payload's millisecond stamps get skew tolerance. */
fipsd_nostr_status fipsd_nostr_traversal_open(const fips_identity *recipient,
    const NostrEvent *event, uint64_t now_ms, uint64_t ttl_ms,
    fipsd_nostr_traversal *out, fipsd_nostr_freshness *freshness);
#define FIPSD_PUNCH_SIZE 24u
typedef struct { bool ack; uint32_t sequence; uint8_t hash[16]; } fipsd_punch_packet;
bool fipsd_is_punch_packet(const uint8_t *, size_t);
void fipsd_punch_build(bool ack, uint32_t sequence, const char *session, uint8_t out[FIPSD_PUNCH_SIZE]);
bool fipsd_punch_parse(const uint8_t *, size_t, fipsd_punch_packet *);
#endif
