/* NIP-CAS-0010: producer — build, sign and publish OTLP-over-Nostr events. */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nostr-keys.h"
#include "nostr-relay.h"
#include "otel_internal.h"

struct NostrOtelProducer {
    NostrOtelSigner signer;
    NostrOtelPublishFn publish;
    void *publish_user_data;
    NostrOtelFormat format;
    NostrOtelCompression compression;
    size_t compression_threshold;
    size_t max_event_bytes;
    char *service;
    char **recipients;
    size_t recipient_count;
};

/* ------------------------------------------------------------- local signer */

struct NostrOtelLocalSigner {
    char *privkey_hex;
    size_t privkey_len;
    char *pubkey_hex;
};

static int local_signer_sign(NostrEvent *event, void *user_data) {
    NostrOtelLocalSigner *signer = (NostrOtelLocalSigner *)user_data;
    if (!event || !signer || !signer->privkey_hex) return -1;
    return nostr_event_sign(event, signer->privkey_hex);
}

NostrOtelLocalSigner *nostr_otel_local_signer_new(const char *privkey_hex) {
    if (!privkey_hex || strlen(privkey_hex) != 64) return NULL;
    NostrOtelLocalSigner *signer = calloc(1, sizeof(*signer));
    if (!signer) return NULL;
    signer->privkey_hex = strdup(privkey_hex);
    if (!signer->privkey_hex) {
        free(signer);
        return NULL;
    }
    signer->privkey_len = strlen(signer->privkey_hex);
    signer->pubkey_hex = nostr_key_get_public(privkey_hex);
    if (!signer->pubkey_hex) {
        nostr_otel_local_signer_free(signer);
        return NULL;
    }
    return signer;
}

void nostr_otel_local_signer_free(NostrOtelLocalSigner *signer) {
    if (!signer) return;
    if (signer->privkey_hex) {
        /* Wipe the key copy before returning the allocation to the heap. */
        memset(signer->privkey_hex, 0, signer->privkey_len);
        free(signer->privkey_hex);
    }
    free(signer->pubkey_hex);
    free(signer);
}

int nostr_otel_local_signer_bind(NostrOtelLocalSigner *signer, NostrOtelSigner *out) {
    if (!signer || !out) return NOSTR_OTEL_ERR_INVALID_ARG;
    out->sign = local_signer_sign;
    out->user_data = signer;
    return NOSTR_OTEL_OK;
}

const char *nostr_otel_local_signer_pubkey(const NostrOtelLocalSigner *signer) {
    return signer ? signer->pubkey_hex : NULL;
}

/* ----------------------------------------------------------- pool publisher */

int nostr_otel_publish_via_pool(NostrEvent *event, void *user_data) {
    NostrSimplePool *pool = (NostrSimplePool *)user_data;
    if (!event || !pool) return NOSTR_OTEL_ERR_INVALID_ARG;

    int delivered = 0;
    pthread_mutex_lock(&pool->pool_mutex);
    for (size_t i = 0; i < pool->relay_count; i++) {
        NostrRelay *relay = pool->relays[i];
        if (!relay) continue;
        nostr_relay_publish(relay, event);
        delivered++;
    }
    pthread_mutex_unlock(&pool->pool_mutex);
    return delivered > 0 ? 0 : NOSTR_OTEL_ERR_PUBLISH;
}

/* -------------------------------------------------------------- lifecycle */

int nostr_otel_producer_new(const NostrOtelProducerConfig *cfg, NostrOtelProducer **out) {
    if (!out) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out = NULL;
    if (!cfg || !cfg->signer.sign || !cfg->publish) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (cfg->recipient_count > 0 && !cfg->recipients) return NOSTR_OTEL_ERR_INVALID_ARG;

    NostrOtelFormat format = cfg->format;
    if (format == NOSTR_OTEL_FORMAT_UNKNOWN) format = NOSTR_OTEL_FORMAT_OTLP_PROTO;
    if (!nostr_otel_format_string(format)) return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;

    NostrOtelCompression compression = cfg->compression;
    if (compression == NOSTR_OTEL_COMPRESSION_UNKNOWN) {
        compression = NOSTR_OTEL_COMPRESSION_IDENTITY;
    }
    if (!nostr_otel_compression_string(compression)) return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
    if (compression == NOSTR_OTEL_COMPRESSION_ZSTD && !nostr_otel_zstd_available()) {
        return NOSTR_OTEL_ERR_UNSUPPORTED_ENCODING;
    }

    NostrOtelProducer *producer = calloc(1, sizeof(*producer));
    if (!producer) return NOSTR_OTEL_ERR_NOMEM;
    producer->signer = cfg->signer;
    producer->publish = cfg->publish;
    producer->publish_user_data = cfg->publish_user_data;
    producer->format = format;
    producer->compression = compression;
    producer->compression_threshold = cfg->compression_threshold
                                          ? cfg->compression_threshold
                                          : NOSTR_OTEL_DEFAULT_COMPRESSION_THRESHOLD;
    producer->max_event_bytes =
        cfg->max_event_bytes ? cfg->max_event_bytes : NOSTR_OTEL_DEFAULT_MAX_EVENT_BYTES;

    if (cfg->service) {
        producer->service = strdup(cfg->service);
        if (!producer->service) {
            nostr_otel_producer_free(producer);
            return NOSTR_OTEL_ERR_NOMEM;
        }
    }
    if (cfg->recipient_count > 0) {
        producer->recipients = calloc(cfg->recipient_count, sizeof(char *));
        if (!producer->recipients) {
            nostr_otel_producer_free(producer);
            return NOSTR_OTEL_ERR_NOMEM;
        }
        for (size_t i = 0; i < cfg->recipient_count; i++) {
            if (!cfg->recipients[i]) {
                nostr_otel_producer_free(producer);
                return NOSTR_OTEL_ERR_INVALID_ARG;
            }
            producer->recipients[i] = strdup(cfg->recipients[i]);
            if (!producer->recipients[i]) {
                producer->recipient_count = i;
                nostr_otel_producer_free(producer);
                return NOSTR_OTEL_ERR_NOMEM;
            }
        }
        producer->recipient_count = cfg->recipient_count;
    }
    *out = producer;
    return NOSTR_OTEL_OK;
}

void nostr_otel_producer_free(NostrOtelProducer *producer) {
    if (!producer) return;
    free(producer->service);
    for (size_t i = 0; i < producer->recipient_count; i++) {
        free(producer->recipients[i]);
    }
    free(producer->recipients);
    free(producer);
}

/* ------------------------------------------------------------ event build */

static NostrTags *build_tags(const NostrOtelProducer *producer, NostrOtelSignal signal,
                             NostrOtelCompression used) {
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return NULL;

    struct {
        const char *name;
        const char *value;
    } entries[3];
    size_t entry_count = 0;

    entries[entry_count].name = NOSTR_OTEL_TAG_DOMAIN;
    entries[entry_count].value = nostr_otel_signal_domain(signal);
    entry_count++;
    entries[entry_count].name = NOSTR_OTEL_TAG_SCHEMA;
    entries[entry_count].value = nostr_otel_signal_schema(signal);
    entry_count++;
    if (producer->service) {
        entries[entry_count].name = NOSTR_OTEL_TAG_SERVICE;
        entries[entry_count].value = producer->service;
        entry_count++;
    }

    for (size_t i = 0; i < entry_count; i++) {
        NostrTag *tag = nostr_tag_new(entries[i].name, entries[i].value, NULL);
        if (!tag) goto fail;
        nostr_tags_append(tags, tag);
    }

    /* Exactly one well-formed `enc` tag, always emitted so consumers never rely
     * on the default path for our own events. */
    NostrTag *enc = nostr_tag_new(NOSTR_OTEL_TAG_ENC,
                                  nostr_otel_format_string(producer->format),
                                  nostr_otel_compression_string(used), NULL);
    if (!enc) goto fail;
    nostr_tags_append(tags, enc);

    for (size_t i = 0; i < producer->recipient_count; i++) {
        NostrTag *p = nostr_tag_new(NOSTR_OTEL_TAG_P, producer->recipients[i], NULL);
        if (!p) goto fail;
        nostr_tags_append(tags, p);
    }
    return tags;

fail:
    nostr_tags_free(tags);
    return NULL;
}

/* Takes ownership of @content on every path, success or failure. */
static int build_signed_event(NostrOtelProducer *producer, NostrOtelSignal signal,
                              char *content, NostrOtelCompression used,
                              NostrEvent **out_event) {
    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(content);
        return NOSTR_OTEL_ERR_NOMEM;
    }

    NostrTags *tags = build_tags(producer, signal, used);
    if (!tags) {
        nostr_event_free(event);
        free(content);
        return NOSTR_OTEL_ERR_NOMEM;
    }
    event->kind = nostr_otel_signal_kind(signal);
    event->created_at = (int64_t)time(NULL);
    event->tags = tags;
    event->content = content; /* ownership moves into the event */

    if (producer->signer.sign(event, producer->signer.user_data) != 0) {
        nostr_event_free(event);
        return NOSTR_OTEL_ERR_SIGN;
    }
    if (!event->pubkey || !event->sig || !event->id) {
        nostr_event_free(event);
        return NOSTR_OTEL_ERR_SIGN;
    }
    *out_event = event;
    return NOSTR_OTEL_OK;
}

static int encode_and_build(NostrOtelProducer *producer, NostrOtelSignal signal,
                            const uint8_t *body, size_t len, NostrEvent **out_event) {
    char *content = NULL;
    NostrOtelCompression used = NOSTR_OTEL_COMPRESSION_IDENTITY;
    size_t threshold = producer->compression == NOSTR_OTEL_COMPRESSION_ZSTD
                           ? producer->compression_threshold
                           : SIZE_MAX;
    int rc = nostr_otel_encode_body(body, len, producer->compression, threshold, &content, &used);
    if (rc != NOSTR_OTEL_OK) return rc;
    if (strlen(content) > producer->max_event_bytes) {
        free(content);
        return NOSTR_OTEL_ERR_BATCH_TOO_LARGE;
    }
    /* build_signed_event() owns @content from here, including on failure. */
    return build_signed_event(producer, signal, content, used, out_event);
}

int nostr_otel_producer_build_event(NostrOtelProducer *producer, NostrOtelSignal signal,
                                    const uint8_t *body, size_t len, NostrEvent **out_event) {
    if (!out_event) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out_event = NULL;
    if (!producer || (len > 0 && !body)) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (nostr_otel_signal_kind(signal) == 0) return NOSTR_OTEL_ERR_UNEXPECTED_KIND;
    return encode_and_build(producer, signal, body, len, out_event);
}

/* ---------------------------------------------------------------- publish */

/* Reports whether @len bytes encode to content within the producer's cap. */
static int fits(NostrOtelProducer *producer, const uint8_t *body, size_t len, bool *out_fits) {
    char *content = NULL;
    size_t threshold = producer->compression == NOSTR_OTEL_COMPRESSION_ZSTD
                           ? producer->compression_threshold
                           : SIZE_MAX;
    int rc = nostr_otel_encode_body(body, len, producer->compression, threshold, &content, NULL);
    if (rc != NOSTR_OTEL_OK) return rc;
    *out_fits = strlen(content) <= producer->max_event_bytes;
    free(content);
    return NOSTR_OTEL_OK;
}

static int emit(NostrOtelProducer *producer, NostrOtelSignal signal, const uint8_t *body,
                size_t len, size_t *out_event_count) {
    NostrEvent *event = NULL;
    int rc = encode_and_build(producer, signal, body, len, &event);
    if (rc != NOSTR_OTEL_OK) return rc;
    int prc = producer->publish(event, producer->publish_user_data);
    nostr_event_free(event);
    if (prc != 0) return NOSTR_OTEL_ERR_PUBLISH;
    if (out_event_count) (*out_event_count)++;
    return NOSTR_OTEL_OK;
}

int nostr_otel_producer_publish(NostrOtelProducer *producer, NostrOtelSignal signal,
                                const NostrOtelPayload *payloads, size_t count,
                                size_t *out_event_count) {
    if (out_event_count) *out_event_count = 0;
    if (!producer) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (count > 0 && !payloads) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (nostr_otel_signal_kind(signal) == 0) return NOSTR_OTEL_ERR_UNEXPECTED_KIND;
    if (count == 0) return NOSTR_OTEL_OK;

    /* Accumulate payloads until the encoded content would exceed the cap, then
     * flush. Concatenation is the protobuf repeated-field merge of OTLP *Data
     * messages, so a flushed batch stays a single valid OTLP message. */
    uint8_t *batch = NULL;
    size_t batch_len = 0, batch_cap = 0;
    int rc = NOSTR_OTEL_OK;

    for (size_t i = 0; i < count; i++) {
        const NostrOtelPayload *p = &payloads[i];
        if (p->len > 0 && !p->data) {
            rc = NOSTR_OTEL_ERR_INVALID_ARG;
            goto done;
        }
        if (p->len == 0) continue;

        if (batch_len + p->len < batch_len) {
            rc = NOSTR_OTEL_ERR_NOMEM;
            goto done;
        }
        size_t needed = batch_len + p->len;
        if (needed > batch_cap) {
            size_t new_cap = batch_cap ? batch_cap * 2 : needed;
            if (new_cap < needed) new_cap = needed;
            uint8_t *grown = realloc(batch, new_cap);
            if (!grown) {
                rc = NOSTR_OTEL_ERR_NOMEM;
                goto done;
            }
            batch = grown;
            batch_cap = new_cap;
        }
        memcpy(batch + batch_len, p->data, p->len);

        bool ok = false;
        rc = fits(producer, batch, needed, &ok);
        if (rc != NOSTR_OTEL_OK) goto done;
        if (ok) {
            batch_len = needed;
            continue;
        }
        if (batch_len > 0) {
            /* Flush what fitted and retry this payload on an empty batch. */
            rc = emit(producer, signal, batch, batch_len, out_event_count);
            if (rc != NOSTR_OTEL_OK) goto done;
            batch_len = 0;
            i--;
            continue;
        }
        /* A single payload over the cap cannot be split without parsing OTLP. */
        rc = NOSTR_OTEL_ERR_BATCH_TOO_LARGE;
        goto done;
    }
    if (batch_len > 0) {
        rc = emit(producer, signal, batch, batch_len, out_event_count);
    }

done:
    free(batch);
    return rc;
}
