/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * test_nip10_thread_manager.c - Unit tests for the unified NIP-10 parser
 *
 * nostrc-pp64 (Epic 4.3/4.4): Tests canonical NIP-10 parsing, caching,
 * positional fallback, explicit markers, NIP-22 uppercase E tags,
 * addressable event references (A tags), and root kind (k tags).
 */

#include <glib.h>
#include "../src/model/gnostr-nip10-thread-manager.h"

#define ROOT_ID  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define REPLY_ID "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define EVENT_ID "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define PUBKEY   "1111111111111111111111111111111111111111111111111111111111111111"

/* ========== Test helpers ========== */

static void test_explicit_markers(void) {
    /* Event with explicit root and reply markers */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"test\","
        "\"tags\":["
        "[\"e\",\"" ROOT_ID "\",\"wss://relay.example\",\"root\"],"
        "[\"e\",\"" REPLY_ID "\",\"wss://relay2.example\",\"reply\"]"
        "]}";

    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_id, ==, ROOT_ID);
    g_assert_cmpstr(info.reply_id, ==, REPLY_ID);
    g_assert_cmpstr(info.root_relay_hint, ==, "wss://relay.example");
    g_assert_cmpstr(info.reply_relay_hint, ==, "wss://relay2.example");
    g_assert_true(info.has_explicit_markers);
}

static void test_positional_single_etag(void) {
    /* Single e-tag without marker -> root */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"test\","
        "\"tags\":[[\"e\",\"" ROOT_ID "\"]]}";

    gnostr_nip10_cache_clear();
    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_id, ==, ROOT_ID);
    g_assert_null(info.reply_id);
    g_assert_false(info.has_explicit_markers);
}

static void test_positional_two_etags(void) {
    /* Two e-tags without markers -> first=root, last=reply */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"test\","
        "\"tags\":["
        "[\"e\",\"" ROOT_ID "\"],"
        "[\"e\",\"" REPLY_ID "\"]"
        "]}";

    gnostr_nip10_cache_clear();
    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_id, ==, ROOT_ID);
    g_assert_cmpstr(info.reply_id, ==, REPLY_ID);
    g_assert_false(info.has_explicit_markers);
}

static void test_nip22_uppercase_etag(void) {
    /* NIP-22 uses uppercase "E" tags */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1111,"
        "\"created_at\":1700000000,\"content\":\"comment\","
        "\"tags\":[[\"E\",\"" ROOT_ID "\",\"\",\"root\"]]}";

    gnostr_nip10_cache_clear();
    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_id, ==, ROOT_ID);
    g_assert_true(info.has_explicit_markers);
}

static void test_no_etags(void) {
    /* Event with no e-tags */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"hello world\","
        "\"tags\":[[\"p\",\"" PUBKEY "\"]]}";

    gnostr_nip10_cache_clear();
    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_null(info.root_id);
    g_assert_null(info.reply_id);
}

static void test_caching(void) {
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"test\","
        "\"tags\":[[\"e\",\"" ROOT_ID "\",\"\",\"root\"]]}";

    gnostr_nip10_cache_clear();
    g_assert_cmpuint(gnostr_nip10_cache_size(), ==, 0);

    /* First parse should cache */
    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpuint(gnostr_nip10_cache_size(), ==, 1);

    /* Second parse should hit cache */
    GnostrNip10ThreadInfo info2;
    g_assert_true(gnostr_nip10_parse_thread(json, &info2));
    g_assert_cmpstr(info2.root_id, ==, ROOT_ID);
    g_assert_cmpuint(gnostr_nip10_cache_size(), ==, 1); /* no new entry */

    /* Lookup by ID */
    GnostrNip10ThreadInfo info3;
    g_assert_true(gnostr_nip10_lookup_cached(EVENT_ID, &info3));
    g_assert_cmpstr(info3.root_id, ==, ROOT_ID);

    /* Clear and verify */
    gnostr_nip10_cache_clear();
    g_assert_cmpuint(gnostr_nip10_cache_size(), ==, 0);
    g_assert_false(gnostr_nip10_lookup_cached(EVENT_ID, &info3));
}

static void test_is_thread_reply(void) {
    gnostr_nip10_cache_clear();

    /* Reply event */
    const char *reply =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"test\","
        "\"tags\":[[\"e\",\"" ROOT_ID "\",\"\",\"root\"]]}";
    g_assert_true(gnostr_nip10_is_thread_reply(reply));

    /* Non-reply event */
    gnostr_nip10_cache_clear();
    const char *standalone =
        "{\"id\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\","
        "\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"hello\",\"tags\":[]}";
    g_assert_false(gnostr_nip10_is_thread_reply(standalone));
}

static void test_get_thread_root(void) {
    gnostr_nip10_cache_clear();

    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"test\","
        "\"tags\":[[\"e\",\"" ROOT_ID "\",\"wss://r.example\",\"root\"]]}";

    const char *root = gnostr_nip10_get_thread_root(json);
    g_assert_cmpstr(root, ==, ROOT_ID);
}

static void test_relay_hints_with_positional(void) {
    gnostr_nip10_cache_clear();

    /* Positional e-tags with relay hints */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"test\","
        "\"tags\":["
        "[\"e\",\"" ROOT_ID "\",\"wss://first.relay\"],"
        "[\"e\",\"" REPLY_ID "\",\"wss://last.relay\"]"
        "]}";

    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_id, ==, ROOT_ID);
    g_assert_cmpstr(info.reply_id, ==, REPLY_ID);
    g_assert_cmpstr(info.root_relay_hint, ==, "wss://first.relay");
    g_assert_cmpstr(info.reply_relay_hint, ==, "wss://last.relay");
}

static void test_nip22_a_tag(void) {
    gnostr_nip10_cache_clear();

    /* NIP-22 comment on an article (addressable event) with A tag */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1111,"
        "\"created_at\":1700000000,\"content\":\"great article!\","
        "\"tags\":["
        "[\"E\",\"" ROOT_ID "\",\"wss://relay.example\",\"root\"],"
        "[\"A\",\"30023:" PUBKEY ":my-article\",\"wss://author.relay\"],"
        "[\"k\",\"30023\"]"
        "]}";

    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_id, ==, ROOT_ID);
    g_assert_cmpstr(info.root_addr, ==, "30023:" PUBKEY ":my-article");
    g_assert_cmpstr(info.root_addr_relay, ==, "wss://author.relay");
    g_assert_cmpint(info.root_kind, ==, 30023);
    g_assert_true(info.has_explicit_markers);
}

static void test_nip22_k_tag_only(void) {
    gnostr_nip10_cache_clear();

    /* Comment with k tag but no A tag (comment on a regular kind event) */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1111,"
        "\"created_at\":1700000000,\"content\":\"nice note!\","
        "\"tags\":["
        "[\"E\",\"" ROOT_ID "\",\"\",\"root\"],"
        "[\"k\",\"1\"]"
        "]}";

    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_id, ==, ROOT_ID);
    g_assert_null(info.root_addr);
    g_assert_cmpint(info.root_kind, ==, 1);
}

static void test_nip22_lowercase_a_tag(void) {
    gnostr_nip10_cache_clear();

    /* Lowercase "a" tag also works */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1111,"
        "\"created_at\":1700000000,\"content\":\"comment\","
        "\"tags\":["
        "[\"e\",\"" ROOT_ID "\",\"\",\"root\"],"
        "[\"a\",\"30023:" PUBKEY ":blog-post\",\"wss://relay3.example\"],"
        "[\"k\",\"30023\"]"
        "]}";

    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_cmpstr(info.root_addr, ==, "30023:" PUBKEY ":blog-post");
    g_assert_cmpstr(info.root_addr_relay, ==, "wss://relay3.example");
    g_assert_cmpint(info.root_kind, ==, 30023);
}

static void test_no_nip22_fields(void) {
    gnostr_nip10_cache_clear();

    /* Regular kind:1 note has no A/k tags */
    const char *json =
        "{\"id\":\"" EVENT_ID "\",\"pubkey\":\"" PUBKEY "\",\"kind\":1,"
        "\"created_at\":1700000000,\"content\":\"plain note\","
        "\"tags\":[[\"e\",\"" ROOT_ID "\",\"\",\"root\"]]}";

    GnostrNip10ThreadInfo info;
    g_assert_true(gnostr_nip10_parse_thread(json, &info));
    g_assert_null(info.root_addr);
    g_assert_null(info.root_addr_relay);
    g_assert_cmpint(info.root_kind, ==, -1);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/nip10-thread-manager/explicit-markers", test_explicit_markers);
    g_test_add_func("/nip10-thread-manager/positional-single", test_positional_single_etag);
    g_test_add_func("/nip10-thread-manager/positional-two", test_positional_two_etags);
    g_test_add_func("/nip10-thread-manager/nip22-uppercase", test_nip22_uppercase_etag);
    g_test_add_func("/nip10-thread-manager/no-etags", test_no_etags);
    g_test_add_func("/nip10-thread-manager/caching", test_caching);
    g_test_add_func("/nip10-thread-manager/is-thread-reply", test_is_thread_reply);
    g_test_add_func("/nip10-thread-manager/get-thread-root", test_get_thread_root);
    g_test_add_func("/nip10-thread-manager/relay-hints-positional", test_relay_hints_with_positional);
    g_test_add_func("/nip10-thread-manager/nip22-a-tag", test_nip22_a_tag);
    g_test_add_func("/nip10-thread-manager/nip22-k-tag-only", test_nip22_k_tag_only);
    g_test_add_func("/nip10-thread-manager/nip22-lowercase-a", test_nip22_lowercase_a_tag);
    g_test_add_func("/nip10-thread-manager/no-nip22-fields", test_no_nip22_fields);

    return g_test_run();
}
