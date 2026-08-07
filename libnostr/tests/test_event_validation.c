#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-envelope.h"
#include "nostr-tag.h"

static const char *TEST_SK =
    "0000000000000000000000000000000000000000000000000000000000000001";

static NostrEvent *make_signed_event(const char *content) {
    NostrEvent *event = nostr_event_new();
    assert(event != NULL);
    nostr_event_set_created_at(event, 1700000000);
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, content);
    assert(nostr_event_sign(event, TEST_SK) == 0);
    return event;
}

static void test_valid_and_forged_declared_id(void) {
    NostrEvent *event = make_signed_event("valid");
    char canonical_id[65];
    assert(nostr_event_validate(event, canonical_id) == NOSTR_EVENT_VALIDATION_OK);
    assert(strcmp(event->id, canonical_id) == 0);
    assert(nostr_event_check_signature(event));

    char *real_id = strdup(event->id);
    assert(real_id != NULL);
    memset(event->id, 'f', 64);
    event->id[64] = '\0';

    assert(nostr_event_validate(event, canonical_id) ==
           NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH);
    assert(strcmp(canonical_id, real_id) == 0);
    assert(!nostr_event_check_signature(event));

    char *computed = nostr_event_get_id(event);
    assert(computed != NULL);
    assert(strcmp(computed, real_id) == 0);
    assert(strcmp(event->id, computed) != 0);

    free(computed);
    free(real_id);
    nostr_event_free(event);
    printf("  [ok] forged declared id is rejected\n");
}

static void test_mutation_never_returns_stale_id(void) {
    NostrEvent *event = make_signed_event("before");
    char *signed_id = strdup(event->id);
    assert(signed_id != NULL);

    nostr_event_set_content(event, "after");
    char expected[65];
    assert(nostr_event_compute_id(event, expected) == NOSTR_EVENT_VALIDATION_OK);
    assert(strcmp(expected, signed_id) != 0);

    char *got = nostr_event_get_id(event);
    assert(got != NULL);
    assert(strcmp(got, expected) == 0);
    assert(strcmp(event->id, signed_id) == 0);
    assert(nostr_event_validate(event, NULL) ==
           NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH);

    free(got);
    free(signed_id);
    nostr_event_free(event);
    printf("  [ok] mutation recomputes id and invalidates admission\n");
}

static void assert_missing_field(const char *json) {
    NostrEvent *event = nostr_event_new();
    assert(event != NULL);
    assert(nostr_event_deserialize_signed(event, json, NULL) ==
           NOSTR_EVENT_VALIDATION_MISSING_FIELD);
    nostr_event_free(event);
}

static void test_strict_required_fields_and_template_compatibility(void) {
    const char *id =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *pk =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *sig =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    char json[1024];

    snprintf(json, sizeof json,
             "{\"pubkey\":\"%s\",\"created_at\":1,\"kind\":1,"
             "\"tags\":[],\"content\":\"x\",\"sig\":\"%s\"}", pk, sig);
    assert_missing_field(json);
    snprintf(json, sizeof json,
             "{\"id\":\"%s\",\"created_at\":1,\"kind\":1,"
             "\"tags\":[],\"content\":\"x\",\"sig\":\"%s\"}", id, sig);
    assert_missing_field(json);
    snprintf(json, sizeof json,
             "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1,"
             "\"kind\":1,\"content\":\"x\",\"sig\":\"%s\"}", id, pk, sig);
    assert_missing_field(json);
    snprintf(json, sizeof json,
             "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1,"
             "\"kind\":1,\"tags\":[],\"sig\":\"%s\"}", id, pk, sig);
    assert_missing_field(json);
    snprintf(json, sizeof json,
             "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1,"
             "\"kind\":1,\"tags\":[],\"content\":\"x\"}", id, pk);
    assert_missing_field(json);
    snprintf(json, sizeof json,
             "{\"id\":\"%s\",\"pubkey\":\"%s\",\"kind\":1,"
             "\"tags\":[],\"content\":\"x\",\"sig\":\"%s\"}", id, pk, sig);
    assert_missing_field(json);

    NostrEvent *template = nostr_event_new();
    assert(template != NULL);
    assert(nostr_event_deserialize_compact(
               template, "{\"kind\":1,\"content\":\"sign me\"}", NULL) == 1);
    assert(nostr_event_deserialize_signed(
               template, "{\"kind\":1,\"content\":\"sign me\"}", NULL) ==
           NOSTR_EVENT_VALIDATION_MISSING_FIELD);
    nostr_event_free(template);
    printf("  [ok] strict signed parsing is separate from templates\n");
}

static void test_signed_roundtrip_and_envelope_separation(void) {
    NostrEvent *event = make_signed_event("roundtrip");
    char *json = nostr_event_serialize_compact(event);
    assert(json != NULL);

    NostrEvent *parsed = nostr_event_new();
    assert(parsed != NULL);
    assert(nostr_event_deserialize_signed(parsed, json, NULL) ==
           NOSTR_EVENT_VALIDATION_OK);
    assert(nostr_event_validate(parsed, NULL) == NOSTR_EVENT_VALIDATION_OK);

    size_t frame_len = strlen(json) + 32;
    char *frame = malloc(frame_len);
    assert(frame != NULL);
    snprintf(frame, frame_len, "[\"EVENT\",\"sub\",%s]", json);
    NostrEnvelope *envelope = nostr_envelope_parse(frame);
    assert(envelope != NULL);
    assert(envelope->type == NOSTR_ENVELOPE_EVENT);
    NostrEventEnvelope *event_envelope = (NostrEventEnvelope *)envelope;
    assert(nostr_event_validate(event_envelope->event, NULL) ==
           NOSTR_EVENT_VALIDATION_OK);
    nostr_envelope_free(envelope);

    assert(nostr_envelope_parse(
               "[\"EVENT\",\"sub\",{\"kind\":1,\"content\":\"template\"}]") ==
           NULL);

    free(frame);
    nostr_event_free(parsed);
    free(json);
    nostr_event_free(event);
    printf("  [ok] network envelopes require signed structure; validation stays explicit\n");
}

static void test_fixed_nip01_canonical_hash_vector(void) {
    NostrEvent *event = nostr_event_new();
    assert(event != NULL);
    nostr_event_set_pubkey(
        event,
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    nostr_event_set_created_at(event, 1234567890);
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "hello\n\"nostr\"\\");

    NostrTags *tags = nostr_tags_new(0);
    assert(tags != NULL);
    NostrTag *p = nostr_tag_new("p", "abc", NULL);
    NostrTag *e = nostr_tag_new("e", "def", "wss://nos.lol", NULL);
    assert(p != NULL && e != NULL);
    nostr_tags_append(tags, p);
    nostr_tags_append(tags, e);
    nostr_event_set_tags(event, tags);

    char canonical_id[65];
    assert(nostr_event_compute_id(event, canonical_id) == NOSTR_EVENT_VALIDATION_OK);
    assert(strcmp(canonical_id,
                  "88ac97884d1867e9f3b071906bfe74dcb0a489762ace4a9f69fb6c6c2b387017") == 0);

    char *json = nostr_event_serialize_compact(event);
    assert(json != NULL);
    NostrEvent *roundtrip = nostr_event_new();
    assert(roundtrip != NULL);
    assert(nostr_event_deserialize_compact(roundtrip, json, NULL) == 1);
    char roundtrip_id[65];
    assert(nostr_event_compute_id(roundtrip, roundtrip_id) ==
           NOSTR_EVENT_VALIDATION_OK);
    assert(strcmp(roundtrip_id, canonical_id) == 0);

    nostr_event_free(roundtrip);
    free(json);
    nostr_event_free(event);
    printf("  [ok] fixed NIP-01 canonical serialization/hash vector\n");
}


static void test_strict_duplicate_fields(void) {
    NostrEvent *event = make_signed_event("duplicates");
    char *json = nostr_event_serialize_compact(event);
    assert(json != NULL);
    char *close = strrchr(json, '}');
    assert(close != NULL);
    size_t prefix_len = (size_t)(close - json);

    char id_dup[96], pubkey_dup[96], sig_dup[160];
    snprintf(id_dup, sizeof id_dup, ",\"id\":\"%s\"", event->id);
    snprintf(pubkey_dup, sizeof pubkey_dup, ",\"pubkey\":\"%s\"", event->pubkey);
    snprintf(sig_dup, sizeof sig_dup, ",\"sig\":\"%s\"", event->sig);
    const char *duplicates[] = {
        id_dup,
        pubkey_dup,
        ",\"created_at\":1700000000",
        ",\"kind\":1",
        ",\"tags\":[]",
        ",\"content\":\"different\"",
        sig_dup
    };

    for (size_t i = 0; i < sizeof duplicates / sizeof duplicates[0]; i++) {
        size_t len = prefix_len + strlen(duplicates[i]) + 2;
        char *duplicate_json = malloc(len);
        assert(duplicate_json != NULL);
        memcpy(duplicate_json, json, prefix_len);
        snprintf(duplicate_json + prefix_len, len - prefix_len,
                 "%s}", duplicates[i]);

        NostrEvent *parsed = nostr_event_new();
        assert(parsed != NULL);
        assert(nostr_event_deserialize_signed(parsed, duplicate_json, NULL) ==
               NOSTR_EVENT_VALIDATION_SERIALIZATION_ERROR);
        nostr_event_free(parsed);
        free(duplicate_json);
    }

    free(json);
    nostr_event_free(event);
    printf("  [ok] strict parsing rejects duplicate signed fields\n");
}

static void uppercase_first_hex_letter(char *hex) {
    for (; *hex; hex++) {
        if (*hex >= 'a' && *hex <= 'f') {
            *hex = (char)(*hex - 'a' + 'A');
            return;
        }
    }
    assert(!"test vector unexpectedly contained no hex letters");
}

static void test_canonical_hex_formats(void) {
    NostrEvent *event = make_signed_event("hex format");
    char *saved_id = strdup(event->id);
    char *saved_pubkey = strdup(event->pubkey);
    char *saved_sig = strdup(event->sig);
    assert(saved_id && saved_pubkey && saved_sig);

    uppercase_first_hex_letter(event->id);
    assert(nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_BAD_ID);
    free(event->id);
    event->id = strdup(saved_id);

    uppercase_first_hex_letter(event->pubkey);
    assert(nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_BAD_PUBKEY);
    free(event->pubkey);
    event->pubkey = strdup(saved_pubkey);

    uppercase_first_hex_letter(event->sig);
    assert(nostr_event_validate(event, NULL) ==
           NOSTR_EVENT_VALIDATION_BAD_SIGNATURE_FORMAT);
    free(event->sig);
    event->sig = strdup(saved_sig);

    free(event->id);
    event->id = malloc(66);
    assert(event->id != NULL);
    snprintf(event->id, 66, "%sx", saved_id);
    assert(nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_BAD_ID);

    free(saved_sig);
    free(saved_pubkey);
    free(saved_id);
    nostr_event_free(event);
    printf("  [ok] admission requires exact lowercase hex fields\n");
}

static void test_strict_tag_limit(void) {
    size_t tag_count = 101;
    size_t tags_cap = tag_count * 12 + 4;
    char *tags = malloc(tags_cap);
    assert(tags != NULL);
    size_t used = 0;
    tags[used++] = '[';
    for (size_t i = 0; i < tag_count; i++) {
        int n = snprintf(tags + used, tags_cap - used,
                         "%s[\"t\",\"x\"]", i ? "," : "");
        assert(n > 0);
        used += (size_t)n;
    }
    tags[used++] = ']';
    tags[used] = '\0';

    const char *id =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *pk =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *sig =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    size_t json_cap = strlen(tags) + 512;
    char *json = malloc(json_cap);
    assert(json != NULL);
    snprintf(json, json_cap,
             "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1,"
             "\"kind\":1,\"tags\":%s,\"content\":\"x\",\"sig\":\"%s\"}",
             id, pk, tags, sig);

    NostrEvent *event = nostr_event_new();
    assert(event != NULL);
    assert(nostr_event_deserialize_signed(event, json, NULL) ==
           NOSTR_EVENT_VALIDATION_LIMIT);

    nostr_event_free(event);
    free(json);
    free(tags);
    printf("  [ok] strict parsing enforces the tag-count limit\n");
}

static void test_unsigned_event_structure_and_id(void) {
    NostrEvent *rumor = nostr_event_new();
    assert(rumor != NULL);
    nostr_event_set_pubkey(
        rumor,
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    nostr_event_set_created_at(rumor, 1700000001);
    nostr_event_set_kind(rumor, 14);
    nostr_event_set_content(rumor, "rumor");

    char *json = nostr_event_serialize_compact(rumor);
    assert(json != NULL);
    NostrEvent *parsed = nostr_event_new();
    assert(parsed != NULL);
    assert(nostr_event_deserialize_unsigned(parsed, json, NULL) ==
           NOSTR_EVENT_VALIDATION_OK);
    char canonical_id[65];
    assert(nostr_event_compute_id(parsed, canonical_id) ==
           NOSTR_EVENT_VALIDATION_OK);

    parsed->id = strdup(canonical_id);
    assert(parsed->id != NULL);
    assert(nostr_event_validate_id(parsed, canonical_id) ==
           NOSTR_EVENT_VALIDATION_OK);
    nostr_event_set_content(parsed, "tampered");
    assert(nostr_event_validate_id(parsed, canonical_id) ==
           NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH);

    NostrEvent *missing = nostr_event_new();
    assert(missing != NULL);
    assert(nostr_event_deserialize_unsigned(
               missing,
               "{\"created_at\":1,\"kind\":14,\"tags\":[],\"content\":\"x\"}",
               NULL) == NOSTR_EVENT_VALIDATION_MISSING_FIELD);

    nostr_event_free(missing);
    nostr_event_free(parsed);
    free(json);
    nostr_event_free(rumor);
    printf("  [ok] unsigned events require rumor structure and canonical ids\n");
}


static void assert_strict_tags_rejected(const char *tags_json) {
    const char *id =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *pk =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *sig =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    char json[1024];
    snprintf(json, sizeof json,
             "{\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":1,"
             "\"kind\":1,\"tags\":%s,\"content\":\"x\",\"sig\":\"%s\"}",
             id, pk, tags_json, sig);

    NostrEvent *event = nostr_event_new();
    assert(event != NULL);
    assert(nostr_event_deserialize_signed(event, json, NULL) ==
           NOSTR_EVENT_VALIDATION_SERIALIZATION_ERROR);
    nostr_event_free(event);
}

static void test_strict_tag_separators(void) {
    assert_strict_tags_rejected("[[\"a\" \"b\"]]");
    assert_strict_tags_rejected("[[\"a\",]]");
    assert_strict_tags_rejected("[[\"a\"][\"b\"]]");
    assert_strict_tags_rejected("[[\"a\"],]");
    printf("  [ok] strict parsing rejects malformed tag separators\n");
}

int main(void) {
    printf("libnostr canonical event validation tests:\n");
    test_valid_and_forged_declared_id();
    test_mutation_never_returns_stale_id();
    test_strict_required_fields_and_template_compatibility();
    test_signed_roundtrip_and_envelope_separation();
    test_fixed_nip01_canonical_hash_vector();
    test_strict_duplicate_fields();
    test_canonical_hex_formats();
    test_strict_tag_limit();
    test_unsigned_event_structure_and_id();
    test_strict_tag_separators();
    printf("All canonical event validation tests passed.\n");
    return 0;
}
