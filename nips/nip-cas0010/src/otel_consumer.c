/* NIP-CAS-0010: consumer — verify, validate, admit and decode OTLP events. */

#include <stdlib.h>
#include <string.h>

#include "otel_internal.h"

#define OTEL_MAX_SIGNALS 3

struct NostrOtelConsumer {
    NostrOtelSignal signals[OTEL_MAX_SIGNALS];
    size_t signal_count;
    NostrOtelAdmitFn admit;
    void *admit_user_data;
    NostrOtelAdmissionPolicy policy;
    size_t max_decoded_bytes;
    NostrOtelHandlerFn handler;
    void *handler_user_data;
    NostrOtelErrorFn on_error;
    void *error_user_data;
};

static bool consumer_accepts(const NostrOtelConsumer *consumer, NostrOtelSignal signal) {
    for (size_t i = 0; i < consumer->signal_count; i++) {
        if (consumer->signals[i] == signal) return true;
    }
    return false;
}

int nostr_otel_consumer_new(const NostrOtelConsumerConfig *cfg, NostrOtelConsumer **out) {
    if (!out) return NOSTR_OTEL_ERR_INVALID_ARG;
    *out = NULL;
    if (!cfg || !cfg->handler) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (cfg->signal_count > OTEL_MAX_SIGNALS) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (cfg->signal_count > 0 && !cfg->signals) return NOSTR_OTEL_ERR_INVALID_ARG;
    if (cfg->policy != NOSTR_OTEL_ADMISSION_DROP && cfg->policy != NOSTR_OTEL_ADMISSION_FLAG) {
        return NOSTR_OTEL_ERR_INVALID_ARG;
    }

    NostrOtelConsumer *consumer = calloc(1, sizeof(*consumer));
    if (!consumer) return NOSTR_OTEL_ERR_NOMEM;

    if (cfg->signal_count == 0) {
        consumer->signals[0] = NOSTR_OTEL_SIGNAL_TRACES;
        consumer->signals[1] = NOSTR_OTEL_SIGNAL_METRICS;
        consumer->signals[2] = NOSTR_OTEL_SIGNAL_LOGS;
        consumer->signal_count = OTEL_MAX_SIGNALS;
    } else {
        for (size_t i = 0; i < cfg->signal_count; i++) {
            if (nostr_otel_signal_kind(cfg->signals[i]) == 0) {
                free(consumer);
                return NOSTR_OTEL_ERR_INVALID_ARG;
            }
            consumer->signals[i] = cfg->signals[i];
        }
        consumer->signal_count = cfg->signal_count;
    }
    consumer->admit = cfg->admit;
    consumer->admit_user_data = cfg->admit_user_data;
    consumer->policy = cfg->policy;
    consumer->max_decoded_bytes =
        cfg->max_decoded_bytes ? cfg->max_decoded_bytes : NOSTR_OTEL_DEFAULT_MAX_DECODED_BYTES;
    consumer->handler = cfg->handler;
    consumer->handler_user_data = cfg->handler_user_data;
    consumer->on_error = cfg->on_error;
    consumer->error_user_data = cfg->error_user_data;
    *out = consumer;
    return NOSTR_OTEL_OK;
}

void nostr_otel_consumer_free(NostrOtelConsumer *consumer) {
    free(consumer);
}

NostrFilters *nostr_otel_consumer_filters(const NostrOtelConsumer *consumer) {
    if (!consumer) return NULL;
    NostrFilters *filters = nostr_filters_new();
    if (!filters) return NULL;
    NostrFilter *filter = nostr_filter_new();
    if (!filter) {
        nostr_filters_free(filters);
        return NULL;
    }
    int kinds[OTEL_MAX_SIGNALS];
    for (size_t i = 0; i < consumer->signal_count; i++) {
        kinds[i] = nostr_otel_signal_kind(consumer->signals[i]);
    }
    nostr_filter_set_kinds(filter, kinds, consumer->signal_count);
    if (!nostr_filters_add(filters, filter)) {
        nostr_filter_free(filter);
        nostr_filters_free(filters);
        return NULL;
    }
    /* nostr_filters_add() moved the internals and zeroed the source; freeing the
     * emptied shell is the documented cleanup (API.md, "Filters — ownership"). */
    nostr_filter_free(filter);
    return filters;
}

static void report(const NostrOtelConsumer *consumer, int err, NostrEvent *event) {
    if (consumer->on_error) consumer->on_error(err, event, consumer->error_user_data);
}

int nostr_otel_consumer_process(NostrOtelConsumer *consumer, NostrEvent *event) {
    if (!consumer || !event) return NOSTR_OTEL_ERR_INVALID_ARG;

    int rc;

    /* 1. Cryptographic attribution first: everything downstream is attributed to
     *    event->pubkey, so an unverified event is never inspected further. */
    if (!nostr_event_check_signature(event)) {
        rc = NOSTR_OTEL_ERR_INVALID_SIGNATURE;
        goto fail;
    }
    if (!event->pubkey) {
        rc = NOSTR_OTEL_ERR_INVALID_SIGNATURE;
        goto fail;
    }

    /* 2. Kind must be one this consumer subscribed to. */
    NostrOtelSignal signal = nostr_otel_signal_for_kind(event->kind);
    if (signal == NOSTR_OTEL_SIGNAL_NONE || !consumer_accepts(consumer, signal)) {
        rc = NOSTR_OTEL_ERR_UNEXPECTED_KIND;
        goto fail;
    }

    /* 3. Tags: exactly one `domain` and one `schema`, both matching the kind. */
    const char *domain = NULL;
    rc = nostr_otel_required_tag_value(event->tags, NOSTR_OTEL_TAG_DOMAIN, &domain);
    if (rc != NOSTR_OTEL_OK) goto fail;
    if (strcmp(domain, nostr_otel_signal_domain(signal)) != 0) {
        rc = NOSTR_OTEL_ERR_MALFORMED_EVENT;
        goto fail;
    }
    const char *schema = NULL;
    rc = nostr_otel_required_tag_value(event->tags, NOSTR_OTEL_TAG_SCHEMA, &schema);
    if (rc != NOSTR_OTEL_OK) goto fail;
    if (strcmp(schema, nostr_otel_signal_schema(signal)) != 0) {
        rc = NOSTR_OTEL_ERR_MALFORMED_EVENT;
        goto fail;
    }

    NostrOtelEncoding enc;
    rc = nostr_otel_parse_encoding(event->tags, &enc);
    if (rc != NOSTR_OTEL_OK) goto fail;

    /* 4. Admission on the signing pubkey. */
    bool admitted = consumer->admit == NULL ||
                    consumer->admit(event->pubkey, consumer->admit_user_data);
    if (!admitted && consumer->policy != NOSTR_OTEL_ADMISSION_FLAG) {
        rc = NOSTR_OTEL_ERR_UNADMITTED;
        goto fail;
    }

    /* 5. Decode the opaque OTLP bytes. */
    uint8_t *body = NULL;
    size_t body_len = 0;
    rc = nostr_otel_decode_body(event->content ? event->content : "", enc,
                                consumer->max_decoded_bytes, &body, &body_len);
    if (rc != NOSTR_OTEL_OK) goto fail;

    NostrOtelReceived received = {
        .signal = signal,
        .event = event,
        .pubkey = event->pubkey,
        .event_id = event->id,
        .admitted = admitted,
        .service = nostr_otel_optional_tag_value(event->tags, NOSTR_OTEL_TAG_SERVICE),
        .encoding = enc,
        .payload = body,
        .payload_len = body_len,
    };
    int hrc = consumer->handler(&received, consumer->handler_user_data);
    free(body);
    if (hrc != 0) {
        rc = NOSTR_OTEL_ERR_HANDLER;
        goto fail;
    }
    return NOSTR_OTEL_OK;

fail:
    report(consumer, rc, event);
    return rc;
}

/* --------------------------------------------------------------- pool glue */

static void consumer_middleware(NostrIncomingEvent *incoming, void *user_data) {
    NostrOtelConsumer *consumer = (NostrOtelConsumer *)user_data;
    if (!incoming || !incoming->event || !consumer) return;
    (void)nostr_otel_consumer_process(consumer, incoming->event);
}

int nostr_otel_consumer_subscribe(NostrOtelConsumer *consumer, NostrSimplePool *pool,
                                  const char **urls, size_t url_count, bool async) {
    if (!consumer || !pool || !urls || url_count == 0) return NOSTR_OTEL_ERR_INVALID_ARG;

    NostrFilters *filters = nostr_otel_consumer_filters(consumer);
    if (!filters) return NOSTR_OTEL_ERR_NOMEM;

    nostr_simple_pool_set_event_middleware_ex(pool, consumer_middleware, consumer);
    if (async) {
        nostr_simple_pool_subscribe_async(pool, urls, url_count, *filters, true);
    } else {
        nostr_simple_pool_subscribe(pool, urls, url_count, *filters, true);
    }
    /* The pool copies what it needs from the by-value filters argument. */
    nostr_filters_free(filters);
    return NOSTR_OTEL_OK;
}
