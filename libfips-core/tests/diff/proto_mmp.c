#include "proto_mmp.h"
#include <fips/proto/mmp/wire.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
result check_proto_sender_report(diff_context *ctx, const proto_case *c)
{
    const diff_value *input=FIELD(c->record, "input");
    fips_mmp_sender_report v={0}, decoded;
    v.interval_start_counter=(uint64_t)UINT(input, "interval_start_counter");
    v.interval_end_counter=(uint64_t)UINT(input, "interval_end_counter");
    v.interval_start_timestamp=(uint32_t)UINT(input, "interval_start_timestamp");
    v.interval_end_timestamp=(uint32_t)UINT(input, "interval_end_timestamp");
    v.interval_bytes_sent=(uint32_t)UINT(input, "interval_bytes_sent");
    v.cumulative_packets_sent=(uint64_t)UINT(input, "cumulative_packets_sent");
    v.cumulative_bytes_sent=(uint64_t)UINT(input, "cumulative_bytes_sent");
    diff_bytes bytes=BYTES(c->record, "bytes");
    if (bytes.len!=48 || UINT(c->record, "decode_offset")!=1) return diff_error(ctx, "MMP size/offset"), FAILED;
    uint8_t encoded[48]; fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    if (fips_mmp_sender_report_encode(&v, &w, NULL)!=FIPS_OK ||
        !diff_compare(ctx->id, bytes.data, bytes.len, encoded, w.pos)) return FAILED;
    fips_reader r; fips_reader_init(&r, bytes.data+1, bytes.len-1);
    if (fips_mmp_sender_report_decode(&r, &decoded, NULL)!=FIPS_OK) return diff_error(ctx, "MMP decode"), FAILED;
    if (v.interval_start_counter!=decoded.interval_start_counter) return diff_error(ctx, "MMP field interval_start_counter"), FAILED;
    if (v.interval_end_counter!=decoded.interval_end_counter) return diff_error(ctx, "MMP field interval_end_counter"), FAILED;
    if (v.interval_start_timestamp!=decoded.interval_start_timestamp) return diff_error(ctx, "MMP field interval_start_timestamp"), FAILED;
    if (v.interval_end_timestamp!=decoded.interval_end_timestamp) return diff_error(ctx, "MMP field interval_end_timestamp"), FAILED;
    if (v.interval_bytes_sent!=decoded.interval_bytes_sent) return diff_error(ctx, "MMP field interval_bytes_sent"), FAILED;
    if (v.cumulative_packets_sent!=decoded.cumulative_packets_sent) return diff_error(ctx, "MMP field cumulative_packets_sent"), FAILED;
    if (v.cumulative_bytes_sent!=decoded.cumulative_bytes_sent) return diff_error(ctx, "MMP field cumulative_bytes_sent"), FAILED;
    char debug[1024];
    snprintf(debug, sizeof(debug), "SenderReport { interval_start_counter: %" PRIu64 ", interval_end_counter: %" PRIu64 ", interval_start_timestamp: %u, interval_end_timestamp: %u, interval_bytes_sent: %u, cumulative_packets_sent: %" PRIu64 ", cumulative_bytes_sent: %" PRIu64 " }", decoded.interval_start_counter, decoded.interval_end_counter, (unsigned)decoded.interval_start_timestamp, (unsigned)decoded.interval_end_timestamp, (unsigned)decoded.interval_bytes_sent, decoded.cumulative_packets_sent, decoded.cumulative_bytes_sent);
    if (strcmp(debug, TEXT(c->record, "decoded_debug"))) return diff_error(ctx, "MMP Debug fields"), FAILED;
    fips_proto_error error;
    fips_reader_init(&r, NULL, 0);
    const diff_value *expected=FIELD(c->record, "empty_input_error");
    if (fips_mmp_sender_report_decode(&r, &decoded, &error)!=FIPS_ERR_RANGE ||
        error.variant!=FIPS_PROTO_MessageTooShort ||
        strcmp(fips_proto_error_name(error.variant), TEXT(expected, "variant")) ||
        error.as.message_too_short.expected!=UINT(expected, "expected") ||
        error.as.message_too_short.got!=UINT(expected, "got")) return diff_error(ctx, "MMP error payload"), FAILED;
    return CHECKED;
}
result check_proto_session_sender_report(diff_context *ctx, const proto_case *c)
{
    const diff_value *input=FIELD(c->record, "input");
    fips_mmp_session_sender_report v={0}, decoded;
    v.interval_start_counter=(uint64_t)UINT(input, "interval_start_counter");
    v.interval_end_counter=(uint64_t)UINT(input, "interval_end_counter");
    v.interval_start_timestamp=(uint32_t)UINT(input, "interval_start_timestamp");
    v.interval_end_timestamp=(uint32_t)UINT(input, "interval_end_timestamp");
    v.interval_bytes_sent=(uint32_t)UINT(input, "interval_bytes_sent");
    v.cumulative_packets_sent=(uint64_t)UINT(input, "cumulative_packets_sent");
    v.cumulative_bytes_sent=(uint64_t)UINT(input, "cumulative_bytes_sent");
    diff_bytes bytes=BYTES(c->record, "bytes");
    if (bytes.len!=46 || UINT(c->record, "decode_offset")!=0) return diff_error(ctx, "MMP size/offset"), FAILED;
    uint8_t encoded[46]; fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    if (fips_mmp_session_sender_report_encode(&v, &w, NULL)!=FIPS_OK ||
        !diff_compare(ctx->id, bytes.data, bytes.len, encoded, w.pos)) return FAILED;
    fips_reader r; fips_reader_init(&r, bytes.data+0, bytes.len-0);
    if (fips_mmp_session_sender_report_decode(&r, &decoded, NULL)!=FIPS_OK) return diff_error(ctx, "MMP decode"), FAILED;
    if (v.interval_start_counter!=decoded.interval_start_counter) return diff_error(ctx, "MMP field interval_start_counter"), FAILED;
    if (v.interval_end_counter!=decoded.interval_end_counter) return diff_error(ctx, "MMP field interval_end_counter"), FAILED;
    if (v.interval_start_timestamp!=decoded.interval_start_timestamp) return diff_error(ctx, "MMP field interval_start_timestamp"), FAILED;
    if (v.interval_end_timestamp!=decoded.interval_end_timestamp) return diff_error(ctx, "MMP field interval_end_timestamp"), FAILED;
    if (v.interval_bytes_sent!=decoded.interval_bytes_sent) return diff_error(ctx, "MMP field interval_bytes_sent"), FAILED;
    if (v.cumulative_packets_sent!=decoded.cumulative_packets_sent) return diff_error(ctx, "MMP field cumulative_packets_sent"), FAILED;
    if (v.cumulative_bytes_sent!=decoded.cumulative_bytes_sent) return diff_error(ctx, "MMP field cumulative_bytes_sent"), FAILED;
    char debug[1024];
    snprintf(debug, sizeof(debug), "SessionSenderReport { interval_start_counter: %" PRIu64 ", interval_end_counter: %" PRIu64 ", interval_start_timestamp: %u, interval_end_timestamp: %u, interval_bytes_sent: %u, cumulative_packets_sent: %" PRIu64 ", cumulative_bytes_sent: %" PRIu64 " }", decoded.interval_start_counter, decoded.interval_end_counter, (unsigned)decoded.interval_start_timestamp, (unsigned)decoded.interval_end_timestamp, (unsigned)decoded.interval_bytes_sent, decoded.cumulative_packets_sent, decoded.cumulative_bytes_sent);
    if (strcmp(debug, TEXT(c->record, "decoded_debug"))) return diff_error(ctx, "MMP Debug fields"), FAILED;
    fips_proto_error error;
    fips_reader_init(&r, NULL, 0);
    const diff_value *expected=FIELD(c->record, "empty_input_error");
    if (fips_mmp_session_sender_report_decode(&r, &decoded, &error)!=FIPS_ERR_RANGE ||
        error.variant!=FIPS_PROTO_MessageTooShort ||
        strcmp(fips_proto_error_name(error.variant), TEXT(expected, "variant")) ||
        error.as.message_too_short.expected!=UINT(expected, "expected") ||
        error.as.message_too_short.got!=UINT(expected, "got")) return diff_error(ctx, "MMP error payload"), FAILED;
    return CHECKED;
}
result check_proto_receiver_report(diff_context *ctx, const proto_case *c)
{
    const diff_value *input=FIELD(c->record, "input");
    fips_mmp_receiver_report v={0}, decoded;
    v.highest_counter=(uint64_t)UINT(input, "highest_counter");
    v.cumulative_packets_recv=(uint64_t)UINT(input, "cumulative_packets_recv");
    v.cumulative_bytes_recv=(uint64_t)UINT(input, "cumulative_bytes_recv");
    v.timestamp_echo=(uint32_t)UINT(input, "timestamp_echo");
    v.dwell_time=(uint16_t)UINT(input, "dwell_time");
    v.max_burst_loss=(uint16_t)UINT(input, "max_burst_loss");
    v.mean_burst_loss=(uint16_t)UINT(input, "mean_burst_loss");
    v.jitter=(uint32_t)UINT(input, "jitter");
    v.ecn_ce_count=(uint32_t)UINT(input, "ecn_ce_count");
    v.owd_trend=(int32_t)FIELD(input, "owd_trend")->as.signed_number;
    v.burst_loss_count=(uint32_t)UINT(input, "burst_loss_count");
    v.cumulative_reorder_count=(uint32_t)UINT(input, "cumulative_reorder_count");
    v.interval_packets_recv=(uint32_t)UINT(input, "interval_packets_recv");
    v.interval_bytes_recv=(uint32_t)UINT(input, "interval_bytes_recv");
    diff_bytes bytes=BYTES(c->record, "bytes");
    if (bytes.len!=68 || UINT(c->record, "decode_offset")!=1) return diff_error(ctx, "MMP size/offset"), FAILED;
    uint8_t encoded[68]; fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    if (fips_mmp_receiver_report_encode(&v, &w, NULL)!=FIPS_OK ||
        !diff_compare(ctx->id, bytes.data, bytes.len, encoded, w.pos)) return FAILED;
    fips_reader r; fips_reader_init(&r, bytes.data+1, bytes.len-1);
    if (fips_mmp_receiver_report_decode(&r, &decoded, NULL)!=FIPS_OK) return diff_error(ctx, "MMP decode"), FAILED;
    if (v.highest_counter!=decoded.highest_counter) return diff_error(ctx, "MMP field highest_counter"), FAILED;
    if (v.cumulative_packets_recv!=decoded.cumulative_packets_recv) return diff_error(ctx, "MMP field cumulative_packets_recv"), FAILED;
    if (v.cumulative_bytes_recv!=decoded.cumulative_bytes_recv) return diff_error(ctx, "MMP field cumulative_bytes_recv"), FAILED;
    if (v.timestamp_echo!=decoded.timestamp_echo) return diff_error(ctx, "MMP field timestamp_echo"), FAILED;
    if (v.dwell_time!=decoded.dwell_time) return diff_error(ctx, "MMP field dwell_time"), FAILED;
    if (v.max_burst_loss!=decoded.max_burst_loss) return diff_error(ctx, "MMP field max_burst_loss"), FAILED;
    if (v.mean_burst_loss!=decoded.mean_burst_loss) return diff_error(ctx, "MMP field mean_burst_loss"), FAILED;
    if (v.jitter!=decoded.jitter) return diff_error(ctx, "MMP field jitter"), FAILED;
    if (v.ecn_ce_count!=decoded.ecn_ce_count) return diff_error(ctx, "MMP field ecn_ce_count"), FAILED;
    if (v.owd_trend!=decoded.owd_trend) return diff_error(ctx, "MMP field owd_trend"), FAILED;
    if (v.burst_loss_count!=decoded.burst_loss_count) return diff_error(ctx, "MMP field burst_loss_count"), FAILED;
    if (v.cumulative_reorder_count!=decoded.cumulative_reorder_count) return diff_error(ctx, "MMP field cumulative_reorder_count"), FAILED;
    if (v.interval_packets_recv!=decoded.interval_packets_recv) return diff_error(ctx, "MMP field interval_packets_recv"), FAILED;
    if (v.interval_bytes_recv!=decoded.interval_bytes_recv) return diff_error(ctx, "MMP field interval_bytes_recv"), FAILED;
    char debug[1024];
    snprintf(debug, sizeof(debug), "ReceiverReport { highest_counter: %" PRIu64 ", cumulative_packets_recv: %" PRIu64 ", cumulative_bytes_recv: %" PRIu64 ", timestamp_echo: %u, dwell_time: %u, max_burst_loss: %u, mean_burst_loss: %u, jitter: %u, ecn_ce_count: %u, owd_trend: %" PRId32 ", burst_loss_count: %u, cumulative_reorder_count: %u, interval_packets_recv: %u, interval_bytes_recv: %u }", decoded.highest_counter, decoded.cumulative_packets_recv, decoded.cumulative_bytes_recv, (unsigned)decoded.timestamp_echo, (unsigned)decoded.dwell_time, (unsigned)decoded.max_burst_loss, (unsigned)decoded.mean_burst_loss, (unsigned)decoded.jitter, (unsigned)decoded.ecn_ce_count, decoded.owd_trend, (unsigned)decoded.burst_loss_count, (unsigned)decoded.cumulative_reorder_count, (unsigned)decoded.interval_packets_recv, (unsigned)decoded.interval_bytes_recv);
    if (strcmp(debug, TEXT(c->record, "decoded_debug"))) return diff_error(ctx, "MMP Debug fields"), FAILED;
    fips_proto_error error;
    fips_reader_init(&r, NULL, 0);
    const diff_value *expected=FIELD(c->record, "empty_input_error");
    if (fips_mmp_receiver_report_decode(&r, &decoded, &error)!=FIPS_ERR_RANGE ||
        error.variant!=FIPS_PROTO_MessageTooShort ||
        strcmp(fips_proto_error_name(error.variant), TEXT(expected, "variant")) ||
        error.as.message_too_short.expected!=UINT(expected, "expected") ||
        error.as.message_too_short.got!=UINT(expected, "got")) return diff_error(ctx, "MMP error payload"), FAILED;
    return CHECKED;
}
result check_proto_session_receiver_report(diff_context *ctx, const proto_case *c)
{
    const diff_value *input=FIELD(c->record, "input");
    fips_mmp_session_receiver_report v={0}, decoded;
    v.highest_counter=(uint64_t)UINT(input, "highest_counter");
    v.cumulative_packets_recv=(uint64_t)UINT(input, "cumulative_packets_recv");
    v.cumulative_bytes_recv=(uint64_t)UINT(input, "cumulative_bytes_recv");
    v.timestamp_echo=(uint32_t)UINT(input, "timestamp_echo");
    v.dwell_time=(uint16_t)UINT(input, "dwell_time");
    v.max_burst_loss=(uint16_t)UINT(input, "max_burst_loss");
    v.mean_burst_loss=(uint16_t)UINT(input, "mean_burst_loss");
    v.jitter=(uint32_t)UINT(input, "jitter");
    v.ecn_ce_count=(uint32_t)UINT(input, "ecn_ce_count");
    v.owd_trend=(int32_t)FIELD(input, "owd_trend")->as.signed_number;
    v.burst_loss_count=(uint32_t)UINT(input, "burst_loss_count");
    v.cumulative_reorder_count=(uint32_t)UINT(input, "cumulative_reorder_count");
    v.interval_packets_recv=(uint32_t)UINT(input, "interval_packets_recv");
    v.interval_bytes_recv=(uint32_t)UINT(input, "interval_bytes_recv");
    diff_bytes bytes=BYTES(c->record, "bytes");
    if (bytes.len!=66 || UINT(c->record, "decode_offset")!=0) return diff_error(ctx, "MMP size/offset"), FAILED;
    uint8_t encoded[66]; fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    if (fips_mmp_session_receiver_report_encode(&v, &w, NULL)!=FIPS_OK ||
        !diff_compare(ctx->id, bytes.data, bytes.len, encoded, w.pos)) return FAILED;
    fips_reader r; fips_reader_init(&r, bytes.data+0, bytes.len-0);
    if (fips_mmp_session_receiver_report_decode(&r, &decoded, NULL)!=FIPS_OK) return diff_error(ctx, "MMP decode"), FAILED;
    if (v.highest_counter!=decoded.highest_counter) return diff_error(ctx, "MMP field highest_counter"), FAILED;
    if (v.cumulative_packets_recv!=decoded.cumulative_packets_recv) return diff_error(ctx, "MMP field cumulative_packets_recv"), FAILED;
    if (v.cumulative_bytes_recv!=decoded.cumulative_bytes_recv) return diff_error(ctx, "MMP field cumulative_bytes_recv"), FAILED;
    if (v.timestamp_echo!=decoded.timestamp_echo) return diff_error(ctx, "MMP field timestamp_echo"), FAILED;
    if (v.dwell_time!=decoded.dwell_time) return diff_error(ctx, "MMP field dwell_time"), FAILED;
    if (v.max_burst_loss!=decoded.max_burst_loss) return diff_error(ctx, "MMP field max_burst_loss"), FAILED;
    if (v.mean_burst_loss!=decoded.mean_burst_loss) return diff_error(ctx, "MMP field mean_burst_loss"), FAILED;
    if (v.jitter!=decoded.jitter) return diff_error(ctx, "MMP field jitter"), FAILED;
    if (v.ecn_ce_count!=decoded.ecn_ce_count) return diff_error(ctx, "MMP field ecn_ce_count"), FAILED;
    if (v.owd_trend!=decoded.owd_trend) return diff_error(ctx, "MMP field owd_trend"), FAILED;
    if (v.burst_loss_count!=decoded.burst_loss_count) return diff_error(ctx, "MMP field burst_loss_count"), FAILED;
    if (v.cumulative_reorder_count!=decoded.cumulative_reorder_count) return diff_error(ctx, "MMP field cumulative_reorder_count"), FAILED;
    if (v.interval_packets_recv!=decoded.interval_packets_recv) return diff_error(ctx, "MMP field interval_packets_recv"), FAILED;
    if (v.interval_bytes_recv!=decoded.interval_bytes_recv) return diff_error(ctx, "MMP field interval_bytes_recv"), FAILED;
    char debug[1024];
    snprintf(debug, sizeof(debug), "SessionReceiverReport { highest_counter: %" PRIu64 ", cumulative_packets_recv: %" PRIu64 ", cumulative_bytes_recv: %" PRIu64 ", timestamp_echo: %u, dwell_time: %u, max_burst_loss: %u, mean_burst_loss: %u, jitter: %u, ecn_ce_count: %u, owd_trend: %" PRId32 ", burst_loss_count: %u, cumulative_reorder_count: %u, interval_packets_recv: %u, interval_bytes_recv: %u }", decoded.highest_counter, decoded.cumulative_packets_recv, decoded.cumulative_bytes_recv, (unsigned)decoded.timestamp_echo, (unsigned)decoded.dwell_time, (unsigned)decoded.max_burst_loss, (unsigned)decoded.mean_burst_loss, (unsigned)decoded.jitter, (unsigned)decoded.ecn_ce_count, decoded.owd_trend, (unsigned)decoded.burst_loss_count, (unsigned)decoded.cumulative_reorder_count, (unsigned)decoded.interval_packets_recv, (unsigned)decoded.interval_bytes_recv);
    if (strcmp(debug, TEXT(c->record, "decoded_debug"))) return diff_error(ctx, "MMP Debug fields"), FAILED;
    fips_proto_error error;
    fips_reader_init(&r, NULL, 0);
    const diff_value *expected=FIELD(c->record, "empty_input_error");
    if (fips_mmp_session_receiver_report_decode(&r, &decoded, &error)!=FIPS_ERR_RANGE ||
        error.variant!=FIPS_PROTO_MessageTooShort ||
        strcmp(fips_proto_error_name(error.variant), TEXT(expected, "variant")) ||
        error.as.message_too_short.expected!=UINT(expected, "expected") ||
        error.as.message_too_short.got!=UINT(expected, "got")) return diff_error(ctx, "MMP error payload"), FAILED;
    return CHECKED;
}
result check_proto_path_mtu(diff_context *ctx, const proto_case *c)
{
    const diff_value *input=FIELD(c->record, "input");
    fips_mmp_path_mtu_notification v={0}, decoded;
    v.path_mtu=(uint16_t)UINT(input, "path_mtu");
    diff_bytes bytes=BYTES(c->record, "bytes");
    if (bytes.len!=2 || UINT(c->record, "decode_offset")!=0) return diff_error(ctx, "MMP size/offset"), FAILED;
    uint8_t encoded[2]; fips_writer w;
    fips_writer_init(&w, encoded, sizeof(encoded));
    if (fips_mmp_path_mtu_notification_encode(&v, &w, NULL)!=FIPS_OK ||
        !diff_compare(ctx->id, bytes.data, bytes.len, encoded, w.pos)) return FAILED;
    fips_reader r; fips_reader_init(&r, bytes.data+0, bytes.len-0);
    if (fips_mmp_path_mtu_notification_decode(&r, &decoded, NULL)!=FIPS_OK) return diff_error(ctx, "MMP decode"), FAILED;
    if (v.path_mtu!=decoded.path_mtu) return diff_error(ctx, "MMP field path_mtu"), FAILED;
    char debug[1024];
    snprintf(debug, sizeof(debug), "PathMtuNotification { path_mtu: %u }", (unsigned)decoded.path_mtu);
    if (strcmp(debug, TEXT(c->record, "decoded_debug"))) return diff_error(ctx, "MMP Debug fields"), FAILED;
    fips_proto_error error;
    fips_reader_init(&r, NULL, 0);
    const diff_value *expected=FIELD(c->record, "empty_input_error");
    if (fips_mmp_path_mtu_notification_decode(&r, &decoded, &error)!=FIPS_ERR_RANGE ||
        error.variant!=FIPS_PROTO_MessageTooShort ||
        strcmp(fips_proto_error_name(error.variant), TEXT(expected, "variant")) ||
        error.as.message_too_short.expected!=UINT(expected, "expected") ||
        error.as.message_too_short.got!=UINT(expected, "got")) return diff_error(ctx, "MMP error payload"), FAILED;
    return CHECKED;
}
