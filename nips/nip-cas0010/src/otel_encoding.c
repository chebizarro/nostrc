/* NIP-CAS-0010: signal mapping, `enc` tag handling and body codec. */

#include <stdlib.h>
#include <string.h>

#ifdef NOSTR_OTEL_HAVE_ZSTD
#include <zstd.h>
#endif

#include "otel_internal.h"

const char *nostr_otel_strerror(int code) {
    switch (code) {
    case NOSTR_OTEL_OK: return "ok";
    case NOSTR_OTEL_ERR_INVALID_ARG: return "invalid argument";
    case NOSTR_OTEL_ERR_NOMEM: return "out of memory";
    case NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING: return "unsupported enc";
    case NOSTR_OTEL_ERR_MALFORMED_EVENT: return "malformed telemetry event";
    case NOSTR_OTEL_ERR_INVALID_SIGNATURE: return "invalid event id or signature";
    case NOSTR_OTEL_ERR_UNEXPECTED_KIND: return "unexpected event kind";
    case NOSTR_OTEL_ERR_UNADMITTED: return "signer pubkey not admitted";
    case NOSTR_OTEL_ERR_PAYLOAD_TOO_LARGE: return "decoded payload exceeds limit";
    case NOSTR_OTEL_ERR_BATCH_TOO_LARGE: return "payload cannot fit under event size limit";
    case NOSTR_OTEL_ERR_SIGN: return "signing failed";
    case NOSTR_OTEL_ERR_PUBLISH: return "publish failed";
    case NOSTR_OTEL_ERR_COMPRESSION: return "compression failed";
    case NOSTR_OTEL_ERR_HANDLER: return "handler reported failure";
    default: return "unknown error";
    }
}

int nostr_otel_signal_kind(NostrOtelSignal signal) {
    switch (signal) {
    case NOSTR_OTEL_SIGNAL_TRACES: return NOSTR_OTEL_KIND_TRACES;
    case NOSTR_OTEL_SIGNAL_METRICS: return NOSTR_OTEL_KIND_METRICS;
    case NOSTR_OTEL_SIGNAL_LOGS: return NOSTR_OTEL_KIND_LOGS;
    default: return 0;
    }
}

NostrOtelSignal nostr_otel_signal_for_kind(int kind) {
    switch (kind) {
    case NOSTR_OTEL_KIND_TRACES: return NOSTR_OTEL_SIGNAL_TRACES;
    case NOSTR_OTEL_KIND_METRICS: return NOSTR_OTEL_SIGNAL_METRICS;
    case NOSTR_OTEL_KIND_LOGS: return NOSTR_OTEL_SIGNAL_LOGS;
    default: return NOSTR_OTEL_SIGNAL_NONE;
    }
}

const char *nostr_otel_signal_domain(NostrOtelSignal signal) {
    switch (signal) {
    case NOSTR_OTEL_SIGNAL_TRACES: return "traces";
    case NOSTR_OTEL_SIGNAL_METRICS: return "metrics";
    case NOSTR_OTEL_SIGNAL_LOGS: return "logs";
    default: return NULL;
    }
}

const char *nostr_otel_signal_schema(NostrOtelSignal signal) {
    switch (signal) {
    case NOSTR_OTEL_SIGNAL_TRACES: return "cascadia.otel.traces.v1";
    case NOSTR_OTEL_SIGNAL_METRICS: return "cascadia.otel.metrics.v1";
    case NOSTR_OTEL_SIGNAL_LOGS: return "cascadia.otel.logs.v1";
    default: return NULL;
    }
}

const char *nostr_otel_format_string(NostrOtelFormat format) {
    switch (format) {
    case NOSTR_OTEL_FORMAT_OTLP_PROTO: return "otlp-proto";
    case NOSTR_OTEL_FORMAT_OTLP_JSON: return "otlp-json";
    default: return NULL;
    }
}

const char *nostr_otel_compression_string(NostrOtelCompression compression) {
    switch (compression) {
    case NOSTR_OTEL_COMPRESSION_IDENTITY: return "identity";
    case NOSTR_OTEL_COMPRESSION_ZSTD: return "zstd";
    default: return NULL;
    }
}

bool nostr_otel_zstd_available(void) {
#ifdef NOSTR_OTEL_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

/* ------------------------------------------------------------ tag helpers */

int nostr_otel_required_tag_value(const NostrTags *tags, const char *name,
                                  const char **out_value) {
    if (!name || !out_value) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out_value = NULL;
    if (!tags) return NOSTR_OTEL_ERR_MALFORMED_EVENT;

    const NostrTag *found = NULL;
    size_t count = 0;
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; i++) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 1) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (key && strcmp(key, name) == 0) {
            found = tag;
            count++;
        }
    }
    /* Exactly one occurrence, exactly two elements: duplicates are a rejection,
     * not a "last one wins" ambiguity a producer could exploit. */
    if (count != 1) return NOSTR_OTEL_ERR_MALFORMED_EVENT;
    if (nostr_tag_size(found) != 2) return NOSTR_OTEL_ERR_MALFORMED_EVENT;
    const char *value = nostr_tag_get(found, 1);
    if (!value) return NOSTR_OTEL_ERR_MALFORMED_EVENT;
    *out_value = value;
    return NOSTR_OTEL_OK;
}

const char *nostr_otel_optional_tag_value(const NostrTags *tags, const char *name) {
    if (!tags || !name) return NULL;
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; i++) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (key && strcmp(key, name) == 0) return nostr_tag_get(tag, 1);
    }
    return NULL;
}

int nostr_otel_parse_encoding(const NostrTags *tags, NostrOtelEncoding *out) {
    if (!out) return NOSTR_OTEL_ERR_INVALID_ARG;
    out->format = NOSTR_OTEL_FORMAT_UNKNOWN;
    out->compression = NOSTR_OTEL_COMPRESSION_UNKNOWN;

    const NostrTag *found = NULL;
    size_t count = 0;
    size_t n = tags ? nostr_tags_size(tags) : 0;
    for (size_t i = 0; i < n; i++) {
        const NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 1) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (key && strcmp(key, NOSTR_OTEL_TAG_ENC) == 0) {
            found = tag;
            count++;
        }
    }
    if (count == 0) {
        /* NIP-CAS-0010 §3.2: a missing `enc` means uncompressed OTLP protobuf. */
        out->format = NOSTR_OTEL_FORMAT_OTLP_PROTO;
        out->compression = NOSTR_OTEL_COMPRESSION_IDENTITY;
        return NOSTR_OTEL_OK;
    }
    if (count > 1) return NOSTR_OTEL_ERR_MALFORMED_EVENT;
    if (nostr_tag_size(found) != 3) return NOSTR_OTEL_ERR_MALFORMED_EVENT;

    const char *format = nostr_tag_get(found, 1);
    const char *compression = nostr_tag_get(found, 2);
    if (!format || !compression) return NOSTR_OTEL_ERR_MALFORMED_EVENT;

    if (strcmp(format, "otlp-proto") == 0) {
        out->format = NOSTR_OTEL_FORMAT_OTLP_PROTO;
    } else if (strcmp(format, "otlp-json") == 0) {
        out->format = NOSTR_OTEL_FORMAT_OTLP_JSON;
    } else {
        return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
    }
    if (strcmp(compression, "identity") == 0) {
        out->compression = NOSTR_OTEL_COMPRESSION_IDENTITY;
    } else if (strcmp(compression, "zstd") == 0) {
        out->compression = NOSTR_OTEL_COMPRESSION_ZSTD;
    } else {
        return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
    }
    return NOSTR_OTEL_OK;
}

/* --------------------------------------------------------------- body codec */

int nostr_otel_encode_body(const uint8_t *body, size_t len,
                           NostrOtelCompression requested, size_t threshold,
                           char **out_content, NostrOtelCompression *out_used) {
    if (!out_content) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out_content = NULL;
    if (out_used) *out_used = NOSTR_OTEL_COMPRESSION_IDENTITY;
    if (len > 0 && !body) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (requested == NOSTR_OTEL_COMPRESSION_UNKNOWN) {
        requested = NOSTR_OTEL_COMPRESSION_IDENTITY;
    }
    if (requested != NOSTR_OTEL_COMPRESSION_IDENTITY &&
        requested != NOSTR_OTEL_COMPRESSION_ZSTD) {
        return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
    }

    NostrOtelCompression used = NOSTR_OTEL_COMPRESSION_IDENTITY;
    const uint8_t *payload = body;
    size_t payload_len = len;
    uint8_t *compressed = NULL;

    if (requested == NOSTR_OTEL_COMPRESSION_ZSTD && len > threshold) {
#ifdef NOSTR_OTEL_HAVE_ZSTD
        size_t bound = ZSTD_compressBound(len);
        compressed = malloc(bound ? bound : 1);
        if (!compressed) return NOSTR_OTEL_ERR_NOMEM;
        size_t written = ZSTD_compress(compressed, bound, body, len, 3);
        if (ZSTD_isError(written)) {
            free(compressed);
            return NOSTR_OTEL_ERR_COMPRESSION;
        }
        payload = compressed;
        payload_len = written;
        used = NOSTR_OTEL_COMPRESSION_ZSTD;
#else
        return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
#endif
    } else if (requested == NOSTR_OTEL_COMPRESSION_ZSTD && !nostr_otel_zstd_available()) {
        return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
    }

    /* Content is base64 for every encoding, identity included (§3.2). */
    int rc = nostr_otel_base64_encode(payload, payload_len, out_content);
    free(compressed);
    if (rc != NOSTR_OTEL_OK) return rc;
    if (out_used) *out_used = used;
    return NOSTR_OTEL_OK;
}

int nostr_otel_decode_body(const char *content, NostrOtelEncoding enc,
                           size_t max_decoded, uint8_t **out_body, size_t *out_len) {
    if (!out_body || !out_len) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out_body = NULL;
    *out_len = 0;
    if (!content) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (max_decoded == 0) max_decoded = NOSTR_OTEL_DEFAULT_MAX_DECODED_BYTES;

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    int rc = nostr_otel_base64_decode(content, &raw, &raw_len);
    if (rc != NOSTR_OTEL_OK) return rc;

    if (enc.compression == NOSTR_OTEL_COMPRESSION_IDENTITY) {
        if (raw_len > max_decoded) {
            free(raw);
            return NOSTR_OTEL_ERR_PAYLOAD_TOO_LARGE;
        }
        *out_body = raw;
        *out_len = raw_len;
        return NOSTR_OTEL_OK;
    }
    if (enc.compression != NOSTR_OTEL_COMPRESSION_ZSTD) {
        free(raw);
        return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
    }

#ifdef NOSTR_OTEL_HAVE_ZSTD
    unsigned long long declared = ZSTD_getFrameContentSize(raw, raw_len);
    if (declared == ZSTD_CONTENTSIZE_ERROR) {
        free(raw);
        return NOSTR_OTEL_ERR_MALFORMED_EVENT;
    }
    /* Refuse streamed frames without a declared size, and anything that
     * declares more than the caller's bound: both are decompression bombs. */
    if (declared == ZSTD_CONTENTSIZE_UNKNOWN || declared > (unsigned long long)max_decoded) {
        free(raw);
        return NOSTR_OTEL_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t out_cap = (size_t)declared;
    uint8_t *plain = malloc(out_cap + 1);
    if (!plain) {
        free(raw);
        return NOSTR_OTEL_ERR_NOMEM;
    }
    size_t written = ZSTD_decompress(plain, out_cap, raw, raw_len);
    free(raw);
    if (ZSTD_isError(written) || written != out_cap) {
        free(plain);
        return NOSTR_OTEL_ERR_MALFORMED_EVENT;
    }
    plain[written] = '\0';
    *out_body = plain;
    *out_len = written;
    return NOSTR_OTEL_OK;
#else
    free(raw);
    return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
#endif
}
