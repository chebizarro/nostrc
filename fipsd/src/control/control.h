#ifndef FIPSD_CONTROL_H
#define FIPSD_CONTROL_H
#include "control/protocol.h"
#include "runtime.h"
#include "config/config.h"
#include "fiber_hooks.h"
#include <limits.h>
#include <stdatomic.h>

#define FIPSD_CONTROL_MAX_CONNECTIONS 32u

/* Pure resolution policy. Production probes directory existence once at start;
 * tests pass observations without creating/modifying canonical system paths. */
typedef struct fipsd_control_path_env {
    bool run_exists, var_run_exists, macos, freebsd, privileged;
    const char *xdg_runtime_dir;
} fipsd_control_path_env;
int fipsd_control_resolve_path(const char *override, const fipsd_control_path_env *env,
                              char *out, size_t cap);

typedef struct fipsd_control_service {
    fipsd_subsystem subsystem;
    GoSwap *configs; /* borrowed; immutable configuration snapshots */
    fipsd_control_provider provider;
    fipsd_runtime *runtime;
    nsync_mu listener_mu; /* listener and accept_waiter lifetime; never held across I/O */
    gof_fiber_handle accept_waiter; /* protected by listener_mu; cleared before fiber exit */
    _Atomic unsigned connections; /* includes both connection and deadline fibers */
    int listener; /* accept fiber owns/ultimately closes it */
    int cancel_socket; /* preallocated non-listening replacement, independent of pathname */
    char path[PATH_MAX];
    uint64_t socket_device, socket_inode;
    bool bound;
} fipsd_control_service;
void fipsd_control_service_init(fipsd_control_service *service, GoSwap *configs,
                               const fipsd_control_provider *provider);
/* After runtime has joined every fiber (or start failed). Removes only the
 * inode this service bound; never a subsequently replaced file/socket. */
void fipsd_control_service_clear(fipsd_control_service *service);
#endif
