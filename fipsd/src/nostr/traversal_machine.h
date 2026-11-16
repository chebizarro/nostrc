#ifndef FIPSD_NOSTR_TRAVERSAL_MACHINE_H
#define FIPSD_NOSTR_TRAVERSAL_MACHINE_H
#include <stdbool.h>
#include <stdint.h>
/* The upstream async session loop expressed as a sans-I/O transition table.
 * Cross-session replay admission remains in runtime.c's bounded session table. */
typedef enum { FIPSD_TR_IDLE, FIPSD_TR_ANSWER, FIPSD_TR_PUNCHING, FIPSD_TR_SETTLING,
    FIPSD_TR_ESTABLISHED, FIPSD_TR_COOLDOWN, FIPSD_TR_CANCELLED, FIPSD_TR_STATES } fipsd_tr_state;
typedef enum { FIPSD_TR_OFFER, FIPSD_TR_ANSWERED, FIPSD_TR_PUNCH, FIPSD_TR_TIMEOUT,
    FIPSD_TR_CANCEL, FIPSD_TR_EVENTS } fipsd_tr_event;
enum { FIPSD_TR_SEND_OFFER = 1, FIPSD_TR_SEND_ANSWER = 2, FIPSD_TR_SEND_PUNCH = 4,
    FIPSD_TR_SEND_ACK = 8, FIPSD_TR_HANDOFF = 16, FIPSD_TR_FAILED = 32,
    FIPSD_TR_FINISHED = 64, FIPSD_TR_RETRY = 128 };
typedef struct {
    uint64_t answer_ms, attempt_ms, start_delay_ms, interval_ms, duration_ms;
    uint64_t retry_ms, max_retry_ms, cooldown_ms;
    uint32_t retries, failure_threshold;
} fipsd_tr_config;
typedef struct {
    fipsd_tr_state state;
    fipsd_tr_config config;
    uint64_t deadline, next_punch, send_until, settle_at;
    uint32_t attempts, failures, sequence;
    bool initiator;
} fipsd_tr_machine;
typedef struct { fipsd_tr_event event; bool initiator, accepted, probe, repair; unsigned rank; } fipsd_tr_input;
void fipsd_tr_config_default(fipsd_tr_config *);
void fipsd_tr_init(fipsd_tr_machine *, const fipsd_tr_config *);
unsigned fipsd_tr_step(fipsd_tr_machine *, fipsd_tr_input, uint64_t now_ms);
uint64_t fipsd_tr_deadline(const fipsd_tr_machine *);
/* Smaller NodeAddr's outbound session wins only when both initiated. */
bool fipsd_tr_suppress(const uint8_t ours[16], const uint8_t peer[16], bool active_initiator);
#endif
