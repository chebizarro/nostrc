#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "transport/transport.h"
#include "transport/stream.h"
#include "transport/sockbuf.h"
#include "test_tmpdir.h"
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <net/if_media.h>
#endif
static void framing(void)
{
    /* Upstream framing.rs unit-vector shapes. No transport vector directory
     * exists upstream; FMP bytes are also covered by the core vector gate. */
    uint8_t msg1[114] = {1, 0, 110, 0}, msg2[69] = {2, 0, 65, 0};
    uint8_t established[256] = {0, 7, 224, 0};
    const uint8_t *vectors[] = {msg1, msg2, established};
    size_t sizes[] = {sizeof msg1, sizeof msg2, sizeof established};
    uint8_t output[512], pair[512];
    for (size_t k = 0; k < 3; ++k) {
        const uint8_t *frame = vectors[k]; size_t n = sizes[k];
        assert(fipsd_datagram_frame(frame, n, 1280) == FIPS_OK);
        /* Every truncation offset, every split, and complete concatenation. */
        for (size_t cut = 0; cut < n; ++cut) {
            assert(fipsd_datagram_frame(frame, cut, 1280) != FIPS_OK);
            fipsd_stream s; fipsd_stream_init(&s, output, sizeof output, 1280);
            size_t used; assert(fipsd_stream_feed(&s, frame, cut, &used) == 0 && used == cut);
            assert(fipsd_stream_eof(&s) == (cut == 0));
            assert(fipsd_stream_feed(&s, frame + cut, n - cut, &used) == 1 && used == n - cut);
            assert(!memcmp(frame, output, n));
            fipsd_stream_reset(&s); assert(fipsd_stream_eof(&s));
        }
        memcpy(pair, frame, n); memcpy(pair + n, frame, n);
        fipsd_stream s; fipsd_stream_init(&s, output, sizeof output, 1280);
        size_t used; assert(fipsd_stream_feed(&s, pair, n * 2, &used) == 1 && used == n);
        fipsd_stream_reset(&s);
        assert(fipsd_stream_feed(&s, pair + n, n, &used) == 1 && used == n);
        for (size_t cap = 0; cap < n; ++cap) {
            fipsd_stream_init(&s, output, cap, 1280);
            assert(fipsd_stream_feed(&s, frame, n, &used) == -1);
            assert(fipsd_stream_feed(&s, frame, n, &used) == -1 && used == 0);
        }
        for (unsigned byte = 0; byte < 256; ++byte) {
            uint8_t prefix[4]; memcpy(prefix, frame, 4); prefix[0] = (uint8_t)byte;
            size_t value = 9999;
            bool valid = byte == frame[0];
            /* Established payload 110/65 can also frame handshake-sized input,
             * but phase changes need not describe the original packet. */
            if (byte == 0) valid = true;
            assert((fipsd_frame_size(prefix, 1280, &value) == FIPS_OK) == valid);
            if (!valid) assert(value == 9999);
        }
    }
    size_t n = 123;
    uint8_t maximum[4] = {0, 0, 0xdf, 0xff}; /* 65503 + 32 */
    assert(fipsd_frame_size(maximum, UINT16_MAX, &n) == FIPS_OK && n == UINT16_MAX);
    maximum[2]++; assert(fipsd_frame_size(maximum, UINT16_MAX, &n) != FIPS_OK);
    uint8_t bad[4] = {1, 0, 109, 0}; assert(fipsd_frame_size(bad, 1280, &n) != FIPS_OK);
    bad[0] = 2; bad[2] = 66; assert(fipsd_frame_size(bad, 1280, &n) != FIPS_OK);
    bad[0] = 0; bad[2] = 0; assert(fipsd_frame_size(bad, 31, &n) != FIPS_OK);
}
static unsigned fake_index(const char *name) { assert(!strcmp(name, "fake0")); return 23; }
#ifdef __APPLE__
typedef struct fake_ioctl { int word; bool fail, loopback; unsigned calls; } fake_ioctl;
static int ioctl_fake(void *arg, int fd, unsigned long req, void *data)
{
    (void)fd; fake_ioctl *f = arg; f->calls++;
    if (f->fail) return -1;
    if (req == SIOCGIFFLAGS) { ((struct ifreq *)data)->ifr_flags = f->loopback ? IFF_LOOPBACK : IFF_UP; return 0; }
    assert(req == SIOCGIFMEDIA); ((struct ifmediareq *)data)->ifm_active = f->word; return 0;
}
#endif
static void presence_medium(void)
{
    struct ifaddrs a = {.ifa_name = "fake0", .ifa_flags = IFF_UP};
    fipsd_presence_facts f = fipsd_presence_from_ifaddrs(&a, "fake0", fake_index);
    assert(f.valid && f.exists && f.up && !f.carrier && f.index == 23);
    a.ifa_flags = IFF_RUNNING; f = fipsd_presence_from_ifaddrs(&a, "fake0", fake_index);
    assert(!f.up && f.carrier);
    f = fipsd_presence_from_ifaddrs(&a, "absent", fake_index); assert(f.valid && !f.exists);
    assert(fipsd_medium_linux(1, false, false, NULL, true) == FIPS_PATH_Wired);
    assert(fipsd_medium_linux(1, true, false, NULL, true) == FIPS_PATH_Wireless);
    assert(fipsd_medium_linux(1, false, true, NULL, true) == FIPS_PATH_Wireless);
    assert(fipsd_medium_linux(1, false, false, "wlan", false) == FIPS_PATH_Wireless);
    assert(fipsd_medium_linux(1, false, false, "bridge", true) == FIPS_PATH_Unknown);
    assert(fipsd_medium_linux(1, false, false, NULL, false) == FIPS_PATH_Unknown);
    assert(fipsd_medium_media_word(0x26) == FIPS_PATH_Wired);
    assert(fipsd_medium_media_word(0x87) == FIPS_PATH_Wireless);
    assert(fipsd_medium_media_word(0) == FIPS_PATH_Unknown);
#ifdef __APPLE__
    fake_ioctl io = {.word = 0x87};
    assert(fipsd_medium_read_ioctl("fake0", -1, ioctl_fake, &io) == FIPS_PATH_Wireless && io.calls == 2);
    io.word = 0x26; assert(fipsd_medium_read_ioctl("fake0", -1, ioctl_fake, &io) == FIPS_PATH_Wired);
    io.fail = true; assert(fipsd_medium_read_ioctl("fake0", -1, ioctl_fake, &io) == FIPS_PATH_Unknown);
    io.fail = false; io.loopback = true;
    assert(fipsd_medium_read_ioctl("fake0", -1, ioctl_fake, &io) == FIPS_PATH_Unknown);
    const char *loop = "lo0";
#else
    const char *loop = "lo";
#endif
    f = fipsd_presence_read(loop); assert(f.valid && f.exists && f.up && f.index);
    assert(fipsd_medium_read(loop) == FIPS_PATH_Unknown);
    printf("native %s: index=%u up=%d carrier=%d medium=%s\n", loop, f.index, f.up, f.carrier, fipsd_medium_name(f.medium));
#ifdef __APPLE__
    f = fipsd_presence_read("en0"); assert(f.valid);
    printf("native en0: exists=%d index=%u up=%d carrier=%d medium=%s\n", f.exists, f.index, f.up, f.carrier, fipsd_medium_name(f.medium));
#endif
    fipsd_presence p; fipsd_presence_init(&p, false, 100);
    fipsd_presence_transition(&p, FIPSD_BINDING, 200); fipsd_presence_transition(&p, FIPSD_ABSENT, 300);
    assert(fipsd_presence_snapshot(&p).since_ms == 100);
    assert(fipsd_presence_attempt(&p) == 1);
    fipsd_presence_bound(&p, f, 400); assert(fipsd_presence_snapshot(&p).attempts == 0);
    assert(fipsd_presence_snapshot(&p).binds == 1 && fipsd_presence_snapshot(&p).since_ms == 400);
    uint64_t backoff[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (uint32_t i = 0; i < 7; ++i) assert(fipsd_bind_backoff_ms(i + 1) == backoff[i]);
    fips_path_rival rivals[8] = {0};
    rivals[0] = (fips_path_rival){.used = true, .index = 0, .has_score = true, .score = 10, .fits = true, .medium = FIPS_PATH_Wireless};
    rivals[1] = rivals[0]; rivals[1].index = 1; rivals[1].score = 10.1; rivals[1].medium = FIPS_PATH_Wired;
    assert(fips_path_select_choose(rivals, 1.15) == 1);
    rivals[1].score = 100; assert(fips_path_select_choose(rivals, 1.15) == 0);
}
static void buffers_links(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0); assert(fd >= 0);
    for (int b = 0; b < 2; ++b) {
        fipsd_buffer_granted g; assert(fipsd_sockbuf_set(fd, (fipsd_socket_buffer)b, 32768, &g) == 0);
        int actual; socklen_t len = sizeof actual;
        assert(getsockopt(fd, SOL_SOCKET, b ? SO_SNDBUF : SO_RCVBUF, &actual, &len) == 0);
        assert(g.requested == 32768 && g.actual == (size_t)actual);
    }
    close(fd);
    fipsd_buffer_granted g = {1, 2}; assert(fipsd_sockbuf_set(-1, FIPSD_SOCKET_RECV, 1, &g) == -1 && g.actual == 2);
    fipsd_transport t = {0}; fipsd_transport_shared s; fipsd_transport_shared_init(&t, &s, false);
    assert(fipsd_transport_medium(&t) == FIPS_PATH_Unknown);
    fips_transport_addr a; assert(fips_transport_addr_from_string("peer", 4, &a) == FIPS_OK);
    fips_link_id old, current;
    assert(fipsd_transport_link_add(&s, &a, FIPS_LINK_INBOUND, 1280, &old) == 0);
    assert(fipsd_transport_link_add(&s, &a, FIPS_LINK_OUTBOUND, 1400, &current) == 0);
    assert(!fipsd_transport_link_remove(&s, &a, old));
    assert(!fipsd_transport_link_record(&s, &a, old, 42, true, 1));
    assert(fipsd_transport_link_record(&s, &a, current, 42, true, 1));
    fipsd_transport_link link; assert(fipsd_transport_link_get(&s, &a, &link) && link.mtu == 1400 && link.stats.bytes_sent == 42);
    assert(atomic_load(&s.stats.inbound) == 0 && atomic_load(&s.stats.outbound) == 1);
    assert(fipsd_transport_link_remove(&s, &a, current));
    assert(!fipsd_transport_link_remove(&s, &a, current) && atomic_load(&s.stats.outbound) == 0);
    for (unsigned i = 0; i < FIPSD_TRANSPORT_LINKS; ++i) {
        a.bytes[0] = (uint8_t)i;
        assert(fipsd_transport_link_add(&s, &a, FIPS_LINK_INBOUND, 1280, &current) == 0);
    }
    a.bytes[0] = 255; assert(fipsd_transport_link_add(&s, &a, FIPS_LINK_INBOUND, 1280, &current) == -1);
    fipsd_transport_links_clear(&s); assert(!atomic_load(&s.stats.inbound));
}
int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    framing(); presence_medium(); buffers_links();
    puts("shared transport codecs, presence, medium, sockbuf and links passed"); return 0;
}
