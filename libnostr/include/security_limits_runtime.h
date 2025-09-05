#ifndef NOSTR_SECURITY_LIMITS_RUNTIME_H
#define NOSTR_SECURITY_LIMITS_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime getters for security limits with environment overrides.
 * Fallback defaults come from security_limits.h macros. */

int64_t nostr_limit_max_frame_len(void);
int64_t nostr_limit_max_frames_per_sec(void);
int64_t nostr_limit_max_bytes_per_sec(void);
int64_t nostr_limit_max_event_size(void);

int64_t nostr_limit_max_tags_per_event(void);
int64_t nostr_limit_max_tag_depth(void);
int64_t nostr_limit_max_ids_per_filter(void);
int64_t nostr_limit_max_filters_per_req(void);

/* WebSocket slowloris/timeouts */
int64_t nostr_limit_ws_read_timeout_seconds(void);
int64_t nostr_limit_ws_progress_window_ms(void);
int64_t nostr_limit_ws_min_bytes_per_window(void);

int64_t nostr_limit_invalidsig_window_seconds(void);
int64_t nostr_limit_invalidsig_threshold(void);
int64_t nostr_limit_invalidsig_ban_seconds(void);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_SECURITY_LIMITS_RUNTIME_H */
