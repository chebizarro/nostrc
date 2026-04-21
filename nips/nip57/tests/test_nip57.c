/**
 * NIP-57: Lightning Zaps — Unit Tests
 *
 * Tests BOLT11 parsing, zap request/receipt types, split calculations,
 * LNURL helpers, and memory cleanup functions.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip57/nip57.h"
#include "nostr/nip57/nip57_types.h"
#include "nostr-event.h"
#include "nostr-tag.h"

/* ── BOLT11 amount parsing ───────────────────────────────────────────── */

static void test_bolt11_null(void) {
    assert(nostr_nip57_parse_bolt11_amount(NULL) == 0);
}

static void test_bolt11_empty(void) {
    assert(nostr_nip57_parse_bolt11_amount("") == 0);
}

static void test_bolt11_no_ln_prefix(void) {
    assert(nostr_nip57_parse_bolt11_amount("invalid_string") == 0);
}

static void test_bolt11_milli_bitcoin(void) {
    /* lnbc1m1... = 1 milli-bitcoin = 100,000,000 msat */
    uint64_t amount = nostr_nip57_parse_bolt11_amount("lnbc1m1rest");
    assert(amount == 100000000ULL);
}

static void test_bolt11_micro_bitcoin(void) {
    /* lnbc100u1... = 100 micro-bitcoin = 10,000,000 msat */
    uint64_t amount = nostr_nip57_parse_bolt11_amount("lnbc100u1rest");
    assert(amount == 10000000ULL);
}

static void test_bolt11_nano_bitcoin(void) {
    /* lnbc1000n1... = 1000 nano-bitcoin = 100,000 msat */
    uint64_t amount = nostr_nip57_parse_bolt11_amount("lnbc1000n1rest");
    assert(amount == 100000ULL);
}

static void test_bolt11_pico_bitcoin(void) {
    /* lnbc1000p1... = 1000 pico-bitcoin = 1000 msat */
    uint64_t amount = nostr_nip57_parse_bolt11_amount("lnbc1000p1rest");
    assert(amount == 1000ULL);
}

static void test_bolt11_no_amount(void) {
    /* lnbc1... (no amount digits before multiplier or separator) */
    uint64_t amount = nostr_nip57_parse_bolt11_amount("lnbc1restofdata");
    assert(amount == 0);
}

static void test_bolt11_testnet(void) {
    /* lntb prefix for testnet */
    uint64_t amount = nostr_nip57_parse_bolt11_amount("lntb500u1rest");
    assert(amount == 50000000ULL);
}

/* ── Zap request free ────────────────────────────────────────────────── */

static void test_zap_request_free_null(void) {
    /* Should not crash */
    nostr_nip57_zap_request_free(NULL);
}

static void test_zap_request_free_populated(void) {
    NostrZapRequest *req = calloc(1, sizeof(NostrZapRequest));
    req->recipient_pubkey = strdup("aabbccdd11223344aabbccdd11223344aabbccdd11223344aabbccdd11223344");
    req->sender_pubkey = strdup("eeff0011eeff0011eeff0011eeff0011eeff0011eeff0011eeff0011eeff0011");
    req->event_id = strdup("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    req->content = strdup("Great post!");
    req->lnurl = strdup("lnurl1...");
    req->event_coordinate = strdup("31337:pubkey:identifier");
    req->amount = 1000;
    req->created_at = 1700000000;
    req->event_kind = 1;

    /* Allocate relays */
    req->relay_count = 2;
    req->relays = calloc(3, sizeof(char *));
    req->relays[0] = strdup("wss://relay1.example.com");
    req->relays[1] = strdup("wss://relay2.example.com");

    /* Should not crash or leak */
    nostr_nip57_zap_request_free(req);
}

/* ── Zap receipt free ────────────────────────────────────────────────── */

static void test_zap_receipt_free_null(void) {
    nostr_nip57_zap_receipt_free(NULL);
}

static void test_zap_receipt_free_populated(void) {
    NostrZapReceipt *receipt = calloc(1, sizeof(NostrZapReceipt));
    receipt->bolt11 = strdup("lnbc100u1...");
    receipt->preimage = strdup("0123456789abcdef");
    receipt->description = strdup("{\"kind\":9734}");
    receipt->recipient_pubkey = strdup("aabbccdd11223344aabbccdd11223344aabbccdd11223344aabbccdd11223344");
    receipt->sender_pubkey = strdup("eeff0011eeff0011eeff0011eeff0011eeff0011eeff0011eeff0011eeff0011");
    receipt->event_id = strdup("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    receipt->provider_pubkey = strdup("99887766554433221100aabbccddeeff99887766554433221100aabbccddeeff");
    receipt->created_at = 1700000000;
    receipt->event_kind = 1;

    nostr_nip57_zap_receipt_free(receipt);
}

/* ── LNURL pay info free ─────────────────────────────────────────────── */

static void test_lnurl_pay_info_free_null(void) {
    nostr_nip57_lnurl_pay_info_free(NULL);
}

static void test_lnurl_pay_info_free_populated(void) {
    NostrLnurlPayInfo *info = calloc(1, sizeof(NostrLnurlPayInfo));
    info->callback = strdup("https://example.com/lnurl/callback");
    info->nostr_pubkey = strdup("aabbccdd11223344aabbccdd11223344aabbccdd11223344aabbccdd11223344");
    info->min_sendable = 1000;
    info->max_sendable = 100000000;
    info->allows_nostr = true;

    nostr_nip57_lnurl_pay_info_free(info);
}

/* ── Zap split config free ───────────────────────────────────────────── */

static void test_split_config_free_null(void) {
    nostr_nip57_zap_split_config_free(NULL);
}

static void test_split_config_free_populated(void) {
    NostrZapSplitConfig *config = calloc(1, sizeof(NostrZapSplitConfig));
    config->count = 2;
    config->total_weight = 10;
    config->splits = calloc(2, sizeof(NostrZapSplit));
    config->splits[0].pubkey = strdup("aabb");
    config->splits[0].relay = strdup("wss://relay.example.com");
    config->splits[0].weight = 7;
    config->splits[1].pubkey = strdup("ccdd");
    config->splits[1].relay = strdup("wss://relay2.example.com");
    config->splits[1].weight = 3;

    nostr_nip57_zap_split_config_free(config);
}

/* ── Split amount calculation ────────────────────────────────────────── */

static void test_calculate_split_null(void) {
    assert(nostr_nip57_calculate_split_amount(NULL, 0, 1000) == 0);
}

static void test_calculate_split_zero_total(void) {
    NostrZapSplitConfig config = {0};
    config.count = 1;
    assert(nostr_nip57_calculate_split_amount(&config, 0, 0) == 0);
}

static void test_calculate_split_out_of_bounds(void) {
    NostrZapSplitConfig config = {0};
    config.count = 2;
    assert(nostr_nip57_calculate_split_amount(&config, 5, 1000) == 0);
}

static void test_calculate_split_equal_no_weights(void) {
    /* No weights → equal split */
    NostrZapSplit splits[2] = {
        {.pubkey = "aa", .weight = 0},
        {.pubkey = "bb", .weight = 0},
    };
    NostrZapSplitConfig config = {
        .splits = splits,
        .count = 2,
        .total_weight = 0,
    };

    assert(nostr_nip57_calculate_split_amount(&config, 0, 1000) == 500);
    assert(nostr_nip57_calculate_split_amount(&config, 1, 1000) == 500);
}

static void test_calculate_split_weighted(void) {
    /* 70/30 split of 10000 msat */
    NostrZapSplit splits[2] = {
        {.pubkey = "aa", .weight = 7},
        {.pubkey = "bb", .weight = 3},
    };
    NostrZapSplitConfig config = {
        .splits = splits,
        .count = 2,
        .total_weight = 10,
    };

    assert(nostr_nip57_calculate_split_amount(&config, 0, 10000) == 7000);
    assert(nostr_nip57_calculate_split_amount(&config, 1, 10000) == 3000);
}

/* ── LNURL helpers ───────────────────────────────────────────────────── */

static void test_lud16_to_url_null(void) {
    assert(nostr_nip57_lud16_to_lnurl_url(NULL) == NULL);
}

static void test_lud16_to_url_no_at(void) {
    assert(nostr_nip57_lud16_to_lnurl_url("nodomain") == NULL);
}

static void test_lud16_to_url_valid(void) {
    char *url = nostr_nip57_lud16_to_lnurl_url("satoshi@example.com");
    assert(url != NULL);
    assert(strcmp(url, "https://example.com/.well-known/lnurlp/satoshi") == 0);
    free(url);
}

static void test_lud16_to_url_empty_user(void) {
    assert(nostr_nip57_lud16_to_lnurl_url("@example.com") == NULL);
}

static void test_lud16_to_url_empty_domain(void) {
    assert(nostr_nip57_lud16_to_lnurl_url("user@") == NULL);
}

/* ── LNURL pay response parsing ──────────────────────────────────────── */

static void test_parse_lnurl_pay_null(void) {
    assert(nostr_nip57_parse_lnurl_pay_response(NULL) == NULL);
}

static void test_parse_lnurl_pay_valid(void) {
    const char *json =
        "{\"callback\":\"https://example.com/lnurl/cb\","
        "\"allowsNostr\":true,"
        "\"nostrPubkey\":\"aabbccdd11223344aabbccdd11223344aabbccdd11223344aabbccdd11223344\","
        "\"minSendable\":1000,"
        "\"maxSendable\":100000000}";

    NostrLnurlPayInfo *info = nostr_nip57_parse_lnurl_pay_response(json);
    assert(info != NULL);
    assert(info->allows_nostr == true);
    assert(info->callback != NULL);
    assert(strcmp(info->callback, "https://example.com/lnurl/cb") == 0);
    assert(info->nostr_pubkey != NULL);
    assert(info->min_sendable == 1000);
    assert(info->max_sendable == 100000000);

    nostr_nip57_lnurl_pay_info_free(info);
}

static void test_parse_lnurl_pay_no_nostr(void) {
    const char *json =
        "{\"callback\":\"https://example.com/cb\","
        "\"minSendable\":1000,\"maxSendable\":100000000}";

    NostrLnurlPayInfo *info = nostr_nip57_parse_lnurl_pay_response(json);
    assert(info != NULL);
    assert(info->allows_nostr == false);
    assert(info->nostr_pubkey == NULL);

    nostr_nip57_lnurl_pay_info_free(info);
}

/* ── Zap splits parsing via event tags ───────────────────────────────── */

static void test_parse_zap_splits_null(void) {
    assert(nostr_nip57_parse_zap_splits(NULL) == NULL);
}

static void test_parse_zap_splits_no_zap_tags(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);

    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("p", "aabb", NULL));
    nostr_event_set_tags(ev, tags);

    NostrZapSplitConfig *config = nostr_nip57_parse_zap_splits(ev);
    assert(config == NULL);

    nostr_event_free(ev);
}

static void test_parse_zap_splits_with_weights(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);

    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("zap", "aabb", "wss://relay1.com", "7", NULL));
    nostr_tags_append(tags, nostr_tag_new("zap", "ccdd", "wss://relay2.com", "3", NULL));
    nostr_event_set_tags(ev, tags);

    NostrZapSplitConfig *config = nostr_nip57_parse_zap_splits(ev);
    assert(config != NULL);
    assert(config->count == 2);
    assert(config->total_weight == 10);
    assert(strcmp(config->splits[0].pubkey, "aabb") == 0);
    assert(config->splits[0].weight == 7);
    assert(strcmp(config->splits[1].pubkey, "ccdd") == 0);
    assert(config->splits[1].weight == 3);

    nostr_nip57_zap_split_config_free(config);
    nostr_event_free(ev);
}

/* ── Validate zap request ────────────────────────────────────────────── */

static void test_validate_zap_request_null_event(void) {
    /* NULL event should return false */
    assert(nostr_nip57_validate_zap_request(NULL) == false);
}

int main(void) {
    /* BOLT11 parsing */
    test_bolt11_null();
    test_bolt11_empty();
    test_bolt11_no_ln_prefix();
    test_bolt11_milli_bitcoin();
    test_bolt11_micro_bitcoin();
    test_bolt11_nano_bitcoin();
    test_bolt11_pico_bitcoin();
    test_bolt11_no_amount();
    test_bolt11_testnet();

    /* Free functions */
    test_zap_request_free_null();
    test_zap_request_free_populated();
    test_zap_receipt_free_null();
    test_zap_receipt_free_populated();
    test_lnurl_pay_info_free_null();
    test_lnurl_pay_info_free_populated();
    test_split_config_free_null();
    test_split_config_free_populated();

    /* Split calculations */
    test_calculate_split_null();
    test_calculate_split_zero_total();
    test_calculate_split_out_of_bounds();
    test_calculate_split_equal_no_weights();
    test_calculate_split_weighted();

    /* LNURL helpers */
    test_lud16_to_url_null();
    test_lud16_to_url_no_at();
    test_lud16_to_url_valid();
    test_lud16_to_url_empty_user();
    test_lud16_to_url_empty_domain();

    /* LNURL pay response */
    test_parse_lnurl_pay_null();
    test_parse_lnurl_pay_valid();
    test_parse_lnurl_pay_no_nostr();

    /* Zap splits */
    test_parse_zap_splits_null();
    test_parse_zap_splits_no_zap_tags();
    test_parse_zap_splits_with_weights();

    /* Validation */
    test_validate_zap_request_null_event();

    printf("nip57 ok\n");
    return 0;
}
