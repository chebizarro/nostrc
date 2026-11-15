#ifndef FIPS_PROTO_PATH_PARAMS_H
#define FIPS_PROTO_PATH_PARAMS_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_PATH_RING 8
#define FIPS_PATH_SILENCE_MAX ((fips_duration){60,0})
#define FIPS_PATH_DEAD_PROBE_INTERVAL_MIN ((fips_duration){1,0})
#define FIPS_PATH_DEAD_PROBE_INTERVAL_MAX ((fips_duration){30,0})
#define FIPS_PATH_DEAD_RECHECK_SPAN ((fips_duration){30,0})
typedef struct fips_path_config {
    bool enabled;
    double switch_margin;
    fips_duration switch_dwell;
    uint8_t min_samples;
    fips_duration active_interval, standby_interval;
    bool mix_kinds;
    fips_duration timeout, verdict_cap, horizon;
    uint8_t suspect, retire, discovery;
    fips_duration heartbeat, dead_interval, grace;
    uint8_t auth_limit, ring, budget;
    fips_duration span, cap;
} fips_path_config;
typedef enum fips_path_params_error_kind { FIPS_PATH_PARAMS_Zero, FIPS_PATH_PARAMS_Margin, FIPS_PATH_PARAMS_Ring } fips_path_params_error_kind;
typedef struct fips_path_params_error { fips_path_params_error_kind kind; const char *field; } fips_path_params_error;
typedef struct fips_path_params { fips_path_config cfg; fips_duration window; } fips_path_params;
FIPS_CORE_API fips_path_config fips_path_config_draft(void);
FIPS_CORE_API fips_path_params fips_path_params_draft(void);
FIPS_CORE_API fips_status fips_path_params_new(fips_path_params *out, fips_path_config cfg, fips_path_params_error *error);
#ifdef __cplusplus
}
#endif
#endif
