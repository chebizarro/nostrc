#ifndef FIPSD_NOSTR_INBOX_H
#define FIPSD_NOSTR_INBOX_H
#include "nostr/events.h"
typedef struct { size_t count; char relays[FIPSD_NOSTR_RELAYS_MAX][FIPSD_NOSTR_URL_MAX + 1]; } fipsd_nostr_inbox;
/* NIP-17 API, adapted to deployed FIPS bare r tags and explicit timestamp.
 * Readers accept both relay and bare r, ignore NIP-65 marked r, deduplicate. */
fipsd_nostr_status fipsd_nostr_inbox_build(const fips_identity *identity,
    const fipsd_nostr_inbox *inbox, uint64_t created_at_secs, NostrEvent **out);
fipsd_nostr_status fipsd_nostr_inbox_parse(const NostrEvent *event, fipsd_nostr_inbox *out);
#endif
