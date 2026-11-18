/* The kernel calls of the Windows TUN backend (tun_windows.c): the Wintun
 * 0.14 session API, loaded from wintun.dll at run time, and the IP Helper
 * rows of tun/winnet.h. A device takes them through fipsd_tun_ops.wintun, so
 * a test drives the whole backend (adapter, receive thread, frames, rows,
 * teardown) against fakes with no driver and no elevation. Handles are
 * opaque; every call that can fail returns its Win32 code (0 on success). */
#ifndef FIPSD_TUN_WINTUN_H
#define FIPSD_TUN_WINTUN_H
#include "tun/winnet.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct fipsd_tun_wintun_ops {
    void *user;
    /* Resolve the Wintun API once; ERROR_MOD_NOT_FOUND when no wintun.dll sits
     * beside the executable or in System32, ERROR_PROC_NOT_FOUND for a DLL
     * without the 0.14 exports. */
    uint32_t (*load)(void *user);
    /* WintunOpenAdapter (a probe: closing an opened adapter keeps it). */
    uint32_t (*open_adapter)(void *user, const uint16_t *name, void **adapter);
    /* WintunCreateAdapter with a system-chosen GUID; WintunCloseAdapter
     * removes a created adapter. */
    uint32_t (*create_adapter)(void *user, const uint16_t *name, const uint16_t *tunnel_type, void **adapter);
    void (*close_adapter)(void *user, void *adapter);
    uint64_t (*adapter_luid)(void *user, void *adapter);
    uint32_t (*driver_version)(void *user); /* WintunGetRunningDriverVersion; 0 unknown */
    uint32_t (*start_session)(void *user, void *adapter, uint32_t capacity, void **session);
    void (*end_session)(void *user, void *session);
    /* The session's read-wait event (a HANDLE the session owns). */
    void *(*read_event)(void *user, void *session);
    /* ERROR_NO_MORE_ITEMS: ring empty, wait on read_event; ERROR_HANDLE_EOF:
     * the session is ending; ERROR_INVALID_DATA: the ring is corrupt. */
    uint32_t (*receive)(void *user, void *session, uint8_t **packet, uint32_t *size);
    void (*release)(void *user, void *session, const uint8_t *packet);
    /* ERROR_BUFFER_OVERFLOW: the ring toward the host is full. */
    uint32_t (*allocate)(void *user, void *session, uint32_t size, uint8_t **packet);
    void (*send)(void *user, void *session, const uint8_t *packet);
    /* IP Helper (tun/winnet.h). */
    uint32_t (*alias_in_use)(void *user, const uint16_t *alias, bool *in_use);
    uint32_t (*ipv6_interface)(void *user, uint64_t luid, uint32_t timeout_ms);
    uint32_t (*address)(void *user, const fipsd_winnet_address *row, bool add);
    uint32_t (*mtu)(void *user, uint64_t luid, uint32_t mtu);
    uint32_t (*route)(void *user, const fipsd_winnet_route *row, bool add);
} fipsd_tun_wintun_ops;

#if defined(_WIN32)
/* wintun.dll through LoadLibraryExW(LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
 * LOAD_LIBRARY_SEARCH_SYSTEM32) and IP Helper. */
const fipsd_tun_wintun_ops *fipsd_tun_wintun_system_ops(void);
/* Receive-thread drops (no memory for a packet's copy) and the Win32 code
 * that ended the receive thread, for tests and status. */
typedef struct fipsd_tun_wintun_stats { uint64_t received, dropped; uint32_t end_code; } fipsd_tun_wintun_stats;
struct fipsd_tun;
fipsd_tun_wintun_stats fipsd_tun_wintun_stats_read(const struct fipsd_tun *);
uint64_t fipsd_tun_wintun_luid(const struct fipsd_tun *);
#endif
#endif
