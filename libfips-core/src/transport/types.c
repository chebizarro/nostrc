#include <fips/transport/types.h>
#include <sodium/crypto_shorthash_siphash24.h>
#include <string.h>

_Static_assert(sizeof(float) == 4, "LinkStats loss_rate must match Rust f32");

static const char hex[] = "0123456789abcdef";

static size_t write_uint(char *buf, uint64_t value)
{
    char digits[20];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (size_t i = 0; i < n; ++i)
        buf[i] = digits[n - i - 1];
    return n;
}

static fips_status copy_text(const char *text, size_t n, char *buf,
                             size_t capacity, size_t *len)
{
    if (capacity <= n)
        return FIPS_ERR_RANGE;
    memcpy(buf, text, n);
    buf[n] = '\0';
    *len = n;
    return FIPS_OK;
}

fips_transport_id fips_transport_id_new(uint32_t id)
{
    return (fips_transport_id){id};
}

uint32_t fips_transport_id_as_u32(fips_transport_id id) { return id.value; }
bool fips_transport_id_equal(fips_transport_id a, fips_transport_id b)
{
    return a.value == b.value;
}

fips_status fips_transport_id_format(fips_transport_id id, char *buf,
                                      size_t capacity, size_t *len)
{
    char text[30] = "transport:";
    size_t n = 10 + write_uint(text + 10, id.value);
    return copy_text(text, n, buf, capacity, len);
}

fips_link_id fips_link_id_new(uint64_t id) { return (fips_link_id){id}; }
uint64_t fips_link_id_as_u64(fips_link_id id) { return id.value; }
bool fips_link_id_equal(fips_link_id a, fips_link_id b) { return a.value == b.value; }

fips_status fips_link_id_format(fips_link_id id, char *buf,
                               size_t capacity, size_t *len)
{
    char text[25] = "link:";
    size_t n = 5 + write_uint(text + 5, id.value);
    return copy_text(text, n, buf, capacity, len);
}

fips_status fips_link_direction_format(fips_link_direction direction, char *buf,
                                       size_t capacity, size_t *len)
{
    switch (direction) {
    case FIPS_LINK_OUTBOUND: return copy_text("outbound", 8, buf, capacity, len);
    case FIPS_LINK_INBOUND: return copy_text("inbound", 7, buf, capacity, len);
    default: return FIPS_ERR_RANGE;
    }
}

/* Reject overlong sequences, surrogates, truncation and scalars above U+10FFFF,
 * just as core::str::from_utf8; no locale or platform conversion is involved. */
static bool valid_utf8(const uint8_t *bytes, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = bytes[i++];
        if (c < 0x80)
            continue;
        size_t tail;
        uint32_t scalar, minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            tail = 1; scalar = c & 0x1f; minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            tail = 2; scalar = c & 0x0f; minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            tail = 3; scalar = c & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (tail > len - i)
            return false;
        for (size_t j = 0; j < tail; ++j) {
            c = bytes[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            scalar = (scalar << 6) | (c & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
    }
    return true;
}

fips_status fips_transport_addr_new(const uint8_t *bytes, size_t len,
                                    fips_transport_addr *out)
{
    return fips_transport_addr_from_bytes(bytes, len, out);
}

fips_status fips_transport_addr_from_bytes(const uint8_t *bytes, size_t len,
                                           fips_transport_addr *out)
{
    if (len > FIPS_TRANSPORT_ADDR_CAPACITY)
        return FIPS_ERR_RANGE;
    fips_transport_addr addr = {0};
    if (len != 0)
        memcpy(addr.bytes, bytes, len);
    addr.len = (uint16_t)len;
    *out = addr;
    return FIPS_OK;
}

fips_status fips_transport_addr_from_string(const char *text, size_t len,
                                            fips_transport_addr *out)
{
    if (len > FIPS_TRANSPORT_ADDR_CAPACITY || !valid_utf8((const uint8_t *)text, len))
        return FIPS_ERR_RANGE;
    return fips_transport_addr_from_bytes((const uint8_t *)text, len, out);
}

fips_transport_addr fips_transport_addr_from_mac(const uint8_t mac[6])
{
    fips_transport_addr addr = {0};
    memcpy(addr.bytes, mac, 6);
    addr.len = 6;
    addr.mac = true;
    return addr;
}

static size_t write_ipv4(char *buf, const uint8_t *ip)
{
    size_t n = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (i != 0)
            buf[n++] = '.';
        n += write_uint(buf + n, ip[i]);
    }
    return n;
}

static size_t write_ipv6(char *buf, const uint8_t *ip)
{
    static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(ip, mapped_prefix, sizeof(mapped_prefix)) == 0) {
        memcpy(buf, "::ffff:", 7);
        return 7 + write_ipv4(buf + 7, ip + 12);
    }
    uint16_t words[8];
    size_t best = 8, longest = 1;
    for (size_t i = 0; i < 8; ++i)
        words[i] = (uint16_t)((uint16_t)ip[i * 2] << 8 | ip[i * 2 + 1]);
    for (size_t i = 0; i < 8;) {
        if (words[i] != 0) {
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < 8 && words[end] == 0)
            ++end;
        if (end - i > longest) {
            best = i;
            longest = end - i;
        }
        i = end;
    }
    size_t n = 0;
    for (size_t i = 0; i < 8;) {
        if (i == best) {
            buf[n++] = ':';
            buf[n++] = ':';
            i += longest;
        } else {
            if (n != 0 && buf[n - 1] != ':')
                buf[n++] = ':';
            unsigned int shift = 12;
            while (shift != 0 && ((words[i] >> shift) & 0xf) == 0)
                shift -= 4;
            do {
                buf[n++] = hex[(words[i] >> shift) & 0xf];
                if (shift == 0)
                    break;
                shift -= 4;
            } while (true);
            ++i;
        }
    }
    return n;
}

fips_status fips_transport_addr_from_socket_addr(const uint8_t *ip, size_t ip_len,
    uint16_t port, uint32_t scope_id, fips_transport_addr *out)
{
    char text[64];
    size_t n = 0;
    if (ip_len == 4) {
        n = write_ipv4(text, ip);
    } else if (ip_len == 16) {
        text[n++] = '[';
        n += write_ipv6(text + n, ip);
        if (scope_id != 0) {
            text[n++] = '%';
            n += write_uint(text + n, scope_id);
        }
        text[n++] = ']';
    } else {
        return FIPS_ERR_RANGE;
    }
    text[n++] = ':';
    n += write_uint(text + n, port);
    return fips_transport_addr_from_string(text, n, out);
}

const uint8_t *fips_transport_addr_as_bytes(const fips_transport_addr *addr)
{
    return addr->bytes;
}

fips_status fips_transport_addr_as_str(const fips_transport_addr *addr,
                                       const char **text, size_t *len)
{
    if (!valid_utf8(addr->bytes, addr->len))
        return FIPS_ERR_RANGE;
    *text = (const char *)addr->bytes;
    *len = addr->len;
    return FIPS_OK;
}

size_t fips_transport_addr_len(const fips_transport_addr *addr) { return addr->len; }
bool fips_transport_addr_is_empty(const fips_transport_addr *addr) { return addr->len == 0; }
bool fips_transport_addr_equal(const fips_transport_addr *a, const fips_transport_addr *b)
{
    return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static size_t write_mac(char *buf, const fips_transport_addr *addr)
{
    size_t n = 0;
    for (size_t i = 0; i < addr->len; ++i) {
        if (i != 0)
            buf[n++] = ':';
        buf[n++] = hex[addr->bytes[i] >> 4];
        buf[n++] = hex[addr->bytes[i] & 0xf];
    }
    return n;
}

fips_status fips_transport_addr_format(const fips_transport_addr *addr,
                                       char *buf, size_t capacity, size_t *len)
{
    char text[FIPS_TRANSPORT_ADDR_CAPACITY * 2];
    if (addr->mac || (!valid_utf8(addr->bytes, addr->len) && addr->len == 6)) {
        size_t n = write_mac(text, addr);
        return copy_text(text, n, buf, capacity, len);
    }
    if (valid_utf8(addr->bytes, addr->len))
        return copy_text((const char *)addr->bytes, addr->len, buf, capacity, len);
    for (size_t i = 0; i < addr->len; ++i) {
        text[i * 2] = hex[addr->bytes[i] >> 4];
        text[i * 2 + 1] = hex[addr->bytes[i] & 0xf];
    }
    return copy_text(text, addr->len * 2, buf, capacity, len);
}

fips_status fips_transport_addr_debug(const fips_transport_addr *addr,
                                      char *buf, size_t capacity, size_t *len)
{
    char text[16 + FIPS_TRANSPORT_ADDR_CAPACITY * 5] = "TransportAddr(";
    size_t n = 14;
    if (addr->mac) {
        n += write_mac(text + n, addr);
    } else if (valid_utf8(addr->bytes, addr->len)) {
        text[n++] = '"';
        memcpy(text + n, addr->bytes, addr->len);
        n += addr->len;
        text[n++] = '"';
    } else {
        text[n++] = '[';
        for (size_t i = 0; i < addr->len; ++i) {
            if (i != 0) {
                text[n++] = ',';
                text[n++] = ' ';
            }
            n += write_uint(text + n, addr->bytes[i]);
        }
        text[n++] = ']';
    }
    text[n++] = ')';
    return copy_text(text, n, buf, capacity, len);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

fips_status fips_transport_addr_parse(const char *text, size_t len,
    fips_transport_addr_parse_mode mode, fips_transport_addr *out)
{
    if (mode == FIPS_TRANSPORT_ADDR_PARSE_TEXT)
        return fips_transport_addr_from_string(text, len, out);
    bool mac = mode == FIPS_TRANSPORT_ADDR_PARSE_MAC;
    if (mac ? len != 17 : mode != FIPS_TRANSPORT_ADDR_PARSE_HEX ||
                         len % 2 != 0 || len / 2 > FIPS_TRANSPORT_ADDR_CAPACITY)
        return FIPS_ERR_RANGE;
    fips_transport_addr addr = {0};
    addr.len = mac ? 6 : (uint16_t)(len / 2);
    addr.mac = mac;
    for (size_t i = 0; i < addr.len; ++i) {
        size_t offset = i * (mac ? 3 : 2);
        int hi = hex_digit(text[offset]), lo = hex_digit(text[offset + 1]);
        if (hi < 0 || lo < 0 || (mac && i != 5 && text[offset + 2] != ':'))
            return FIPS_ERR_RANGE;
        addr.bytes[i] = (uint8_t)(hi * 16 + lo);
    }
    *out = addr;
    return FIPS_OK;
}

static uint64_t hash_bytes(const uint8_t *bytes, size_t len, const uint8_t *key)
{
    uint8_t digest[crypto_shorthash_siphash24_BYTES];
    /* SipHash has no fallible inputs for these fixed key/output sizes. */
    crypto_shorthash_siphash24(digest, bytes, (unsigned long long)len, key);
    uint64_t hash = 0;
    for (size_t i = 0; i < sizeof(digest); ++i)
        hash |= (uint64_t)digest[i] << (i * 8);
    return hash;
}

static uint64_t hash_id(uint64_t id, size_t len, const uint8_t *key)
{
    uint8_t bytes[8];
    for (size_t i = 0; i < len; ++i)
        bytes[i] = (uint8_t)(id >> (i * 8));
    return hash_bytes(bytes, len, key);
}

uint64_t fips_transport_id_hash(fips_transport_id id, const uint8_t key[16])
{
    return hash_id(id.value, 4, key);
}

uint64_t fips_link_id_hash(fips_link_id id, const uint8_t key[16])
{
    return hash_id(id.value, 8, key);
}

uint64_t fips_transport_addr_hash(const fips_transport_addr *addr, const uint8_t key[16])
{
    return hash_bytes(addr->bytes, addr->len, key);
}

fips_link_stats fips_link_stats_new(void) { return (fips_link_stats){0}; }

void fips_link_stats_record_sent(fips_link_stats *stats, size_t bytes)
{
    stats->packets_sent += 1;
    stats->bytes_sent += (uint64_t)bytes;
}

void fips_link_stats_record_recv(fips_link_stats *stats, size_t bytes, uint64_t timestamp_ms)
{
    stats->packets_recv += 1;
    stats->bytes_recv += (uint64_t)bytes;
    stats->last_recv_ms = timestamp_ms;
}

bool fips_link_stats_rtt_estimate(const fips_link_stats *stats, fips_duration *out)
{
    if (stats->rtt_estimate == 0)
        return false;
    *out = fips_duration_from_millis(stats->rtt_estimate);
    return true;
}

void fips_link_stats_update_rtt(fips_link_stats *stats, fips_duration rtt)
{
    uint64_t ms = rtt.secs * UINT64_C(1000) + rtt.nanos / 1000000;
    if (stats->rtt_estimate == 0) {
        stats->rtt_estimate = ms;
    } else {
        double fresh = 0.2 * (double)ms;
        double previous = 0.8 * (double)stats->rtt_estimate;
        double estimate = fresh + previous;
        /* Rust float-to-u64 casts saturate; a C out-of-range cast is UB. */
        stats->rtt_estimate = estimate >= 0x1p64 ? UINT64_MAX : (uint64_t)estimate;
    }
}

uint64_t fips_link_stats_time_since_recv(const fips_link_stats *stats, uint64_t current_time_ms)
{
    if (stats->last_recv_ms == 0)
        return UINT64_MAX;
    return current_time_ms < stats->last_recv_ms ? 0 : current_time_ms - stats->last_recv_ms;
}

void fips_link_stats_reset(fips_link_stats *stats) { *stats = fips_link_stats_new(); }
