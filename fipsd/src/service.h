#ifndef FIPSD_SERVICE_H
#define FIPSD_SERVICE_H
/* The Windows service (upstream src/bin/fips.rs, mod service): `fipsd
 * --service` under the service control manager, `--install-service` and
 * `--uninstall-service`. The status sequence the daemon reports is pure data
 * here, tested on every host; the SCM calls are in service_win32.c. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "runtime.h"

/* Upstream names the service "fips"; here it is the daemon's own name, as
 * the systemd unit (fipsd.service) and launchd job (com.fips.fipsd) are. */
#define FIPSD_SERVICE_NAME "fipsd"
#define FIPSD_SERVICE_DISPLAY_NAME "FIPS Mesh Network Daemon"
#define FIPSD_SERVICE_DESCRIPTION "Free Internetworking Peering System - distributed mesh networking protocol"
/* The only directory the service reads (upstream SYSTEM_CONFIG_DIR): its
 * configuration (unless -c or FIPS_CONFIG names another) and its log. */
#define FIPSD_SERVICE_CONFIG_DIR "C:\\ProgramData\\fips"
#define FIPSD_SERVICE_CONFIG FIPSD_SERVICE_CONFIG_DIR "\\fips.yaml"
#define FIPSD_SERVICE_LOG FIPSD_SERVICE_CONFIG_DIR "\\fips.log"

/* winsvc.h values, so the sequence is testable without it. */
enum {
    FIPSD_SVC_STOPPED = 1,
    FIPSD_SVC_START_PENDING = 2,
    FIPSD_SVC_STOP_PENDING = 3,
    FIPSD_SVC_RUNNING = 4,
    FIPSD_SVC_ACCEPT_STOP = 0x1,
    FIPSD_SVC_ACCEPT_SHUTDOWN = 0x4,
    FIPSD_SVC_SPECIFIC_ERROR = 1066 /* ERROR_SERVICE_SPECIFIC_ERROR */
};
/* Start-up is bounded by configuration and subsystem start (the TUN waits at
 * most 5 s for its interface); stop by the runtime's grace plus teardown. */
#define FIPSD_SVC_START_WAIT_MS 30000u
#define FIPSD_SVC_STOP_MARGIN_MS 10000u

/* One SetServiceStatus call's variable fields. */
typedef struct fipsd_service_report {
    uint32_t state, controls_accepted, win32_exit_code, specific_exit_code, checkpoint, wait_hint_ms;
} fipsd_service_report;

typedef struct fipsd_service_status {
    uint32_t state; /* last reported, 0 before the first */
    uint32_t checkpoint;
    uint32_t stop_wait_ms;
} fipsd_service_status;

void fipsd_service_status_init(fipsd_service_status *s, uint32_t grace_ms);
/* The report that moves the service to `state`: START_PENDING, RUNNING,
 * STOP_PENDING, STOPPED, in that order. A pending state may repeat, with the
 * next checkpoint; a step backwards (RUNNING after a stop began) or anything
 * after STOPPED is refused (false). Pending states accept no controls (the
 * SCM sends none then) and carry a wait hint; RUNNING accepts stop and
 * system shutdown; STOPPED carries `exit_code`, a non-zero one as a
 * service-specific error. */
bool fipsd_service_status_next(fipsd_service_status *s, uint32_t state, int exit_code,
                               fipsd_service_report *out);

/* How the daemon body lets its host see the runtime (main.c). */
typedef struct fipsd_daemon_hooks {
    void *ctx;
    bool handle_signals; /* console control events; off under the SCM */
    void (*attach)(void *ctx, fipsd_runtime *rt); /* created: a stop may cancel it now */
    void (*running)(void *ctx);                   /* every subsystem started */
    void (*detach)(void *ctx);                    /* about to be destroyed */
} fipsd_daemon_hooks;

#if defined(_WIN32)
typedef int (*fipsd_service_body)(void *arg, const fipsd_daemon_hooks *hooks);
/* StartServiceCtrlDispatcherW; returns the body's exit code. Outside the
 * SCM it fails at once with a hint (exit 1). */
int fipsd_service_run(fipsd_service_body body, void *arg);
/* Register (or repoint) the service at this executable with `--service`;
 * stop and delete it. Administrator; 0 or 1 with the reason on `err`. */
int fipsd_service_install(FILE *out, FILE *err);
int fipsd_service_uninstall(FILE *out, FILE *err);
#endif
#endif
