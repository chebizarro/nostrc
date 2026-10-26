#ifndef NOSTR_SECURITY_LIMITS_RUNTIME_H
#define NOSTR_SECURITY_LIMITS_RUNTIME_H

#include "nostr-export.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime getters for security limits with environment overrides.
 * Fallback defaults come from security_limits.h macros. */

NOSTR_API int64_t nostr_limit_max_frame_len(void);
NOSTR_API int64_t nostr_limit_max_frames_per_sec(void);
NOSTR_API int64_t nostr_limit_max_bytes_per_sec(void);
NOSTR_API int64_t nostr_limit_max_event_size(void);

/* Signature-verification work budgets. Costs are weighted tokens, not events. */
NOSTR_API int64_t nostr_limit_verification_conn_per_sec(void);
NOSTR_API int64_t nostr_limit_verification_conn_burst(void);
NOSTR_API int64_t nostr_limit_verification_ip_per_sec(void);
NOSTR_API int64_t nostr_limit_verification_ip_burst(void);
NOSTR_API int64_t nostr_limit_verification_global_per_sec(void);
NOSTR_API int64_t nostr_limit_verification_global_burst(void);
NOSTR_API int64_t nostr_limit_max_verification_ips(void);
NOSTR_API int64_t nostr_limit_max_verification_jobs(void);
NOSTR_API int64_t nostr_limit_max_verification_bytes(void);
NOSTR_API int64_t nostr_limit_verification_negative_cache_entries(void);
NOSTR_API int64_t nostr_limit_verification_negative_ttl_seconds(void);

NOSTR_API int64_t nostr_limit_max_tags_per_event(void);
NOSTR_API int64_t nostr_limit_max_tag_depth(void);
NOSTR_API int64_t nostr_limit_max_ids_per_filter(void);
NOSTR_API int64_t nostr_limit_max_filters_per_req(void);

/* WebSocket slowloris/timeouts */
NOSTR_API int64_t nostr_limit_ws_read_timeout_seconds(void);
NOSTR_API int64_t nostr_limit_ws_progress_window_ms(void);
NOSTR_API int64_t nostr_limit_ws_min_bytes_per_window(void);

NOSTR_API int64_t nostr_limit_invalidsig_window_seconds(void);
NOSTR_API int64_t nostr_limit_invalidsig_threshold(void);
NOSTR_API int64_t nostr_limit_invalidsig_ban_seconds(void);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_SECURITY_LIMITS_RUNTIME_H */
