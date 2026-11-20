#ifndef FIPS_REKEY_PROOF_H
#define FIPS_REKEY_PROOF_H
#include <fips/proto/rekey/profile.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_rekey_bond_kind {
    FIPS_REKEY_BOND_Key,
    FIPS_REKEY_BOND_Claimed
} fips_rekey_bond_kind;
typedef struct fips_rekey_bond {
    fips_rekey_bond_kind kind;
    fips_peer_key key;
    fips_node_addr claimed;
} fips_rekey_bond;
typedef enum fips_rekey_evidence_kind {
    FIPS_REKEY_EVIDENCE_Proof,
    FIPS_REKEY_EVIDENCE_Frame,
    FIPS_REKEY_EVIDENCE_Restart,
    FIPS_REKEY_EVIDENCE_Assumed,
    FIPS_REKEY_EVIDENCE_Delay,
    FIPS_REKEY_EVIDENCE_First
} fips_rekey_evidence_kind;
/* PeerMsg1 is a trusted core-to-core capability, never a wire decoder. Only an
 * IK profile that authenticated the whole msg1 may construct this token. Its
 * ephemeral digest must cover the authenticated ephemeral, not a mutable index.
 * The payload is borrowed through step (and through ReplaceRequested execution).
 * C cannot enforce Rust's crate-private Authentic<T,Replayable> constructor. */
typedef struct fips_rekey_msg1_read {
    fips_peer_key remote;
    uint8_t epoch[8], digest[32], ephemeral[32];
    const void *token;
} fips_rekey_msg1_read;
typedef struct fips_rekey_leg {
    fips_rekey_completed done;
    fips_rekey_index_lease lease;
} fips_rekey_leg;
FIPS_CORE_API bool fips_rekey_bond_check(const fips_rekey_bond *bond,
                                         const fips_rekey_completed *done);
FIPS_CORE_API fips_node_addr fips_rekey_bond_addr(const fips_rekey_bond *bond);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_PROOF_H */
