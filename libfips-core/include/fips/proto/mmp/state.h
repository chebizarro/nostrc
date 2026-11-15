#ifndef FIPS_PROTO_MMP_STATE_H
#define FIPS_PROTO_MMP_STATE_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/proto/mmp/sender.h>
#include <fips/proto/mmp/receiver.h>
#include <fips/proto/mmp/metrics.h>
#include <fips/proto/mmp/path_mtu.h>
typedef enum fips_mmp_mode { FIPS_MMP_Full, FIPS_MMP_Lightweight, FIPS_MMP_Minimal } fips_mmp_mode;
FIPS_CORE_API const char *fips_mmp_mode_name(fips_mmp_mode mode);
FIPS_CORE_API bool fips_mmp_mode_parse(const char *name, fips_mmp_mode *out);
typedef struct fips_mmp_peer_state {
    fips_mmp_sender_state sender;
    fips_mmp_receiver_state receiver;
    fips_mmp_metrics metrics;
    fips_mmp_spin_bit_state spin_bit;
    fips_mmp_mode mode;
    uint64_t log_interval_ms, last_log_ms;
    bool has_last_log;
} fips_mmp_peer_state;
/* Session aggregate keeps the same roles, with session cold-start timing. */
typedef struct fips_mmp_session_state {
    fips_mmp_peer_state roles;
    fips_mmp_path_mtu_state path_mtu;
} fips_mmp_session_state;
FIPS_CORE_API fips_status fips_mmp_peer_init(fips_mmp_peer_state *s, fips_mmp_mode mode, uint64_t log_interval_secs, size_t window, bool initiator, fips_mmp_owd_sample *storage, size_t capacity);
FIPS_CORE_API fips_status fips_mmp_session_init(fips_mmp_session_state *s, fips_mmp_mode mode, uint64_t log_interval_secs, size_t window, bool initiator, fips_mmp_owd_sample *storage, size_t capacity);
FIPS_CORE_API void fips_mmp_peer_reset_for_rekey(fips_mmp_peer_state *s, uint64_t now_ms);
FIPS_CORE_API void fips_mmp_session_reset_for_rekey(fips_mmp_session_state *s, uint64_t now_ms);
FIPS_CORE_API bool fips_mmp_peer_should_log(const fips_mmp_peer_state *s, uint64_t now_ms);
FIPS_CORE_API void fips_mmp_peer_mark_logged(fips_mmp_peer_state *s, uint64_t now_ms);
FIPS_CORE_API bool fips_mmp_session_should_log(const fips_mmp_session_state *s, uint64_t now_ms);
FIPS_CORE_API void fips_mmp_session_mark_logged(fips_mmp_session_state *s, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
