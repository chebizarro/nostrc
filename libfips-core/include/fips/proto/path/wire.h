#ifndef FIPS_PROTO_PATH_WIRE_H
#define FIPS_PROTO_PATH_WIRE_H
#include <fips/proto/link.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_PATH_PROBE_MIN_SIZE 27
#define FIPS_PATH_PROBE_FIXED_SIZE 11
#define FIPS_PATH_ACK_FIXED_SIZE 27
#define FIPS_PATH_CLOSE_SIZE 7
#define FIPS_PATH_TLV_CRITICAL 0x8000u
#define FIPS_PATH_ACTIVE 0x01u
#define FIPS_PATH_EXT 0x02u
#define FIPS_PATH_AGGREGATE 0x01u
typedef uint8_t fips_path_flags;
typedef uint8_t fips_path_caps;
typedef struct fips_path_probe {
    uint32_t probe_id;
    fips_path_flags flags;
    uint32_t path_id;
    fips_path_caps caps;
    size_t len;
} fips_path_probe;
typedef struct fips_path_ack {
    uint32_t probe_id;
    fips_path_flags flags;
    uint32_t path_id, echo_path_id;
    fips_path_caps caps;
    uint32_t rx_time_us, rx_frames, rx_bytes;
    size_t len;
} fips_path_ack;
typedef enum fips_path_close_reason {
    FIPS_PATH_Unspecified, FIPS_PATH_InterfaceGone, FIPS_PATH_CarrierLost, FIPS_PATH_Operator
} fips_path_close_reason;
typedef struct fips_path_close { uint32_t path_id; fips_path_close_reason reason; } fips_path_close;
/* Encode includes type and zero padding; reserved flags/caps are cleared.
 * Decode takes a reader bounded to the body AFTER type, retains reserved bits,
 * validates/skips extension TLVs, and consumes the whole body including padding.
 * All failures leave reader/writer/output unchanged. error may be NULL.
 * Probe decode accepts a short fixed part; answerable is a separate gate.
 * Ack permits echo_path_id=0; Close permits path_id=0 and ignores trailing bytes. */
FIPS_CORE_API fips_path_probe fips_path_probe_new(uint32_t probe_id, uint32_t path_id, fips_path_flags flags, fips_path_caps caps);
FIPS_CORE_API bool fips_path_probe_answerable(const fips_path_probe *p);
FIPS_CORE_API size_t fips_path_ack_len(size_t probe_len, size_t transport_mtu);
FIPS_CORE_API fips_path_close_reason fips_path_close_reason_from_byte(uint8_t b);
FIPS_CORE_API fips_status fips_path_probe_encode(const fips_path_probe *p, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_path_probe_decode(fips_reader *r, fips_path_probe *p, fips_proto_error *e);
FIPS_CORE_API fips_status fips_path_ack_encode(const fips_path_ack *p, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_path_ack_decode(fips_reader *r, fips_path_ack *p, fips_proto_error *e);
FIPS_CORE_API fips_status fips_path_close_encode(const fips_path_close *p, fips_writer *w, fips_proto_error *e);
FIPS_CORE_API fips_status fips_path_close_decode(fips_reader *r, fips_path_close *p, fips_proto_error *e);
#ifdef __cplusplus
}
#endif
#endif
