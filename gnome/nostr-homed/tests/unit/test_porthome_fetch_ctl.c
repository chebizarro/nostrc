/*
 * test_porthome_fetch_ctl.c — unit tests for the control-payload parser
 * and the progress-line codec. See porthome_fetch_ctl.h.
 *
 * SPDX-License-Identifier: MIT
 * Bead: nostrc-9k4g.
 *
 * These tests link ONLY porthome_fetch_ctl.c — no libnostr, no libhanami,
 * no libcurl — so they run under the base test matrix even when the
 * helper binary itself does not build.
 */

#include "porthome_fetch_ctl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define OK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static const char *GOOD_HEX =
    "0011223344556677889900112233445566778899aabbccddeeff0011223344ff";

static const char *MAKE_GOOD_JSON =
    "{"
    "\"account_pubkey_hex\":\"0011223344556677889900112233445566778899aabbccddeeff0011223344ff\","
    "\"home_root_id_hex\":\"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
    "\"home_key_hex\":\"1122334455667788112233445566778811223344556677881122334455667788\","
    "\"d_tag\":\"nostr-homed.home.v1:personal\","
    "\"relays\":[\"wss://relay.sharegap.net\"],"
    "\"blossom_servers\":[\"https://blossom.sharegap.net\"],"
    "\"bandwidth_cap_bytes\":1048576,"
    "\"per_file_timeout_sec\":30,"
    "\"max_total_bytes\":20971520,"
    "\"relay_timeout_ms\":10000,"
    "\"allow_insecure\":false"
    "}";

static void test_good(void) {
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(MAKE_GOOD_JSON, strlen(MAKE_GOOD_JSON), &c);
    OK(s == NH_PORTHOME_FETCH_CTL_OK);
    OK(strcmp(c.account_pubkey_hex, GOOD_HEX) == 0);
    OK(strcmp(c.d_tag, "nostr-homed.home.v1:personal") == 0);
    OK(c.relays_count == 1);
    OK(strcmp(c.relays[0], "wss://relay.sharegap.net") == 0);
    OK(c.blossom_servers_count == 1);
    OK(strcmp(c.blossom_servers[0], "https://blossom.sharegap.net") == 0);
    OK(c.bandwidth_cap_bytes == 1048576);
    OK(c.per_file_timeout_sec == 30);
    OK(c.max_total_bytes == 20971520);
    OK(c.relay_timeout_ms == 10000);
    OK(c.allow_insecure == 0);
}

static void test_missing_field(void) {
    /* Drop the trailing `allow_insecure` and one required field. */
    const char *bad =
        "{"
        "\"account_pubkey_hex\":\"0011223344556677889900112233445566778899aabbccddeeff0011223344ff\","
        "\"home_root_id_hex\":\"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
        "\"d_tag\":\"x\","
        "\"relays\":[\"wss://r\"],\"blossom_servers\":[\"https://s\"],"
        "\"bandwidth_cap_bytes\":0,\"per_file_timeout_sec\":0,"
        "\"max_total_bytes\":0,\"relay_timeout_ms\":0"
        "}";
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(bad, strlen(bad), &c);
    OK(s == NH_PORTHOME_FETCH_CTL_ERR_MISSING);
}

static void test_bad_hex(void) {
    /* Uppercase hex is refused (spec is lowercase). */
    const char *bad =
        "{"
        "\"account_pubkey_hex\":\"AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899\","
        "\"home_root_id_hex\":\"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
        "\"home_key_hex\":\"1122334455667788112233445566778811223344556677881122334455667788\","
        "\"d_tag\":\"x\","
        "\"relays\":[\"wss://r\"],\"blossom_servers\":[\"https://s\"],"
        "\"bandwidth_cap_bytes\":0,\"per_file_timeout_sec\":0,"
        "\"max_total_bytes\":0,\"relay_timeout_ms\":0"
        "}";
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(bad, strlen(bad), &c);
    OK(s == NH_PORTHOME_FETCH_CTL_ERR_HEX);
}

static void test_unknown_key(void) {
    const char *bad =
        "{"
        "\"account_pubkey_hex\":\"0011223344556677889900112233445566778899aabbccddeeff0011223344ff\","
        "\"home_root_id_hex\":\"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
        "\"home_key_hex\":\"1122334455667788112233445566778811223344556677881122334455667788\","
        "\"d_tag\":\"x\","
        "\"relays\":[\"wss://r\"],\"blossom_servers\":[\"https://s\"],"
        "\"bandwidth_cap_bytes\":0,\"per_file_timeout_sec\":0,"
        "\"max_total_bytes\":0,\"relay_timeout_ms\":0,"
        "\"extra_evil_flag\":true"
        "}";
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(bad, strlen(bad), &c);
    OK(s == NH_PORTHOME_FETCH_CTL_ERR_UNKNOWN_KEY);
}

static void test_bad_url(void) {
    /* http:// (not allowed) for Blossom, strict mode. */
    const char *bad =
        "{"
        "\"account_pubkey_hex\":\"0011223344556677889900112233445566778899aabbccddeeff0011223344ff\","
        "\"home_root_id_hex\":\"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
        "\"home_key_hex\":\"1122334455667788112233445566778811223344556677881122334455667788\","
        "\"d_tag\":\"x\","
        "\"relays\":[\"wss://r\"],\"blossom_servers\":[\"http://s\"],"
        "\"bandwidth_cap_bytes\":0,\"per_file_timeout_sec\":0,"
        "\"max_total_bytes\":0,\"relay_timeout_ms\":0"
        "}";
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(bad, strlen(bad), &c);
    OK(s == NH_PORTHOME_FETCH_CTL_ERR_URL);
}

static void test_allow_insecure(void) {
    /* Same http:// blossom URL but with allow_insecure:true. */
    const char *good =
        "{"
        "\"account_pubkey_hex\":\"0011223344556677889900112233445566778899aabbccddeeff0011223344ff\","
        "\"home_root_id_hex\":\"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
        "\"home_key_hex\":\"1122334455667788112233445566778811223344556677881122334455667788\","
        "\"d_tag\":\"x\","
        "\"relays\":[\"ws://127.0.0.1:8080\"],\"blossom_servers\":[\"http://127.0.0.1:9090\"],"
        "\"bandwidth_cap_bytes\":0,\"per_file_timeout_sec\":0,"
        "\"max_total_bytes\":0,\"relay_timeout_ms\":0,"
        "\"allow_insecure\":true"
        "}";
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(good, strlen(good), &c);
    OK(s == NH_PORTHOME_FETCH_CTL_OK);
    OK(c.allow_insecure == 1);
}

static void test_too_large(void) {
    /* Craft a 20 KiB blob to exceed NH_PORTHOME_FETCH_MAX_CTL_BYTES (16 KiB). */
    size_t len = 20u * 1024u;
    char *buf = (char *)malloc(len);
    OK(buf != NULL);
    memset(buf, ' ', len);
    buf[0] = '{'; buf[len-1] = '}';
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(buf, len, &c);
    OK(s == NH_PORTHOME_FETCH_CTL_ERR_TOO_LARGE);
    free(buf);
}

static void test_no_escapes(void) {
    /* A backslash in a string field is refused (we intentionally
     * disallow all JSON escapes). */
    const char *bad =
        "{"
        "\"account_pubkey_hex\":\"0011223344556677889900112233445566778899aabbccddeeff0011223344ff\","
        "\"home_root_id_hex\":\"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
        "\"home_key_hex\":\"1122334455667788112233445566778811223344556677881122334455667788\","
        "\"d_tag\":\"bad\\ttag\","
        "\"relays\":[\"wss://r\"],\"blossom_servers\":[\"https://s\"],"
        "\"bandwidth_cap_bytes\":0,\"per_file_timeout_sec\":0,"
        "\"max_total_bytes\":0,\"relay_timeout_ms\":0"
        "}";
    nh_porthome_fetch_ctl c = {0};
    nh_porthome_fetch_ctl_status s =
        nh_porthome_fetch_ctl_parse(bad, strlen(bad), &c);
    OK(s == NH_PORTHOME_FETCH_CTL_ERR_JSON);
}

static void test_progress_roundtrip(void) {
    struct { uint64_t b, f; nh_porthome_fetch_phase ph; const char *want; } cases[] = {
        {0, 0, NH_PORTHOME_FETCH_PHASE_MANIFEST,
             "{\"bytes\":0,\"files\":0,\"phase\":\"manifest\"}"},
        {65536, 3, NH_PORTHOME_FETCH_PHASE_CHUNK,
             "{\"bytes\":65536,\"files\":3,\"phase\":\"chunk\"}"},
        {1048576, 42, NH_PORTHOME_FETCH_PHASE_DECODE,
             "{\"bytes\":1048576,\"files\":42,\"phase\":\"decode\"}"},
        {1u<<30, 100, NH_PORTHOME_FETCH_PHASE_DONE,
             "{\"bytes\":1073741824,\"files\":100,\"phase\":\"done\"}"},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        char buf[128];
        nh_porthome_fetch_progress p = { .bytes = cases[i].b,
                                         .files = cases[i].f,
                                         .phase = cases[i].ph };
        int n = nh_porthome_fetch_progress_format(buf, sizeof buf, &p);
        OK(n > 0);
        OK(strcmp(buf, cases[i].want) == 0);
        nh_porthome_fetch_progress round = {0};
        nh_porthome_fetch_progress_status s =
            nh_porthome_fetch_progress_parse(buf, (size_t)n, &round);
        OK(s == NH_PORTHOME_FETCH_PROG_OK);
        OK(round.bytes == cases[i].b);
        OK(round.files == cases[i].f);
        OK(round.phase == cases[i].ph);
    }
}

static void test_progress_reject(void) {
    /* Extra key. */
    const char *bad = "{\"bytes\":1,\"files\":1,\"phase\":\"chunk\",\"extra\":1}";
    nh_porthome_fetch_progress p = {0};
    OK(nh_porthome_fetch_progress_parse(bad, strlen(bad), &p) !=
       NH_PORTHOME_FETCH_PROG_OK);

    /* Unknown phase. */
    const char *bad2 = "{\"bytes\":1,\"files\":1,\"phase\":\"bogus\"}";
    OK(nh_porthome_fetch_progress_parse(bad2, strlen(bad2), &p) !=
       NH_PORTHOME_FETCH_PROG_OK);

    /* Negative bytes (we only accept unsigned). */
    const char *bad3 = "{\"bytes\":-1,\"files\":1,\"phase\":\"chunk\"}";
    OK(nh_porthome_fetch_progress_parse(bad3, strlen(bad3), &p) !=
       NH_PORTHOME_FETCH_PROG_OK);

    /* Missing field. */
    const char *bad4 = "{\"bytes\":1,\"phase\":\"chunk\"}";
    OK(nh_porthome_fetch_progress_parse(bad4, strlen(bad4), &p) !=
       NH_PORTHOME_FETCH_PROG_OK);

    /* Over-long line. */
    char big[NH_PORTHOME_FETCH_MAX_PROGRESS_LINE + 1];
    memset(big, ' ', sizeof big);
    OK(nh_porthome_fetch_progress_parse(big, sizeof big, &p) ==
       NH_PORTHOME_FETCH_PROG_TOO_LONG);
}

int main(void) {
    test_good();
    test_missing_field();
    test_bad_hex();
    test_unknown_key();
    test_bad_url();
    test_allow_insecure();
    test_too_large();
    test_no_escapes();
    test_progress_roundtrip();
    test_progress_reject();
    if (failures) {
        fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    fprintf(stdout, "test_porthome_fetch_ctl: OK\n");
    return 0;
}
