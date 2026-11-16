#ifndef FIPSD_CONFIG_H
#define FIPSD_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <swap.h>
#include <fips/proto/rekey/params.h>
#include <fips/node/rate_limit.h>
#include <fips/proto/mmp/state.h>
#include "log.h"
#include "runtime.h"

typedef struct fipsd_config fipsd_config;
struct fipsd_acl;
struct fipsd_acl_paths;
/* Explicit pre-publication resource initialization. Parsing remains I/O-free.
 * NULL paths selects upstream platform defaults. Reloads retain these paths. */
bool fipsd_config_enable_acl(fipsd_config *, const struct fipsd_acl_paths *);
const struct fipsd_acl *fipsd_config_acl(const fipsd_config *);
typedef struct fipsd_config_value fipsd_config_value;
typedef struct fipsd_config_log {
    struct fipsd_config_log_entry *entries;
    size_t count;
    char error[2048];
} fipsd_config_log;
void fipsd_config_log_clear(fipsd_config_log *log);
void fipsd_config_log_replay(const fipsd_config_log *held, fipsd_log *logger);
const char *fipsd_config_log_message(const fipsd_config_log *log, size_t index);
fipsd_log_level fipsd_config_log_level(const fipsd_config_log *log, size_t index);
/* Logs must be zero-initialized and cleared before reuse after an error.
 * A NULL log discards diagnostics.
 * Parsing accepts partial/empty documents; loading files rejects files which
 * together configure nothing. No identity generation, network or core I/O. */
fipsd_config *fipsd_config_parse(const char *yaml, size_t len, fipsd_config_log *log);
fipsd_config *fipsd_config_load(const char *path, fipsd_config_log *log);
/* Low to high priority, missing files skipped. Explicit load never skips. */
fipsd_config *fipsd_config_load_paths(const char *const *paths, size_t count,
                                    fipsd_config_log *log);
void fipsd_config_free(void *config);
/* Owned absolute search paths, low to high priority. NULL on allocation/path
 * failure; free each entry and the array with search_paths_free. */
char **fipsd_config_search_paths(size_t *count);
void fipsd_config_search_paths_free(char **paths, size_t count);
/* Borrowed immutable values, valid for the life of their owning snapshot.
 * Paths use dots and numeric array indices, e.g. peers.0.addresses.0.addr.
 * get() returns NULL for explicit null as well as absent optional values.
 * Named transports retain their names; singleton fields stay directly under
 * transports.udp (etc). Absent optional values return NULL. */
const fipsd_config_value *fipsd_config_get(const fipsd_config *config, const char *path);
const fipsd_config_value *fipsd_config_member(const fipsd_config_value *v, const char *key);
const fipsd_config_value *fipsd_config_at(const fipsd_config_value *v, size_t i);
size_t fipsd_config_count(const fipsd_config_value *v);
const char *fipsd_config_key(const fipsd_config_value *v, size_t i);
const char *fipsd_config_string(const fipsd_config_value *v);
uint64_t fipsd_config_uint(const fipsd_config_value *v);
double fipsd_config_number(const fipsd_config_value *v);
bool fipsd_config_bool(const fipsd_config_value *v);
/* Stable, sorted, flattened effective dump. Secrets are always redacted. */
int fipsd_config_dump(const fipsd_config *config, FILE *out);
uint64_t fipsd_config_log_max_bytes(const fipsd_config *config);

typedef struct fipsd_config_core {
    fips_rekey_config link_rekey, session_rekey;
    fips_bucket_params handshake, established, session_setup;
    size_t max_pending_inbound, coord_size;
    uint64_t coord_ttl_ms;
    fips_mmp_mode mmp_mode, session_mmp_mode;
    uint64_t mmp_log_interval_secs, session_mmp_log_interval_secs;
    size_t mmp_window, session_mmp_window;
} fipsd_config_core;
void fipsd_config_core_params(const fipsd_config *config, fipsd_config_core *out);
/* Reload is single-writer. It builds/validates a candidate, retains all
 * restart-only values and logs every refused change before publication.
 * Readers use go_swap_load/release; GoSwap currently uses a short mutex, not
 * a lock-free retain. Failed loads keep the entire last good snapshot. */
bool fipsd_config_reload(GoSwap *slot, const char *path, fipsd_config_log *log);
void fipsd_config_apply_logging(const fipsd_config *config, fipsd_log *logger, fipsd_log_file *file);
typedef struct fipsd_config_service {
    fipsd_subsystem subsystem;
    GoSwap *snapshots;                  /* owned by caller, outlives runtime */
    const char *path;                    /* NULL: default search paths */
    fipsd_log *logger;
    fipsd_log_file *log_file;            /* borrowed; NULL for stderr */
    fipsd_runtime *runtime;
} fipsd_config_service;
void fipsd_config_service_init(fipsd_config_service *service, GoSwap *slot,
                               const char *path, fipsd_log *logger,
                               fipsd_log_file *log_file);
#endif
