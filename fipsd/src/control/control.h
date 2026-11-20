#ifndef FIPSD_CONTROL_H
#define FIPSD_CONTROL_H
#include "control/protocol.h"
#include "runtime.h"
#include "config/config.h"
#include "fiber_hooks.h"
#include <libgo/fiber.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>

#define FIPSD_CONTROL_MAX_CONNECTIONS 32u

/* Pure resolution policy. Production probes directory existence once at start;
 * tests pass observations without creating/modifying canonical system paths. */
typedef struct fipsd_control_path_env {
    bool run_exists, var_run_exists, macos, freebsd, privileged;
    const char *xdg_runtime_dir;
} fipsd_control_path_env;
int fipsd_control_resolve_path(const char *override, const fipsd_control_path_env *env,
                              char *out, size_t cap);

/* Windows (upstream control/mod.rs windows_impl): the control socket is a TCP
 * listener on 127.0.0.1 and node.control.socket_path is its port, 21210 by
 * default. Pure: true when `value` parses as Rust's u16 does (an optional
 * '+', then decimal digits up to 65535); otherwise *port is the default and
 * the caller warns, as upstream falls back with a warning. NULL is the
 * default without a warning. Port 0 binds an ephemeral port. */
#define FIPSD_CONTROL_DEFAULT_PORT 21210u
bool fipsd_control_resolve_port(const char *value, uint16_t *port);

typedef struct fipsd_control_service {
    fipsd_subsystem subsystem;
    GoSwap *configs; /* borrowed; immutable configuration snapshots */
    fipsd_control_provider provider;
    fipsd_runtime *runtime;
    nsync_mu listener_mu; /* listener and accept_waiter lifetime; never held across I/O */
    gof_fiber_handle accept_waiter; /* protected by listener_mu; cleared before fiber exit */
    _Atomic unsigned connections; /* includes both connection and deadline fibers */
    gof_socket_t listener; /* accept fiber owns/ultimately closes it */
    gof_socket_t cancel_socket; /* POSIX: preallocated non-listening replacement, independent of pathname */
    char path[PATH_MAX]; /* POSIX: the socket path; Windows: "127.0.0.1:<port>" */
    uint16_t port; /* Windows: the bound port (the ephemeral one for port 0) */
    uint64_t socket_device, socket_inode;
    bool bound;
} fipsd_control_service;
void fipsd_control_service_init(fipsd_control_service *service, GoSwap *configs,
                               const fipsd_control_provider *provider);
/* After runtime has joined every fiber (or start failed). Removes only the
 * inode this service bound; never a subsequently replaced file/socket. */
void fipsd_control_service_clear(fipsd_control_service *service);
#endif
