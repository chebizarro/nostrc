#ifndef FIPS_PROTO_SETTLE_GATES_H
#define FIPS_PROTO_SETTLE_GATES_H
#include <fips/proto/settle/settle.h>
#ifdef __cplusplus
extern "C" {
#endif
/* C-local per-identity memory bounds, not wire limits. All slots are caller
 * owned/private-by-contract; params above these bounds fail transactionally.
 * Durations must be normalized and all instants share a host clock domain. */
#define FIPS_SETTLE_GATE_EPHEMERALS_MAX 64u
typedef enum fips_settle_source {
    FIPS_SETTLE_SOURCE_NOT_HELD, FIPS_SETTLE_SOURCE_RESTART,
    FIPS_SETTLE_SOURCE_REPLACEMENT, FIPS_SETTLE_SOURCE_REDIAL,
    FIPS_SETTLE_SOURCE_OWN_MSG2, FIPS_SETTLE_SOURCE_REKEY_MSG2
} fips_settle_source;
typedef enum fips_settle_refusal {
    FIPS_SETTLE_GATE_ACCEPT, FIPS_SETTLE_GATE_UNHEARD, FIPS_SETTLE_GATE_HEARD,
    FIPS_SETTLE_GATE_DAMPENED, FIPS_SETTLE_GATE_REPLAYED,
    FIPS_SETTLE_GATE_REPLAY_FULL, FIPS_SETTLE_GATE_SILENT, FIPS_SETTLE_GATE_REDIALED
} fips_settle_refusal;
typedef struct fips_settle_liveness {
    bool heard, has_last;
    fips_instant installed, last;
} fips_settle_liveness;
typedef struct fips_settle_ask {
    fips_settle_source source;
    bool has_held, has_ephemeral, has_epoch, has_memory;
    fips_settle_liveness held;
    uint8_t ephemeral[32], epoch[8];
    fips_instant memory;
} fips_settle_ask;
typedef struct fips_settle_remembered {
    uint8_t ephemeral[32];
    fips_instant at;
} fips_settle_remembered;
typedef struct fips_settle_silence {
    bool present, has_epoch, has_until;
    uint8_t epoch[8];
    uint32_t count, refused;
    fips_instant last, until;
    uint8_t counted[FIPS_SETTLE_GATE_EPHEMERALS_MAX][32];
    size_t counted_count;
} fips_settle_silence;
typedef struct fips_settle_memory {
    bool active, has_stamp;
    fips_node_addr peer;
    fips_instant stamp;
    fips_settle_remembered replays[FIPS_SETTLE_GATE_EPHEMERALS_MAX];
    fips_settle_remembered redials[FIPS_SETTLE_GATE_EPHEMERALS_MAX];
    size_t replay_count, redial_count;
    fips_settle_silence silence;
} fips_settle_memory;
typedef struct fips_settle_gates {
    fips_settle_memory *slots;
    size_t capacity;
} fips_settle_gates;
typedef struct fips_settle_silencing {
    bool has_epoch;
    uint8_t epoch[8];
    uint32_t silent;
    fips_duration remaining;
} fips_settle_silencing;
FIPS_CORE_API fips_status fips_settle_gates_init(fips_settle_gates *g,
    fips_settle_memory *slots, size_t capacity);
FIPS_CORE_API const fips_settle_memory *fips_settle_gate_get(const fips_settle_gates *g,
    const fips_node_addr *peer);
/* Successful queries lapse expired identities; a query with no memory inserts
 * nothing. Acceptance stamps only heard restart/replacement, never a msg2 or
 * an unproven redial. Errors leave all slots and outputs untouched. */
FIPS_CORE_API fips_status fips_settle_gate(fips_settle_gates *g, fips_node_addr peer,
    const fips_settle_ask *ask, fips_instant now, const fips_settle_params *p,
    fips_settle_refusal *out);
FIPS_CORE_API fips_status fips_settle_gate_promoted(fips_settle_gates *g,
    fips_node_addr peer, const uint8_t ephemeral[32], fips_instant now,
    const fips_instant *memory, const fips_settle_params *p);
FIPS_CORE_API void fips_settle_gate_heard(fips_settle_gates *g, const fips_node_addr *peer);
FIPS_CORE_API void fips_settle_gate_framed(fips_settle_gates *g, const fips_node_addr *peer);
FIPS_CORE_API void fips_settle_gate_forget(fips_settle_gates *g, const fips_node_addr *peer);
/* epoch/setup/memory may be NULL. started=false writes a zero length. */
FIPS_CORE_API fips_status fips_settle_gate_ended(fips_settle_gates *g,
    fips_node_addr peer, const uint8_t *epoch, const uint8_t *setup,
    fips_instant now, const fips_instant *memory, const fips_settle_params *p,
    bool *started, fips_duration *length);
FIPS_CORE_API fips_status fips_settle_gate_redialed(fips_settle_gates *g,
    fips_node_addr peer, const uint8_t ephemeral[32], fips_instant now,
    const fips_settle_params *p);
FIPS_CORE_API fips_status fips_settle_gate_swapped(fips_settle_gates *g,
    fips_node_addr peer, bool heard, fips_instant now);
FIPS_CORE_API bool fips_settle_gate_silenced(const fips_settle_gates *g,
    const fips_node_addr *peer, const uint8_t *epoch, fips_instant now);
FIPS_CORE_API bool fips_settle_gate_refusing(const fips_settle_gates *g,
    const fips_node_addr *peer, fips_instant now, fips_settle_silencing *out);
FIPS_CORE_API bool fips_settle_gate_count_refused(fips_settle_gates *g,
    const fips_node_addr *peer, uint32_t *out);
FIPS_CORE_API uint32_t fips_settle_gate_take_refused(fips_settle_gates *g,
    const fips_node_addr *peer);
#ifdef __cplusplus
}
#endif
#endif
