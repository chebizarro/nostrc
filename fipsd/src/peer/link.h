#ifndef FIPSD_PEER_LINK_H
#define FIPSD_PEER_LINK_H
#include "peer/active.h"
#include <fips/proto/rekey/machine.h>

/* HOST-INTERNAL PROFILE / SESSION-TABLE ATTACHMENT SEAM (f12v2.2).
 * Production IK profile is injected, never fabricated here. Env's index/tag/
 * local/route backing stores outlive the machine. Caller executes every output
 * (Bind/Unbind/Delegate/Revoke/Release included), then outputs_clear, on ALL paths.
 * This surface exposes machine inputs/observations, NOT session slots/mutators.
 * The FSP XK session table is a separate node owner, not a link peer's table. */
bool fipsd_active_peer_link_attached(const fipsd_active_peer *peer);
fips_status fipsd_active_peer_link_established(fipsd_active_peer *peer,
    const fips_rekey_profile *profile, fips_rekey_params params, fips_node_addr me,
    fips_rekey_completed *first, fips_rekey_ids *ids, fips_rekey_setup_facts *facts,
    fips_instant now, fips_rekey_env *env, fips_rekey_outputs *out);
fips_status fipsd_active_peer_link_step(fipsd_active_peer *peer, fips_instant now,
    fips_rekey_input input, fips_rekey_env *env, fips_rekey_outputs *out);
fips_instant fipsd_active_peer_link_next_wake(const fipsd_active_peer *peer);
fips_rekey_seal_refusal fipsd_active_peer_link_seal(fipsd_active_peer *peer,
    fips_instant now, fips_rekey_sealed *out);
fips_rekey_view fipsd_active_peer_link_view(const fipsd_active_peer *peer);
void fipsd_active_peer_link_close(fipsd_active_peer *peer, fips_rekey_close reason,
    fips_rekey_outputs *out, fips_rekey_carry *carry);
#endif
