#ifndef FIPS_PROTO_SETTLE_MODEL_JUDGE_H
#define FIPS_PROTO_SETTLE_MODEL_JUDGE_H
#include <fips/proto/settle/model/history.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Independent reference judge: no settlement/rekey/path core types or helper
 * calls. Built only with FIPS_TEST_HOOKS, never linked into production fips_core.
 * Fixed caller-owned value storage; all failed steps/runs preserve state and
 * outputs. State, input-event arrays and output arrays must not overlap.
 * The C-local bound applies to each vector and one run's event count.
 * Legal histories advance time monotonically; fixture edits must preserve that
 * temporal invariant. Model values are copyable; no leases or secrets exist. */
#define FIPS_SETTLE_MODEL_CAPACITY 128u
typedef struct fips_settle_model_params { uint64_t unheard, interval, linkage, sessionage; } fips_settle_model_params;
typedef enum fips_settle_model_class {
    FIPS_MODEL_CLASS_NOT_HELD, FIPS_MODEL_CLASS_RESTART, FIPS_MODEL_CLASS_RESEND,
    FIPS_MODEL_CLASS_COPY, FIPS_MODEL_CLASS_REPLACEMENT, FIPS_MODEL_CLASS_OFF_LINK,
    FIPS_MODEL_CLASS_PENDING, FIPS_MODEL_CLASS_RECORDED, FIPS_MODEL_CLASS_SIMULTANEOUS,
    FIPS_MODEL_CLASS_REKEY, FIPS_MODEL_CLASS_REDIAL
} fips_settle_model_class;
typedef enum fips_settle_model_gate { FIPS_MODEL_GATE_UNHEARD, FIPS_MODEL_GATE_HEARD, FIPS_MODEL_GATE_DAMPENED, FIPS_MODEL_GATE_SLOT } fips_settle_model_gate;
typedef enum fips_settle_model_reply { FIPS_MODEL_REPLY_SILENT, FIPS_MODEL_REPLY_MSG2, FIPS_MODEL_REPLY_STORED, FIPS_MODEL_REPLY_DEFERRED } fips_settle_model_reply;
typedef enum fips_settle_model_transition {
    FIPS_MODEL_TRANSITION_PROMOTE, FIPS_MODEL_TRANSITION_REPLACE, FIPS_MODEL_TRANSITION_MOVE,
    FIPS_MODEL_TRANSITION_KEEP, FIPS_MODEL_TRANSITION_REKEY_CUTOVER, FIPS_MODEL_TRANSITION_RESTARTED,
    FIPS_MODEL_TRANSITION_ROAM, FIPS_MODEL_TRANSITION_REMOVE
} fips_settle_model_transition;
typedef enum fips_settle_model_rule {
    FIPS_MODEL_RULE_LATE_SECOND_DIAL, FIPS_MODEL_RULE_STORED_LATE, FIPS_MODEL_RULE_DAMPENER_STAMP,
    FIPS_MODEL_RULE_AGED_LINK, FIPS_MODEL_RULE_BETWEEN_FLOORS, FIPS_MODEL_RULE_RECORD_SCOPE,
    FIPS_MODEL_RULE_SESSION_AGE, FIPS_MODEL_RULE_DEFERRED_REDIAL
} fips_settle_model_rule;
typedef struct fips_settle_model_step {
    bool has_class, has_refused, has_reply, has_change, has_rule, disconnect, open;
    fips_settle_model_class class_;
    fips_settle_model_gate refused;
    fips_settle_model_reply reply;
    fips_settle_model_transition change;
    fips_settle_model_rule rule;
    fips_settle_model_addr closes[1];
    size_t close_count;
} fips_settle_model_step;
typedef struct fips_settle_model_fallback {
    uint32_t hs, link;
    fips_settle_model_binding binding;
    fips_settle_model_addr setup, current;
} fips_settle_model_fallback;
typedef struct fips_settle_model_pending {
    uint32_t hs, armed, restarted;
    bool initiator, has_armed, has_restarted;
} fips_settle_model_pending;
typedef struct fips_settle_model_peering {
    uint32_t hs, epoch, link, msg1;
    fips_settle_model_binding binding;
    fips_settle_model_addr setup, current;
    bool dialed, has_msg1, heard, has_last, has_pending, has_fallback;
    uint64_t installed, linkborn, sessionborn, last;
    fips_settle_model_pending pending;
    fips_settle_model_fallback fallback;
    uint32_t record[FIPS_SETTLE_MODEL_CAPACITY];
    size_t record_count;
} fips_settle_model_peering;
typedef struct fips_settle_model_attempt {
    uint32_t hs, link;
    fips_settle_model_addr to;
    fips_settle_model_binding binding;
    bool aged;
} fips_settle_model_attempt;
typedef struct fips_settle_model_leg {
    uint32_t hs, bytes, epoch, link;
    fips_settle_model_addr from;
    fips_settle_model_binding binding;
} fips_settle_model_leg;
typedef struct fips_settle_model_redial { fips_settle_model_leg leg; uint64_t at; bool sent; } fips_settle_model_redial;
typedef struct fips_settle_model_judge {
    uint8_t me, peer;
    fips_settle_model_params params;
    uint64_t now, stamp;
    uint32_t rekey;
    bool has_peering, has_redial, has_rekey, has_stamp;
    fips_settle_model_peering peering;
    fips_settle_model_redial redial;
    fips_settle_model_attempt attempts[FIPS_SETTLE_MODEL_CAPACITY];
    fips_settle_model_leg legs[FIPS_SETTLE_MODEL_CAPACITY];
    fips_settle_model_addr closed[FIPS_SETTLE_MODEL_CAPACITY];
    size_t attempt_count, leg_count, closed_count;
} fips_settle_model_judge;
typedef struct fips_settle_model_need { uint32_t link; fips_settle_model_addr at; } fips_settle_model_need;
#ifdef FIPS_TEST_HOOKS
FIPS_CORE_API fips_settle_model_params fips_settle_model_params_default(void);
FIPS_CORE_API void fips_settle_model_init(fips_settle_model_judge *j, uint8_t me, uint8_t peer, fips_settle_model_params params);
FIPS_CORE_API fips_status fips_settle_model_step_event(fips_settle_model_judge *j, fips_settle_model_event event, fips_settle_model_step *out);
FIPS_CORE_API fips_status fips_settle_model_run(fips_settle_model_judge *j, const fips_settle_model_event *events, size_t count, fips_settle_model_step *out, size_t capacity);
FIPS_CORE_API bool fips_settle_model_settled(const fips_settle_model_judge *j, fips_settle_model_need *out, uint32_t *hs);
FIPS_CORE_API fips_status fips_settle_model_needs(const fips_settle_model_judge *j, fips_settle_model_need *out, size_t capacity, size_t *count);
#endif
#ifdef __cplusplus
}
#endif
#endif
