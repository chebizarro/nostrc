#include <fips/transport/types.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static const uint8_t hash_key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static int check_format(const fips_transport_addr *addr, const char *expected)
{
    char text[FIPS_TRANSPORT_ADDR_CAPACITY * 2 + 1];
    size_t len = SIZE_MAX;
    CHECK(fips_transport_addr_format(addr, text, sizeof(text), &len) == FIPS_OK);
    CHECK(len == strlen(expected));
    CHECK(memcmp(text, expected, len + 1) == 0);
    return 0;
}

/* Ports the two core/src/transport/tests/types.rs cases. The Rust Arc pointer
 * sharing assertion becomes independent inline storage: C POD assignment needs
 * no shared allocation/refcount/lifetime, while retaining identical values. */
static int transport_addr_clone_shares_immutable_bytes(void)
{
    fips_transport_addr addr;
    CHECK(fips_transport_addr_from_string("192.168.1.1:2121", 16, &addr) == FIPS_OK);
    fips_transport_addr cloned = addr;
    CHECK(sizeof(addr) <= 264);
    CHECK(fips_transport_addr_equal(&addr, &cloned));
    CHECK(fips_transport_addr_as_bytes(&addr) != fips_transport_addr_as_bytes(&cloned));
    memset(&addr, 0, sizeof(addr));
    CHECK(check_format(&cloned, "192.168.1.1:2121") == 0);
    return 0;
}

static int transport_addr_equality_and_hash_use_byte_values(void)
{
    fips_transport_addr original, same_value, different;
    const char *text = "192.168.1.1:2121";
    CHECK(fips_transport_addr_from_string(text, strlen(text), &original) == FIPS_OK);
    CHECK(fips_transport_addr_from_bytes((const uint8_t *)text, strlen(text), &same_value) == FIPS_OK);
    CHECK(fips_transport_addr_as_bytes(&original) != fips_transport_addr_as_bytes(&same_value));
    CHECK(fips_transport_addr_equal(&original, &same_value));
    CHECK(fips_transport_addr_hash(&original, hash_key) == fips_transport_addr_hash(&same_value, hash_key));
    same_value.bytes[FIPS_TRANSPORT_ADDR_CAPACITY - 1] = 0x80; /* Ignore unused storage. */
    CHECK(fips_transport_addr_equal(&original, &same_value));
    CHECK(fips_transport_addr_hash(&original, hash_key) == fips_transport_addr_hash(&same_value, hash_key));
    CHECK(fips_transport_addr_from_string("different", 9, &different) == FIPS_OK);
    CHECK(!fips_transport_addr_equal(&original, &different));
    same_value.len--;
    CHECK(!fips_transport_addr_equal(&original, &same_value));
    return 0;
}

/* The following fourteen cases port value-type tests that remain in the root
 * crate's src/transport/mod.rs. Host transport/link state tests are not core. */
static int test_transport_id(void)
{
    fips_transport_id id = fips_transport_id_new(42);
    char text[32]; size_t len;
    CHECK(fips_transport_id_as_u32(id) == 42);
    CHECK(fips_transport_id_format(id, text, sizeof(text), &len) == FIPS_OK);
    CHECK(len == 12 && strcmp(text, "transport:42") == 0);
    CHECK(fips_transport_id_equal(id, fips_transport_id_new(42)));
    CHECK(!fips_transport_id_equal(id, fips_transport_id_new(43)));
    CHECK(fips_transport_id_hash(id, hash_key) == fips_transport_id_hash(fips_transport_id_new(42), hash_key));
    return 0;
}

static int test_link_id(void)
{
    fips_link_id id = fips_link_id_new(12345);
    char text[32]; size_t len;
    CHECK(fips_link_id_as_u64(id) == 12345);
    CHECK(fips_link_id_format(id, text, sizeof(text), &len) == FIPS_OK);
    CHECK(len == 10 && strcmp(text, "link:12345") == 0);
    CHECK(fips_link_id_equal(id, fips_link_id_new(12345)));
    CHECK(!fips_link_id_equal(id, fips_link_id_new(12346)));
    CHECK(fips_link_id_hash(id, hash_key) == fips_link_id_hash(fips_link_id_new(12345), hash_key));
    return 0;
}

static int test_transport_addr_string(void)
{
    fips_transport_addr addr;
    const char *text; size_t len;
    CHECK(fips_transport_addr_from_string("192.168.1.1:2121", 16, &addr) == FIPS_OK);
    CHECK(check_format(&addr, "192.168.1.1:2121") == 0);
    CHECK(fips_transport_addr_as_str(&addr, &text, &len) == FIPS_OK);
    CHECK(len == 16 && memcmp(text, "192.168.1.1:2121", len) == 0);
    return 0;
}

static int test_transport_addr_binary(void)
{
    const uint8_t bytes[] = {0xff, 0x80, 0x2b, 0x3c, 0x4d, 0x5e};
    fips_transport_addr addr;
    const char *text = "unchanged"; size_t len = 99;
    CHECK(fips_transport_addr_new(bytes, sizeof(bytes), &addr) == FIPS_OK);
    CHECK(check_format(&addr, "ff:80:2b:3c:4d:5e") == 0);
    CHECK(fips_transport_addr_as_str(&addr, &text, &len) == FIPS_ERR_RANGE);
    CHECK(strcmp(text, "unchanged") == 0 && len == 99);
    CHECK(fips_transport_addr_len(&addr) == 6);
    return 0;
}

static int test_transport_addr_mac_display(void)
{
    const uint8_t bytes[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    fips_transport_addr addr;
    CHECK(fips_transport_addr_from_bytes(bytes, sizeof(bytes), &addr) == FIPS_OK);
    CHECK(check_format(&addr, "aa:bb:cc:dd:ee:ff") == 0);
    return 0;
}

static int a_mac_address_whose_bytes_are_valid_utf8_displays_as_a_mac(void)
{
    const uint8_t bytes[] = {0x32, 0x7c, 0xd1, 0xac, 0x5a, 0x64};
    fips_transport_addr mac = fips_transport_addr_from_mac(bytes);
    const char *text; size_t len;
    CHECK(fips_transport_addr_as_str(&mac, &text, &len) == FIPS_OK);
    CHECK(len == sizeof(bytes) && memcmp(text, bytes, len) == 0);
    CHECK(check_format(&mac, "32:7c:d1:ac:5a:64") == 0);
    char debug[64];
    CHECK(fips_transport_addr_debug(&mac, debug, sizeof(debug), &len) == FIPS_OK);
    CHECK(strcmp(debug, "TransportAddr(32:7c:d1:ac:5a:64)") == 0);
    CHECK(memcmp(fips_transport_addr_as_bytes(&mac), bytes, sizeof(bytes)) == 0);
    return 0;
}

static int a_mac_address_equals_and_hashes_as_its_bytes(void)
{
    const uint8_t bytes[] = {0x32, 0x7c, 0xd1, 0xac, 0x5a, 0x64};
    fips_transport_addr mac = fips_transport_addr_from_mac(bytes), raw;
    CHECK(fips_transport_addr_from_bytes(bytes, sizeof(bytes), &raw) == FIPS_OK);
    CHECK(fips_transport_addr_equal(&mac, &raw));
    CHECK(fips_transport_addr_hash(&mac, hash_key) == fips_transport_addr_hash(&raw, hash_key));
    return 0;
}

static int test_transport_addr_non_mac_binary_is_bare_hex(void)
{
    const uint8_t bytes[] = {0xff, 0x80, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f};
    fips_transport_addr addr;
    CHECK(fips_transport_addr_new(bytes, 3, &addr) == FIPS_OK);
    CHECK(check_format(&addr, "ff802b") == 0);
    CHECK(fips_transport_addr_new(bytes, 7, &addr) == FIPS_OK);
    CHECK(check_format(&addr, "ff802b3c4d5e6f") == 0);
    return 0;
}

static int test_transport_addr_from_string(void)
{
    fips_transport_addr addr, addr2;
    const char *text; size_t len;
    CHECK(fips_transport_addr_from_string("test:1234", 9, &addr) == FIPS_OK);
    CHECK(fips_transport_addr_as_str(&addr, &text, &len) == FIPS_OK);
    CHECK(len == 9 && memcmp(text, "test:1234", len) == 0);
    CHECK(fips_transport_addr_from_string("hello", 5, &addr2) == FIPS_OK);
    CHECK(fips_transport_addr_as_str(&addr2, &text, &len) == FIPS_OK);
    CHECK(len == 5 && memcmp(text, "hello", len) == 0);
    return 0;
}

static int test_transport_addr_from_socket_addr(void)
{
    static const struct {
        uint8_t ip[16]; size_t ip_len; uint16_t port; uint32_t scope;
        const char *text;
    } cases[] = {
        {{0}, 4, 0, 0, "0.0.0.0:0"},
        {{127, 0, 0, 1}, 4, 2121, 0, "127.0.0.1:2121"},
        {{255, 255, 255, 255}, 4, 65535, 0, "255.255.255.255:65535"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 16, 2121, 0, "[::1]:2121"},
        {{0x20, 1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 16, 2121, 0, "[2001:db8::1]:2121"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 255, 255}, 16, 65535, 0, "[::ffff:255.255.255.255]:65535"},
        {{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 16, 65535, UINT32_MAX, "[fe80::1%4294967295]:65535"},
        {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, 16, 65535, UINT32_MAX,
         "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295]:65535"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fips_transport_addr addr, parsed;
        const char *text; size_t len;
        CHECK(fips_transport_addr_from_socket_addr(cases[i].ip, cases[i].ip_len,
              cases[i].port, cases[i].scope, &addr) == FIPS_OK);
        CHECK(check_format(&addr, cases[i].text) == 0);
        CHECK(fips_transport_addr_as_str(&addr, &text, &len) == FIPS_OK);
        CHECK(len == strlen(cases[i].text) && memcmp(text, cases[i].text, len) == 0);
        CHECK(fips_transport_addr_parse(text, len, FIPS_TRANSPORT_ADDR_PARSE_TEXT, &parsed) == FIPS_OK);
        CHECK(fips_transport_addr_equal(&addr, &parsed));
    }
    return 0;
}

static int test_link_stats_basic(void)
{
    fips_link_stats stats = fips_link_stats_new();
    CHECK(stats.loss_rate == 0.0f && stats.throughput_estimate == 0);
    fips_link_stats_record_sent(&stats, 100);
    fips_link_stats_record_recv(&stats, 200, 1000);
    CHECK(stats.packets_sent == 1 && stats.bytes_sent == 100);
    CHECK(stats.packets_recv == 1 && stats.bytes_recv == 200 && stats.last_recv_ms == 1000);
    return 0;
}

static int test_link_stats_rtt(void)
{
    fips_link_stats stats = fips_link_stats_new();
    fips_duration rtt = {123, 456};
    CHECK(!fips_link_stats_rtt_estimate(&stats, &rtt));
    CHECK(rtt.secs == 123 && rtt.nanos == 456);
    fips_link_stats_update_rtt(&stats, fips_duration_from_millis(100));
    CHECK(fips_link_stats_rtt_estimate(&stats, &rtt));
    CHECK(rtt.secs == 0 && rtt.nanos == 100000000);
    fips_link_stats_update_rtt(&stats, fips_duration_from_millis(200));
    CHECK(fips_link_stats_rtt_estimate(&stats, &rtt));
    CHECK(rtt.secs == 0 && rtt.nanos == 120000000); /* Exact EMA, stronger than Rust's 110..130. */
    return 0;
}

static int test_link_stats_time_since_recv(void)
{
    fips_link_stats stats = fips_link_stats_new();
    CHECK(fips_link_stats_time_since_recv(&stats, 1000) == UINT64_MAX);
    fips_link_stats_record_recv(&stats, 100, 500);
    CHECK(fips_link_stats_time_since_recv(&stats, 1000) == 500);
    CHECK(fips_link_stats_time_since_recv(&stats, 500) == 0);
    CHECK(fips_link_stats_time_since_recv(&stats, 499) == 0);
    return 0;
}

static int test_link_direction_display(void)
{
    char text[16]; size_t len;
    CHECK(fips_link_direction_format(FIPS_LINK_OUTBOUND, text, sizeof(text), &len) == FIPS_OK);
    CHECK(len == 8 && strcmp(text, "outbound") == 0);
    CHECK(fips_link_direction_format(FIPS_LINK_INBOUND, text, sizeof(text), &len) == FIPS_OK);
    CHECK(len == 7 && strcmp(text, "inbound") == 0);
    return 0;
}

static int test_text_round_trips(void)
{
    /* Protocol text is deliberately opaque: this core neither resolves hosts
     * nor validates per-transport syntax. All are just UTF-8 byte addresses. */
    const char *cases[] = {
        "192.0.2.1:2121", "[2001:db8::1]:2121", "fips.example:2121",
        "abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwxyz.onion:2121",
        "recipient.identity@gateway", "12:34:56:78:9a:bc", "ff", "", "hello \xf0\x9f\x8c\x8d"
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fips_transport_addr addr, parsed;
        char text[513]; size_t len;
        CHECK(fips_transport_addr_from_string(cases[i], strlen(cases[i]), &addr) == FIPS_OK);
        CHECK(fips_transport_addr_format(&addr, text, sizeof(text), &len) == FIPS_OK);
        CHECK(fips_transport_addr_parse(text, len, FIPS_TRANSPORT_ADDR_PARSE_TEXT, &parsed) == FIPS_OK);
        CHECK(fips_transport_addr_equal(&addr, &parsed));
        CHECK(!parsed.mac);
        CHECK(check_format(&parsed, cases[i]) == 0);
    }
    fips_transport_addr addr, parsed;
    const char embedded_nul[] = {'a', '\0', 'b'};
    char text[8]; size_t len;
    CHECK(fips_transport_addr_from_string(embedded_nul, sizeof(embedded_nul), &addr) == FIPS_OK);
    CHECK(fips_transport_addr_format(&addr, text, sizeof(text), &len) == FIPS_OK);
    CHECK(len == sizeof(embedded_nul) && memcmp(text, embedded_nul, len) == 0 && text[len] == 0);
    CHECK(fips_transport_addr_parse(text, len, FIPS_TRANSPORT_ADDR_PARSE_TEXT, &parsed) == FIPS_OK);
    CHECK(fips_transport_addr_equal(&addr, &parsed));
    return 0;
}

static int test_mac_round_trips(void)
{
    const uint8_t cases[][6] = {
        {0}, {255, 255, 255, 255, 255, 255}, {'a', 'b', 'c', 'd', 'e', 'f'},
        {0x32, 0x7c, 0xd1, 0xac, 0x5a, 0x64}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fips_transport_addr addr = fips_transport_addr_from_mac(cases[i]), parsed;
        char text[18]; size_t len;
        CHECK(fips_transport_addr_format(&addr, text, sizeof(text), &len) == FIPS_OK);
        CHECK(fips_transport_addr_parse(text, len, FIPS_TRANSPORT_ADDR_PARSE_MAC, &parsed) == FIPS_OK);
        CHECK(parsed.mac && fips_transport_addr_equal(&addr, &parsed));
        CHECK(check_format(&parsed, text) == 0);
    }
    fips_transport_addr addr;
    CHECK(fips_transport_addr_parse("AA:BB:CC:DD:EE:FF", 17, FIPS_TRANSPORT_ADDR_PARSE_MAC, &addr) == FIPS_OK);
    CHECK(check_format(&addr, "aa:bb:cc:dd:ee:ff") == 0);
    return 0;
}

static int test_hex_round_trips(void)
{
    uint8_t bytes[256];
    memset(bytes, 0xff, sizeof(bytes));
    for (size_t n = 0; n <= sizeof(bytes); ++n) {
        fips_transport_addr addr, parsed;
        char text[513]; size_t len;
        CHECK(fips_transport_addr_from_bytes(bytes, n, &addr) == FIPS_OK);
        CHECK(fips_transport_addr_format(&addr, text, sizeof(text), &len) == FIPS_OK);
        fips_transport_addr_parse_mode mode = n == 6 ? FIPS_TRANSPORT_ADDR_PARSE_MAC : FIPS_TRANSPORT_ADDR_PARSE_HEX;
        CHECK(fips_transport_addr_parse(text, len, mode, &parsed) == FIPS_OK);
        CHECK(fips_transport_addr_equal(&addr, &parsed));
        CHECK(check_format(&parsed, text) == 0);
    }
    fips_transport_addr text, binary;
    CHECK(fips_transport_addr_parse("ff", 2, FIPS_TRANSPORT_ADDR_PARSE_TEXT, &text) == FIPS_OK);
    CHECK(fips_transport_addr_parse("FF", 2, FIPS_TRANSPORT_ADDR_PARSE_HEX, &binary) == FIPS_OK);
    CHECK(!fips_transport_addr_equal(&text, &binary));
    CHECK(fips_transport_addr_parse("616263", 6, FIPS_TRANSPORT_ADDR_PARSE_HEX, &binary) == FIPS_OK);
    CHECK(check_format(&binary, "abc") == 0); /* No invented raw/hex display tag. */
    return 0;
}

static int test_malformed_text(void)
{
    const char *bad_mac[] = {"", "aa:bb:cc:dd:ee", "aa:bb:cc:dd:ee:ff:", "a:bb:cc:dd:ee:ff",
        "aa-bb-cc-dd-ee-ff", "ag:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ee:fg", "aa:bb:cc:dd:ee:ff "};
    const char *bad_hex[] = {"f", "abc", "0xff", "ff ff", "ff:00", "gg", "ff\n"};
    fips_transport_addr addr = fips_transport_addr_from_mac((const uint8_t *)"abcdef");
    fips_transport_addr saved = addr;
    for (size_t i = 0; i < sizeof(bad_mac) / sizeof(bad_mac[0]); ++i) {
        CHECK(fips_transport_addr_parse(bad_mac[i], strlen(bad_mac[i]), FIPS_TRANSPORT_ADDR_PARSE_MAC, &addr) == FIPS_ERR_RANGE);
        CHECK(addr.mac == saved.mac && fips_transport_addr_equal(&addr, &saved));
    }
    for (size_t i = 0; i < sizeof(bad_hex) / sizeof(bad_hex[0]); ++i) {
        CHECK(fips_transport_addr_parse(bad_hex[i], strlen(bad_hex[i]), FIPS_TRANSPORT_ADDR_PARSE_HEX, &addr) == FIPS_ERR_RANGE);
        CHECK(addr.mac == saved.mac && fips_transport_addr_equal(&addr, &saved));
    }
    CHECK(fips_transport_addr_parse("", 0, (fips_transport_addr_parse_mode)99, &addr) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_equal(&addr, &saved) && addr.mac);
    const char nul_mac[] = "aa:bb:cc:dd:ee:ff";
    char bad[17]; memcpy(bad, nul_mac, sizeof(bad)); bad[16] = 0;
    CHECK(fips_transport_addr_parse(bad, sizeof(bad), FIPS_TRANSPORT_ADDR_PARSE_MAC, &addr) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_equal(&addr, &saved) && addr.mac);
    return 0;
}

static int test_utf8_validation(void)
{
    static const struct { const char *text; size_t len; } invalid[] = {
        {"\x80", 1}, {"\xc0\x80", 2}, {"\xc1\xbf", 2}, {"\xc2", 1}, {"\xc2\x20", 2},
        {"\xe0\x80\x80", 3}, {"\xed\xa0\x80", 3}, {"\xe1\x80", 2},
        {"\xf0\x80\x80\x80", 4}, {"\xf4\x90\x80\x80", 4}, {"\xf5\x80\x80\x80", 4},
        {"\xff", 1}, {"\xf0\x90\x80", 3}
    };
    fips_transport_addr saved;
    CHECK(fips_transport_addr_from_string("saved", 5, &saved) == FIPS_OK);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        fips_transport_addr addr = saved;
        CHECK(fips_transport_addr_from_string(invalid[i].text, invalid[i].len, &addr) == FIPS_ERR_RANGE);
        CHECK(fips_transport_addr_equal(&addr, &saved));
        CHECK(fips_transport_addr_parse(invalid[i].text, invalid[i].len, FIPS_TRANSPORT_ADDR_PARSE_TEXT, &addr) == FIPS_ERR_RANGE);
        CHECK(fips_transport_addr_equal(&addr, &saved));
        CHECK(fips_transport_addr_from_bytes((const uint8_t *)invalid[i].text, invalid[i].len, &addr) == FIPS_OK);
        const char *text = "saved"; size_t len = 99;
        CHECK(fips_transport_addr_as_str(&addr, &text, &len) == FIPS_ERR_RANGE);
        CHECK(strcmp(text, "saved") == 0 && len == 99);
    }
    const char valid[] = "\0\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf";
    fips_transport_addr addr;
    CHECK(fips_transport_addr_from_string(valid, sizeof(valid) - 1, &addr) == FIPS_OK);
    const char *text; size_t len;
    CHECK(fips_transport_addr_as_str(&addr, &text, &len) == FIPS_OK);
    CHECK(len == sizeof(valid) - 1 && memcmp(text, valid, len) == 0);
    return 0;
}

static int test_capacity_and_failure_outputs(void)
{
    char bytes[257]; memset(bytes, 'a', sizeof(bytes));
    fips_transport_addr addr, saved;
    CHECK(fips_transport_addr_from_string(bytes, 256, &addr) == FIPS_OK);
    CHECK(fips_transport_addr_len(&addr) == 256 && !fips_transport_addr_is_empty(&addr));
    saved = addr;
    CHECK(fips_transport_addr_from_bytes((const uint8_t *)bytes, 257, &addr) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_equal(&addr, &saved));
    CHECK(fips_transport_addr_from_string(bytes, SIZE_MAX, &addr) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_equal(&addr, &saved));
    CHECK(fips_transport_addr_parse(bytes, SIZE_MAX - 1, FIPS_TRANSPORT_ADDR_PARSE_HEX, &addr) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_equal(&addr, &saved));
    CHECK(fips_transport_addr_from_socket_addr((const uint8_t *)bytes, 5, 0, 0, &addr) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_equal(&addr, &saved));
    char text[1600], sentinel[1600]; memset(text, 'z', sizeof(text)); memcpy(sentinel, text, sizeof(text));
    size_t len = 777;
    CHECK(fips_transport_addr_format(&addr, text, 256, &len) == FIPS_ERR_RANGE);
    CHECK(len == 777 && memcmp(text, sentinel, sizeof(text)) == 0);
    CHECK(fips_transport_addr_debug(&addr, text, 10, &len) == FIPS_ERR_RANGE);
    CHECK(len == 777 && memcmp(text, sentinel, sizeof(text)) == 0);
    CHECK(fips_transport_id_format(fips_transport_id_new(42), text, 12, &len) == FIPS_ERR_RANGE);
    CHECK(len == 777 && memcmp(text, sentinel, sizeof(text)) == 0);
    CHECK(fips_link_id_format(fips_link_id_new(42), text, 7, &len) == FIPS_ERR_RANGE);
    CHECK(len == 777 && memcmp(text, sentinel, sizeof(text)) == 0);
    CHECK(fips_link_direction_format(FIPS_LINK_INBOUND, text, 7, &len) == FIPS_ERR_RANGE);
    CHECK(len == 777 && memcmp(text, sentinel, sizeof(text)) == 0);
    CHECK(fips_link_direction_format((fips_link_direction)99, text, sizeof(text), &len) == FIPS_ERR_RANGE);
    CHECK(len == 777 && memcmp(text, sentinel, sizeof(text)) == 0);
    CHECK(fips_transport_addr_format(&addr, text, 257, &len) == FIPS_OK && len == 256);
    CHECK(memcmp(text, bytes, len) == 0 && text[len] == 0);
    CHECK(fips_transport_addr_from_bytes(NULL, 0, &addr) == FIPS_OK);
    CHECK(fips_transport_addr_is_empty(&addr) && check_format(&addr, "") == 0);
    CHECK(fips_transport_addr_from_string(NULL, 0, &addr) == FIPS_OK);
    CHECK(fips_transport_addr_parse(NULL, 0, FIPS_TRANSPORT_ADDR_PARSE_HEX, &addr) == FIPS_OK);
    CHECK(fips_transport_addr_format(&addr, text, 0, &len) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_format(&addr, text, 1, &len) == FIPS_OK && len == 0 && text[0] == 0);
    CHECK(fips_transport_id_format(fips_transport_id_new(UINT32_MAX), text, sizeof(text), &len) == FIPS_OK);
    CHECK(strcmp(text, "transport:4294967295") == 0);
    CHECK(fips_link_id_format(fips_link_id_new(UINT64_MAX), text, sizeof(text), &len) == FIPS_OK);
    CHECK(strcmp(text, "link:18446744073709551615") == 0);
    CHECK(fips_transport_id_format(fips_transport_id_new(0), text, sizeof(text), &len) == FIPS_OK);
    CHECK(strcmp(text, "transport:0") == 0);
    CHECK(fips_link_id_format(fips_link_id_new(0), text, sizeof(text), &len) == FIPS_OK);
    CHECK(strcmp(text, "link:0") == 0);
    memset(bytes, 0xff, 256);
    CHECK(fips_transport_addr_from_bytes((const uint8_t *)bytes, 256, &addr) == FIPS_OK);
    CHECK(fips_transport_addr_format(&addr, text, 512, &len) == FIPS_ERR_RANGE);
    CHECK(fips_transport_addr_format(&addr, text, 513, &len) == FIPS_OK && len == 512);
    for (size_t i = 0; i < len; ++i) CHECK(text[i] == 'f');
    CHECK(fips_transport_addr_debug(&addr, text, sizeof(text), &len) == FIPS_OK);
    CHECK(len == 1295 && text[len] == 0);
    CHECK(memcmp(text, "TransportAddr([255, 255", 22) == 0 && strcmp(text + len - 6, " 255])") == 0);
    CHECK(fips_transport_addr_from_string("text", 4, &addr) == FIPS_OK);
    CHECK(fips_transport_addr_debug(&addr, text, sizeof(text), &len) == FIPS_OK);
    CHECK(strcmp(text, "TransportAddr(\"text\")") == 0);
    return 0;
}

static int test_socket_zero_compression(void)
{
    static const struct { uint8_t ip[16]; const char *text; } cases[] = {
        {{0}, "[::]:0"},
        {{0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 3, 0, 4}, "[1::2:0:0:3:4]:0"},
        {{0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3, 0, 4}, "[1:0:2::3:4]:0"},
        {{0, 1, 0, 0, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7}, "[1:0:2:3:4:5:6:7]:0"},
        {{0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 0, 0, 0}, "[1:2:3:4:5:6::]:0"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 0, 2, 1}, "[::c000:201]:0"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fips_transport_addr addr;
        CHECK(fips_transport_addr_from_socket_addr(cases[i].ip, 16, 0, 0, &addr) == FIPS_OK);
        CHECK(check_format(&addr, cases[i].text) == 0);
    }
    return 0;
}

static int test_hash_keys(void)
{
    uint8_t other_key[16] = {0};
    fips_transport_addr empty;
    CHECK(fips_transport_addr_from_bytes(NULL, 0, &empty) == FIPS_OK);
    /* Standard SipHash-2-4 empty-message vector, little-endian output. */
    CHECK(fips_transport_addr_hash(&empty, hash_key) == UINT64_C(0x726fdb47dd0e0e31));
    CHECK(fips_transport_addr_hash(&empty, hash_key) != fips_transport_addr_hash(&empty, other_key));
    CHECK(fips_transport_id_hash(fips_transport_id_new(UINT32_MAX), hash_key) !=
          fips_transport_id_hash(fips_transport_id_new(UINT32_MAX), other_key));
    CHECK(fips_link_id_hash(fips_link_id_new(UINT64_MAX), hash_key) !=
          fips_link_id_hash(fips_link_id_new(UINT64_MAX), other_key));
    return 0;
}

static int test_stats_edges_and_reset(void)
{
    fips_link_stats stats = fips_link_stats_new();
    fips_link_stats_update_rtt(&stats, (fips_duration){0, 999999});
    CHECK(stats.rtt_estimate == 0);
    fips_link_stats_update_rtt(&stats, (fips_duration){1, 999999999});
    CHECK(stats.rtt_estimate == 1999);
    stats.rtt_estimate = UINT64_MAX;
    fips_link_stats_update_rtt(&stats, fips_duration_from_millis(UINT64_MAX));
    CHECK(stats.rtt_estimate == UINT64_MAX); /* Saturating float cast, not C UB. */
    stats.rtt_estimate = 0;
    fips_link_stats_update_rtt(&stats, (fips_duration){UINT64_MAX, 999999999});
    CHECK(stats.rtt_estimate == UINT64_MAX); /* as_millis() as u64 truncates. */
    stats.packets_sent = stats.packets_recv = UINT64_MAX;
    stats.bytes_sent = stats.bytes_recv = UINT64_MAX;
    fips_link_stats_record_sent(&stats, 2);
    fips_link_stats_record_recv(&stats, 3, UINT64_MAX);
    CHECK(stats.packets_sent == 0 && stats.bytes_sent == 1);
    CHECK(stats.packets_recv == 0 && stats.bytes_recv == 2 && stats.last_recv_ms == UINT64_MAX);
    stats.loss_rate = 0.25f;
    stats.throughput_estimate = 12345;
    fips_link_stats copy = stats;
    fips_link_stats_reset(&stats);
    CHECK(stats.packets_sent == 0 && stats.packets_recv == 0 && stats.bytes_sent == 0 && stats.bytes_recv == 0);
    CHECK(stats.last_recv_ms == 0 && stats.rtt_estimate == 0 && stats.loss_rate == 0.0f && stats.throughput_estimate == 0);
    CHECK(copy.loss_rate == 0.25f && copy.throughput_estimate == 12345 && copy.rtt_estimate == UINT64_MAX);
    return 0;
}

int main(void)
{
    int (*const tests[])(void) = {
        transport_addr_clone_shares_immutable_bytes,
        transport_addr_equality_and_hash_use_byte_values,
        test_transport_id, test_link_id, test_transport_addr_string,
        test_transport_addr_binary, test_transport_addr_mac_display,
        a_mac_address_whose_bytes_are_valid_utf8_displays_as_a_mac,
        a_mac_address_equals_and_hashes_as_its_bytes,
        test_transport_addr_non_mac_binary_is_bare_hex, test_transport_addr_from_string,
        test_transport_addr_from_socket_addr, test_link_stats_basic, test_link_stats_rtt,
        test_link_stats_time_since_recv, test_link_direction_display,
        test_text_round_trips, test_mac_round_trips, test_hex_round_trips,
        test_malformed_text, test_utf8_validation, test_capacity_and_failure_outputs,
        test_socket_zero_compression, test_hash_keys, test_stats_edges_and_reset
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
        if (tests[i]() != 0) return 1;
    printf("transport types: %zu tests passed (16 upstream ports, 9 additional)\n",
           sizeof(tests) / sizeof(tests[0]));
    return 0;
}
