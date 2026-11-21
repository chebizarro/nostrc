#ifndef FIPS_PROTO_SETTLE_JOIN_H
#define FIPS_PROTO_SETTLE_JOIN_H
#include <fips/proto/settle/input.h>
#include <fips/proto/path/machine.h>
#include <fips/proto/rekey/machine.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_settle_setup {
    fips_rekey_completed done;
    fips_rekey_index_lease lease;
    fips_rekey_index theirs;
    fips_rekey_transport transport;
} fips_settle_setup;
typedef struct fips_settle_caps {
    const fips_rekey_profile *profile;
    fips_rekey_params params;
    fips_node_addr me;
    fips_instant now;
    fips_rekey_env *env;
} fips_settle_caps;
typedef fips_settle_proven (*fips_settle_path_read)(void *ctx,const fips_path_ref *path);
typedef enum fips_settle_join_kind {
    FIPS_SETTLE_JOIN_START,FIPS_SETTLE_JOIN_SHIFT,FIPS_SETTLE_JOIN_STOP,
    FIPS_SETTLE_JOIN_REKEY,FIPS_SETTLE_JOIN_PATHS,FIPS_SETTLE_JOIN_ROAM
} fips_settle_join_kind;
/* Fields are selected by kind. START/SHIFT use rekey+has_rekey, setup,caps,to;
 * START additionally paths+has_paths,key,facts,path_params,entropy,path_env;
 * SHIFT optionally paths (NULL withholds renewal),path_env;
 * STOP uses both slots and why. REKEY borrows rekey_out; PATHS borrows
 * path_out+read+read_ctx+link/released. ROAM uses link/from/to/standby.
 * Flags mirror Option slots; referenced machines cannot move while live. */
typedef struct fips_settle_join_event {
    fips_settle_join_kind kind;
    fips_rekey *rekey; bool *has_rekey;
    fips_path_bundle *paths; bool *has_paths;
    fips_peer_key key;
    fips_settle_setup *setup;
    fips_rekey_setup_facts *facts;
    fips_settle_caps *caps;
    fips_settle_pair to,from;
    fips_path_params path_params;
    fips_entropy entropy;
    const fips_path_env *path_env;
    fips_rekey_close why;
    fips_rekey_outputs *rekey_out;
    fips_path_outputs *path_out;
    fips_settle_path_read read; void *read_ctx;
    uint64_t link,released;
    bool has_link,has_released,has_from,standby;
} fips_settle_join_event;
typedef enum fips_settle_join_step_kind {
    FIPS_SETTLE_STEP_CORE,FIPS_SETTLE_STEP_REPLACE,FIPS_SETTLE_STEP_REDIAL,
    FIPS_SETTLE_STEP_REKEY,FIPS_SETTLE_STEP_PATH
} fips_settle_join_step_kind;
typedef struct fips_settle_join_step {
    fips_settle_join_step_kind kind;
    fips_settle_input input;
    /* Borrowed output owner, execute/consume in place before next. For Replace
     * and Redial, rekey->data.tok is the authenticated token; the redial digest
     * is tok.ephemeral. Record it only AFTER the core arms the redial. */
    fips_rekey_output *rekey;
    fips_path_output *path;
} fips_settle_join_step;
/* Noncopyable caller-owned iterator/workspace, initially {0}. No allocation,
 * IO or clock reads. Complete/clear before reuse; borrowed sources, read ctx,
 * profile/env referents and machine slots outlive traversal. No arguments,
 * workspace or output arrays may alias. PATHS accepts at most
 * FIPS_PATH_OUTPUTS_MAX outputs and snapshots read() during join, before any
 * shell effect (as upstream does), not lazily during traversal.
 * Execute every step front-to-back.
 * next borrows owners, never copies them; outputs_clear on the original source
 * remains the caller's job. clear wipes internal secret outputs and diagnoses
 * unreturned leases, but cannot execute host releases or driver cleanup.
 * RANGE preserves workspace, machines, setup/facts and host entropy. A START
 * with a foreign authenticated key is RANGE (unlike Rust's consuming Err).
 * A refused SHIFT succeeds with built=false and emits Release of setup. */
typedef struct fips_settle_joined {
    bool active,built,has_carry,paths_first,mirror,has_tail,has_link,has_released;
    fips_node_addr peer;
    fips_rekey_carry carry;
    fips_rekey_outputs close_out,build_out;
    fips_path_output path_items[FIPS_PATH_OUTPUTS_MAX];
    struct { bool present; fips_settle_input input; } mirrors[FIPS_PATH_OUTPUTS_MAX];
    fips_path_outputs owned_paths;
    fips_rekey_outputs *rekeys[2];
    fips_path_outputs *paths;
    size_t group,rekey_pos,path_pos;
    uint64_t link,released;
    fips_settle_path_read read; void *read_ctx;
    fips_settle_input pending,tail;
    bool has_pending;
} fips_settle_joined;
FIPS_CORE_API fips_status fips_settle_join(fips_settle_joined *j,fips_node_addr peer,const fips_settle_join_event *event);
FIPS_CORE_API bool fips_settle_join_next(fips_settle_joined *j,fips_settle_join_step *out);
FIPS_CORE_API void fips_settle_join_clear(fips_settle_joined *j);
FIPS_CORE_API fips_settle_pair fips_settle_path_pair(const fips_path_ref *path);
FIPS_CORE_API fips_settle_proven fips_settle_path_proven(const fips_path_bundle *bundle,const fips_path_ref *path);
#ifdef __cplusplus
}
#endif
#endif
