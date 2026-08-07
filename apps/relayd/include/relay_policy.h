#ifndef RELAYD_RELAY_POLICY_H
#define RELAYD_RELAY_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RelayPolicy RelayPolicy;

typedef enum {
  RELAY_REPLAY_RESERVED = 0,
  RELAY_REPLAY_DUPLICATE,
  RELAY_REPLAY_IN_PROGRESS,
  RELAY_REPLAY_DISABLED,
  RELAY_REPLAY_ERROR
} RelayReplayStatus;

RelayPolicy *relay_policy_create(size_t capacity, int replay_ttl_seconds,
                                 int future_skew_seconds,
                                 int past_skew_seconds);
void relay_policy_destroy(RelayPolicy *policy);

int relay_policy_get_replay_ttl(const RelayPolicy *policy);
void relay_policy_get_skew(const RelayPolicy *policy, int *future_seconds,
                           int *past_seconds);
size_t relay_policy_size(const RelayPolicy *policy);

int relay_policy_hex_to_id(const char id_hex[65], unsigned char id_out[32]);
RelayReplayStatus relay_policy_reserve(RelayPolicy *policy,
                                       const unsigned char id[32],
                                       uint64_t now_ms);
int relay_policy_commit(RelayPolicy *policy, const unsigned char id[32],
                        uint64_t now_ms);
int relay_policy_rollback(RelayPolicy *policy, const unsigned char id[32]);
int relay_policy_created_at_out_of_range(const RelayPolicy *policy,
                                         int64_t created_at, time_t now);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_RELAY_POLICY_H */
