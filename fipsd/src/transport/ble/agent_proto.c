/* Upstream transport/ble/io_macos/agent_proto.rs (version 2): encode and
 * decode the agent socket frames. The vectors in tests/test_ble_agent.c are
 * the pinned upstream encoder's own output. */
#include "transport/ble/agent_proto.h"
#include <string.h>

static size_t payload_len(const fipsd_ble_agent_frame *f)
{
    switch (f->tag) {
    case FIPSD_BLE_AGENT_CONNECT: return 8 + 6 + 2;
    case FIPSD_BLE_AGENT_START_ADVERTISING: return 2;
    case FIPSD_BLE_AGENT_STOP_ADVERTISING: case FIPSD_BLE_AGENT_START_SCANNING:
    case FIPSD_BLE_AGENT_STOP_SCANNING: case FIPSD_BLE_AGENT_BUSY: return 0;
    case FIPSD_BLE_AGENT_CLOSE: case FIPSD_BLE_AGENT_CLOSED: return 4;
    case FIPSD_BLE_AGENT_SEND: case FIPSD_BLE_AGENT_RECV: return 4 + f->len;
    case FIPSD_BLE_AGENT_HELLO: return 1 + 2;
    case FIPSD_BLE_AGENT_INBOUND: return 4 + 6 + 2;
    case FIPSD_BLE_AGENT_CONNECT_RESULT: return 8 + 1 + 4 + 6 + 2;
    case FIPSD_BLE_AGENT_SCAN: return 6 + 2 + 2;
    case FIPSD_BLE_AGENT_CREDIT: return 4 + 4;
    }
    return SIZE_MAX;
}
size_t fipsd_ble_agent_encoded_len(const fipsd_ble_agent_frame *f)
{
    bool data = f->tag == FIPSD_BLE_AGENT_SEND || f->tag == FIPSD_BLE_AGENT_RECV;
    if (data && (f->len >= FIPSD_BLE_AGENT_MAX_FRAME || (f->len && !f->data))) return 0;
    size_t n = payload_len(f);
    if (n == SIZE_MAX || n >= FIPSD_BLE_AGENT_MAX_FRAME) return 0; /* the tag must fit too */
    return FIPSD_BLE_AGENT_HEADER + 1 + n;
}

static uint8_t *put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return p + 4; }
static uint8_t *put64(uint8_t *p, uint64_t v) { return put32(put32(p, (uint32_t)(v >> 32)), (uint32_t)v); }
static uint8_t *put_addr(uint8_t *p, const uint8_t a[6]) { memcpy(p, a, 6); return p + 6; }
size_t fipsd_ble_agent_encode(const fipsd_ble_agent_frame *f, uint8_t *out, size_t cap)
{
    size_t total = fipsd_ble_agent_encoded_len(f);
    if (!total || cap < total) return 0;
    uint8_t *p = put32(out, (uint32_t)(total - FIPSD_BLE_AGENT_HEADER));
    *p++ = (uint8_t)f->tag;
    switch (f->tag) {
    case FIPSD_BLE_AGENT_CONNECT: p = put16(put_addr(put64(p, (uint64_t)f->id), f->addr), f->psm); break;
    case FIPSD_BLE_AGENT_START_ADVERTISING: p = put16(p, f->psm); break;
    case FIPSD_BLE_AGENT_STOP_ADVERTISING: case FIPSD_BLE_AGENT_START_SCANNING:
    case FIPSD_BLE_AGENT_STOP_SCANNING: case FIPSD_BLE_AGENT_BUSY: break;
    case FIPSD_BLE_AGENT_CLOSE: case FIPSD_BLE_AGENT_CLOSED: p = put32(p, f->ch); break;
    case FIPSD_BLE_AGENT_SEND: case FIPSD_BLE_AGENT_RECV:
        p = put32(p, f->ch);
        if (f->len) memcpy(p, f->data, f->len);
        p += f->len;
        break;
    case FIPSD_BLE_AGENT_HELLO: *p++ = f->version; p = put16(p, f->psm); break;
    case FIPSD_BLE_AGENT_INBOUND: p = put16(put_addr(put32(p, f->ch), f->addr), f->mtu); break;
    case FIPSD_BLE_AGENT_CONNECT_RESULT:
        p = put64(p, (uint64_t)f->id); *p++ = f->ok ? 1 : 0;
        p = put16(put_addr(put32(p, f->ch), f->addr), f->mtu);
        break;
    case FIPSD_BLE_AGENT_SCAN: p = put16(put16(put_addr(p, f->addr), f->psm), (uint16_t)f->rssi); break;
    case FIPSD_BLE_AGENT_CREDIT: p = put32(put32(p, f->ch), f->bytes); break;
    }
    return (size_t)(p - out);
}

bool fipsd_ble_agent_frame_len(const uint8_t h[FIPSD_BLE_AGENT_HEADER], size_t max, size_t *len)
{
    uint32_t n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    if (!n || n > max) return false;
    *len = n;
    return true;
}

typedef struct reader { const uint8_t *p; size_t left; bool ok; } reader;
static const uint8_t *take(reader *r, size_t n)
{
    if (!r->ok || r->left < n) { r->ok = false; return NULL; }
    const uint8_t *p = r->p; r->p += n; r->left -= n;
    return p;
}
static uint8_t get8(reader *r) { const uint8_t *p = take(r, 1); return p ? p[0] : 0; }
static uint16_t get16(reader *r) { const uint8_t *p = take(r, 2); return p ? (uint16_t)(p[0] << 8 | p[1]) : 0; }
static uint32_t get32(reader *r)
{ const uint8_t *p = take(r, 4); return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : 0; }
static uint64_t get64(reader *r) { uint64_t hi = get32(r); return hi << 32 | get32(r); }
static void get_addr(reader *r, uint8_t out[6]) { const uint8_t *p = take(r, 6); if (p) memcpy(out, p, 6); }
bool fipsd_ble_agent_decode(const uint8_t *body, size_t len, fipsd_ble_agent_frame *f)
{
    if (!body || !len) return false;
    memset(f, 0, sizeof *f);
    reader r = {body + 1, len - 1, true};
    switch (body[0]) {
    case FIPSD_BLE_AGENT_CONNECT: f->id = (int64_t)get64(&r); get_addr(&r, f->addr); f->psm = get16(&r); break;
    case FIPSD_BLE_AGENT_START_ADVERTISING: f->psm = get16(&r); break;
    case FIPSD_BLE_AGENT_STOP_ADVERTISING: case FIPSD_BLE_AGENT_START_SCANNING:
    case FIPSD_BLE_AGENT_STOP_SCANNING: case FIPSD_BLE_AGENT_BUSY: break;
    case FIPSD_BLE_AGENT_CLOSE: case FIPSD_BLE_AGENT_CLOSED: f->ch = get32(&r); break;
    case FIPSD_BLE_AGENT_SEND: case FIPSD_BLE_AGENT_RECV:
        f->ch = get32(&r);
        if (r.ok) { f->data = r.p; f->len = r.left; }
        break;
    case FIPSD_BLE_AGENT_HELLO: f->version = get8(&r); f->psm = get16(&r); break;
    case FIPSD_BLE_AGENT_INBOUND: f->ch = get32(&r); get_addr(&r, f->addr); f->mtu = get16(&r); break;
    case FIPSD_BLE_AGENT_CONNECT_RESULT:
        f->id = (int64_t)get64(&r); f->ok = get8(&r) != 0; f->ch = get32(&r); get_addr(&r, f->addr); f->mtu = get16(&r);
        break;
    case FIPSD_BLE_AGENT_SCAN: get_addr(&r, f->addr); f->psm = get16(&r); f->rssi = (int16_t)get16(&r); break;
    case FIPSD_BLE_AGENT_CREDIT: f->ch = get32(&r); f->bytes = get32(&r); break;
    default: return false;
    }
    f->tag = (fipsd_ble_agent_tag)body[0];
    return r.ok;
}

void fipsd_ble_agent_device_from_identifier(const uint8_t id[16], uint8_t out[6])
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < 16; ++i) h = (h ^ id[i]) * UINT64_C(0x100000001b3);
    for (size_t i = 0; i < 6; ++i) out[i] = (uint8_t)(h >> (56 - 8 * i));
    out[0] = (uint8_t)((out[0] | 0x02) & ~0x01);
}
