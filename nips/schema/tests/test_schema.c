#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/schema/schema.h"

/* ---- Type validators ---- */

static void test_valid_id(void) {
    assert(nostr_schema_is_valid_id(
        "aabbccddee00112233445566778899aabbccddee00112233445566778899aabb"));
    assert(!nostr_schema_is_valid_id("short"));
    assert(!nostr_schema_is_valid_id(
        "gghhccddee00112233445566778899aabbccddee00112233445566778899aabb"));
    assert(!nostr_schema_is_valid_id(NULL));
    assert(!nostr_schema_is_valid_id(""));
}

static void test_valid_pubkey(void) {
    assert(nostr_schema_is_valid_pubkey(
        "aabbccddee00112233445566778899aabbccddee00112233445566778899aabb"));
    assert(!nostr_schema_is_valid_pubkey("tooshort"));
}

static void test_valid_relay_url(void) {
    assert(nostr_schema_is_valid_relay_url("wss://relay.example.com"));
    assert(nostr_schema_is_valid_relay_url("ws://localhost:8080"));
    assert(!nostr_schema_is_valid_relay_url("https://example.com"));
    assert(!nostr_schema_is_valid_relay_url("wss://"));
    assert(!nostr_schema_is_valid_relay_url(NULL));
    assert(!nostr_schema_is_valid_relay_url(""));
}

static void test_valid_kind_str(void) {
    assert(nostr_schema_is_valid_kind_str("0"));
    assert(nostr_schema_is_valid_kind_str("30023"));
    assert(!nostr_schema_is_valid_kind_str("abc"));
    assert(!nostr_schema_is_valid_kind_str("-1"));
    assert(!nostr_schema_is_valid_kind_str(""));
    assert(!nostr_schema_is_valid_kind_str(NULL));
}

static void test_valid_timestamp(void) {
    assert(nostr_schema_is_valid_timestamp("1700000000"));
    assert(!nostr_schema_is_valid_timestamp("abc"));
}

static void test_valid_hex(void) {
    assert(nostr_schema_is_valid_hex("aabb"));
    assert(nostr_schema_is_valid_hex("AABB"));
    assert(!nostr_schema_is_valid_hex("aab")); /* odd length */
    assert(!nostr_schema_is_valid_hex("ggbb"));
    assert(!nostr_schema_is_valid_hex(""));
    assert(!nostr_schema_is_valid_hex(NULL));
}

static void test_is_trimmed(void) {
    assert(nostr_schema_is_trimmed("hello"));
    assert(nostr_schema_is_trimmed(""));
    assert(nostr_schema_is_trimmed(NULL));
    assert(!nostr_schema_is_trimmed(" hello"));
    assert(!nostr_schema_is_trimmed("hello "));
    assert(!nostr_schema_is_trimmed(" "));
}

static void test_valid_json(void) {
    assert(nostr_schema_is_valid_json("{\"name\":\"test\"}"));
    assert(nostr_schema_is_valid_json("[1,2,3]"));
    assert(nostr_schema_is_valid_json("{}"));
    assert(nostr_schema_is_valid_json("{\"a\":{\"b\":1}}"));
    assert(!nostr_schema_is_valid_json("not json"));
    assert(!nostr_schema_is_valid_json("{unclosed"));
    assert(!nostr_schema_is_valid_json(""));
    assert(!nostr_schema_is_valid_json(NULL));
}

static void test_valid_addr(void) {
    assert(nostr_schema_is_valid_addr(
        "30023:aabbccddee00112233445566778899aabbccddee00112233445566778899aabb:slug"));
    assert(nostr_schema_is_valid_addr(
        "30023:aabbccddee00112233445566778899aabbccddee00112233445566778899aabb:"));
    assert(!nostr_schema_is_valid_addr("nocolon"));
    assert(!nostr_schema_is_valid_addr("30023:short:d"));
    assert(!nostr_schema_is_valid_addr(":pubkey:d"));
    assert(!nostr_schema_is_valid_addr(NULL));
}

/* ---- Kind classification ---- */

static void test_kind_classification(void) {
    assert(nostr_schema_is_addressable(30023));
    assert(nostr_schema_is_addressable(30009));
    assert(!nostr_schema_is_addressable(1));
    assert(!nostr_schema_is_addressable(10002));

    assert(nostr_schema_is_replaceable(0));
    assert(nostr_schema_is_replaceable(3));
    assert(nostr_schema_is_replaceable(10002));
    assert(!nostr_schema_is_replaceable(1));
    assert(!nostr_schema_is_replaceable(30023));

    assert(nostr_schema_is_ephemeral(20000));
    assert(nostr_schema_is_ephemeral(24242));
    assert(!nostr_schema_is_ephemeral(1));
    assert(!nostr_schema_is_ephemeral(30023));
}

/* ---- Validate type dispatch ---- */

static void test_validate_type(void) {
    assert(nostr_schema_validate_type("id",
        "aabbccddee00112233445566778899aabbccddee00112233445566778899aabb"));
    assert(!nostr_schema_validate_type("id", "short"));
    assert(nostr_schema_validate_type("pubkey",
        "aabbccddee00112233445566778899aabbccddee00112233445566778899aabb"));
    assert(nostr_schema_validate_type("relay", "wss://r.com"));
    assert(nostr_schema_validate_type("kind", "30023"));
    assert(nostr_schema_validate_type("timestamp", "1700000000"));
    assert(nostr_schema_validate_type("hex", "aabb"));
    assert(nostr_schema_validate_type("json", "{}"));
    assert(nostr_schema_validate_type("addr",
        "30023:aabbccddee00112233445566778899aabbccddee00112233445566778899aabb:d"));
    assert(nostr_schema_validate_type("free", "anything"));
    assert(!nostr_schema_validate_type("unknown_type", "x"));
    assert(!nostr_schema_validate_type(NULL, "x"));
    assert(!nostr_schema_validate_type("id", NULL));
}

/* ---- Event validation ---- */

static void test_validate_kind1(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "Hello world");

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(r.valid);

    nostr_event_free(ev);
}

static void test_validate_kind0_json(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 0);
    nostr_event_set_content(ev, "{\"name\":\"test\"}");

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(r.valid);

    nostr_event_free(ev);
}

static void test_validate_kind0_invalid_json(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 0);
    nostr_event_set_content(ev, "not json");

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(strstr(r.error, "JSON") != NULL);

    nostr_event_free(ev);
}

static void test_validate_content_whitespace(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, " leading space");

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(strstr(r.error, "whitespace") != NULL);

    nostr_event_free(ev);
}

static void test_validate_e_tag_valid(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("e",
        "aabbccddee00112233445566778899aabbccddee00112233445566778899aabb",
        "wss://relay.com", NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(r.valid);

    nostr_event_free(ev);
}

static void test_validate_e_tag_invalid_id(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("e", "not_a_valid_id", NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(r.tag_index == 0);
    assert(strstr(r.error, "event ID") != NULL);

    nostr_event_free(ev);
}

static void test_validate_e_tag_invalid_relay(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("e",
        "aabbccddee00112233445566778899aabbccddee00112233445566778899aabb",
        "https://not-a-relay.com", NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(strstr(r.error, "relay URL") != NULL);

    nostr_event_free(ev);
}

static void test_validate_p_tag_invalid(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "badpubkey", NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(strstr(r.error, "pubkey") != NULL);

    nostr_event_free(ev);
}

static void test_validate_a_tag_valid(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("a",
        "30023:aabbccddee00112233445566778899aabbccddee00112233445566778899aabb:slug",
        NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(r.valid);

    nostr_event_free(ev);
}

static void test_validate_a_tag_invalid(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("a", "not:valid", NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(strstr(r.error, "addr") != NULL);

    nostr_event_free(ev);
}

static void test_validate_addressable_needs_d_tag(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 30023);
    nostr_event_set_content(ev, "article");

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(strstr(r.error, "'d' tag") != NULL);

    nostr_event_free(ev);
}

static void test_validate_addressable_with_d_tag(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 30023);
    nostr_event_set_content(ev, "article");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "my-slug", NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(r.valid);

    nostr_event_free(ev);
}

static void test_validate_d_tag_non_addressable(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "test");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "slug", NULL));

    NostrSchemaResult r = nostr_schema_validate_event(ev);
    assert(!r.valid);
    assert(strstr(r.error, "non-addressable") != NULL);

    nostr_event_free(ev);
}

static void test_validate_null_event(void) {
    NostrSchemaResult r = nostr_schema_validate_event(NULL);
    assert(!r.valid);
}

int main(void) {
    /* Type validators */
    test_valid_id();
    test_valid_pubkey();
    test_valid_relay_url();
    test_valid_kind_str();
    test_valid_timestamp();
    test_valid_hex();
    test_is_trimmed();
    test_valid_json();
    test_valid_addr();

    /* Kind classification */
    test_kind_classification();

    /* Type dispatch */
    test_validate_type();

    /* Event validation */
    test_validate_kind1();
    test_validate_kind0_json();
    test_validate_kind0_invalid_json();
    test_validate_content_whitespace();
    test_validate_e_tag_valid();
    test_validate_e_tag_invalid_id();
    test_validate_e_tag_invalid_relay();
    test_validate_p_tag_invalid();
    test_validate_a_tag_valid();
    test_validate_a_tag_invalid();
    test_validate_addressable_needs_d_tag();
    test_validate_addressable_with_d_tag();
    test_validate_d_tag_non_addressable();
    test_validate_null_event();

    printf("schema ok\n");
    return 0;
}
