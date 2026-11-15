#ifndef FIPS_PROTO_MMP_CORE_H
#define FIPS_PROTO_MMP_CORE_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <fips/identity/node_addr.h>
#include <fips/proto/mmp/state.h>
typedef struct fips_mmp_peer_liveness_snapshot { fips_node_addr peer; bool time_dead, rekey_active, heartbeat_due; } fips_mmp_peer_liveness_snapshot;
typedef struct fips_mmp_link_report_snapshot {
    fips_node_addr peer; fips_mmp_mode mode; bool send_sr, send_rr, sr_due, rr_due, log_due;
} fips_mmp_link_report_snapshot;
typedef struct fips_mmp_session_report_snapshot {
    fips_node_addr dest; fips_mmp_mode mode; bool sr_due, rr_due, mtu_due, log_due;
} fips_mmp_session_report_snapshot;
typedef enum fips_mmp_link_report_kind { FIPS_MMP_LinkSender, FIPS_MMP_LinkReceiver } fips_mmp_link_report_kind;
typedef enum fips_mmp_session_report_kind { FIPS_MMP_SessionSender, FIPS_MMP_SessionReceiver, FIPS_MMP_SessionPathMtu } fips_mmp_session_report_kind;
typedef enum fips_mmp_action_kind { FIPS_MMP_ReapPeer, FIPS_MMP_Heartbeat, FIPS_MMP_SendLinkReport, FIPS_MMP_LogLink, FIPS_MMP_SendSessionReport, FIPS_MMP_LogSession } fips_mmp_action_kind;
typedef struct fips_mmp_action {
    fips_mmp_action_kind kind;
    fips_node_addr peer; /* dest for session actions */
    union { fips_mmp_link_report_kind link; fips_mmp_session_report_kind session; } report;
} fips_mmp_action;
typedef struct fips_mmp_action_list { fips_mmp_action *items; size_t len, capacity; } fips_mmp_action_list;
typedef struct fips_mmp_send_result { fips_node_addr dest; bool ok; } fips_mmp_send_result;
typedef enum fips_mmp_backoff_kind { FIPS_MMP_Success, FIPS_MMP_Failure } fips_mmp_backoff_kind;
typedef struct fips_mmp_backoff_update { fips_mmp_backoff_kind kind; fips_node_addr dest; } fips_mmp_backoff_update;
typedef struct fips_mmp_backoff_list { fips_mmp_backoff_update *items; size_t len, capacity; } fips_mmp_backoff_list;
/* Stateless Mmp anchor is a namespace in C. Append atomically to caller list;
 * RANGE leaves list and items untouched. Snapshot/storage ranges must not
 * overlap; len <= capacity and required pointers valid. Logs precede sends.
 * Host builds only while driving actions; heartbeat success is marked only
 * after a successful send, separately from the attempt stamp. */
FIPS_CORE_API fips_status fips_mmp_plan_heartbeats(const fips_mmp_peer_liveness_snapshot *peers, size_t count, fips_mmp_action_list *out);
FIPS_CORE_API fips_status fips_mmp_plan_link_reports(const fips_mmp_link_report_snapshot *peers, size_t count, fips_mmp_action_list *out);
FIPS_CORE_API fips_status fips_mmp_plan_session_reports(const fips_mmp_session_report_snapshot *sessions, size_t count, fips_mmp_action_list *out);
/* OR-fold success per distinct address, emitted in address-sorted order. */
FIPS_CORE_API fips_status fips_mmp_plan_backoff(const fips_mmp_send_result *results, size_t count, fips_mmp_backoff_list *out);

#ifdef __cplusplus
}
#endif
#endif
