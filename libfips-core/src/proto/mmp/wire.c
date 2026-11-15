#include <fips/proto/mmp/wire.h>
#include <string.h>
#include <limits.h>

fips_status fips_mmp_sender_report_encode(const fips_mmp_sender_report *v, fips_writer *w, fips_proto_error *error)
{
    uint8_t buf[48] = {0};
    fips_writer b; fips_writer_init(&b, buf, sizeof(buf));
    fips_writer_write_u8(&b, 1, NULL);
    b.pos = 4;
    fips_writer_write_u64_le(&b, v->interval_start_counter, NULL);
    fips_writer_write_u64_le(&b, v->interval_end_counter, NULL);
    fips_writer_write_u32_le(&b, v->interval_start_timestamp, NULL);
    fips_writer_write_u32_le(&b, v->interval_end_timestamp, NULL);
    fips_writer_write_u32_le(&b, v->interval_bytes_sent, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_packets_sent, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_bytes_sent, NULL);
    return fips_writer_write_bytes(w, buf, sizeof(buf), error);
}
fips_status fips_mmp_sender_report_decode(fips_reader *r, fips_mmp_sender_report *out, fips_proto_error *error)
{
    if (fips_reader_require(r, 47, error) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader b = *r;
    fips_mmp_sender_report v = {0};
    b.pos += 3;
    fips_reader_read_u64_le(&b, &v.interval_start_counter, NULL);
    fips_reader_read_u64_le(&b, &v.interval_end_counter, NULL);
    fips_reader_read_u32_le(&b, &v.interval_start_timestamp, NULL);
    fips_reader_read_u32_le(&b, &v.interval_end_timestamp, NULL);
    fips_reader_read_u32_le(&b, &v.interval_bytes_sent, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_packets_sent, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_bytes_sent, NULL);
    *out = v; *r = b; return FIPS_OK;
}

fips_status fips_mmp_session_sender_report_encode(const fips_mmp_session_sender_report *v, fips_writer *w, fips_proto_error *error)
{
    uint8_t buf[46] = {0};
    fips_writer b; fips_writer_init(&b, buf, sizeof(buf));
    b.pos = 2;
    fips_writer_write_u64_le(&b, v->interval_start_counter, NULL);
    fips_writer_write_u64_le(&b, v->interval_end_counter, NULL);
    fips_writer_write_u32_le(&b, v->interval_start_timestamp, NULL);
    fips_writer_write_u32_le(&b, v->interval_end_timestamp, NULL);
    fips_writer_write_u32_le(&b, v->interval_bytes_sent, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_packets_sent, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_bytes_sent, NULL);
    return fips_writer_write_bytes(w, buf, sizeof(buf), error);
}
fips_status fips_mmp_session_sender_report_decode(fips_reader *r, fips_mmp_session_sender_report *out, fips_proto_error *error)
{
    if (fips_reader_require(r, 46, error) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader b = *r;
    fips_mmp_session_sender_report v = {0};
    b.pos += 2;
    fips_reader_read_u64_le(&b, &v.interval_start_counter, NULL);
    fips_reader_read_u64_le(&b, &v.interval_end_counter, NULL);
    fips_reader_read_u32_le(&b, &v.interval_start_timestamp, NULL);
    fips_reader_read_u32_le(&b, &v.interval_end_timestamp, NULL);
    fips_reader_read_u32_le(&b, &v.interval_bytes_sent, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_packets_sent, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_bytes_sent, NULL);
    *out = v; *r = b; return FIPS_OK;
}

fips_status fips_mmp_receiver_report_encode(const fips_mmp_receiver_report *v, fips_writer *w, fips_proto_error *error)
{
    uint8_t buf[68] = {0};
    fips_writer b; fips_writer_init(&b, buf, sizeof(buf));
    fips_writer_write_u8(&b, 2, NULL);
    b.pos = 4;
    fips_writer_write_u64_le(&b, v->highest_counter, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_packets_recv, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_bytes_recv, NULL);
    fips_writer_write_u32_le(&b, v->timestamp_echo, NULL);
    fips_writer_write_u16_le(&b, v->dwell_time, NULL);
    fips_writer_write_u16_le(&b, v->max_burst_loss, NULL);
    fips_writer_write_u16_le(&b, v->mean_burst_loss, NULL);
    b.pos += 2; /* reserved */
    fips_writer_write_u32_le(&b, v->jitter, NULL);
    fips_writer_write_u32_le(&b, v->ecn_ce_count, NULL);
    fips_writer_write_u32_le(&b, (uint32_t)v->owd_trend, NULL);
    fips_writer_write_u32_le(&b, v->burst_loss_count, NULL);
    fips_writer_write_u32_le(&b, v->cumulative_reorder_count, NULL);
    fips_writer_write_u32_le(&b, v->interval_packets_recv, NULL);
    fips_writer_write_u32_le(&b, v->interval_bytes_recv, NULL);
    return fips_writer_write_bytes(w, buf, sizeof(buf), error);
}
fips_status fips_mmp_receiver_report_decode(fips_reader *r, fips_mmp_receiver_report *out, fips_proto_error *error)
{
    if (fips_reader_require(r, 67, error) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader b = *r;
    fips_mmp_receiver_report v = {0};
    b.pos += 3;
    fips_reader_read_u64_le(&b, &v.highest_counter, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_packets_recv, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_bytes_recv, NULL);
    fips_reader_read_u32_le(&b, &v.timestamp_echo, NULL);
    fips_reader_read_u16_le(&b, &v.dwell_time, NULL);
    fips_reader_read_u16_le(&b, &v.max_burst_loss, NULL);
    fips_reader_read_u16_le(&b, &v.mean_burst_loss, NULL);
    b.pos += 2;
    fips_reader_read_u32_le(&b, &v.jitter, NULL);
    fips_reader_read_u32_le(&b, &v.ecn_ce_count, NULL);
    uint32_t signed_bits;
    fips_reader_read_u32_le(&b, &signed_bits, NULL);
    v.owd_trend = signed_bits <= INT32_MAX ? (int32_t)signed_bits : -1 - (int32_t)(UINT32_MAX - signed_bits);
    fips_reader_read_u32_le(&b, &v.burst_loss_count, NULL);
    fips_reader_read_u32_le(&b, &v.cumulative_reorder_count, NULL);
    fips_reader_read_u32_le(&b, &v.interval_packets_recv, NULL);
    fips_reader_read_u32_le(&b, &v.interval_bytes_recv, NULL);
    *out = v; *r = b; return FIPS_OK;
}

fips_status fips_mmp_session_receiver_report_encode(const fips_mmp_session_receiver_report *v, fips_writer *w, fips_proto_error *error)
{
    uint8_t buf[66] = {0};
    fips_writer b; fips_writer_init(&b, buf, sizeof(buf));
    b.pos = 2;
    fips_writer_write_u64_le(&b, v->highest_counter, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_packets_recv, NULL);
    fips_writer_write_u64_le(&b, v->cumulative_bytes_recv, NULL);
    fips_writer_write_u32_le(&b, v->timestamp_echo, NULL);
    fips_writer_write_u16_le(&b, v->dwell_time, NULL);
    fips_writer_write_u16_le(&b, v->max_burst_loss, NULL);
    fips_writer_write_u16_le(&b, v->mean_burst_loss, NULL);
    b.pos += 2; /* reserved */
    fips_writer_write_u32_le(&b, v->jitter, NULL);
    fips_writer_write_u32_le(&b, v->ecn_ce_count, NULL);
    fips_writer_write_u32_le(&b, (uint32_t)v->owd_trend, NULL);
    fips_writer_write_u32_le(&b, v->burst_loss_count, NULL);
    fips_writer_write_u32_le(&b, v->cumulative_reorder_count, NULL);
    fips_writer_write_u32_le(&b, v->interval_packets_recv, NULL);
    fips_writer_write_u32_le(&b, v->interval_bytes_recv, NULL);
    return fips_writer_write_bytes(w, buf, sizeof(buf), error);
}
fips_status fips_mmp_session_receiver_report_decode(fips_reader *r, fips_mmp_session_receiver_report *out, fips_proto_error *error)
{
    if (fips_reader_require(r, 66, error) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader b = *r;
    fips_mmp_session_receiver_report v = {0};
    b.pos += 2;
    fips_reader_read_u64_le(&b, &v.highest_counter, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_packets_recv, NULL);
    fips_reader_read_u64_le(&b, &v.cumulative_bytes_recv, NULL);
    fips_reader_read_u32_le(&b, &v.timestamp_echo, NULL);
    fips_reader_read_u16_le(&b, &v.dwell_time, NULL);
    fips_reader_read_u16_le(&b, &v.max_burst_loss, NULL);
    fips_reader_read_u16_le(&b, &v.mean_burst_loss, NULL);
    b.pos += 2;
    fips_reader_read_u32_le(&b, &v.jitter, NULL);
    fips_reader_read_u32_le(&b, &v.ecn_ce_count, NULL);
    uint32_t signed_bits;
    fips_reader_read_u32_le(&b, &signed_bits, NULL);
    v.owd_trend = signed_bits <= INT32_MAX ? (int32_t)signed_bits : -1 - (int32_t)(UINT32_MAX - signed_bits);
    fips_reader_read_u32_le(&b, &v.burst_loss_count, NULL);
    fips_reader_read_u32_le(&b, &v.cumulative_reorder_count, NULL);
    fips_reader_read_u32_le(&b, &v.interval_packets_recv, NULL);
    fips_reader_read_u32_le(&b, &v.interval_bytes_recv, NULL);
    *out = v; *r = b; return FIPS_OK;
}

fips_status fips_mmp_path_mtu_notification_encode(const fips_mmp_path_mtu_notification *v, fips_writer *w, fips_proto_error *error)
{
    uint8_t buf[2] = {0};
    fips_writer b; fips_writer_init(&b, buf, sizeof(buf));
    fips_writer_write_u16_le(&b, v->path_mtu, NULL);
    return fips_writer_write_bytes(w, buf, sizeof(buf), error);
}
fips_status fips_mmp_path_mtu_notification_decode(fips_reader *r, fips_mmp_path_mtu_notification *out, fips_proto_error *error)
{
    if (fips_reader_require(r, 2, error) != FIPS_OK) return FIPS_ERR_RANGE;
    fips_reader b = *r;
    fips_mmp_path_mtu_notification v = {0};
    fips_reader_read_u16_le(&b, &v.path_mtu, NULL);
    *out = v; *r = b; return FIPS_OK;
}

fips_mmp_session_sender_report fips_mmp_session_sender_report_from_sender_report(const fips_mmp_sender_report *v)
{
    return (fips_mmp_session_sender_report){.interval_start_counter = v->interval_start_counter, .interval_end_counter = v->interval_end_counter, .interval_start_timestamp = v->interval_start_timestamp, .interval_end_timestamp = v->interval_end_timestamp, .interval_bytes_sent = v->interval_bytes_sent, .cumulative_packets_sent = v->cumulative_packets_sent, .cumulative_bytes_sent = v->cumulative_bytes_sent};
}
fips_mmp_sender_report fips_mmp_sender_report_from_session_sender_report(const fips_mmp_session_sender_report *v)
{
    return (fips_mmp_sender_report){.interval_start_counter = v->interval_start_counter, .interval_end_counter = v->interval_end_counter, .interval_start_timestamp = v->interval_start_timestamp, .interval_end_timestamp = v->interval_end_timestamp, .interval_bytes_sent = v->interval_bytes_sent, .cumulative_packets_sent = v->cumulative_packets_sent, .cumulative_bytes_sent = v->cumulative_bytes_sent};
}
fips_mmp_session_receiver_report fips_mmp_session_receiver_report_from_receiver_report(const fips_mmp_receiver_report *v)
{
    return (fips_mmp_session_receiver_report){.highest_counter = v->highest_counter, .cumulative_packets_recv = v->cumulative_packets_recv, .cumulative_bytes_recv = v->cumulative_bytes_recv, .timestamp_echo = v->timestamp_echo, .dwell_time = v->dwell_time, .max_burst_loss = v->max_burst_loss, .mean_burst_loss = v->mean_burst_loss, .jitter = v->jitter, .ecn_ce_count = v->ecn_ce_count, .owd_trend = v->owd_trend, .burst_loss_count = v->burst_loss_count, .cumulative_reorder_count = v->cumulative_reorder_count, .interval_packets_recv = v->interval_packets_recv, .interval_bytes_recv = v->interval_bytes_recv};
}
fips_mmp_receiver_report fips_mmp_receiver_report_from_session_receiver_report(const fips_mmp_session_receiver_report *v)
{
    return (fips_mmp_receiver_report){.highest_counter = v->highest_counter, .cumulative_packets_recv = v->cumulative_packets_recv, .cumulative_bytes_recv = v->cumulative_bytes_recv, .timestamp_echo = v->timestamp_echo, .dwell_time = v->dwell_time, .max_burst_loss = v->max_burst_loss, .mean_burst_loss = v->mean_burst_loss, .jitter = v->jitter, .ecn_ce_count = v->ecn_ce_count, .owd_trend = v->owd_trend, .burst_loss_count = v->burst_loss_count, .cumulative_reorder_count = v->cumulative_reorder_count, .interval_packets_recv = v->interval_packets_recv, .interval_bytes_recv = v->interval_bytes_recv};
}
