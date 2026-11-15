#ifndef FIPS_REKEY_HOOKS_H
#define FIPS_REKEY_HOOKS_H
#include <fips/proto/rekey/machine.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef FIPS_TEST_HOOKS
FIPS_CORE_API void fips_rekey_start_now(fips_rekey *m, fips_instant now, fips_rekey_env *env,
                                        fips_rekey_outputs *out);
FIPS_CORE_API void fips_rekey_trigger(fips_rekey *m, fips_instant now, fips_rekey_env *env,
                                      fips_rekey_outputs *out);
FIPS_CORE_API void fips_rekey_quiet(fips_rekey *m);
FIPS_CORE_API void fips_rekey_backdate_started(fips_rekey *m, fips_duration age);
FIPS_CORE_API void fips_rekey_backdate_base(fips_rekey *m, fips_duration age);
FIPS_CORE_API void fips_rekey_backdate_drain(fips_rekey *m, fips_duration age);
FIPS_CORE_API void fips_rekey_backdate_pending(fips_rekey *m, fips_duration age);
FIPS_CORE_API void fips_rekey_backdate_dampening(fips_rekey *m, fips_duration age);
FIPS_CORE_API void fips_rekey_force_k(fips_rekey *m, bool k);
FIPS_CORE_API void fips_rekey_force_peer_epoch(fips_rekey *m, const uint8_t *epoch);
FIPS_CORE_API bool fips_rekey_dampened(const fips_rekey *m, fips_instant now);
FIPS_CORE_API bool fips_rekey_reserve_slot(fips_rekey *m, fips_rekey_slot slot, uint64_t *counter,
                                           fips_cipher_state *key);
FIPS_CORE_API void fips_rekey_offer_held(const fips_rekey *m, fips_rekey_outputs *out);
/* Observe fixed-arena erasure without exporting keys to production. */
FIPS_CORE_API bool fips_rekey_cleared_slots_are_zero(const fips_rekey *m);
#endif
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_HOOKS_H */
