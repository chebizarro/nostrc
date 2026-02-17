/*
 * libmarmot - TLS Presentation Language codec tests
 *
 * Tests round-trip serialization/deserialization of all TLS primitives.
 */

#include "mls/mls-internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TEST(name) do { printf("  %-50s", #name); name(); printf("PASS\n"); } while(0)

/* ── Integer round-trip ─────────────────────────────────────────────────── */

static void test_u8_roundtrip(void)
{
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 16) == 0);
    assert(mls_tls_write_u8(&buf, 0) == 0);
    assert(mls_tls_write_u8(&buf, 42) == 0);
    assert(mls_tls_write_u8(&buf, 255) == 0);
    assert(buf.len == 3);

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    uint8_t v;
    assert(mls_tls_read_u8(&r, &v) == 0 && v == 0);
    assert(mls_tls_read_u8(&r, &v) == 0 && v == 42);
    assert(mls_tls_read_u8(&r, &v) == 0 && v == 255);
    assert(mls_tls_reader_done(&r));
    mls_tls_buf_free(&buf);
}

static void test_u16_roundtrip(void)
{
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 16) == 0);
    assert(mls_tls_write_u16(&buf, 0) == 0);
    assert(mls_tls_write_u16(&buf, 0x1234) == 0);
    assert(mls_tls_write_u16(&buf, 0xFFFF) == 0);
    assert(buf.len == 6);

    /* Verify big-endian encoding */
    assert(buf.data[2] == 0x12 && buf.data[3] == 0x34);

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    uint16_t v;
    assert(mls_tls_read_u16(&r, &v) == 0 && v == 0);
    assert(mls_tls_read_u16(&r, &v) == 0 && v == 0x1234);
    assert(mls_tls_read_u16(&r, &v) == 0 && v == 0xFFFF);
    assert(mls_tls_reader_done(&r));
    mls_tls_buf_free(&buf);
}

static void test_u32_roundtrip(void)
{
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 16) == 0);
    assert(mls_tls_write_u32(&buf, 0xDEADBEEF) == 0);
    assert(buf.len == 4);
    assert(buf.data[0] == 0xDE && buf.data[1] == 0xAD);
    assert(buf.data[2] == 0xBE && buf.data[3] == 0xEF);

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    uint32_t v;
    assert(mls_tls_read_u32(&r, &v) == 0 && v == 0xDEADBEEF);
    assert(mls_tls_reader_done(&r));
    mls_tls_buf_free(&buf);
}

static void test_u64_roundtrip(void)
{
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 16) == 0);
    assert(mls_tls_write_u64(&buf, 0x0102030405060708ULL) == 0);
    assert(buf.len == 8);

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    uint64_t v;
    assert(mls_tls_read_u64(&r, &v) == 0 && v == 0x0102030405060708ULL);
    assert(mls_tls_reader_done(&r));
    mls_tls_buf_free(&buf);
}

/* ── Opaque vector round-trip ──────────────────────────────────────────── */

static void test_opaque8_roundtrip(void)
{
    const uint8_t data[] = "hello, marmot!";
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 64) == 0);
    assert(mls_tls_write_opaque8(&buf, data, sizeof(data) - 1) == 0);
    assert(buf.len == 1 + 14); /* 1-byte length + 14 bytes data */

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(mls_tls_read_opaque8(&r, &out, &out_len) == 0);
    assert(out_len == 14);
    assert(memcmp(out, data, 14) == 0);
    assert(mls_tls_reader_done(&r));
    free(out);
    mls_tls_buf_free(&buf);
}

static void test_opaque16_roundtrip(void)
{
    /* Test with larger data */
    uint8_t data[300];
    memset(data, 0xAB, sizeof(data));

    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_tls_write_opaque16(&buf, data, sizeof(data)) == 0);
    assert(buf.len == 2 + 300);

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(mls_tls_read_opaque16(&r, &out, &out_len) == 0);
    assert(out_len == 300);
    assert(memcmp(out, data, 300) == 0);
    free(out);
    mls_tls_buf_free(&buf);
}

static void test_opaque_empty(void)
{
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 16) == 0);
    assert(mls_tls_write_opaque8(&buf, NULL, 0) == 0);
    assert(buf.len == 1); /* just the zero-length byte */

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    uint8_t *out = NULL;
    size_t out_len = 0;
    assert(mls_tls_read_opaque8(&r, &out, &out_len) == 0);
    assert(out == NULL);
    assert(out_len == 0);
    mls_tls_buf_free(&buf);
}

/* ── Mixed types ──────────────────────────────────────────────────────── */

static void test_mixed_types(void)
{
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 128) == 0);

    assert(mls_tls_write_u16(&buf, 0xF2EE) == 0);
    assert(mls_tls_write_u8(&buf, 2) == 0);
    uint8_t fixed[32];
    memset(fixed, 0x42, 32);
    assert(mls_tls_buf_append(&buf, fixed, 32) == 0);
    assert(mls_tls_write_opaque16(&buf, (const uint8_t *)"test", 4) == 0);

    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);

    uint16_t ext_type;
    assert(mls_tls_read_u16(&r, &ext_type) == 0 && ext_type == 0xF2EE);
    uint8_t version;
    assert(mls_tls_read_u8(&r, &version) == 0 && version == 2);
    uint8_t gid[32];
    assert(mls_tls_read_fixed(&r, gid, 32) == 0);
    assert(gid[0] == 0x42 && gid[31] == 0x42);
    uint8_t *name = NULL;
    size_t name_len = 0;
    assert(mls_tls_read_opaque16(&r, &name, &name_len) == 0);
    assert(name_len == 4 && memcmp(name, "test", 4) == 0);
    free(name);

    assert(mls_tls_reader_done(&r));
    mls_tls_buf_free(&buf);
}

/* ── Error cases ──────────────────────────────────────────────────────── */

static void test_read_past_end(void)
{
    uint8_t data[] = {0x01};
    MlsTlsReader r;
    mls_tls_reader_init(&r, data, 1);

    uint8_t v8;
    assert(mls_tls_read_u8(&r, &v8) == 0);
    /* Now empty — further reads should fail */
    assert(mls_tls_read_u8(&r, &v8) != 0);

    uint16_t v16;
    mls_tls_reader_init(&r, data, 1);
    assert(mls_tls_read_u16(&r, &v16) != 0);
}

static void test_opaque8_overflow(void)
{
    /* opaque8 max is 255 bytes */
    uint8_t big[256];
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 512) == 0);
    assert(mls_tls_write_opaque8(&buf, big, 256) != 0);
    mls_tls_buf_free(&buf);
}

/* ── Buffer growth ────────────────────────────────────────────────────── */

static void test_buf_growth(void)
{
    MlsTlsBuf buf;
    assert(mls_tls_buf_init(&buf, 4) == 0); /* tiny initial cap */

    /* Write more than initial capacity */
    for (int i = 0; i < 100; i++) {
        assert(mls_tls_write_u32(&buf, (uint32_t)i) == 0);
    }
    assert(buf.len == 400);
    assert(buf.cap >= 400);

    /* Verify data integrity */
    MlsTlsReader r;
    mls_tls_reader_init(&r, buf.data, buf.len);
    for (int i = 0; i < 100; i++) {
        uint32_t v;
        assert(mls_tls_read_u32(&r, &v) == 0 && v == (uint32_t)i);
    }
    assert(mls_tls_reader_done(&r));
    mls_tls_buf_free(&buf);
}

int main(void)
{
    printf("libmarmot: TLS codec tests\n");
    TEST(test_u8_roundtrip);
    TEST(test_u16_roundtrip);
    TEST(test_u32_roundtrip);
    TEST(test_u64_roundtrip);
    TEST(test_opaque8_roundtrip);
    TEST(test_opaque16_roundtrip);
    TEST(test_opaque_empty);
    TEST(test_mixed_types);
    TEST(test_read_past_end);
    TEST(test_opaque8_overflow);
    TEST(test_buf_growth);
    printf("All TLS codec tests passed.\n");
    return 0;
}
