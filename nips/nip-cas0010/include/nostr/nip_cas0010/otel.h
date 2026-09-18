#ifndef NOSTR_NIP_CAS0010_OTEL_H
#define NOSTR_NIP_CAS0010_OTEL_H

/*
 * NIP-CAS-0010 — OTLP-over-Nostr transport primitives.
 *
 * Producer/consumer primitives for carrying OpenTelemetry OTLP payloads over
 * Nostr as ephemeral kinds 24900 (traces), 24901 (metrics) and 24902 (logs).
 *
 * This module transports OPAQUE OTLP bytes. It never parses or serializes OTLP
 * protobuf/JSON: the caller supplies already-encoded OTLP bytes and receives the
 * same bytes back on the consumer side.
 *
 * REDACTION IS THE CALLER'S RESPONSIBILITY. Events are plaintext (base64 is
 * not encryption) and readable by any relay subscriber, and this module cannot
 * see inside the opaque OTLP bytes, so it performs no redaction. Redact
 * secrets/PII (secret-like attribute keys; Authorization/Bearer credentials,
 * nsec1 keys, bunker:// URIs, Cashu tokens, JWTs, URL userinfo, secret env
 * assignments, hex private keys in attribute values, log bodies, span names and
 * status messages) BEFORE serializing and passing OTLP bytes to
 * nostr_otel_producer_build_event() / nostr_otel_producer_publish(). See
 * README.md "Redaction is the caller's responsibility".
 *
 * Wire rules implemented here (NIP-CAS-0010 §3):
 *   - `event.content` is ALWAYS base64 (standard alphabet, padded), even for
 *     the `identity` compression.
 *   - Tags: `domain`, `schema`, optional `service`, at most one `enc`, optional
 *     `p` recipients.
 *   - `enc` is `["enc", <format>, <compression>]` and MUST appear at most once.
 *     A missing `enc` defaults to `otlp-proto` + `identity`.
 *   - `domain` and `schema` MUST each appear exactly once with exactly two
 *     elements, and MUST match the event kind.
 *
 * Memory ownership follows the repository convention: every `*_new()` has a
 * matching `*_free()`, functions returning heap buffers document the caller's
 * obligation to `free()` them, and failures return a negative NostrOtelError
 * (or NULL) leaving out-parameters untouched/NULL.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-simple-pool.h"
#include "nostr-tag.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ kinds */

#define NOSTR_OTEL_KIND_TRACES  24900
#define NOSTR_OTEL_KIND_METRICS 24901
#define NOSTR_OTEL_KIND_LOGS    24902

/* ------------------------------------------------------------- tag names */

#define NOSTR_OTEL_TAG_ENC     "enc"
#define NOSTR_OTEL_TAG_DOMAIN  "domain"
#define NOSTR_OTEL_TAG_SCHEMA  "schema"
#define NOSTR_OTEL_TAG_SERVICE "service"
#define NOSTR_OTEL_TAG_P       "p"

/* --------------------------------------------------------------- defaults */

/** Recommended per-event cap on `content` length (NIP-CAS-0010 §3.2). */
#define NOSTR_OTEL_DEFAULT_MAX_EVENT_BYTES (64u * 1024u)
/** Body size above which a zstd-capable producer compresses. */
#define NOSTR_OTEL_DEFAULT_COMPRESSION_THRESHOLD 1024u
/** Consumer bound on decoded body size (decompression-bomb guard). */
#define NOSTR_OTEL_DEFAULT_MAX_DECODED_BYTES (16u * 1024u * 1024u)

/* ---------------------------------------------------------------- errors */

typedef enum {
    NOSTR_OTEL_OK = 0,
    NOSTR_OTEL_ERR_INVALID_ARG = -1,
    NOSTR_OTEL_ERR_NOMEM = -2,
    NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING = -3,
    NOSTR_OTEL_ERR_MALFORMED_EVENT = -4,
    NOSTR_OTEL_ERR_INVALID_SIGNATURE = -5,
    NOSTR_OTEL_ERR_UNEXPECTED_KIND = -6,
    NOSTR_OTEL_ERR_UNADMITTED = -7,
    NOSTR_OTEL_ERR_PAYLOAD_TOO_LARGE = -8,
    NOSTR_OTEL_ERR_BATCH_TOO_LARGE = -9,
    NOSTR_OTEL_ERR_SIGN = -10,
    NOSTR_OTEL_ERR_PUBLISH = -11,
    NOSTR_OTEL_ERR_COMPRESSION = -12,
    NOSTR_OTEL_ERR_HANDLER = -13
} NostrOtelError;

/** Returns: (transfer none): static human-readable description of @code. */
const char *nostr_otel_strerror(int code);

/* --------------------------------------------------------------- signals */

typedef enum {
    NOSTR_OTEL_SIGNAL_NONE = 0,
    NOSTR_OTEL_SIGNAL_TRACES = 1,
    NOSTR_OTEL_SIGNAL_METRICS = 2,
    NOSTR_OTEL_SIGNAL_LOGS = 3
} NostrOtelSignal;

/** Returns: the NIP-CAS-0010 kind for @signal, or 0 when unknown. */
int nostr_otel_signal_kind(NostrOtelSignal signal);
/** Returns: the signal for @kind, or NOSTR_OTEL_SIGNAL_NONE when unknown. */
NostrOtelSignal nostr_otel_signal_for_kind(int kind);
/** Returns: (transfer none) (nullable): the `domain` tag value for @signal. */
const char *nostr_otel_signal_domain(NostrOtelSignal signal);
/** Returns: (transfer none) (nullable): the `schema` tag value for @signal. */
const char *nostr_otel_signal_schema(NostrOtelSignal signal);

/* -------------------------------------------------------------- encoding */

typedef enum {
    NOSTR_OTEL_FORMAT_UNKNOWN = 0,
    NOSTR_OTEL_FORMAT_OTLP_PROTO = 1,
    NOSTR_OTEL_FORMAT_OTLP_JSON = 2
} NostrOtelFormat;

typedef enum {
    NOSTR_OTEL_COMPRESSION_UNKNOWN = 0,
    NOSTR_OTEL_COMPRESSION_IDENTITY = 1,
    NOSTR_OTEL_COMPRESSION_ZSTD = 2
} NostrOtelCompression;

typedef struct {
    NostrOtelFormat format;
    NostrOtelCompression compression;
} NostrOtelEncoding;

/** Returns: (transfer none) (nullable): the wire token for @format. */
const char *nostr_otel_format_string(NostrOtelFormat format);
/** Returns: (transfer none) (nullable): the wire token for @compression. */
const char *nostr_otel_compression_string(NostrOtelCompression compression);

/** Returns: whether this build can produce and consume zstd bodies. */
bool nostr_otel_zstd_available(void);

/**
 * nostr_otel_parse_encoding:
 * @tags: (nullable): event tags
 * @out: (out): parsed encoding
 *
 * Reads the optional `enc` tag. A missing tag yields `otlp-proto`+`identity`.
 * Duplicate, short, or overlong `enc` tags are NOSTR_OTEL_ERR_MALFORMED_EVENT;
 * unknown tokens are NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING.
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_parse_encoding(const NostrTags *tags, NostrOtelEncoding *out);

/* ---------------------------------------------------------------- base64 */

/**
 * nostr_otel_base64_encode:
 * @data: (array length=len) (nullable): bytes to encode
 * @len: number of bytes
 * @out: (out) (transfer full): NUL-terminated standard-alphabet base64
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError. Caller frees *@out.
 */
int nostr_otel_base64_encode(const uint8_t *data, size_t len, char **out);

/**
 * nostr_otel_base64_decode:
 * @text: (nullable): NUL-terminated base64 (padded, no embedded whitespace)
 * @out: (out) (transfer full): decoded bytes, always NUL-terminated for safety
 * @out_len: (out): decoded length
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError. Caller frees *@out.
 */
int nostr_otel_base64_decode(const char *text, uint8_t **out, size_t *out_len);

/* ------------------------------------------------------- body encode/decode */

/**
 * nostr_otel_encode_body:
 * @body: (array length=len): opaque OTLP bytes
 * @len: body length
 * @requested: compression to use when @len exceeds @threshold
 * @threshold: size above which @requested is applied; SIZE_MAX disables it
 * @out_content: (out) (transfer full): base64 event content
 * @out_used: (out) (optional): compression actually applied
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError. Caller frees *@out_content.
 */
int nostr_otel_encode_body(const uint8_t *body, size_t len,
                           NostrOtelCompression requested, size_t threshold,
                           char **out_content, NostrOtelCompression *out_used);

/**
 * nostr_otel_decode_body:
 * @content: (nullable): base64 event content
 * @enc: advertised encoding
 * @max_decoded: bound on decoded size; 0 selects NOSTR_OTEL_DEFAULT_MAX_DECODED_BYTES
 * @out_body: (out) (transfer full): decoded opaque OTLP bytes
 * @out_len: (out): decoded length
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError. Caller frees *@out_body.
 */
int nostr_otel_decode_body(const char *content, NostrOtelEncoding enc,
                           size_t max_decoded, uint8_t **out_body, size_t *out_len);

/* ---------------------------------------------------------------- signer */

/**
 * NostrOtelSignFn:
 * @event: (transfer none): unsigned event with kind/tags/content/created_at set
 * @user_data: signer state
 *
 * Signs @event in place, setting `pubkey`, `id` and `sig`. Implementations may
 * be remote (Signet / NIP-46) or local. Returns 0 on success.
 */
typedef int (*NostrOtelSignFn)(NostrEvent *event, void *user_data);

typedef struct {
    NostrOtelSignFn sign;
    void *user_data;
} NostrOtelSigner;

/**
 * NostrOtelLocalSigner:
 *
 * Convenience local-private-key signer. It exists for TESTS and local
 * development only — production producers MUST use a Signet/NIP-46 signer.
 * No private key is ever embedded in this module; the caller supplies one,
 * typically from nostr_key_generate_private().
 */
typedef struct NostrOtelLocalSigner NostrOtelLocalSigner;

/** Returns: (transfer full) (nullable): signer holding a copy of @privkey_hex. */
NostrOtelLocalSigner *nostr_otel_local_signer_new(const char *privkey_hex);
/** @signer: (transfer full) (nullable): signer to free; wipes the key copy. */
void nostr_otel_local_signer_free(NostrOtelLocalSigner *signer);
/** Fills @out with the callback/user-data pair for @signer. */
int nostr_otel_local_signer_bind(NostrOtelLocalSigner *signer, NostrOtelSigner *out);
/** Returns: (transfer none) (nullable): the signer's x-only public key hex. */
const char *nostr_otel_local_signer_pubkey(const NostrOtelLocalSigner *signer);

/* -------------------------------------------------------------- producer */

typedef struct {
    const uint8_t *data;
    size_t len;
} NostrOtelPayload;

/**
 * NostrOtelPublishFn:
 * @event: (transfer none): signed event; the callback must not free it
 *
 * Returns: 0 on success, non-zero on delivery failure.
 */
typedef int (*NostrOtelPublishFn)(NostrEvent *event, void *user_data);

typedef struct {
    NostrOtelSigner signer;            /* required */
    NostrOtelPublishFn publish;        /* required */
    void *publish_user_data;
    NostrOtelFormat format;            /* UNKNOWN => otlp-proto */
    NostrOtelCompression compression;  /* UNKNOWN => identity */
    size_t compression_threshold;      /* 0 => default; only used with zstd */
    size_t max_event_bytes;            /* 0 => default 64 KiB of content */
    const char *service;               /* optional `service` tag value */
    const char *const *recipients;     /* optional hex pubkeys for `p` tags */
    size_t recipient_count;
} NostrOtelProducerConfig;

typedef struct NostrOtelProducer NostrOtelProducer;

/**
 * nostr_otel_producer_new:
 * @cfg: (transfer none): configuration; strings are copied
 * @out: (out) (transfer full): new producer
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_producer_new(const NostrOtelProducerConfig *cfg, NostrOtelProducer **out);
/** @producer: (transfer full) (nullable) */
void nostr_otel_producer_free(NostrOtelProducer *producer);

/**
 * nostr_otel_producer_build_event:
 * @producer: (transfer none)
 * @signal: signal type
 * @body: (array length=len): opaque OTLP bytes for ONE event
 * @len: body length
 * @out_event: (out) (transfer full): signed event
 *
 * Builds and signs a single event without publishing it. @body is NOT
 * redacted: the caller must scrub secrets/PII before serializing it. Returns
 * NOSTR_OTEL_ERR_BATCH_TOO_LARGE when the encoded content exceeds the
 * configured cap. Caller frees *@out_event with nostr_event_free().
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_producer_build_event(NostrOtelProducer *producer, NostrOtelSignal signal,
                                    const uint8_t *body, size_t len, NostrEvent **out_event);

/**
 * nostr_otel_producer_publish:
 * @producer: (transfer none)
 * @signal: signal type
 * @payloads: (array length=count) (transfer none): opaque OTLP payloads
 * @count: number of payloads
 * @out_event_count: (out) (optional): number of events published
 *
 * Batches @payloads into as few events as fit under the per-event cap and
 * publishes each. @payloads are published as-is (plaintext): redact
 * secrets/PII before serializing them. Payloads are concatenated, which is the protobuf
 * repeated-field merge of OTLP `TracesData`/`MetricsData`/`LogsData` messages,
 * so a batch decodes as one OTLP message on the consumer side.
 *
 * Because this module does not parse OTLP, a SINGLE payload that cannot fit
 * under the cap is reported as NOSTR_OTEL_ERR_BATCH_TOO_LARGE; splitting it is
 * the caller's responsibility (it owns the OTLP structure).
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_producer_publish(NostrOtelProducer *producer, NostrOtelSignal signal,
                                const NostrOtelPayload *payloads, size_t count,
                                size_t *out_event_count);

/**
 * nostr_otel_publish_via_pool:
 *
 * A NostrOtelPublishFn that publishes to every relay currently in the
 * NostrSimplePool passed as @user_data, using nostr_relay_publish().
 * Returns 0 when at least one relay accepted the write.
 */
int nostr_otel_publish_via_pool(NostrEvent *event, void *user_data);

/* -------------------------------------------------------------- consumer */

typedef enum {
    NOSTR_OTEL_ADMISSION_DROP = 0, /**< reject events from unadmitted pubkeys */
    NOSTR_OTEL_ADMISSION_FLAG = 1, /**< deliver them with admitted = false */
    NOSTR_OTEL_ADMISSION_OPEN = 2  /**< explicitly admit every valid signer */
} NostrOtelAdmissionPolicy;

/** Returns: whether @pubkey_hex (x-only, lowercase hex) is admitted. */
typedef bool (*NostrOtelAdmitFn)(const char *pubkey_hex, void *user_data);

typedef struct {
    NostrOtelSignal signal;
    const NostrEvent *event;    /**< borrowed, valid for the callback only */
    const char *pubkey;         /**< borrowed signer pubkey (attribution) */
    const char *event_id;       /**< borrowed event id */
    bool admitted;
    const char *service;        /**< borrowed `service` tag value, or NULL */
    NostrOtelEncoding encoding;
    const uint8_t *payload;     /**< borrowed opaque OTLP bytes */
    size_t payload_len;
} NostrOtelReceived;

/** Returns: 0 on success; non-zero is surfaced as NOSTR_OTEL_ERR_HANDLER. */
typedef int (*NostrOtelHandlerFn)(const NostrOtelReceived *received, void *user_data);

/** Observes rejected events and handler failures. */
typedef void (*NostrOtelErrorFn)(int err, const NostrEvent *event, void *user_data);

typedef struct {
    const NostrOtelSignal *signals; /**< NULL/0 => all three signals */
    size_t signal_count;
    NostrOtelAdmitFn admit;         /**< required unless policy is OPEN */
    void *admit_user_data;
    NostrOtelAdmissionPolicy policy;
    size_t max_decoded_bytes;       /**< 0 => default */
    NostrOtelHandlerFn handler;     /**< required */
    void *handler_user_data;
    NostrOtelErrorFn on_error;      /**< optional */
    void *error_user_data;
} NostrOtelConsumerConfig;

typedef struct NostrOtelConsumer NostrOtelConsumer;

/**
 * nostr_otel_consumer_new:
 * @cfg: (transfer none): configuration; copied
 * @out: (out) (transfer full): new consumer
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_consumer_new(const NostrOtelConsumerConfig *cfg, NostrOtelConsumer **out);
/** @consumer: (transfer full) (nullable) */
void nostr_otel_consumer_free(NostrOtelConsumer *consumer);

/**
 * nostr_otel_consumer_filters:
 * @consumer: (transfer none)
 *
 * Returns: (transfer full) (nullable): filters matching the configured kinds.
 * Caller frees with nostr_filters_free().
 */
NostrFilters *nostr_otel_consumer_filters(const NostrOtelConsumer *consumer);

/**
 * nostr_otel_consumer_process:
 * @consumer: (transfer none)
 * @event: (transfer none): event to verify, validate, decode and dispatch
 *
 * Verifies the id+signature, checks the kind against the configured signals,
 * validates `domain`/`schema`/`enc` tags, applies the admission policy, decodes
 * the body and invokes the handler with the opaque OTLP bytes attributed to the
 * signing pubkey. It touches no network, so bridges and tests can feed events
 * from any source.
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_consumer_process(NostrOtelConsumer *consumer, NostrEvent *event);

/**
 * nostr_otel_consumer_subscribe:
 * @consumer: (transfer none)
 * @pool: (transfer none): pool that will deliver events
 * @urls: (array length=url_count) (transfer none): relay URLs
 * @url_count: number of URLs
 * @async: use nostr_simple_pool_subscribe_async() instead of the blocking dial
 *
 * Installs the consumer as the pool's extended event middleware and subscribes
 * to the configured kinds. Events delivered by the pool are routed through
 * nostr_otel_consumer_process(). The pool must outlive the consumer's use.
 *
 * Returns: NOSTR_OTEL_OK or a negative NostrOtelError.
 */
int nostr_otel_consumer_subscribe(NostrOtelConsumer *consumer, NostrSimplePool *pool,
                                  const char **urls, size_t url_count, bool async);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP_CAS0010_OTEL_H */
