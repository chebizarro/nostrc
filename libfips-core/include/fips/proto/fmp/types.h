#ifndef FIPS_PROTO_FMP_TYPES_H
#define FIPS_PROTO_FMP_TYPES_H
#include <fips/identity/peer.h>
#include <fips/transport/types.h>
#include <fips/utils/index.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_fmp_epoch { bool present; uint8_t bytes[8]; } fips_fmp_epoch;
typedef struct fips_fmp_msg1_digest { uint8_t bytes[32]; } fips_fmp_msg1_digest;
typedef struct fips_fmp_ephemeral_digest { uint8_t bytes[32]; } fips_fmp_ephemeral_digest;
typedef struct fips_fmp_bytes { const uint8_t *data; size_t len; } fips_fmp_bytes;
FIPS_CORE_API bool fips_fmp_epochs_differ(fips_fmp_epoch a, fips_fmp_epoch b);
FIPS_CORE_API void fips_fmp_msg1_digest_of(const uint8_t *wire, size_t len, fips_fmp_msg1_digest *out);
FIPS_CORE_API void fips_fmp_msg1_digest_prefix(const fips_fmp_msg1_digest *digest, uint8_t out[4]);
FIPS_CORE_API bool fips_fmp_ephemeral_digest_of_noise_msg1(const uint8_t *noise, size_t len, fips_fmp_ephemeral_digest *out);
/* Whole milliseconds, saturating to u64, in one caller clock domain. */
FIPS_CORE_API uint64_t fips_fmp_elapsed_ms(fips_instant now, fips_instant earlier);
#ifdef __cplusplus
}
#endif
#endif
