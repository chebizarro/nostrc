#ifndef FIPS_PROTO_MMP_DELIVERY_H
#define FIPS_PROTO_MMP_DELIVERY_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/identity/node_addr.h>
#define FIPS_MMP_DELIVERY_FALLBACK_MS UINT64_C(30000)
#define FIPS_MMP_DELIVERY_MAXGAP_MS UINT64_C(60000)
#define FIPS_MMP_DELIVERY_QUIET_MS UINT64_C(120000)
#define FIPS_MMP_DELIVERY_UNVERIFIED_BUDGET 1u
#define FIPS_MMP_DELIVERY_LOSS_BUDGET 3u
typedef struct fips_mmp_rr_counters { uint64_t highest, received; uint32_t reordered; } fips_mmp_rr_counters;
typedef struct fips_mmp_link_evidence { uint64_t epoch, next_counter; fips_mmp_rr_counters rr; bool has_rr; } fips_mmp_link_evidence;
FIPS_CORE_API bool fips_mmp_link_evidence_usable_rr(const fips_mmp_link_evidence *l, fips_mmp_rr_counters *out);
typedef enum fips_mmp_resend_reason { FIPS_MMP_Loss, FIPS_MMP_Unverified, FIPS_MMP_SessionChanged, FIPS_MMP_Timeout } fips_mmp_resend_reason;
typedef enum fips_mmp_delivery_base { FIPS_MMP_BaseUnknown, FIPS_MMP_BaseCounted, FIPS_MMP_BaseZero } fips_mmp_delivery_base;
typedef struct fips_mmp_sent_announce {
    uint64_t counter, epoch;
    fips_mmp_delivery_base base_kind;
    fips_mmp_rr_counters base;
    bool complete, orphan;
    uint64_t at_ms;
} fips_mmp_sent_announce;
typedef struct fips_mmp_ack_state {
    fips_node_addr peer;
    uint64_t first_epoch;
    fips_mmp_sent_announce sent;
    bool has_sent;
    uint8_t level;
    uint64_t resent_ms;
    bool has_resent;
    uint64_t budget_epoch;
    uint8_t unverified_left, loss_left;
} fips_mmp_ack_state;
typedef struct fips_mmp_acks { uint64_t fallback_ms; fips_mmp_ack_state *peers; size_t len, capacity; } fips_mmp_acks;
/* Caller storage outlives state; RANGE on new peer preserves everything.
 * One Acks per announce kind. false/None preserves result out-parameters.
 * check consumes a resend budget, NOT a sent stamp: host must arrange resend
 * and call record only after transport acceptance. */
FIPS_CORE_API void fips_mmp_acks_init(fips_mmp_acks *s, fips_mmp_ack_state *storage, size_t capacity);
FIPS_CORE_API void fips_mmp_acks_set_fallback(fips_mmp_acks *s, uint64_t ms);
FIPS_CORE_API fips_status fips_mmp_acks_record(fips_mmp_acks *s, fips_node_addr peer, bool fresh, uint64_t counter, const fips_mmp_link_evidence *l, uint64_t now_ms);
FIPS_CORE_API bool fips_mmp_acks_check(fips_mmp_acks *s, const fips_node_addr *peer, const fips_mmp_link_evidence *l, uint64_t now_ms, fips_mmp_resend_reason *out);
FIPS_CORE_API bool fips_mmp_acks_outstanding(const fips_mmp_acks *s, const fips_node_addr *peer, uint64_t *counter);
FIPS_CORE_API void fips_mmp_acks_remove(fips_mmp_acks *s, const fips_node_addr *peer);

#ifdef __cplusplus
}
#endif
#endif
