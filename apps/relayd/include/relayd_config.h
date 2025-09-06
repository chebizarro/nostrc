#ifndef RELAYD_CONFIG_H
#define RELAYD_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define RELAYD_MAX_LISTEN_LEN 128
#define RELAYD_MAX_DRIVER_LEN 64
#define RELAYD_MAX_SUPPORTED_NIPS 32
#define RELAYD_MAX_STR 256

typedef struct {
  char listen[RELAYD_MAX_LISTEN_LEN];      /* e.g., "127.0.0.1:4848" */
  char storage_driver[RELAYD_MAX_DRIVER_LEN];
  int supported_nips[RELAYD_MAX_SUPPORTED_NIPS];
  int supported_nips_count;
  /* Basic limits */
  int max_filters;   /* maximum filters per REQ/COUNT */
  int max_limit;     /* maximum limit per filter */
  int max_subs;      /* maximum concurrent subscriptions per connection */
  /* Rate limiting */
  int rate_ops_per_sec; /* tokens per second */
  int rate_burst;       /* max tokens */
  /* NIP-11 identity/metadata */
  char name[RELAYD_MAX_STR];
  char software[RELAYD_MAX_STR];
  char version[RELAYD_MAX_STR];
  char description[RELAYD_MAX_STR];
  char contact[RELAYD_MAX_STR];
  /* AUTH mode: off|optional|required */
  char auth[RELAYD_MAX_STR];
  /* NIP-77 negentropy feature flag */
  int negentropy_enabled;
} RelaydConfig;

/* Load config from file path. Returns 0 on success, fills defaults if file not found. */
int relayd_config_load(const char *path, RelaydConfig *out);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CONFIG_H */
