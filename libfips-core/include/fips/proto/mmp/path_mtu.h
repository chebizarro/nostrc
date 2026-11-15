#ifndef FIPS_PROTO_MMP_PATH_MTU_H
#define FIPS_PROTO_MMP_PATH_MTU_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/proto/mmp/limits.h>
typedef struct fips_mmp_path_mtu_state {
    uint16_t current_mtu, last_observed_mtu;
    bool observed_changed, has_last_notification;
    uint64_t last_notification_ms;
    uint16_t last_notified_mtu;
    uint64_t notification_interval_ms;
    uint8_t consecutive_increase_count;
    uint64_t first_increase_ms;
    bool has_first_increase;
    uint16_t pending_increase_mtu;
} fips_mmp_path_mtu_state;
FIPS_CORE_API void fips_mmp_path_mtu_init(fips_mmp_path_mtu_state *s);
FIPS_CORE_API void fips_mmp_path_mtu_update_interval_from_srtt(fips_mmp_path_mtu_state *s, double srtt_ms);
FIPS_CORE_API void fips_mmp_path_mtu_seed_source_mtu(fips_mmp_path_mtu_state *s, uint16_t mtu);
FIPS_CORE_API void fips_mmp_path_mtu_observe_incoming_mtu(fips_mmp_path_mtu_state *s, uint16_t mtu);
FIPS_CORE_API bool fips_mmp_path_mtu_should_send_notification(const fips_mmp_path_mtu_state *s, uint64_t now_ms);
FIPS_CORE_API bool fips_mmp_path_mtu_build_notification(fips_mmp_path_mtu_state *s, uint64_t now_ms, uint16_t *out);
FIPS_CORE_API bool fips_mmp_path_mtu_apply_notification(fips_mmp_path_mtu_state *s, uint16_t mtu, uint64_t now_ms);
FIPS_CORE_API bool fips_mmp_path_mtu_apply_reactive_decrease(fips_mmp_path_mtu_state *s, uint16_t mtu);
FIPS_CORE_API void fips_mmp_path_mtu_reset_source_mtu(fips_mmp_path_mtu_state *s);

#ifdef __cplusplus
}
#endif
#endif
