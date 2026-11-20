#ifndef FIPSD_NOSTR_SIGNAL_GATE_H
#define FIPSD_NOSTR_SIGNAL_GATE_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { FIPSD_SIGNAL_ADMIT, FIPSD_SIGNAL_SHED_SHARED, FIPSD_SIGNAL_SHED_RESERVE } fipsd_signal_admission;
/* Actor/middleware lock owns this pure state. Thousandth tokens, monotonic ms. */
typedef struct { uint64_t shared, reserve, shared_at, reserve_at; } fipsd_signal_gate;
void fipsd_signal_gate_init(fipsd_signal_gate *, uint64_t now_ms);
fipsd_signal_admission fipsd_signal_gate_admit(fipsd_signal_gate *, bool awaiting_answers, uint64_t now_ms);
#endif
