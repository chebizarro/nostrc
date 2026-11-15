#ifndef FIPS_REKEY_ACCESS_H
#define FIPS_REKEY_ACCESS_H
#include <fips/proto/rekey/machine.h>
#ifdef __cplusplus
extern "C" {
#endif
FIPS_CORE_API void fips_rekey_note_setup(fips_rekey *m, fips_rekey_record *setup);
FIPS_CORE_API const fips_rekey_held_answer *fips_rekey_answer(const fips_rekey *m);
FIPS_CORE_API const fips_noise_session *fips_rekey_keys(const fips_rekey *m, fips_rekey_slot slot);
FIPS_CORE_API bool fips_rekey_tag(const fips_rekey *m, fips_rekey_slot slot,
                                  fips_rekey_epoch_tag *tag);
FIPS_CORE_API fips_rekey_index fips_rekey_ours(const fips_rekey *m, fips_rekey_slot slot);
FIPS_CORE_API fips_rekey_index fips_rekey_theirs(const fips_rekey *m, fips_rekey_slot slot);
FIPS_CORE_API fips_instant fips_rekey_started(const fips_rekey *m, fips_rekey_slot slot);
FIPS_CORE_API fips_instant fips_rekey_base(const fips_rekey *m);
FIPS_CORE_API bool fips_rekey_k(const fips_rekey *m);
FIPS_CORE_API uint64_t fips_rekey_sent(const fips_rekey *m);
FIPS_CORE_API int32_t fips_rekey_jitter_secs(const fips_rekey *m);
FIPS_CORE_API fips_instant fips_rekey_drain_deadline(const fips_rekey *m);
FIPS_CORE_API fips_instant fips_rekey_last_cutover(const fips_rekey *m);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_ACCESS_H */
