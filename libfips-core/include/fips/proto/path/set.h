#ifndef FIPS_PROTO_PATH_SET_H
#define FIPS_PROTO_PATH_SET_H
#include <fips/proto/path/live.h>
#include <fips/transport/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_PATH_MAX_PER_PEER 8
#define FIPS_PATH_MAX_PER_TRANSPORT 4
#define FIPS_PATH_NONE SIZE_MAX
typedef enum fips_path_medium { FIPS_PATH_Ble, FIPS_PATH_Wired, FIPS_PATH_Wireless, FIPS_PATH_Unknown } fips_path_medium;
typedef enum fips_path_state { FIPS_PATH_NoState=-1, FIPS_PATH_Probing, FIPS_PATH_Live, FIPS_PATH_Suspect, FIPS_PATH_Dead } fips_path_state;
typedef struct fips_path_ref { fips_transport_id transport; fips_transport_addr addr; } fips_path_ref;
typedef struct fips_peer_path {
    fips_transport_id transport_id; fips_transport_addr addr;
    uint32_t path_id,peer_id; /* peer_id=0 means None. */
    fips_path_state state; bool acked;
    fips_instant dead_since;
    fips_path_liveness live;
    void *driver_slot;
} fips_peer_path;
/* Inline, caller-owned bounded set. Removal shifts indexes, preserving order.
 * Borrowed paths/indexes last until the next mutation. Only driver_slot may be
 * modified directly by a host. No driver destructor is called: all evictions
 * return the complete path to the caller, who owns releasing the slot. */
typedef struct fips_path_set { fips_peer_path paths[8]; size_t len,active; } fips_path_set;
typedef struct fips_path_admission { bool known; size_t index; bool has_evicted; fips_peer_path evicted; } fips_path_admission;
typedef enum fips_path_refused { FIPS_PATH_TransportFull, FIPS_PATH_PeerFull } fips_path_refused;
typedef enum fips_path_rebind { FIPS_PATH_Unchanged, FIPS_PATH_Switched, FIPS_PATH_Refused } fips_path_rebind;
typedef enum fips_path_match_kind { FIPS_PATH_MatchNone, FIPS_PATH_Unproven, FIPS_PATH_Proven } fips_path_match_kind;
typedef struct fips_path_match { fips_path_match_kind kind; bool active; fips_path_state state; } fips_path_match;
typedef enum fips_path_roam_kind { FIPS_PATH_Unmoved, FIPS_PATH_Bound, FIPS_PATH_Roamed, FIPS_PATH_Standby } fips_path_roam_kind;
typedef struct fips_path_roam { fips_path_roam_kind kind; fips_path_ref from; size_t index; } fips_path_roam;
FIPS_CORE_API void fips_path_set_new(fips_path_set *s);
FIPS_CORE_API const fips_peer_path *fips_path_set_get(const fips_path_set *s,size_t i);
FIPS_CORE_API fips_peer_path *fips_path_set_path_mut(fips_path_set *s,size_t i);
FIPS_CORE_API const fips_peer_path *fips_path_set_active(const fips_path_set *s);
FIPS_CORE_API size_t fips_path_set_find(const fips_path_set *s,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API size_t fips_path_set_find_by_peer(const fips_path_set *s,fips_transport_id t,uint32_t peer_id);
FIPS_CORE_API fips_path_match fips_path_set_classify(const fips_path_set *s,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API bool fips_path_set_learn_peer(fips_path_set *s,size_t i,uint32_t peer_id);
FIPS_CORE_API void fips_path_set_mark_acked(fips_path_set *s,size_t i);
FIPS_CORE_API void fips_path_set_transition(fips_path_set *s,size_t i,fips_path_state state,fips_instant now);
FIPS_CORE_API bool fips_path_set_readdress(fips_path_set *s,size_t i,fips_transport_addr addr,fips_transport_addr *old);
FIPS_CORE_API bool fips_path_set_discard(fips_path_set *s,size_t i,fips_peer_path *out);
FIPS_CORE_API bool fips_path_set_activate(fips_path_set *s,size_t i);
/* Entropy is retried until a nonzero unique path ID is drawn, as upstream.
 * Refusal does not draw entropy or change set/out. */
FIPS_CORE_API fips_status fips_path_set_admit(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_path_medium medium,fips_entropy e,fips_path_admission *out,fips_path_refused *error);
FIPS_CORE_API fips_status fips_path_set_bind_original(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_entropy e,fips_path_admission *out,fips_path_refused *error);
FIPS_CORE_API fips_path_roam fips_path_set_roam(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_entropy e);
/* Refused may still admit a new unproven standby and evict a Dead path. */
FIPS_CORE_API fips_path_rebind fips_path_set_rebind(fips_path_set *s,fips_transport_id t,fips_transport_addr a,fips_path_medium medium,fips_entropy e,fips_path_admission *out);
#ifdef __cplusplus
}
#endif
#endif
