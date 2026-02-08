#include "nip11.h"
#include "json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_parse_minimal(void) {
    const char *json =
        "{"
        "\"name\":\"Example Relay\"," 
        "\"software\":\"my-relay\"," 
        "\"version\":\"1.2.3\"," 
        "\"supported_nips\":[1,11,15],"
        "\"limitation\":{"
        "  \"max_message_length\": 5000,"
        "  \"max_subscriptions\": 64,"
        "  \"auth_required\": true,"
        "  \"payment_required\": false"
        "}"
        "}";

    RelayInformationDocument *info = nostr_nip11_parse_info(json);
    assert(info != NULL);
    assert(info->name && strcmp(info->name, "Example Relay") == 0);
    assert(info->software && strcmp(info->software, "my-relay") == 0);
    assert(info->version && strcmp(info->version, "1.2.3") == 0);
    assert(info->supported_nips && info->supported_nips_count == 3);
    assert(info->supported_nips[0] == 1);
    assert(info->supported_nips[1] == 11);
    assert(info->supported_nips[2] == 15);
    assert(info->limitation != NULL);
    assert(info->limitation->max_message_length == 5000);
    assert(info->limitation->max_subscriptions == 64);
    assert(info->limitation->auth_required == true);
    assert(info->limitation->payment_required == false);

    nostr_nip11_free_info(info);
}

static void test_parse_invalid_returns_null(void) {
    const char *bad = "[not-an-object]"; // per NIP-11 should be an object
    RelayInformationDocument *info = nostr_nip11_parse_info(bad);
    assert(info == NULL);
}

static void test_parse_extended_fields(void) {
    const char *json =
        "{"
        "\"name\":\"X\"," 
        "\"supported_nips\":[1,2],"
        "\"relay_countries\":[\"US\",\"DE\"],"
        "\"language_tags\":[\"en\",\"de\"],"
        "\"tags\":[\"nostr\",\"relay\"],"
        "\"posting_policy\":\"https://example/policy\"," 
        "\"payments_url\":\"https://example/pay\","
        "\"icon\":\"https://example/icon.png\"," 
        "\"fees\":{"
        "  \"admission\":[{\"amount\":10,\"unit\":\"sat\"}],"
        "  \"subscription\":[{\"amount\":20,\"unit\":\"sat\"}],"
        "  \"publication\":[{\"kinds\":[1,30023],\"amount\":5,\"unit\":\"sat\"}]"
        "}"
        "}";

    RelayInformationDocument *info = nostr_nip11_parse_info(json);
    assert(info != NULL);
    assert(info->relay_countries && info->relay_countries_count == 2);
    assert(strcmp(info->relay_countries[0], "US") == 0);
    assert(info->language_tags && info->language_tags_count == 2);
    assert(strcmp(info->language_tags[1], "de") == 0);
    assert(info->tags && info->tags_count == 2);
    assert(strcmp(info->tags[0], "nostr") == 0);
    assert(info->posting_policy && strstr(info->posting_policy, "policy") != NULL);
    assert(info->payments_url && strstr(info->payments_url, "pay") != NULL);
    assert(info->icon && strstr(info->icon, "icon.png") != NULL);
    assert(info->fees != NULL);
    assert(info->fees->admission.count == 1);
    assert(info->fees->admission.items[0].amount == 10);
    assert(strcmp(info->fees->admission.items[0].unit, "sat") == 0);
    assert(info->fees->subscription.count == 1);
    assert(info->fees->subscription.items[0].amount == 20);
    assert(strcmp(info->fees->subscription.items[0].unit, "sat") == 0);
    assert(info->fees->publication.count == 2);
    assert(info->fees->publication.kinds[0] == 1);
    assert(info->fees->publication.kinds[1] == 30023);
    assert(info->fees->publication.amount == 5);
    assert(strcmp(info->fees->publication.unit, "sat") == 0);

    // Free smoke (ASAN/UBSAN will catch leaks/double-free)
    nostr_nip11_free_info(info);
}

int main(void) {
    nostr_json_init();
    test_parse_minimal();
    test_parse_invalid_returns_null();
    test_parse_extended_fields();
    printf("test_nip11: OK\n");
    nostr_json_cleanup();
    return 0;
}
