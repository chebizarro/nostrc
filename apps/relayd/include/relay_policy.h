#ifndef RELAYD_RELAY_POLICY_H
#define RELAYD_RELAY_POLICY_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void relay_policy_set_replay_ttl(int seconds);
int  relay_policy_get_replay_ttl(void);
void relay_policy_set_skew(int future_seconds, int past_seconds);
void relay_policy_get_skew(int *future_seconds, int *past_seconds);
int  relay_policy_seen_id_check_and_add(const char *id_hex, time_t now);
int  relay_policy_created_at_out_of_range(int64_t created_at, time_t now);

/* Public NIP-01 policy API, declared in protocol_nip01.h. */
void nostr_relay_set_replay_ttl(int seconds);
void nostr_relay_set_skew(int future_seconds, int past_seconds);
int  nostr_relay_get_replay_ttl(void);
void nostr_relay_get_skew(int *future_seconds, int *past_seconds);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_RELAY_POLICY_H */
