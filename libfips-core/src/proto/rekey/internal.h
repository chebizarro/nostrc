#ifndef FIPS_REKEY_INTERNAL_H
#define FIPS_REKEY_INTERNAL_H
#include "time.h"
#include <assert.h>
#include <fips/proto/rekey/mod.h>
#include <sodium.h>
#include <string.h>
/* Proofs/commits never cross the public ABI; only judge constructs them. */
typedef struct rk_frame_proof {
    fips_rekey_slot slot;
    fips_rekey_epoch_tag tag;
    uint64_t counter;
    bool k, probe;
    size_t len;
} rk_frame_proof;
typedef enum rk_trial { RK_OPENED, RK_REFUSED, RK_FAILED } rk_trial;
fips_rekey_output *rk_output(fips_rekey_outputs *out, fips_rekey_output_kind kind);
void rk_send(fips_rekey_outputs *out, const fips_rekey_wire *msg, fips_rekey_msg_kind kind,
             fips_rekey_route route);
void rk_reject(fips_rekey_outputs *out, fips_rekey_reason reason);
void rk_abandoned(fips_rekey_outputs *out, fips_rekey_abandon why);
void rk_prompt(fips_rekey_outputs *out, fips_rekey_epoch_tag tag);
fips_rekey_epoch *rk_epoch(fips_rekey *m, fips_rekey_slot slot);
const fips_rekey_epoch *rk_epoch_const(const fips_rekey *m, fips_rekey_slot slot);
void rk_epoch_new(fips_rekey *m, fips_rekey_epoch *ep, fips_rekey_completed *done,
                  fips_rekey_ids *ids, bool k, fips_rekey_role role, fips_instant now,
                  fips_rekey_env *env);
void rk_epoch_move(fips_rekey_epoch *from, fips_rekey_epoch *to);
void rk_retire(fips_rekey *m, fips_rekey_epoch *ep, fips_rekey_outputs *out);
rk_trial rk_epoch_trial(const fips_rekey *m, const fips_rekey_epoch *ep, fips_rekey_frame_hint hint,
                        const uint8_t *buf, size_t len, fips_rekey_env *env, size_t *opened);
void rk_delegate(const fips_rekey *m, const fips_rekey_epoch *ep, bool workers,
                 fips_rekey_outputs *out);
void rk_cycle_end(fips_rekey *m, fips_rekey_abandon why, fips_rekey_close closed,
                  fips_rekey_outputs *out);
void rk_pending_record(fips_rekey *m);
void rk_cycle_clear(fips_rekey_cycle *c);
void rk_start(fips_rekey *m, fips_instant now, fips_rekey_env *env, fips_rekey_outputs *out);
size_t rk_cand_find(const fips_rekey_candidates *c, const uint8_t *msg, size_t len);
bool rk_cand_room(const fips_rekey_candidates *c, const fips_rekey_profile *p,
                  fips_node_addr carrier);
size_t rk_cand_victim(const fips_rekey_candidates *c, const fips_rekey_profile *p,
                      fips_node_addr carrier, fips_instant now, fips_duration young);
void rk_cand_remove(fips_rekey_candidates *c, size_t i);
void rk_cand_clear(fips_rekey_candidates *c);
void rk_cand_add(fips_rekey_candidates *c, fips_rekey_candidate *candidate);
int32_t rk_jitter(fips_rekey_env *env, uint16_t j);
#endif
