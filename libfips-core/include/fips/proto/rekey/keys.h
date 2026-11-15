#ifndef FIPS_REKEY_KEYS_H
#define FIPS_REKEY_KEYS_H
#include <fips/proto/rekey/profile.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Borrowed C counterpart of NodeKeys. No hidden identity/keypair copies. */
typedef struct fips_rekey_node_keys {
    const fips_secp *secp;
    const fips_identity *keypair;
    uint8_t epoch[8];
    fips_entropy entropy;
} fips_rekey_node_keys;
FIPS_CORE_API extern const fips_rekey_session_keys fips_rekey_noise_keys;
FIPS_CORE_API void fips_rekey_pinned(const fips_peer_key *peer, uint8_t compressed[33]);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_KEYS_H */
