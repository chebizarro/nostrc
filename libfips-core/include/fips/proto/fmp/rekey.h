#ifndef FIPS_PROTO_FMP_REKEY_H
#define FIPS_PROTO_FMP_REKEY_H
#include <fips/proto/fmp/core.h>
#include <fips/proto/rekey/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
/* IK binding. The link host keeps this bound but dormant, as at the Rust pin:
 * no automatic rekey polling or replacement of the deployed link lifecycle. */
FIPS_CORE_API extern const fips_rekey_profile fips_fmp_rekey_profile;
/* Authenticated, replayable node-level capability. Never copy a live token;
 * clear after step AND ordered ReplaceRequested execution have finished. */
typedef struct fips_fmp_ik_msg1 {
    fips_rekey_hs hs;
    uint32_t theirs;
    fips_rekey_msg1_read proof;
} fips_fmp_ik_msg1;
FIPS_CORE_API bool fips_fmp_rekey_read_msg1(const fips_rekey_node_keys *local,
    const uint8_t *bytes, size_t len, fips_fmp_ik_msg1 *out);
/* Setup replay record for facts passed to established; msg2 may be NULL for
 * the original initiator. Framing/hashes only: this is NOT authentication. */
FIPS_CORE_API bool fips_fmp_rekey_setup_record(const uint8_t *msg1, size_t len,
    const fips_rekey_wire *msg2, fips_rekey_record *out);
FIPS_CORE_API void fips_fmp_rekey_msg1_clear(fips_fmp_ik_msg1 *token);
/* IN_Rekey attachment: only a node-authenticated token, not WireOutcome alone,
 * may enter PeerMsg1. Replacement stays a host decision/output. */
FIPS_CORE_API fips_status fips_fmp_rekey_inbound(fips_rekey *machine,
    fips_instant now, const fips_fmp_inbound_decision *decision,
    const fips_fmp_ik_msg1 *token, bool on_link, bool reachable,
    fips_rekey_env *env, fips_rekey_outputs *out);
FIPS_CORE_API fips_status fips_fmp_rekey_receive(fips_rekey *machine,
    fips_instant now, uint8_t *frame, size_t len, fips_rekey_env *env,
    fips_rekey_outputs *out);
#ifdef __cplusplus
}
#endif
#endif
