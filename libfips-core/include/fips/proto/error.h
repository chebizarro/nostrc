#ifndef FIPS_PROTO_ERROR_H
#define FIPS_PROTO_ERROR_H

#include <fips/export.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Variant suffixes match proto::Error at the upstream pin. */
typedef enum fips_proto_error_variant {
    FIPS_PROTO_InvalidMessageType,
    FIPS_PROTO_MessageTooShort,
    FIPS_PROTO_MessageTooLong,
    FIPS_PROTO_InvalidSignature,
    FIPS_PROTO_UnsupportedVersion,
    FIPS_PROTO_Malformed,
    FIPS_PROTO_BadSizeClass,
    FIPS_PROTO_BadCoord,
    FIPS_PROTO_BadBloom,
    FIPS_PROTO_HopLimitExceeded,
    FIPS_PROTO_TtlExpired
} fips_proto_error_variant;

/* Nested errors are value types, not pointers to future codec objects. */
typedef enum fips_coord_error {
    FIPS_COORD_EmptyCoordinate
} fips_coord_error;
typedef enum fips_bloom_error_variant {
    FIPS_BLOOM_InvalidSize,
    FIPS_BLOOM_SizeNotByteAligned,
    FIPS_BLOOM_ZeroHashCount
} fips_bloom_error_variant;
typedef struct fips_bloom_error {
    fips_bloom_error_variant variant;
    union {
        struct { size_t expected, got; } invalid_size;
        size_t size_not_byte_aligned;
    } as;
} fips_bloom_error;

typedef struct fips_proto_error {
    fips_proto_error_variant variant;
    union {
        uint8_t invalid_message_type;
        struct { size_t expected, got; } message_too_short;
        struct { size_t max, got; } message_too_long;
        uint8_t unsupported_version;
        const char *malformed; /* Borrowed static field name, as in Rust. */
        struct { uint8_t got, max; } bad_size_class;
        fips_coord_error bad_coord;
        fips_bloom_error bad_bloom;
    } as;
} fips_proto_error;

/** Static Rust variant name; "Unknown" for an unrecognized discriminant. */
FIPS_CORE_API const char *fips_proto_error_name(fips_proto_error_variant variant);

#ifdef __cplusplus
}
#endif
#endif /* FIPS_PROTO_ERROR_H */
