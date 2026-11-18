/* Pure Wintun/IP Helper computations (tun/winnet.h), built on every host. */
#include "tun/winnet.h"
#include <errno.h>
#include <string.h>

bool fipsd_winnet_node_address(uint64_t luid, const uint8_t address[16], fipsd_winnet_address *out)
{
    if (!luid || !address || !out || address[0] != 0xfd) return false;
    *out = (fipsd_winnet_address){.luid = luid, .prefix_len = 128,
        .prefix_origin = FIPSD_WINNET_ORIGIN_MANUAL, .suffix_origin = FIPSD_WINNET_ORIGIN_MANUAL};
    memcpy(out->address, address, 16);
    return true;
}
bool fipsd_winnet_mesh_route(uint64_t luid, fipsd_winnet_route *out)
{
    if (!luid || !out) return false;
    *out = (fipsd_winnet_route){.luid = luid, .prefix = {0xfd}, .prefix_len = 8,
        .metric = FIPSD_WINNET_ROUTE_METRIC, .protocol = FIPSD_WINNET_PROTO_NETMGMT};
    return true;
}
bool fipsd_winnet_route_covers(const fipsd_winnet_route *r, const uint8_t address[16])
{
    if (!r || !address || r->prefix_len > 128) return false;
    unsigned whole = r->prefix_len / 8u, bits = r->prefix_len % 8u;
    if (memcmp(r->prefix, address, whole)) return false;
    if (!bits) return true;
    uint8_t mask = (uint8_t)(0xffu << (8u - bits));
    return (r->prefix[whole] & mask) == (address[whole] & mask);
}
fipsd_winnet_outcome fipsd_winnet_outcome_of(uint32_t code, bool create)
{
    if (!code) return FIPSD_WINNET_DONE;
    if (create && code == FIPSD_WIN_ERROR_OBJECT_ALREADY_EXISTS) return FIPSD_WINNET_EXISTS;
    if (!create && (code == FIPSD_WIN_ERROR_NOT_FOUND || code == FIPSD_WIN_ERROR_FILE_NOT_FOUND)) return FIPSD_WINNET_ABSENT;
    return FIPSD_WINNET_FAILED;
}
int fipsd_winnet_errno(uint32_t code)
{
    switch (code) {
    case 0: return 0;
    case FIPSD_WIN_ERROR_ACCESS_DENIED: case FIPSD_WIN_ERROR_ELEVATION_REQUIRED: return EPERM;
    case FIPSD_WIN_ERROR_FILE_NOT_FOUND: case FIPSD_WIN_ERROR_PATH_NOT_FOUND:
    case FIPSD_WIN_ERROR_NOT_FOUND: case FIPSD_WIN_ERROR_MOD_NOT_FOUND: return ENOENT;
    case FIPSD_WIN_ERROR_PROC_NOT_FOUND: return ENOSYS;
    case FIPSD_WIN_ERROR_ALREADY_EXISTS: case FIPSD_WIN_ERROR_OBJECT_ALREADY_EXISTS:
    case FIPSD_WIN_ERROR_DUP_NAME: case FIPSD_WIN_ERROR_BUSY: return EBUSY;
    case FIPSD_WIN_ERROR_INVALID_PARAMETER: return EINVAL;
    case FIPSD_WIN_ERROR_INVALID_HANDLE: return EBADF;
    case FIPSD_WIN_ERROR_NOT_ENOUGH_MEMORY: case FIPSD_WIN_ERROR_OUTOFMEMORY: return ENOMEM;
    case FIPSD_WIN_ERROR_BUFFER_OVERFLOW: return ENOBUFS;
    case FIPSD_WIN_ERROR_HANDLE_EOF: case FIPSD_WIN_ERROR_DEV_NOT_EXIST:
    case FIPSD_WIN_ERROR_DEVICE_NOT_AVAILABLE: return ENODEV;
    case FIPSD_WIN_ERROR_NOT_SUPPORTED: return ENOTSUP;
    case FIPSD_WIN_WAIT_TIMEOUT: case FIPSD_WIN_ERROR_TIMEOUT: return ETIMEDOUT;
    default: return EIO; /* FIPSD_WIN_ERROR_INVALID_DATA, FIPSD_WIN_ERROR_GEN_FAILURE, ... */
    }
}
bool fipsd_winnet_ring_capacity_valid(uint32_t c)
{
    return c >= FIPSD_WINTUN_MIN_RING_CAPACITY && c <= FIPSD_WINTUN_MAX_RING_CAPACITY && !(c & (c - 1u));
}
/* One code point from UTF-8 (RFC 3629: no overlongs, surrogates or values
 * past U+10FFFF); the bytes it took, or 0. */
static size_t decode(const unsigned char *s, uint32_t *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    size_t n = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3 : s[0] >= 0xc2 ? 2 : 0;
    if (!n || s[0] > 0xf4) return 0;
    uint32_t v = s[0] & (0x7fu >> n);
    for (size_t i = 1; i < n; ++i) {
        if ((s[i] & 0xc0) != 0x80) return 0; /* also stops at the NUL */
        v = v << 6 | (s[i] & 0x3fu);
    }
    static const uint32_t least[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (v < least[n] || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
    *cp = v; return n;
}
size_t fipsd_winnet_wide(const char *name, uint16_t *out, size_t cap)
{
    if (!name || !*name || !out || !cap) return 0;
    const unsigned char *s = (const unsigned char *)name;
    size_t n = 0;
    while (*s) {
        uint32_t cp;
        size_t used = decode(s, &cp);
        if (!used) return 0;
        s += used;
        size_t units = cp >= 0x10000 ? 2 : 1;
        if (n + units > FIPSD_WINTUN_NAME_MAX || n + units + 1 > cap) return 0;
        if (units == 2) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xd800u | cp >> 10);
            out[n++] = (uint16_t)(0xdc00u | (cp & 0x3ffu));
        } else out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
    return n;
}
