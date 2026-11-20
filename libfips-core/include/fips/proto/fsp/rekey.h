#ifndef FIPS_PROTO_FSP_REKEY_H
#define FIPS_PROTO_FSP_REKEY_H
#include <fips/proto/fsp/core.h>
#include <fips/proto/rekey/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
/* XK callbacks emit bare Noise; the ordered executor wraps with CURRENT coords
 * at each send, including resends. local is a borrowed fips_rekey_node_keys. */
FIPS_CORE_API extern const fips_rekey_profile fips_fsp_rekey_profile;
/* FspRekeyProfile attachment: strict endpoint envelope validation, then the
 * SAME machine for first handshake, rotation, and all held receive keys. Route
 * storage is host-owned and must survive retained answers/prompts. */
FIPS_CORE_API fips_status fips_fsp_rekey_receive(fips_rekey *machine,
    fips_instant now, fips_node_addr carrier, fips_rekey_route_handle route,
    uint8_t *frame, size_t len, fips_rekey_env *env, fips_rekey_outputs *out);
#ifdef __cplusplus
}
#endif
#endif
