#ifndef FIPSD_PEER_IDENTITIES_H
#define FIPSD_PEER_IDENTITIES_H
#include <stdbool.h>
#include <fips/identity/local.h>
#include <fips/identity/peer.h>

/* Local key owners, dispatcher-only. Successful add transfers ownership;
 * duplicate/full failure does not. The secp/entropy contexts outlive the set.
 * Remove/destroy only after all rekey Env/local-key borrowers have quiesced. */
typedef struct fipsd_identities fipsd_identities;
fipsd_identities *fipsd_identities_create(size_t capacity);
void fipsd_identities_destroy(fipsd_identities *identities);
bool fipsd_identities_add(fipsd_identities *identities, fips_identity *identity);
const fips_identity *fipsd_identities_find(const fipsd_identities *identities, const fips_node_addr *addr);
bool fipsd_identities_remove(fipsd_identities *identities, const fips_node_addr *addr);
size_t fipsd_identities_count(const fipsd_identities *identities);
/* Public-only upstream IdentityCache analogue, bounded by the host. A touch
 * stamps last use; replacing a prefix changes its key. Adapter feed belongs
 * to the future node/TUN owner, not this local identity set. */
typedef struct fipsd_identity_entry { fips_peer_identity peer; uint64_t used_ms; } fipsd_identity_entry;
typedef struct fipsd_identity_cache {
    fipsd_identity_entry *entries;
    size_t count, capacity;
} fipsd_identity_cache;
void fipsd_identity_cache_init(fipsd_identity_cache *cache, fipsd_identity_entry *entries, size_t capacity);
bool fipsd_identity_cache_put(fipsd_identity_cache *cache, fips_peer_identity peer, uint64_t now_ms);
const fips_peer_identity *fipsd_identity_cache_touch(fipsd_identity_cache *cache, fips_node_prefix prefix, uint64_t now_ms);
bool fipsd_identity_cache_remove(fipsd_identity_cache *cache, fips_node_prefix prefix);
#endif
