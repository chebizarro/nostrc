#ifndef FIPSD_TUN_H
#define FIPSD_TUN_H
#include "runtime.h"
#include <fips/identity/local.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdatomic.h>

#define FIPSD_TUN_NAME_SIZE 16u
/* Host compression shim budget, NOT core FIPS_OVERHEAD (106/110). */
#define FIPSD_TUN_IPV6_OVERHEAD 77u
#define FIPSD_TUN_IPV6_MIN_MTU 1280u

typedef enum fipsd_tun_os { FIPSD_TUN_LINUX, FIPSD_TUN_MACOS, FIPSD_TUN_FREEBSD, FIPSD_TUN_WINDOWS } fipsd_tun_os;
struct fipsd_tun_wintun_ops; /* tun/wintun.h */
/* Every kernel operation is replaceable, including control I/O and descriptor
 * preparation. The default frame methods are gof_read/gof_write; control I/O
 * runs on the runtime starting/stopping thread, never a scheduler worker.
 * Ops and user must outlive the device. No process-global test overrides. */
typedef struct fipsd_tun_ops {
    void *user;
    int (*open)(void *, const char *, int);
    int (*socket)(void *, int, int, int);
    int (*ioctl)(void *, int, unsigned long, void *);
    int (*connect)(void *, int, const struct sockaddr *, socklen_t);
    int (*getsockopt)(void *, int, int, int, void *, socklen_t *);
    int (*prepare)(void *, int);
    int (*route_socket)(void *);
    int (*close)(void *, int);
    unsigned (*ifindex)(void *, const char *);
    int (*device_name)(void *, int, char *, size_t);
    int (*pid)(void *);
    int (*wait_read)(void *, int, int);
    ssize_t (*read)(void *, int, void *, size_t);
    ssize_t (*write)(void *, int, const void *, size_t);
    ssize_t (*frame_read)(void *, int, void *, size_t);
    ssize_t (*frame_write)(void *, int, const void *, size_t);
    /* Windows: the Wintun session and IP Helper calls (tun/wintun.h); NULL
     * takes the system's. Unused elsewhere. */
    const struct fipsd_tun_wintun_ops *wintun;
} fipsd_tun_ops;
const fipsd_tun_ops *fipsd_tun_system_ops(void);

typedef struct fipsd_tun fipsd_tun;
typedef struct fipsd_tun_platform {
    fipsd_tun_os os;
    uint8_t family_header; /* 0: raw IPv6, 30: utun, 28: FreeBSD TUNSIFHEAD */
    int (*open)(fipsd_tun *);
    int (*address)(fipsd_tun *, bool add);
    int (*mtu)(fipsd_tun *);
    int (*up)(fipsd_tun *, bool);
    int (*route)(fipsd_tun *, bool add);
    int (*destroy)(fipsd_tun *);
    /* Optional, for a device whose packets are not read from its descriptor
     * (Wintun's session rings): moves one frame, with the same contract as
     * ops.frame_read/frame_write on fd. NULL: ops.frame_read/frame_write. */
    ssize_t (*frame_read)(fipsd_tun *, uint8_t *, size_t);
    ssize_t (*frame_write)(fipsd_tun *, const uint8_t *, size_t);
} fipsd_tun_platform;
const fipsd_tun_platform *fipsd_tun_native_platform(void);

typedef struct fipsd_tun_config {
    const char *name;
    uint32_t transport_mtu; /* 1357 for the default UDP link */
    uint16_t mtu; /* configured TUN ceiling; 0: no ceiling */
} fipsd_tun_config;
/* Saturating subtraction, IPv6 minimum floor, optional configured ceiling
 * (also floored at 1280). Sub-minimum paths need the later shim fragmenter. */
uint16_t fipsd_tun_mtu(uint32_t transport_mtu, uint16_t configured);
fipsd_tun *fipsd_tun_create(const fipsd_tun_config *, const fipsd_tun_ops *, const fipsd_tun_platform *);
/* Single lifecycle owner, on a non-fiber thread. A failed attempt fully unwinds;
 * EBUSY/ENOENT/ENODEV retries at most three times, never takes a foreign TUN.
 * Identity must already be initialized and node ready. */
int fipsd_tun_start(fipsd_tun *, const fips_identity *);
int fipsd_tun_stop(fipsd_tun *);
void fipsd_tun_destroy(fipsd_tun *);
/* Single reader and single writer, on fibers. The IPv6 shim owns cancellation
 * and joins its I/O fibers BEFORE stop/destroy; no fd close/reuse cancellation.
 * No automatic packet consumer is installed by this device-only task. */
ssize_t fipsd_tun_read(fipsd_tun *, uint8_t *, size_t);
ssize_t fipsd_tun_write(fipsd_tun *, const uint8_t *, size_t);
int fipsd_tun_header_prepend(uint8_t family, const uint8_t *, size_t, uint8_t *, size_t);
/* The IPv6 shim's view of a started device: descriptor (-1 when down; on
 * Windows the receive doorbell socket, see tun_windows.c), kernel
 * name, node-derived address and effective IPv6 MTU. The shim registers its
 * host-drop counter so status reports host_drops; NULL detaches it. */
int fipsd_tun_fd(const fipsd_tun *);
const char *fipsd_tun_name(const fipsd_tun *);
fips_ipv6_address fipsd_tun_address(const fipsd_tun *);
uint16_t fipsd_tun_effective_mtu(const fipsd_tun *);
void fipsd_tun_set_host_drops(fipsd_tun *, const _Atomic uint64_t *);
int fipsd_tun_header_strip(uint8_t family, uint8_t *, size_t);
#endif
