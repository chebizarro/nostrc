#ifndef FIPSD_TUN_STATUS_H
#define FIPSD_TUN_STATUS_H
#include "tun/tun.h"
#include "control/snapshot.h"
typedef enum fipsd_tun_state { FIPSD_TUN_DISABLED, FIPSD_TUN_WAITING, FIPSD_TUN_UP, FIPSD_TUN_FAILED, FIPSD_TUN_STOPPED } fipsd_tun_state;
typedef struct fipsd_tun_status {
    fipsd_tun_state state;
    char name[FIPSD_TUN_NAME_SIZE], address[46];
    uint16_t mtu;
    uint32_t index;
    unsigned attempts;
    int error; /* kernel error from setup or teardown; 0 on success */
    uint64_t host_drops; /* packets the shim dropped toward the host (full queue) */
} fipsd_tun_status;
fipsd_tun_status fipsd_tun_status_read(fipsd_tun *);
json_t *fipsd_tun_status_rows(const fipsd_tun_status *);
/* Composes retained node/TUN views without touching dispatcher-owned state.
 * base.user must outlive the TUN and runtime. Live commands delegate unchanged. */
fipsd_control_provider fipsd_tun_provider(fipsd_tun *, fipsd_control_provider base);
#endif
