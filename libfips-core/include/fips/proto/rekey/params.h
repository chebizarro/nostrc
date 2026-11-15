#ifndef FIPS_REKEY_PARAMS_H
#define FIPS_REKEY_PARAMS_H
#include <fips/proto/rekey/ladder.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_REKEY_MAX_AFTER UINT32_MAX
#define FIPS_REKEY_DEPLOYED_SESSION_FLOOR_SECS 30u
typedef struct fips_rekey_config {
    fips_duration after;
    uint16_t jitter;
    uint64_t messages;
    fips_duration handshake, check, drain, retention, hold, revert, dampening;
    fips_duration ttl, young, link_age, session_age, floor_link, floor_session;
    fips_duration floor_cutover, silence;
    uint8_t resends;
    uint64_t soft, hard;
    fips_duration delay;
    bool arrival;
} fips_rekey_config;
typedef enum fips_rekey_params_error {
    FIPS_REKEY_PARAMS_OK,
    FIPS_REKEY_PARAMS_ZeroMessages,
    FIPS_REKEY_PARAMS_MessagesPastSoft,
    FIPS_REKEY_PARAMS_AfterNotAboveJitter,
    FIPS_REKEY_PARAMS_AfterBelowLinkFloor,
    FIPS_REKEY_PARAMS_AfterTooLong,
    FIPS_REKEY_PARAMS_Zero,
    FIPS_REKEY_PARAMS_HandshakeTooShort,
    FIPS_REKEY_PARAMS_DrainTooShort,
    FIPS_REKEY_PARAMS_RetentionBelowDrain,
    FIPS_REKEY_PARAMS_HoldTooShort,
    FIPS_REKEY_PARAMS_SoftNotBelowHard,
    FIPS_REKEY_PARAMS_InvalidDuration
} fips_rekey_params_error;
typedef struct fips_rekey_inert {
    uint8_t first, probes;
} fips_rekey_inert;
/* Read-only after new: all derived schedules belong to this exact config. */
typedef struct fips_rekey_params {
    fips_rekey_config cfg;
    fips_duration confirm, span, grace, rung, gap, budget;
    fips_rekey_schedule first, fin, probes, revert, elicit, delayed;
    fips_rekey_inert inert;
} fips_rekey_params;
FIPS_CORE_API fips_rekey_config fips_rekey_config_fmp(void);
FIPS_CORE_API fips_rekey_config fips_rekey_config_fsp(void);
FIPS_CORE_API fips_rekey_config fips_rekey_config_with_hold(fips_rekey_config cfg,
                                                            fips_duration silence);
FIPS_CORE_API fips_rekey_params_error fips_rekey_params_new(fips_rekey_config cfg,
                                                            fips_rekey_params *out);
FIPS_CORE_API fips_rekey_params fips_rekey_params_fmp(void);
FIPS_CORE_API fips_rekey_params fips_rekey_params_fsp(void);
FIPS_CORE_API fips_duration fips_rekey_msg1_span(uint8_t resends);
FIPS_CORE_API fips_duration fips_rekey_params_backoff(const fips_rekey_params *p, uint8_t n);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_PARAMS_H */
