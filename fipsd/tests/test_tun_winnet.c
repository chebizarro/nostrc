/* The Windows TUN backend's host configuration as pure data (tun/winnet.h,
 * upstream ipv6tun/winnet.rs and the windows_tun module of tun.rs), checked on
 * every host: the node's /128 row, the fd00::/8 on-link route row, how IP
 * Helper's create/delete codes are judged, the errno vocabulary, the Wintun
 * ring bounds and UTF-16 adapter names. On Windows the same rows are also
 * turned into MIB_* rows and checked field by field (test_tun_winnet_mib.c). */
#include "tun/winnet.h"
#include "host_os.h"
#include <fips/identity/local.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
int fipsd_test_winnet_mib(void);
#endif

static fips_ipv6_address node_address(void)
{
    uint8_t key[32] = {0}; key[31] = 1;
    fips_secp *secp = NULL; fips_identity *id = NULL;
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    assert(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), key, &id) == FIPS_IDENTITY_OK);
    fips_ipv6_address a = fips_identity_address(id);
    fips_identity_destroy(id); fips_identity_destroy_secp(secp);
    return a;
}
static void address_row(void)
{
    fips_ipv6_address node = node_address();
    assert(node.bytes[0] == 0xfd);
    fipsd_winnet_address row;
    assert(!fipsd_winnet_node_address(0, node.bytes, &row));          /* no adapter */
    uint8_t lan[16] = {0xfe, 0x80, [15] = 1};
    assert(!fipsd_winnet_node_address(7, lan, &row));                 /* outside fd00::/8 */
    assert(fipsd_winnet_node_address(0x0600000001000000ull, node.bytes, &row));
    assert(row.luid == 0x0600000001000000ull && !memcmp(row.address, node.bytes, 16));
    assert(row.prefix_len == 128 && row.prefix_origin == 1 && row.suffix_origin == 1);
}
static void route_row(void)
{
    fipsd_winnet_route r;
    assert(!fipsd_winnet_mesh_route(0, &r));
    assert(fipsd_winnet_mesh_route(42, &r));
    static const uint8_t prefix[16] = {0xfd}, unspecified[16] = {0};
    assert(r.luid == 42 && !memcmp(r.prefix, prefix, 16) && r.prefix_len == 8);
    assert(!memcmp(r.next_hop, unspecified, 16)); /* on-link: no gateway */
    assert(r.metric == 256 && r.protocol == 3);  /* netsh's default metric, MIB_IPPROTO_NETMGMT */
    fips_ipv6_address node = node_address();
    uint8_t top[16], ula[16] = {0xfd, 0x52, 0x4d, 0x49, [15] = 1}, fc[16] = {0xfc, [15] = 1}, ll[16] = {0xfe, 0x80, [15] = 1};
    memset(top, 0xff, sizeof top); top[0] = 0xfd;
    assert(fipsd_winnet_route_covers(&r, node.bytes) && fipsd_winnet_route_covers(&r, top));
    /* A LAN ULA /64 is inside fd00::/8 too: its longer prefix keeps it on the LAN. */
    assert(fipsd_winnet_route_covers(&r, ula));
    assert(!fipsd_winnet_route_covers(&r, fc) && !fipsd_winnet_route_covers(&r, ll));
    fipsd_winnet_route odd = r; odd.prefix_len = 7; /* fc00::/7 */
    assert(fipsd_winnet_route_covers(&odd, fc) && !fipsd_winnet_route_covers(&odd, ll));
    odd.prefix_len = 129;
    assert(!fipsd_winnet_route_covers(&odd, node.bytes));
}
static void outcomes(void)
{
    assert(fipsd_winnet_outcome_of(0, true) == FIPSD_WINNET_DONE && fipsd_winnet_outcome_of(0, false) == FIPSD_WINNET_DONE);
    assert(fipsd_winnet_outcome_of(FIPSD_WIN_ERROR_OBJECT_ALREADY_EXISTS, true) == FIPSD_WINNET_EXISTS);
    assert(fipsd_winnet_outcome_of(FIPSD_WIN_ERROR_OBJECT_ALREADY_EXISTS, false) == FIPSD_WINNET_FAILED);
    assert(fipsd_winnet_outcome_of(FIPSD_WIN_ERROR_NOT_FOUND, false) == FIPSD_WINNET_ABSENT);
    assert(fipsd_winnet_outcome_of(FIPSD_WIN_ERROR_FILE_NOT_FOUND, false) == FIPSD_WINNET_ABSENT);
    assert(fipsd_winnet_outcome_of(FIPSD_WIN_ERROR_NOT_FOUND, true) == FIPSD_WINNET_FAILED);
    assert(fipsd_winnet_outcome_of(FIPSD_WIN_ERROR_ACCESS_DENIED, true) == FIPSD_WINNET_FAILED);
    assert(fipsd_winnet_outcome_of(FIPSD_WIN_ERROR_INVALID_PARAMETER, false) == FIPSD_WINNET_FAILED);
}
static void errnos(void)
{
    static const struct { uint32_t code; int error; } map[] = {
        {0, 0}, {FIPSD_WIN_ERROR_ACCESS_DENIED, EPERM}, {FIPSD_WIN_ERROR_ELEVATION_REQUIRED, EPERM},
        {FIPSD_WIN_ERROR_MOD_NOT_FOUND, ENOENT}, {FIPSD_WIN_ERROR_NOT_FOUND, ENOENT}, {FIPSD_WIN_ERROR_FILE_NOT_FOUND, ENOENT},
        {FIPSD_WIN_ERROR_PROC_NOT_FOUND, ENOSYS}, {FIPSD_WIN_ERROR_ALREADY_EXISTS, EBUSY}, {FIPSD_WIN_ERROR_DUP_NAME, EBUSY},
        {FIPSD_WIN_ERROR_OBJECT_ALREADY_EXISTS, EBUSY}, {FIPSD_WIN_ERROR_HANDLE_EOF, ENODEV},
        {FIPSD_WIN_ERROR_DEVICE_NOT_AVAILABLE, ENODEV}, {FIPSD_WIN_ERROR_BUFFER_OVERFLOW, ENOBUFS},
        {FIPSD_WIN_ERROR_TIMEOUT, ETIMEDOUT}, {FIPSD_WIN_WAIT_TIMEOUT, ETIMEDOUT}, {FIPSD_WIN_ERROR_INVALID_PARAMETER, EINVAL},
        {FIPSD_WIN_ERROR_NOT_ENOUGH_MEMORY, ENOMEM}, {FIPSD_WIN_ERROR_INVALID_DATA, EIO}, {FIPSD_WIN_ERROR_GEN_FAILURE, EIO},
        {FIPSD_WIN_ERROR_NOT_SUPPORTED, ENOTSUP}, {999999u, EIO}};
    for (size_t i = 0; i < sizeof map / sizeof *map; ++i) assert(fipsd_winnet_errno(map[i].code) == map[i].error);
    /* The lifecycle retries EBUSY/ENOENT/ENODEV at most three times; missing
     * privilege must not be one of them. */
    int perm = fipsd_winnet_errno(FIPSD_WIN_ERROR_ACCESS_DENIED);
    assert(perm != EBUSY && perm != ENOENT && perm != ENODEV);
}
static void rings(void)
{
    assert(fipsd_winnet_ring_capacity_valid(FIPSD_WINTUN_RING_CAPACITY) && FIPSD_WINTUN_RING_CAPACITY == 0x200000u);
    assert(fipsd_winnet_ring_capacity_valid(0x20000u) && fipsd_winnet_ring_capacity_valid(0x4000000u));
    assert(!fipsd_winnet_ring_capacity_valid(0x10000u) && !fipsd_winnet_ring_capacity_valid(0x8000000u));
    assert(!fipsd_winnet_ring_capacity_valid(0x300000u) && !fipsd_winnet_ring_capacity_valid(0));
}
static void wide(void)
{
    uint16_t w[FIPSD_WINTUN_NAME_MAX + 1];
    const size_t cap = sizeof w / sizeof *w;
    assert(fipsd_winnet_wide("fips0", w, cap) == 5 && w[0] == 'f' && w[4] == '0' && w[5] == 0);
    assert(fipsd_winnet_wide(FIPSD_WINTUN_TUNNEL_TYPE, w, cap) == 4 && w[0] == 'F' && w[3] == 'S' && !w[4]);
    assert(!fipsd_winnet_wide("", w, cap) && !fipsd_winnet_wide(NULL, w, cap) && !fipsd_winnet_wide("x", NULL, cap));
    assert(fipsd_winnet_wide("\xc3\xa9", w, cap) == 1 && w[0] == 0xe9);                  /* U+00E9 */
    assert(fipsd_winnet_wide("\xe2\x82\xac", w, cap) == 1 && w[0] == 0x20ac);            /* U+20AC */
    assert(fipsd_winnet_wide("\xf0\x9f\x98\x80", w, cap) == 2 && w[0] == 0xd83d && w[1] == 0xde00); /* U+1F600 */
    static const char *const bad[] = {"\xc0\x80", "\xc1\xbf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\x80", "\xe2\x82", "a\xff", "\xf0\x9f\x98"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) assert(!fipsd_winnet_wide(bad[i], w, cap));
    char longest[FIPSD_WINTUN_NAME_MAX + 2];
    memset(longest, 'a', sizeof longest); longest[FIPSD_WINTUN_NAME_MAX] = 0;
    assert(fipsd_winnet_wide(longest, w, cap) == FIPSD_WINTUN_NAME_MAX);
    longest[FIPSD_WINTUN_NAME_MAX] = 'a'; longest[FIPSD_WINTUN_NAME_MAX + 1] = 0;
    assert(!fipsd_winnet_wide(longest, w, cap));                                         /* MAX_ADAPTER_NAME */
    uint16_t small[3];
    assert(fipsd_winnet_wide("ab", small, 3) == 2 && !fipsd_winnet_wide("abc", small, 3)); /* room for the NUL */
}
int main(void)
{
    address_row(); route_row(); outcomes(); errnos(); rings(); wide();
#if defined(_WIN32)
    assert(fipsd_test_winnet_mib() == 0);
    puts("winnet rows, codes, names and MIB rows: ok");
#else
    puts("winnet rows, codes and names: ok");
#endif
    return 0;
}
