/* Per-endpoint capability observations. SPDX-License-Identifier: MIT */
#ifndef BLOSSOM_CAPABILITY_H
#define BLOSSOM_CAPABILITY_H
#include <blossom/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BLOSSOM_PREFERRED_CT_MAX 64
#define BLOSSOM_CACHE_REFRESH_SECONDS (24 * 60 * 60)
typedef enum { BLOSSOM_CAP_UNKNOWN, BLOSSOM_CAP_YES, BLOSSOM_CAP_NO } blossom_capability_state_t;
typedef struct {
    blossom_capability_state_t batch_ok, server_tag_ok, strict_x_binding;
    blossom_capability_state_t raw_random_ok, png_shim_ok;
    int64_t last_probe_ts, last_401_ts;
    bool reachable;
    char preferred_content_type[BLOSSOM_PREFERRED_CT_MAX];
    blossom_capability_state_t private_server; /* server-info "private"; UNKNOWN if absent. */
    uint64_t max_size; /* Advertised upload byte limit; zero means unknown. */
} blossom_server_capabilities_t;
BLOSSOM_API void blossom_server_capabilities_init(blossom_server_capabilities_t *);
BLOSSOM_API const char *blossom_capability_state_str(blossom_capability_state_t);
/* Shared by preparation/hash callers and the uploader, so mixed destination
 * sets use ONE wire representation. UNKNOWN is conservative raw, never a
 * positive batch/shim observation. No environment override is consulted. */
typedef enum {
    BLOSSOM_SHIM_REASON_UNKNOWN = 0,
    BLOSSOM_SHIM_REASON_ENV_FORCE_ON, /* Reserved legacy numeric values; never emitted. */
    BLOSSOM_SHIM_REASON_ENV_FORCE_OFF,
    BLOSSOM_SHIM_REASON_AUTO_REQUIRED,
    BLOSSOM_SHIM_REASON_AUTO_RAW_OK,
    BLOSSOM_SHIM_REASON_AUTO_FALLBACK
} blossom_shim_reason_t;
BLOSSOM_API bool blossom_shim_active(void); /* Legacy no-context query: always false. */
BLOSSOM_API bool blossom_shim_active_for(const char *const *, size_t);
BLOSSOM_API bool blossom_shim_active_for_ex(const char *const *, size_t, blossom_shim_reason_t *, char *, size_t);
/* Diagnostic seeding/reset; timestamps use actual wall clock (no immortal
 * sentinel entries). Useful for callers with independently observed probes. */
BLOSSOM_API void blossom_shim_cache_reset(void);
BLOSSOM_API void blossom_shim_cache_set(const char *, blossom_capability_state_t, blossom_capability_state_t);
BLOSSOM_API void blossom_capability_cache_set(const char *, const blossom_server_capabilities_t *);
BLOSSOM_API bool blossom_capability_cache_get(const char *, blossom_server_capabilities_t *);
#ifdef __cplusplus
}
#endif
#endif
