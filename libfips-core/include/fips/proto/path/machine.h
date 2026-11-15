#ifndef FIPS_PROTO_PATH_MACHINE_H
#define FIPS_PROTO_PATH_MACHINE_H
#include <fips/proto/path/wire.h>
#include <fips/proto/path/candidate.h>
#include <fips/proto/path/capable.h>
#include <fips/proto/path/select.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum fips_path_input_kind {
    FIPS_PATH_INPUT_Poll,FIPS_PATH_INPUT_Candidate,FIPS_PATH_INPUT_Unlisted,
    FIPS_PATH_INPUT_Probe,FIPS_PATH_INPUT_Ack,FIPS_PATH_INPUT_Close,
    FIPS_PATH_INPUT_Carrier,FIPS_PATH_INPUT_SendFailed,FIPS_PATH_INPUT_Present,
    FIPS_PATH_INPUT_Removed,FIPS_PATH_INPUT_Connected,FIPS_PATH_INPUT_Pin,
    FIPS_PATH_INPUT_Unpin,FIPS_PATH_INPUT_Report
} fips_path_input_kind;
typedef struct fips_path_input {
    fips_path_input_kind kind; fips_path_ref to;
    union { fips_path_source source; fips_path_probe probe; fips_path_ack ack; fips_path_close close;
        bool up,present,ok,has_addr; } as;
} fips_path_input;
typedef enum fips_path_why {
    FIPS_PATH_WHY_Candidate,FIPS_PATH_WHY_Probed,FIPS_PATH_WHY_Answered,FIPS_PATH_WHY_Closed,
    FIPS_PATH_WHY_Timeout,FIPS_PATH_WHY_Retired,FIPS_PATH_WHY_Exhausted,FIPS_PATH_WHY_Carrier,
    FIPS_PATH_WHY_Unreachable,FIPS_PATH_WHY_Returned,FIPS_PATH_WHY_Found,FIPS_PATH_WHY_Grace,
    FIPS_PATH_WHY_Evicted,FIPS_PATH_WHY_Removed,FIPS_PATH_WHY_AuthLimit,FIPS_PATH_WHY_Connect,FIPS_PATH_WHY_Renewed
} fips_path_why;
typedef enum fips_path_switch { FIPS_PATH_Mandatory,FIPS_PATH_Handover,FIPS_PATH_Discretionary,FIPS_PATH_Pinned } fips_path_switch;
typedef enum fips_path_refusal { FIPS_PATH_REFUSAL_Short,FIPS_PATH_REFUSAL_Full,FIPS_PATH_REFUSAL_Unmatched,FIPS_PATH_REFUSAL_Unknown,FIPS_PATH_REFUSAL_Rule,FIPS_PATH_REFUSAL_Table } fips_path_refusal;
typedef enum fips_path_output_kind {
    FIPS_PATH_OUTPUT_Send,FIPS_PATH_OUTPUT_Moved,FIPS_PATH_OUTPUT_Released,FIPS_PATH_OUTPUT_State,
    FIPS_PATH_OUTPUT_Capable,FIPS_PATH_OUTPUT_NotCapable,FIPS_PATH_OUTPUT_Refused,
    FIPS_PATH_OUTPUT_Switched,FIPS_PATH_OUTPUT_Orphaned,FIPS_PATH_OUTPUT_Connect,FIPS_PATH_OUTPUT_Proven
} fips_path_output_kind;
/* Send holds a typed value, not an allocated Vec. Encode into the host's frame
 * buffer with output_encode; len is the exact padded wire length. Actions own
 * their values, not buffers. Released/Moved transfer the driver_slot to host. */
typedef struct fips_path_output {
    fips_path_output_kind kind; fips_path_ref to;
    union {
        struct { fips_link_message_type type; union { fips_path_probe probe; fips_path_ack ack; fips_path_close close; } as; } send;
        struct { fips_path_ref from; void *driver_slot; } moved;
        struct { void *driver_slot; bool keep; } released;
        struct { uint32_t path_id; fips_path_state from,next; fips_path_why why; } state;
        struct { fips_path_refusal why; fips_path_rule rule; } refused;
        struct { bool has_from; fips_path_ref from; fips_path_switch why; } switched;
        size_t mtu;
    } as;
} fips_path_output;
#define FIPS_PATH_OUTPUTS_MAX 128
/* Every entry point resets len on success and requires capacity>=128. A step
 * conservatively allows 8 trip states, 32 expiry states over two passes,
 * 9 admission attempts (27 actions), 8 revivals, 16 release actions,
 * 26 sends/connections (including close notices), and 4 singleton notices.
 * These mutually exclusive maxima sum to 121; 128 covers their sum.
 * Capacity refusal leaves machine, entropy, outputs and len unchanged.
 * Storage must not overlap machine/input/env; execute actions front-to-back. */
typedef struct fips_path_outputs { fips_path_output *items; size_t capacity,len; } fips_path_outputs;
typedef struct fips_path_account { size_t path; bool sample; fips_path_roam roam; } fips_path_account;
typedef enum fips_path_charge { FIPS_PATH_Nothing,FIPS_PATH_Counted,FIPS_PATH_Warn,FIPS_PATH_Tripped } fips_path_charge;
typedef struct fips_path_view {
    fips_path_ref to; uint32_t id; fips_path_state state;
    bool active,acked,remote,pinned,has_rtt,has_least,has_score,has_mtu,has_heard;
    fips_duration rtt,least,heard; size_t samples,mtu; double etx,score;
    uint32_t frames,bytes,misses,fails;
} fips_path_view;
typedef struct fips_path_bundle_view {
    bool enabled,capable,held,has_pin,pin_enforced; fips_path_pin pin;
    fips_path_view paths[8]; size_t len;
} fips_path_bundle_view;
/* Caller-owned, noncopyable by contract (driver slots). No IO/allocation/clock
 * reads. Entropy is retained by value; its context must outlive the Bundle.
 * Config.enabled is upstream's runtime gate; before peer capability only gated
 * capability probes go. No daemon caller/integration is installed by this API. */
typedef struct fips_path_bundle {
    fips_path_params params; fips_path_set set; bool capable;
    fips_instant origin; uint32_t peer_active,original;
    fips_path_scope scope; fips_path_candidates table; fips_path_capability record;
    bool eligible,has_frame; uint32_t frame_path; size_t frame_len;
    bool orphaned; uint32_t dwell_path; fips_instant dwell_at; uint32_t left;
    bool has_pin; fips_path_pin pin; fips_path_hold hold; fips_entropy entropy;
} fips_path_bundle;
FIPS_CORE_API void fips_path_bundle_new(fips_path_bundle *b,fips_instant now,fips_transport_id t,fips_transport_addr a,fips_path_params params,fips_entropy entropy,const fips_path_env *env);
FIPS_CORE_API fips_status fips_path_bundle_step(fips_path_bundle *b,fips_instant now,const fips_path_input *input,const fips_path_env *env,fips_path_outputs *out);
FIPS_CORE_API fips_instant fips_path_bundle_next_wake(const fips_path_bundle *b);
/* Call after authentication+replay and BEFORE step(message), so the probe's
 * frame contributes to its ack. Non-path frames can use account alone. */
FIPS_CORE_API fips_path_account fips_path_bundle_account(fips_path_bundle *b,fips_instant now,fips_transport_id t,const fips_transport_addr *a,size_t len,uint8_t msg_type);
FIPS_CORE_API fips_path_charge fips_path_bundle_charge(fips_path_bundle *b,fips_instant now,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API void fips_path_bundle_authentic(fips_path_bundle *b,fips_transport_id t,const fips_transport_addr *a);
FIPS_CORE_API fips_status fips_path_bundle_close(fips_path_bundle *b,fips_path_outputs *out);
FIPS_CORE_API fips_status fips_path_bundle_renew(fips_path_bundle *b,fips_instant now,const fips_path_env *env,fips_path_ref to,fips_path_outputs *out);
FIPS_CORE_API void **fips_path_bundle_driver_mut(fips_path_bundle *b,size_t index);
FIPS_CORE_API double fips_path_bundle_cost(const fips_path_bundle *b,fips_instant now,double live);
FIPS_CORE_API void fips_path_bundle_view_get(const fips_path_bundle *b,fips_instant now,fips_path_bundle_view *out);
FIPS_CORE_API fips_status fips_path_output_encode(const fips_path_output *action,fips_writer *w,fips_proto_error *error);
#ifdef __cplusplus
}
#endif
#endif
