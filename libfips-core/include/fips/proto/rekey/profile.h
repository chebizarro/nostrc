#ifndef FIPS_REKEY_PROFILE_H
#define FIPS_REKEY_PROFILE_H
#include <fips/identity/peer.h>
#include <fips/noise/session.h>
#include <fips/proto/rekey/lease.h>
#include <fips/proto/rekey/resolve.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_REKEY_WIRE_MAX 256u
#define FIPS_REKEY_CANDIDATES_MAX 4u
/* Opaque, copyable host route/transport handles. Any referent must outlive the
 * machine and its outstanding outputs. No borrowed packet-buffer pointers.
 * This avoids truncating TreeCoordinate's u16-count domain; the binding owns
 * its bounded coordinate store. The machine never interprets either handle. */
typedef uintptr_t fips_rekey_route_handle;
typedef uintptr_t fips_rekey_transport;
typedef struct fips_rekey_wire {
    uint8_t bytes[FIPS_REKEY_WIRE_MAX];
    size_t len;
} fips_rekey_wire;
typedef enum fips_rekey_route {
    FIPS_REKEY_ROUTE_Established,
    FIPS_REKEY_ROUTE_EstablishedFallback,
    FIPS_REKEY_ROUTE_Arrival
} fips_rekey_route;
typedef enum fips_rekey_layer { FIPS_REKEY_LAYER_Fmp, FIPS_REKEY_LAYER_Fsp } fips_rekey_layer;
typedef enum fips_rekey_initiator_cutover {
    FIPS_REKEY_OnProof,
    FIPS_REKEY_OnEvidence,
    FIPS_REKEY_AfterDelay
} fips_rekey_initiator_cutover;
typedef enum fips_rekey_responder_cutover {
    FIPS_REKEY_OnFrame,
    FIPS_REKEY_AtProof
} fips_rekey_responder_cutover;
typedef enum fips_rekey_dampen_at {
    FIPS_REKEY_DAMPEN_Answer,
    FIPS_REKEY_DAMPEN_Completion
} fips_rekey_dampen_at;
typedef enum fips_rekey_select { FIPS_REKEY_ByIndex, FIPS_REKEY_Trial } fips_rekey_select;
typedef struct fips_rekey_index {
    uint32_t value;
    bool some;
} fips_rekey_index;
typedef struct fips_rekey_frame_hint {
    uint64_t counter;
    bool k;
    uint32_t index;
    size_t body;
} fips_rekey_frame_hint;
/* Inline handshake owner. Profiles may use extra for codec metadata/stub crypto.
 * No owned heap pointers: clear destroys Noise and wipes the entire slot. */
typedef struct fips_rekey_hs {
    fips_handshake_state noise;
    uint8_t extra[128];
} fips_rekey_hs;
typedef struct fips_rekey_completed {
    fips_noise_session session;
    fips_peer_key remote;
    uint8_t epoch[8];
    fips_rekey_index theirs;
} fips_rekey_completed;
typedef struct fips_rekey_read {
    fips_rekey_completed done;
    fips_rekey_wire fin;
    bool has_fin;
} fips_rekey_read;
typedef struct fips_rekey_recv_copy {
    fips_cipher_state key;
    fips_replay_window window;
    uint64_t floor;
    bool has_floor;
} fips_rekey_recv_copy;
/* SessionKeys: profiles may substitute deterministic test crypto. The concrete
 * transport storage is the existing Noise session; all profiles share its
 * clear_slot/take_cleared ownership. open/fresh/sent/copy are read-only. reserve
 * returns a bare key clone pinned against auto-seal, plus its reserved counter. */
typedef struct fips_rekey_session_keys {
    bool (*open)(const fips_noise_session *, size_t aad, fips_rekey_frame_hint, const uint8_t *,
                 size_t, uint8_t *, size_t, size_t *);
    bool (*fresh)(const fips_noise_session *, uint64_t);
    void (*mark)(fips_noise_session *, uint64_t);
    uint64_t (*sent)(const fips_noise_session *);
    bool (*reserve)(fips_noise_session *, uint64_t *, fips_cipher_state *);
    void (*copy)(const fips_noise_session *, bool, uint64_t, fips_rekey_recv_copy *);
} fips_rekey_session_keys;
/* Profile is the hook vtable, not a second state machine. Callbacks must neither
 * mutate retained hs nor leak temporary forks on failure. Every callback output
 * starts zeroed, is bounded by these types, and is cleared by the caller on all
 * exits. ctx/local keys are borrowed for each call, not retained by the machine. */
typedef struct fips_rekey_profile {
    fips_rekey_layer layer;
    uint8_t messages;
    fips_rekey_commit commit;
    fips_rekey_initiator_cutover initiator_cutover;
    fips_rekey_responder_cutover responder_cutover;
    size_t candidates, per_carrier;
    fips_rekey_dampen_at dampen_at;
    fips_rekey_select select;
    bool silence, setup_rtt, indexed;
    /* Profile::arrived: replies carry input.on_link when true, uncapped otherwise. */
    bool cap_replies;
    size_t floor, aad;
    const fips_rekey_session_keys *keys;
    bool (*initiate)(void *local, const fips_peer_key *, fips_rekey_index, fips_rekey_hs *,
                     fips_rekey_wire *);
    bool (*valid)(const uint8_t *, size_t);
    bool (*answer)(void *local, const uint8_t *, size_t, fips_rekey_hs *, fips_rekey_wire *);
    fips_rekey_index (*index)(const uint8_t *, size_t);
    bool (*reply)(const fips_rekey_hs *, void *local, const uint8_t *, size_t, fips_rekey_read *);
    bool (*complete)(const fips_rekey_hs *, const uint8_t *, size_t, fips_rekey_completed *);
    bool (*prompt)(fips_duration, fips_rekey_wire *);
    bool (*is_probe)(const uint8_t *, size_t);
    /* IK node-level authenticated token. No unverified wire token is accepted. */
    bool (*msg1_answer)(const void *token, void *local, fips_rekey_index, fips_rekey_completed *,
                        fips_rekey_wire *);
} fips_rekey_profile;
FIPS_CORE_API bool fips_rekey_profile_valid(const fips_rekey_profile *profile);
FIPS_CORE_API void fips_rekey_hs_clear(fips_rekey_hs *hs);
FIPS_CORE_API void fips_rekey_completed_clear(fips_rekey_completed *done);
FIPS_CORE_API void fips_rekey_recv_copy_clear(fips_rekey_recv_copy *copy);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_PROFILE_H */
