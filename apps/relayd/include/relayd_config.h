#ifndef RELAYD_CONFIG_H
#define RELAYD_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAYD_MAX_LISTEN_LEN 128
#define RELAYD_MAX_DRIVER_LEN 64
#define RELAYD_MAX_SUPPORTED_NIPS 32
#define RELAYD_MAX_STR 256

typedef struct {
  char listen[RELAYD_MAX_LISTEN_LEN];
  char storage_driver[RELAYD_MAX_DRIVER_LEN];
  int supported_nips[RELAYD_MAX_SUPPORTED_NIPS];
  int supported_nips_count;

  int max_filters;
  int max_limit;
  int max_subs;
  int max_event_bytes;

  int rate_ops_per_sec;
  int rate_burst;
  int rate_event_cost;

  int replay_cache_capacity;
  int replay_ttl_seconds;
  int future_skew_seconds;
  int past_skew_seconds;

  int verification_cost;
  int verification_conn_per_sec;
  int verification_conn_burst;
  int verification_ip_per_sec;
  int verification_ip_burst;
  int verification_global_per_sec;
  int verification_global_burst;
  int verification_max_ips;
  int verification_max_jobs;
  int verification_max_bytes;
  int verification_negative_cache_entries;
  int verification_negative_ttl_seconds;

  char name[RELAYD_MAX_STR];
  char software[RELAYD_MAX_STR];
  char version[RELAYD_MAX_STR];
  char description[RELAYD_MAX_STR];
  char contact[RELAYD_MAX_STR];
  char auth[RELAYD_MAX_STR];
  int negentropy_enabled;
} RelaydConfig;

int relayd_config_validate(const RelaydConfig *cfg, char *error,
                           size_t error_size);
int relayd_config_load(const char *path, RelaydConfig *out);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CONFIG_H */
