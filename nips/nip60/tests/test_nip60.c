#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip60/nip60.h"

/* ---- Kind checking ---- */

static void test_kind_checks(void) {
    assert(nostr_nip60_is_wallet_kind(17375));
    assert(!nostr_nip60_is_wallet_kind(7375));
    assert(!nostr_nip60_is_wallet_kind(7376));

    assert(nostr_nip60_is_token_kind(7375));
    assert(!nostr_nip60_is_token_kind(17375));

    assert(nostr_nip60_is_history_kind(7376));
    assert(!nostr_nip60_is_history_kind(7375));
}

/* ---- Direction ---- */

static void test_direction_parse(void) {
    assert(nostr_nip60_direction_parse("in") == NOSTR_NIP60_DIR_IN);
    assert(nostr_nip60_direction_parse("out") == NOSTR_NIP60_DIR_OUT);
    assert(nostr_nip60_direction_parse("invalid") == -1);
    assert(nostr_nip60_direction_parse(NULL) == -1);
}

static void test_direction_string(void) {
    assert(strcmp(nostr_nip60_direction_string(NOSTR_NIP60_DIR_IN), "in") == 0);
    assert(strcmp(nostr_nip60_direction_string(NOSTR_NIP60_DIR_OUT), "out") == 0);
}

/* ---- Unit ---- */

static void test_unit_parse(void) {
    assert(nostr_nip60_unit_parse("sat") == NOSTR_NIP60_UNIT_SAT);
    assert(nostr_nip60_unit_parse("msat") == NOSTR_NIP60_UNIT_MSAT);
    assert(nostr_nip60_unit_parse("usd") == NOSTR_NIP60_UNIT_USD);
    assert(nostr_nip60_unit_parse("eur") == NOSTR_NIP60_UNIT_EUR);
    assert(nostr_nip60_unit_parse("btc") == NOSTR_NIP60_UNIT_UNKNOWN);
    assert(nostr_nip60_unit_parse(NULL) == NOSTR_NIP60_UNIT_UNKNOWN);
}

static void test_unit_string(void) {
    assert(strcmp(nostr_nip60_unit_string(NOSTR_NIP60_UNIT_SAT), "sat") == 0);
    assert(strcmp(nostr_nip60_unit_string(NOSTR_NIP60_UNIT_USD), "usd") == 0);
    assert(nostr_nip60_unit_string(NOSTR_NIP60_UNIT_UNKNOWN) == NULL);
}

static void test_unit_is_valid(void) {
    assert(nostr_nip60_unit_is_valid("sat"));
    assert(nostr_nip60_unit_is_valid("usd"));
    assert(!nostr_nip60_unit_is_valid("btc"));
    assert(!nostr_nip60_unit_is_valid(NULL));
}

/* ---- Amount formatting ---- */

static void test_format_amount_sat(void) {
    char *s = nostr_nip60_format_amount(1000, "sat");
    assert(s != NULL);
    assert(strcmp(s, "1000 sat") == 0);
    free(s);
}

static void test_format_amount_msat(void) {
    char *s = nostr_nip60_format_amount(1500, "msat");
    assert(s != NULL);
    assert(strcmp(s, "1.500 sat") == 0);
    free(s);

    s = nostr_nip60_format_amount(3000, "msat");
    assert(s != NULL);
    assert(strcmp(s, "3 sat") == 0);
    free(s);
}

static void test_format_amount_usd(void) {
    char *s = nostr_nip60_format_amount(150, "usd");
    assert(s != NULL);
    assert(strcmp(s, "1.50 usd") == 0);
    free(s);
}

static void test_format_amount_null_unit(void) {
    assert(nostr_nip60_format_amount(100, NULL) == NULL);
}

/* ---- Token event ---- */

static void test_parse_token(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP60_KIND_TOKEN);
    nostr_event_set_created_at(ev, 1700000000);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("mint", "https://mint.example.com", NULL));

    NostrNip60Token token;
    int rc = nostr_nip60_parse_token(ev, &token);
    assert(rc == 0);
    assert(token.mint != NULL);
    assert(strcmp(token.mint, "https://mint.example.com") == 0);
    assert(token.created_at == 1700000000);

    nostr_event_free(ev);
}

static void test_parse_token_wrong_kind(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);

    NostrNip60Token token;
    assert(nostr_nip60_parse_token(ev, &token) == -EINVAL);

    nostr_event_free(ev);
}

static void test_create_token(void) {
    NostrEvent *ev = nostr_event_new();

    int rc = nostr_nip60_create_token(ev, "encrypted_content", "https://mint.example.com");
    assert(rc == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP60_KIND_TOKEN);
    assert(strcmp(nostr_event_get_content(ev), "encrypted_content") == 0);

    const NostrTags *tags = nostr_event_get_tags(ev);
    assert(nostr_tags_size(tags) == 1);
    const NostrTag *t = nostr_tags_get(tags, 0);
    assert(strcmp(nostr_tag_get(t, 0), "mint") == 0);
    assert(strcmp(nostr_tag_get(t, 1), "https://mint.example.com") == 0);

    nostr_event_free(ev);
}

static void test_create_token_no_mint(void) {
    NostrEvent *ev = nostr_event_new();

    int rc = nostr_nip60_create_token(ev, "encrypted", NULL);
    assert(rc == 0);
    assert(nostr_tags_size(nostr_event_get_tags(ev)) == 0);

    nostr_event_free(ev);
}

/* ---- History ---- */

static void test_parse_history_tags(void) {
    const char *json = "[[\"direction\",\"in\"],[\"amount\",\"500\"],[\"unit\",\"sat\"]]";

    NostrNip60HistoryEntry entry;
    int rc = nostr_nip60_parse_history_tags(json, &entry);
    assert(rc == 0);
    assert(entry.direction == NOSTR_NIP60_DIR_IN);
    assert(entry.amount == 500);
    assert(entry.unit != NULL);
    assert(strcmp(entry.unit, "sat") == 0);
}

static void test_parse_history_tags_out(void) {
    const char *json = "[[\"direction\",\"out\"],[\"amount\",\"1000\"]]";

    NostrNip60HistoryEntry entry;
    int rc = nostr_nip60_parse_history_tags(json, &entry);
    assert(rc == 0);
    assert(entry.direction == NOSTR_NIP60_DIR_OUT);
    assert(entry.amount == 1000);
    assert(entry.unit == NULL);
}

static void test_parse_history_missing_direction(void) {
    const char *json = "[[\"amount\",\"100\"]]";

    NostrNip60HistoryEntry entry;
    assert(nostr_nip60_parse_history_tags(json, &entry) == -EINVAL);
}

static void test_parse_history_missing_amount(void) {
    const char *json = "[[\"direction\",\"in\"]]";

    NostrNip60HistoryEntry entry;
    assert(nostr_nip60_parse_history_tags(json, &entry) == -EINVAL);
}

static void test_parse_history_null_inputs(void) {
    NostrNip60HistoryEntry entry;
    assert(nostr_nip60_parse_history_tags(NULL, &entry) == -EINVAL);
    assert(nostr_nip60_parse_history_tags("[]", NULL) == -EINVAL);
}

static void test_build_history_content(void) {
    char *content = nostr_nip60_build_history_content(NOSTR_NIP60_DIR_IN, 500, "sat");
    assert(content != NULL);
    assert(strstr(content, "\"direction\"") != NULL);
    assert(strstr(content, "\"in\"") != NULL);
    assert(strstr(content, "\"amount\"") != NULL);
    assert(strstr(content, "\"500\"") != NULL);
    assert(strstr(content, "\"unit\"") != NULL);
    assert(strstr(content, "\"sat\"") != NULL);

    /* Verify it roundtrips */
    NostrNip60HistoryEntry entry;
    int rc = nostr_nip60_parse_history_tags(content, &entry);
    assert(rc == 0);
    assert(entry.direction == NOSTR_NIP60_DIR_IN);
    assert(entry.amount == 500);
    assert(strcmp(entry.unit, "sat") == 0);

    free(content);
}

static void test_build_history_content_no_unit(void) {
    char *content = nostr_nip60_build_history_content(NOSTR_NIP60_DIR_OUT, 1000, NULL);
    assert(content != NULL);
    assert(strstr(content, "\"out\"") != NULL);
    assert(strstr(content, "\"1000\"") != NULL);
    assert(strstr(content, "unit") == NULL);
    free(content);
}

static void test_create_history(void) {
    NostrEvent *ev = nostr_event_new();
    int rc = nostr_nip60_create_history(ev, "encrypted_history");
    assert(rc == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP60_KIND_HISTORY);
    assert(strcmp(nostr_event_get_content(ev), "encrypted_history") == 0);
    nostr_event_free(ev);
}

/* ---- Wallet metadata ---- */

static void test_parse_wallet_mints(void) {
    const char *json = "[[\"mint\",\"https://mint1.example.com\"],[\"mint\",\"https://mint2.example.com\"]]";

    char *mints[5];
    size_t count = 0;
    int rc = nostr_nip60_parse_wallet_mints(json, mints, 5, &count);
    assert(rc == 0);
    assert(count == 2);
    assert(strcmp(mints[0], "https://mint1.example.com") == 0);
    assert(strcmp(mints[1], "https://mint2.example.com") == 0);

    free(mints[0]);
    free(mints[1]);
}

static void test_parse_wallet_mints_with_privkey(void) {
    const char *json = "[[\"privkey\",\"deadbeef\"],[\"mint\",\"https://mint.com\"]]";

    char *mints[5];
    size_t count = 0;
    int rc = nostr_nip60_parse_wallet_mints(json, mints, 5, &count);
    assert(rc == 0);
    assert(count == 1);
    assert(strcmp(mints[0], "https://mint.com") == 0);
    free(mints[0]);
}

static void test_parse_wallet_mints_max(void) {
    const char *json = "[[\"mint\",\"a\"],[\"mint\",\"b\"],[\"mint\",\"c\"]]";

    char *mints[2];
    size_t count = 0;
    int rc = nostr_nip60_parse_wallet_mints(json, mints, 2, &count);
    assert(rc == 0);
    assert(count == 2);
    free(mints[0]);
    free(mints[1]);
}

static void test_parse_wallet_mints_null_inputs(void) {
    char *mints[5];
    size_t count = 0;
    assert(nostr_nip60_parse_wallet_mints(NULL, mints, 5, &count) == -EINVAL);
    assert(nostr_nip60_parse_wallet_mints("[]", NULL, 5, &count) == -EINVAL);
    assert(nostr_nip60_parse_wallet_mints("[]", mints, 5, NULL) == -EINVAL);
}

static void test_build_wallet_content(void) {
    const char *urls[] = {
        "https://mint1.example.com",
        "https://mint2.example.com"
    };

    char *content = nostr_nip60_build_wallet_content(urls, 2);
    assert(content != NULL);
    assert(strstr(content, "mint1.example.com") != NULL);
    assert(strstr(content, "mint2.example.com") != NULL);

    /* Verify it roundtrips */
    char *mints[5];
    size_t count = 0;
    int rc = nostr_nip60_parse_wallet_mints(content, mints, 5, &count);
    assert(rc == 0);
    assert(count == 2);
    assert(strcmp(mints[0], "https://mint1.example.com") == 0);
    assert(strcmp(mints[1], "https://mint2.example.com") == 0);

    free(mints[0]);
    free(mints[1]);
    free(content);
}

static void test_build_wallet_content_empty(void) {
    char *content = nostr_nip60_build_wallet_content(NULL, 0);
    assert(content != NULL);
    assert(strcmp(content, "[]") == 0);
    free(content);
}

static void test_create_wallet(void) {
    NostrEvent *ev = nostr_event_new();
    int rc = nostr_nip60_create_wallet(ev, "encrypted_wallet");
    assert(rc == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP60_KIND_WALLET);
    assert(strcmp(nostr_event_get_content(ev), "encrypted_wallet") == 0);
    nostr_event_free(ev);
}

int main(void) {
    test_kind_checks();
    test_direction_parse();
    test_direction_string();
    test_unit_parse();
    test_unit_string();
    test_unit_is_valid();
    test_format_amount_sat();
    test_format_amount_msat();
    test_format_amount_usd();
    test_format_amount_null_unit();
    test_parse_token();
    test_parse_token_wrong_kind();
    test_create_token();
    test_create_token_no_mint();
    test_parse_history_tags();
    test_parse_history_tags_out();
    test_parse_history_missing_direction();
    test_parse_history_missing_amount();
    test_parse_history_null_inputs();
    test_build_history_content();
    test_build_history_content_no_unit();
    test_create_history();
    test_parse_wallet_mints();
    test_parse_wallet_mints_with_privkey();
    test_parse_wallet_mints_max();
    test_parse_wallet_mints_null_inputs();
    test_build_wallet_content();
    test_build_wallet_content_empty();
    test_create_wallet();
    printf("nip60 ok\n");
    return 0;
}
