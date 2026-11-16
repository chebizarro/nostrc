#ifndef FIPSD_GATEWAY_STATE_H
#define FIPSD_GATEWAY_STATE_H
/* The pool state the gateway keeps across a restart, the port of upstream
 * src/gateway/state.rs (nostrc-iq1n9.3).
 *
 * The pool writes which addresses a client may still hold an answer for, and
 * how far it may issue, to a file on tmpfs. The next run reads it once, holds
 * those addresses until the answers have expired, and so never hands a
 * cached address to a different node. The file holds no identities and no
 * mappings. It lives only on tmpfs, so a reboot, which ends every client's
 * cache with it, starts from nothing.
 *
 * A file is trusted for one start only: loading it removes it, and a write
 * that fails removes it too, so a start never replays a stretch the run that
 * wrote it has already issued past. */
#include "gateway/pool.h"

#define FIPSD_GW_STATE_DIR "/var/run/fips-gateway"
#define FIPSD_GW_STATE_FILE "pool.json"
#define FIPSD_GW_STATE_TMP "pool.json.tmp"
/* Longest hold a loaded state may impose on the next run: the largest a run
 * can write (TTL plus grace, each at most u32::MAX seconds). */
#define FIPSD_GW_RESTART_HOLD_MAX_SECS (UINT64_C(2) * UINT32_MAX)

typedef enum fipsd_gw_loaded_kind {
    FIPSD_GW_LOADED_ABSENT,   /* no state file */
    FIPSD_GW_LOADED_RESTORED, /* a valid state for this pool */
    FIPSD_GW_LOADED_INVALID   /* a file that cannot be used; reason says why */
} fipsd_gw_loaded_kind;
typedef struct fipsd_gw_loaded {
    fipsd_gw_loaded_kind kind;
    fipsd_gw_pool_state state; /* RESTORED; owned */
    char reason[256];          /* INVALID */
} fipsd_gw_loaded;
void fipsd_gw_loaded_clear(fipsd_gw_loaded *loaded);

/* Whether path lies on tmpfs: 1, 0, or -1 with errno. Injected so tests can
 * stand in for the file system. */
typedef int (*fipsd_gw_tmpfs_fn)(const char *path, void *user);
int fipsd_gw_is_tmpfs(const char *path, void *unused);

/* Serde's encoding of the state, fields in declaration order; malloc'd. */
char *fipsd_gw_state_encode(const fipsd_gw_pool_state *state);
/* Parse an encoding; false with a reason. */
bool fipsd_gw_state_decode(const char *json, size_t len, fipsd_gw_pool_state *out, char *reason, size_t cap);
/* Why state cannot start pool cidr of total addresses, if it cannot. */
bool fipsd_gw_state_validate(const fipsd_gw_pool_state *state, const char *cidr, uint32_t total, char *reason, size_t cap);

/* Read the state for pool cidr of total addresses from dir, and remove the
 * file. A file that cannot be removed is reported as invalid. */
void fipsd_gw_state_load(const char *dir, const char *cidr, uint32_t total, fipsd_gw_loaded *out);
/* How the pool starts from what was loaded: the state, or fresh at random.
 * out borrows the loaded state. */
void fipsd_gw_state_start_from(const fipsd_gw_loaded *loaded, uint32_t random, fipsd_gw_pool_begin *out);

/* Write state to dir, which must lie on tmpfs. Refuses when dir's parent does
 * not exist or is not on tmpfs; creates dir 0700, checks it again, writes the
 * file 0600 through a rename. Any failure removes the state file. Returns 0,
 * or an errno with a message in err. */
int fipsd_gw_state_save(const char *dir, const fipsd_gw_pool_state *state, fipsd_gw_tmpfs_fn on_tmpfs, void *user,
                        char *err, size_t cap);

typedef struct fipsd_gw_started {
    fipsd_gw_pool *pool;
    fipsd_gw_loaded loaded;
    bool carried;          /* a restored pool's holds were written back */
    int carried_rc;        /* and how that went */
    char carried_err[256];
} fipsd_gw_started;
/* Start pool cidr at now from the state in dir, fresh at random when there is
 * none, and write a restored pool's holds back at once (no stretch). */
fipsd_gw_pool_error fipsd_gw_state_start_pool(const char *dir, const char *cidr, uint32_t total, uint64_t ttl_secs,
                                              uint64_t grace_secs, uint32_t random, fipsd_gw_instant now,
                                              fipsd_gw_tmpfs_fn on_tmpfs, void *user, fipsd_gw_started *out);

/* Logs state writes once per change of outcome. */
typedef struct fipsd_gw_save_log { bool failed; char last[256]; } fipsd_gw_save_log;
typedef enum fipsd_gw_save_report {
    FIPSD_GW_SAVE_OK, FIPSD_GW_SAVE_FAILED, FIPSD_GW_SAVE_REPEATED, FIPSD_GW_SAVE_RECOVERED
} fipsd_gw_save_report;
fipsd_gw_save_report fipsd_gw_save_log_report(fipsd_gw_save_log *log, int rc, const char *err);
#endif
