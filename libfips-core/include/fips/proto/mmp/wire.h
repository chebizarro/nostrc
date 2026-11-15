#ifndef FIPS_PROTO_MMP_WIRE_H
#define FIPS_PROTO_MMP_WIRE_H
#include <fips/proto/codec.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Encoders write the type byte for link reports; decoders start AFTER it.
 * Session codecs consume only the body (no inner header). Reserved bytes are
 * ignored on decode and zero on encode; trailing input is left unread.
 * Atomic on failure: output and cursor unchanged. Required pointers are valid;
 * buffers do not overlap objects. No allocations. */
#define FIPS_MMP_SENDER_REPORT_SIZE 48u
#define FIPS_MMP_RECEIVER_REPORT_SIZE 68u
#define FIPS_MMP_SESSION_SENDER_REPORT_SIZE 46u
#define FIPS_MMP_SESSION_RECEIVER_REPORT_SIZE 66u
#define FIPS_MMP_PATH_MTU_NOTIFICATION_SIZE 2u
typedef struct fips_mmp_sender_report {
    uint64_t interval_start_counter;
    uint64_t interval_end_counter;
    uint32_t interval_start_timestamp;
    uint32_t interval_end_timestamp;
    uint32_t interval_bytes_sent;
    uint64_t cumulative_packets_sent;
    uint64_t cumulative_bytes_sent;
} fips_mmp_sender_report;
FIPS_CORE_API fips_status fips_mmp_sender_report_encode(const fips_mmp_sender_report *v, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_mmp_sender_report_decode(fips_reader *r, fips_mmp_sender_report *out, fips_proto_error *error);
typedef struct fips_mmp_session_sender_report {
    uint64_t interval_start_counter;
    uint64_t interval_end_counter;
    uint32_t interval_start_timestamp;
    uint32_t interval_end_timestamp;
    uint32_t interval_bytes_sent;
    uint64_t cumulative_packets_sent;
    uint64_t cumulative_bytes_sent;
} fips_mmp_session_sender_report;
FIPS_CORE_API fips_status fips_mmp_session_sender_report_encode(const fips_mmp_session_sender_report *v, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_mmp_session_sender_report_decode(fips_reader *r, fips_mmp_session_sender_report *out, fips_proto_error *error);
typedef struct fips_mmp_receiver_report {
    uint64_t highest_counter;
    uint64_t cumulative_packets_recv;
    uint64_t cumulative_bytes_recv;
    uint32_t timestamp_echo;
    uint16_t dwell_time;
    uint16_t max_burst_loss;
    uint16_t mean_burst_loss;
    uint32_t jitter;
    uint32_t ecn_ce_count;
    int32_t owd_trend;
    uint32_t burst_loss_count;
    uint32_t cumulative_reorder_count;
    uint32_t interval_packets_recv;
    uint32_t interval_bytes_recv;
} fips_mmp_receiver_report;
FIPS_CORE_API fips_status fips_mmp_receiver_report_encode(const fips_mmp_receiver_report *v, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_mmp_receiver_report_decode(fips_reader *r, fips_mmp_receiver_report *out, fips_proto_error *error);
typedef struct fips_mmp_session_receiver_report {
    uint64_t highest_counter;
    uint64_t cumulative_packets_recv;
    uint64_t cumulative_bytes_recv;
    uint32_t timestamp_echo;
    uint16_t dwell_time;
    uint16_t max_burst_loss;
    uint16_t mean_burst_loss;
    uint32_t jitter;
    uint32_t ecn_ce_count;
    int32_t owd_trend;
    uint32_t burst_loss_count;
    uint32_t cumulative_reorder_count;
    uint32_t interval_packets_recv;
    uint32_t interval_bytes_recv;
} fips_mmp_session_receiver_report;
FIPS_CORE_API fips_status fips_mmp_session_receiver_report_encode(const fips_mmp_session_receiver_report *v, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_mmp_session_receiver_report_decode(fips_reader *r, fips_mmp_session_receiver_report *out, fips_proto_error *error);
typedef struct fips_mmp_path_mtu_notification {
    uint16_t path_mtu;
} fips_mmp_path_mtu_notification;
FIPS_CORE_API fips_status fips_mmp_path_mtu_notification_encode(const fips_mmp_path_mtu_notification *v, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_mmp_path_mtu_notification_decode(fips_reader *r, fips_mmp_path_mtu_notification *out, fips_proto_error *error);
FIPS_CORE_API fips_mmp_session_sender_report fips_mmp_session_sender_report_from_sender_report(const fips_mmp_sender_report *v);
FIPS_CORE_API fips_mmp_sender_report fips_mmp_sender_report_from_session_sender_report(const fips_mmp_session_sender_report *v);
FIPS_CORE_API fips_mmp_session_receiver_report fips_mmp_session_receiver_report_from_receiver_report(const fips_mmp_receiver_report *v);
FIPS_CORE_API fips_mmp_receiver_report fips_mmp_receiver_report_from_session_receiver_report(const fips_mmp_session_receiver_report *v);

#ifdef __cplusplus
}
#endif
#endif
