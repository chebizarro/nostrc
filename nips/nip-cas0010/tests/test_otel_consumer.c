/* NIP-CAS-0010: consumer tests — signature verification, tag validation,
 * missing-enc default and pubkey admission. */

#include <time.h>

#include "otel_test_util.h"

typedef struct {
    size_t calls;
    bool admitted;
    char pubkey[65];
    char payload[256];
    size_t payload_len;
    NostrOtelSignal signal;
    NostrOtelEncoding encoding;
    const char *service;
    int handler_rc;
} Capture;

static int capture_handler(const NostrOtelReceived *received, void *user_data) {
    Capture *cap = (Capture *)user_data;
    cap->calls++;
    cap->admitted = received->admitted;
    cap->signal = received->signal;
    cap->encoding = received->encoding;
    cap->service = received->service;
    snprintf(cap->pubkey, sizeof(cap->pubkey), "%s", received->pubkey ? received->pubkey : "");
    cap->payload_len = received->payload_len < sizeof(cap->payload) ? received->payload_len
                                                                    : sizeof(cap->payload);
    memcpy(cap->payload, received->payload, cap->payload_len);
    return cap->handler_rc;
}

typedef struct {
    int last_err;
    size_t calls;
} ErrCapture;

static void capture_error(int err, const NostrEvent *event, void *user_data) {
    (void)event;
    ErrCapture *cap = (ErrCapture *)user_data;
    cap->last_err = err;
    cap->calls++;
}

/* Builds and signs an event directly, so tests can inject deliberately
 * malformed tag sets that a well-behaved producer would never emit. */
static NostrEvent *sign_event(const OtelTestSigner *ts, int kind, NostrTags *tags,
                              const char *content) {
    NostrEvent *event = nostr_event_new();
    OTEL_CHECK(event != NULL, "event alloc");
    event->kind = kind;
    event->created_at = (int64_t)time(NULL);
    event->tags = tags;
    event->content = strdup(content);
    OTEL_CHECK(event->content != NULL, "content alloc");
    OTEL_CHECK(ts->signer.sign(event, ts->signer.user_data) == 0, "sign failed");
    return event;
}

static NostrTags *standard_tags(bool with_enc) {
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("domain", "traces", NULL));
    nostr_tags_append(tags, nostr_tag_new("schema", "cascadia.otel.traces.v1", NULL));
    nostr_tags_append(tags, nostr_tag_new("service", "bahia", NULL));
    if (with_enc) {
        nostr_tags_append(tags, nostr_tag_new("enc", "otlp-proto", "identity", NULL));
    }
    return tags;
}

static char *identity_content(const char *body) {
    char *content = NULL;
    OTEL_CHECK_RC(nostr_otel_base64_encode((const uint8_t *)body, strlen(body), &content),
                  NOSTR_OTEL_OK);
    return content;
}

static NostrOtelConsumer *make_consumer(Capture *cap, ErrCapture *errs, NostrOtelAdmitFn admit,
                                        void *admit_data, NostrOtelAdmissionPolicy policy) {
    NostrOtelConsumerConfig cfg = {
        .handler = capture_handler,
        .handler_user_data = cap,
        .on_error = capture_error,
        .error_user_data = errs,
        .admit = admit,
        .admit_user_data = admit_data,
        .policy = policy,
    };
    NostrOtelConsumer *consumer = NULL;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&cfg, &consumer), NOSTR_OTEL_OK);
    return consumer;
}

static void test_admission_required(void) {
    Capture cap = {0};
    NostrOtelConsumerConfig cfg = {
        .handler = capture_handler,
        .handler_user_data = &cap,
        .policy = NOSTR_OTEL_ADMISSION_DROP,
    };
    NostrOtelConsumer *consumer = NULL;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&cfg, &consumer), NOSTR_OTEL_ERR_INVALID_ARG);
    OTEL_CHECK(consumer == NULL, "failed construction leaves output NULL");

    cfg.policy = NOSTR_OTEL_ADMISSION_FLAG;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&cfg, &consumer), NOSTR_OTEL_ERR_INVALID_ARG);
    OTEL_CHECK(consumer == NULL, "flag policy also requires an admitter");

    cfg.policy = NOSTR_OTEL_ADMISSION_OPEN;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&cfg, &consumer), NOSTR_OTEL_OK);
    nostr_otel_consumer_free(consumer);
}

static void test_valid_event(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    ErrCapture errs = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, &errs, NULL, NULL,
                                                NOSTR_OTEL_ADMISSION_OPEN);

    char *content = identity_content("otlp-bytes");
    NostrEvent *event = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, standard_tags(true), content);
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_OK);
    OTEL_CHECK(cap.calls == 1, "handler invoked once");
    OTEL_CHECK(cap.signal == NOSTR_OTEL_SIGNAL_TRACES, "signal");
    OTEL_CHECK(cap.admitted, "explicit open admission admits signer");
    OTEL_CHECK(strcmp(cap.pubkey, otel_test_signer_pubkey(&ts)) == 0,
               "payload attributed to the signing pubkey");
    OTEL_CHECK(cap.payload_len == strlen("otlp-bytes"), "payload length");
    OTEL_CHECK(memcmp(cap.payload, "otlp-bytes", cap.payload_len) == 0, "payload bytes");
    OTEL_CHECK(strcmp(cap.service, "bahia") == 0, "service tag surfaced");
    OTEL_CHECK(errs.calls == 0, "no errors");

    free(content);
    nostr_event_free(event);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

static void test_missing_enc_defaults(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    ErrCapture errs = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, &errs, NULL, NULL,
                                                NOSTR_OTEL_ADMISSION_OPEN);

    char *content = identity_content("no-enc-tag");
    NostrEvent *event = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, standard_tags(false), content);
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_OK);
    OTEL_CHECK(cap.encoding.format == NOSTR_OTEL_FORMAT_OTLP_PROTO, "default format");
    OTEL_CHECK(cap.encoding.compression == NOSTR_OTEL_COMPRESSION_IDENTITY,
               "default compression");
    OTEL_CHECK(cap.payload_len == strlen("no-enc-tag"), "payload survives default path");

    free(content);
    nostr_event_free(event);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

static void test_tampered_content_rejected(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    ErrCapture errs = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, &errs, NULL, NULL,
                                                NOSTR_OTEL_ADMISSION_OPEN);

    char *content = identity_content("authentic-otlp");
    NostrEvent *event = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, standard_tags(true), content);

    /* Swap the body for a validly-base64 but unsigned payload. */
    char *forged = identity_content("forged-otlp-xx");
    free(event->content);
    event->content = forged;

    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_ERR_INVALID_SIGNATURE);
    OTEL_CHECK(cap.calls == 0, "tampered content must not reach the handler");
    OTEL_CHECK(errs.last_err == NOSTR_OTEL_ERR_INVALID_SIGNATURE, "error reported");

    free(content);
    nostr_event_free(event);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

static void test_tampered_tag_rejected(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    ErrCapture errs = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, &errs, NULL, NULL,
                                                NOSTR_OTEL_ADMISSION_OPEN);

    char *content = identity_content("otlp-bytes");
    NostrEvent *event = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, standard_tags(true), content);
    /* Rewriting a signed tag breaks the id binding. */
    nostr_tags_append(event->tags, nostr_tag_new("enc", "otlp-proto", "zstd", NULL));
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_ERR_INVALID_SIGNATURE);
    OTEL_CHECK(cap.calls == 0, "tampered tags must not reach the handler");

    free(content);
    nostr_event_free(event);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

/* Signed-but-malformed tag sets: the producer is authentic, the event is not. */
static void test_signed_malformed_tags_rejected(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    char *content = identity_content("otlp-bytes");

    typedef struct {
        const char *name;
        NostrTags *tags;
        int expected;
    } MalformedCase;
    MalformedCase cases[6];
    size_t n = 0;

    NostrTags *dup_enc = standard_tags(true);
    nostr_tags_append(dup_enc, nostr_tag_new("enc", "otlp-proto", "identity", NULL));
    cases[n++] = (MalformedCase){"duplicate enc", dup_enc, NOSTR_OTEL_ERR_MALFORMED_EVENT};

    NostrTags *dup_domain = standard_tags(true);
    nostr_tags_append(dup_domain, nostr_tag_new("domain", "traces", NULL));
    cases[n++] = (MalformedCase){"duplicate domain", dup_domain,
                                    NOSTR_OTEL_ERR_MALFORMED_EVENT};

    NostrTags *dup_schema = standard_tags(true);
    nostr_tags_append(dup_schema, nostr_tag_new("schema", "cascadia.otel.traces.v1", NULL));
    cases[n++] = (MalformedCase){"duplicate schema", dup_schema,
                                    NOSTR_OTEL_ERR_MALFORMED_EVENT};

    NostrTags *wrong_domain = nostr_tags_new(0);
    nostr_tags_append(wrong_domain, nostr_tag_new("domain", "metrics", NULL));
    nostr_tags_append(wrong_domain, nostr_tag_new("schema", "cascadia.otel.traces.v1", NULL));
    cases[n++] = (MalformedCase){"domain does not match kind", wrong_domain,
                                    NOSTR_OTEL_ERR_MALFORMED_EVENT};

    NostrTags *missing_schema = nostr_tags_new(0);
    nostr_tags_append(missing_schema, nostr_tag_new("domain", "traces", NULL));
    cases[n++] = (MalformedCase){"missing schema", missing_schema,
                                    NOSTR_OTEL_ERR_MALFORMED_EVENT};

    NostrTags *bad_enc = nostr_tags_new(0);
    nostr_tags_append(bad_enc, nostr_tag_new("domain", "traces", NULL));
    nostr_tags_append(bad_enc, nostr_tag_new("schema", "cascadia.otel.traces.v1", NULL));
    nostr_tags_append(bad_enc, nostr_tag_new("enc", "otlp-proto", "identity", "extra", NULL));
    cases[n++] = (MalformedCase){"overlong enc", bad_enc, NOSTR_OTEL_ERR_MALFORMED_EVENT};

    for (size_t i = 0; i < n; i++) {
        Capture cap = {0};
        ErrCapture errs = {0};
        NostrOtelConsumer *consumer = make_consumer(&cap, &errs, NULL, NULL,
                                                    NOSTR_OTEL_ADMISSION_OPEN);
        NostrEvent *event = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, cases[i].tags, content);
        int rc = nostr_otel_consumer_process(consumer, event);
        OTEL_CHECK(rc == cases[i].expected, cases[i].name);
        OTEL_CHECK(cap.calls == 0, "malformed event must not reach the handler");
        nostr_event_free(event);
        nostr_otel_consumer_free(consumer);
    }
    free(content);
    otel_test_signer_clear(&ts);
}

static void test_unexpected_kind(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    ErrCapture errs = {0};

    NostrOtelSignal only_logs[] = {NOSTR_OTEL_SIGNAL_LOGS};
    NostrOtelConsumerConfig cfg = {
        .signals = only_logs,
        .signal_count = 1,
        .handler = capture_handler,
        .handler_user_data = &cap,
        .on_error = capture_error,
        .error_user_data = &errs,
        .policy = NOSTR_OTEL_ADMISSION_OPEN,
    };
    NostrOtelConsumer *consumer = NULL;
    OTEL_CHECK_RC(nostr_otel_consumer_new(&cfg, &consumer), NOSTR_OTEL_OK);

    char *content = identity_content("otlp-bytes");
    NostrEvent *traces = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, standard_tags(true), content);
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, traces), NOSTR_OTEL_ERR_UNEXPECTED_KIND);

    NostrEvent *note = sign_event(&ts, 1, standard_tags(true), content);
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, note), NOSTR_OTEL_ERR_UNEXPECTED_KIND);

    /* The filter follows the configured signals. */
    NostrFilters *filters = nostr_otel_consumer_filters(consumer);
    OTEL_CHECK(filters != NULL && filters->count == 1, "one filter");
    OTEL_CHECK(nostr_filter_kinds_len(&filters->filters[0]) == 1, "one kind");
    OTEL_CHECK(nostr_filter_kinds_get(&filters->filters[0], 0) == NOSTR_OTEL_KIND_LOGS,
               "logs kind in filter");
    nostr_filters_free(filters);

    free(content);
    nostr_event_free(traces);
    nostr_event_free(note);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

static const char *g_allowed;

static bool admit_only_allowed(const char *pubkey_hex, void *user_data) {
    (void)user_data;
    return g_allowed && pubkey_hex && strcmp(pubkey_hex, g_allowed) == 0;
}

static void test_admission(void) {
    OtelTestSigner admitted_signer, stranger;
    otel_test_signer_init(&admitted_signer);
    otel_test_signer_init(&stranger);
    g_allowed = otel_test_signer_pubkey(&admitted_signer);

    char *content = identity_content("otlp-bytes");

    /* DROP: the stranger's telemetry never reaches the handler. */
    Capture cap = {0};
    ErrCapture errs = {0};
    NostrOtelConsumer *drop = make_consumer(&cap, &errs, admit_only_allowed, NULL,
                                            NOSTR_OTEL_ADMISSION_DROP);
    NostrEvent *ok_event = sign_event(&admitted_signer, NOSTR_OTEL_KIND_TRACES,
                                      standard_tags(true), content);
    OTEL_CHECK_RC(nostr_otel_consumer_process(drop, ok_event), NOSTR_OTEL_OK);
    OTEL_CHECK(cap.calls == 1 && cap.admitted, "admitted pubkey delivered");

    NostrEvent *bad_event = sign_event(&stranger, NOSTR_OTEL_KIND_TRACES, standard_tags(true),
                                       content);
    OTEL_CHECK_RC(nostr_otel_consumer_process(drop, bad_event), NOSTR_OTEL_ERR_UNADMITTED);
    OTEL_CHECK(cap.calls == 1, "unadmitted pubkey dropped");
    OTEL_CHECK(errs.last_err == NOSTR_OTEL_ERR_UNADMITTED, "drop reported");
    nostr_otel_consumer_free(drop);

    /* FLAG: delivered, but marked. */
    Capture flag_cap = {0};
    ErrCapture flag_errs = {0};
    NostrOtelConsumer *flag = make_consumer(&flag_cap, &flag_errs, admit_only_allowed, NULL,
                                            NOSTR_OTEL_ADMISSION_FLAG);
    OTEL_CHECK_RC(nostr_otel_consumer_process(flag, bad_event), NOSTR_OTEL_OK);
    OTEL_CHECK(flag_cap.calls == 1, "flagged event delivered");
    OTEL_CHECK(!flag_cap.admitted, "flagged event marked unadmitted");
    OTEL_CHECK(strcmp(flag_cap.pubkey, otel_test_signer_pubkey(&stranger)) == 0,
               "flagged event attributed to its signer");
    nostr_otel_consumer_free(flag);

    free(content);
    nostr_event_free(ok_event);
    nostr_event_free(bad_event);
    otel_test_signer_clear(&admitted_signer);
    otel_test_signer_clear(&stranger);
}

/* Pubkey swap with id recompute: the attacker takes an admitted producer's
 * event, rewrites the body and re-signs the whole thing under their own key.
 * The result is *internally consistent* — pubkey, id and signature all agree —
 * so signature verification alone cannot catch it. Only admission can, and the
 * attribution the consumer reports must name the real signer, never the
 * producer whose event was copied. */
static void test_pubkey_reattribution_rejected(void) {
    OtelTestSigner producer, attacker;
    otel_test_signer_init(&producer);
    otel_test_signer_init(&attacker);
    g_allowed = otel_test_signer_pubkey(&producer);

    char *content = identity_content("authentic-otlp");
    NostrEvent *authentic = sign_event(&producer, NOSTR_OTEL_KIND_TRACES, standard_tags(true),
                                       content);

    NostrEvent *forged = otel_test_event_copy(authentic);
    char *forged_body = identity_content("forged-otlp-xx");
    free(forged->content);
    forged->content = forged_body;
    free(forged->sig);
    forged->sig = NULL;
    OTEL_CHECK(attacker.signer.sign(forged, attacker.signer.user_data) == 0, "re-sign failed");
    OTEL_CHECK(strcmp(forged->pubkey, otel_test_signer_pubkey(&attacker)) == 0,
               "re-signed event must carry the attacker pubkey");
    OTEL_CHECK(strcmp(forged->id, authentic->id) != 0, "re-signed event must have a new id");

    /* The forgery is cryptographically valid: with admission open it is
     * accepted, which proves the id and signature really were recomputed and
     * that the rejections below come from admission, not a malformed event. */
    Capture open_cap = {0};
    ErrCapture open_errs = {0};
    NostrOtelConsumer *open = make_consumer(&open_cap, &open_errs, NULL, NULL,
                                            NOSTR_OTEL_ADMISSION_OPEN);
    OTEL_CHECK_RC(nostr_otel_consumer_process(open, forged), NOSTR_OTEL_OK);
    OTEL_CHECK(open_cap.calls == 1, "re-signed event must be internally consistent");
    nostr_otel_consumer_free(open);

    /* DROP: a different, unadmitted signer — the copied body buys nothing. */
    Capture cap = {0};
    ErrCapture errs = {0};
    NostrOtelConsumer *drop = make_consumer(&cap, &errs, admit_only_allowed, NULL,
                                            NOSTR_OTEL_ADMISSION_DROP);
    OTEL_CHECK_RC(nostr_otel_consumer_process(drop, forged), NOSTR_OTEL_ERR_UNADMITTED);
    OTEL_CHECK(cap.calls == 0, "re-attributed event must not reach the handler");
    OTEL_CHECK(errs.last_err == NOSTR_OTEL_ERR_UNADMITTED, "drop reported");
    nostr_otel_consumer_free(drop);

    /* FLAG: delivered, but attributed to the attacker and marked unadmitted. */
    Capture flag_cap = {0};
    ErrCapture flag_errs = {0};
    NostrOtelConsumer *flag = make_consumer(&flag_cap, &flag_errs, admit_only_allowed, NULL,
                                            NOSTR_OTEL_ADMISSION_FLAG);
    OTEL_CHECK_RC(nostr_otel_consumer_process(flag, forged), NOSTR_OTEL_OK);
    OTEL_CHECK(flag_cap.calls == 1, "flagged event delivered");
    OTEL_CHECK(!flag_cap.admitted, "re-attributed event marked unadmitted");
    OTEL_CHECK(strcmp(flag_cap.pubkey, otel_test_signer_pubkey(&attacker)) == 0,
               "attribution must name the real signer, not the copied producer");
    OTEL_CHECK(strcmp(flag_cap.pubkey, otel_test_signer_pubkey(&producer)) != 0,
               "attribution must never name the producer");
    OTEL_CHECK(flag_cap.payload_len == strlen("forged-otlp-xx") &&
                   memcmp(flag_cap.payload, "forged-otlp-xx", flag_cap.payload_len) == 0,
               "handler sees the attacker body");
    nostr_otel_consumer_free(flag);

    free(content);
    nostr_event_free(authentic);
    nostr_event_free(forged);
    otel_test_signer_clear(&producer);
    otel_test_signer_clear(&attacker);
}

static void test_handler_failure_surfaces(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    cap.handler_rc = -1;
    ErrCapture errs = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, &errs, NULL, NULL,
                                                NOSTR_OTEL_ADMISSION_OPEN);
    char *content = identity_content("otlp-bytes");
    NostrEvent *event = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, standard_tags(true), content);
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_ERR_HANDLER);
    OTEL_CHECK(errs.last_err == NOSTR_OTEL_ERR_HANDLER, "handler error reported");
    free(content);
    nostr_event_free(event);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

static void test_bad_content_rejected(void) {
    OtelTestSigner ts;
    otel_test_signer_init(&ts);
    Capture cap = {0};
    ErrCapture errs = {0};
    NostrOtelConsumer *consumer = make_consumer(&cap, &errs, NULL, NULL,
                                                NOSTR_OTEL_ADMISSION_OPEN);
    /* Signed, but the content is not valid base64. */
    NostrEvent *event = sign_event(&ts, NOSTR_OTEL_KIND_TRACES, standard_tags(true), "not!b64");
    OTEL_CHECK_RC(nostr_otel_consumer_process(consumer, event), NOSTR_OTEL_ERR_MALFORMED_EVENT);
    OTEL_CHECK(cap.calls == 0, "non-base64 content must not reach the handler");
    nostr_event_free(event);
    nostr_otel_consumer_free(consumer);
    otel_test_signer_clear(&ts);
}

int main(void) {
    test_admission_required();
    test_valid_event();
    test_missing_enc_defaults();
    test_tampered_content_rejected();
    test_tampered_tag_rejected();
    test_signed_malformed_tags_rejected();
    test_unexpected_kind();
    test_admission();
    test_pubkey_reattribution_rejected();
    test_handler_failure_surfaces();
    test_bad_content_rejected();
    printf("test_otel_consumer: OK\n");
    return 0;
}
