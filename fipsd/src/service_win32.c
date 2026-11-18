/* fipsd as a Windows service (upstream src/bin/fips.rs, mod service, over the
 * windows-service crate): the service main registers a control handler,
 * reports START_PENDING, runs the same daemon body as a console run with
 * console control events off, and reports RUNNING once every subsystem has
 * started. SERVICE_CONTROL_STOP and SERVICE_CONTROL_SHUTDOWN report
 * STOP_PENDING and cancel the runtime (fipsd_runtime_shutdown), which stops
 * the subsystems in reverse within its grace; the service main then reports
 * STOPPED with the daemon's exit code. Upstream reports RUNNING at once and
 * STOPPED at the end; the pending states and their checkpoints and wait
 * hints are this port's, as the SCM documents them (service.c). */
#include "service.h"

#include "log.h"

#include <windows.h>
#include <winsvc.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MODULE "service"
#define WIDE2(s) L##s
#define WIDE(s) WIDE2(s)

_Static_assert(FIPSD_SVC_STOPPED == SERVICE_STOPPED && FIPSD_SVC_START_PENDING == SERVICE_START_PENDING &&
               FIPSD_SVC_STOP_PENDING == SERVICE_STOP_PENDING && FIPSD_SVC_RUNNING == SERVICE_RUNNING,
               "service states match winsvc.h");
_Static_assert(FIPSD_SVC_ACCEPT_STOP == SERVICE_ACCEPT_STOP && FIPSD_SVC_ACCEPT_SHUTDOWN == SERVICE_ACCEPT_SHUTDOWN &&
               FIPSD_SVC_SPECIFIC_ERROR == ERROR_SERVICE_SPECIFIC_ERROR, "service codes match winsvc.h");

/* One service per process. `lock` orders the control handler (on the
 * dispatcher's thread) against the service main's thread: the status
 * sequence and the runtime pointer, which is valid between attach and
 * detach. fipsd_runtime_shutdown() only cancels a context, so it is called
 * under the lock. */
static struct {
    SERVICE_STATUS_HANDLE handle;
    SRWLOCK lock;
    fipsd_service_status status;
    fipsd_runtime *rt;
    bool stop_requested;
    fipsd_service_body body;
    void *arg;
    int exit_code;
} svc = {.lock = SRWLOCK_INIT, .exit_code = 1};

static void report_locked(uint32_t state, int exit_code)
{
    fipsd_service_report r;
    if (!fipsd_service_status_next(&svc.status, state, exit_code, &r)) return;
    SERVICE_STATUS status = {
        .dwServiceType = SERVICE_WIN32_OWN_PROCESS, .dwCurrentState = r.state,
        .dwControlsAccepted = r.controls_accepted, .dwWin32ExitCode = r.win32_exit_code,
        .dwServiceSpecificExitCode = r.specific_exit_code, .dwCheckPoint = r.checkpoint,
        .dwWaitHint = r.wait_hint_ms};
    if (!SetServiceStatus(svc.handle, &status))
        FIPSD_WARN(MODULE, "SetServiceStatus(state %u) failed (error %lu)", (unsigned)r.state, GetLastError());
}

static void report(uint32_t state, int exit_code)
{
    AcquireSRWLockExclusive(&svc.lock);
    report_locked(state, exit_code);
    ReleaseSRWLockExclusive(&svc.lock);
}

/* Runs on the dispatcher's thread and must return at once. */
static DWORD WINAPI control_handler(DWORD control, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type; (void)data; (void)ctx;
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        FIPSD_INFO(MODULE, "%s requested; shutting down",
                   control == SERVICE_CONTROL_STOP ? "service stop" : "system shutdown");
        AcquireSRWLockExclusive(&svc.lock);
        svc.stop_requested = true;
        report_locked(FIPSD_SVC_STOP_PENDING, 0);
        if (svc.rt) fipsd_runtime_shutdown(svc.rt);
        ReleaseSRWLockExclusive(&svc.lock);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void on_attach(void *ctx, fipsd_runtime *rt)
{
    (void)ctx;
    AcquireSRWLockExclusive(&svc.lock);
    svc.rt = rt;
    if (svc.stop_requested) fipsd_runtime_shutdown(rt);
    else report_locked(FIPSD_SVC_START_PENDING, 0); /* configuration loaded: next checkpoint */
    ReleaseSRWLockExclusive(&svc.lock);
}

static void on_running(void *ctx)
{
    (void)ctx;
    report(FIPSD_SVC_RUNNING, 0);
    FIPSD_INFO(MODULE, "running as the %s service", FIPSD_SERVICE_NAME);
}

static void on_detach(void *ctx)
{
    (void)ctx;
    AcquireSRWLockExclusive(&svc.lock);
    svc.rt = NULL;
    report_locked(FIPSD_SVC_STOP_PENDING, 0); /* a daemon that ends on its own is stopping too */
    ReleaseSRWLockExclusive(&svc.lock);
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;
    svc.handle = RegisterServiceCtrlHandlerExW(WIDE(FIPSD_SERVICE_NAME), control_handler, NULL);
    if (!svc.handle) return; /* nothing to report to; the dispatcher returns */
    fipsd_service_status_init(&svc.status, FIPSD_RUNTIME_DEFAULT_GRACE_MS);
    report(FIPSD_SVC_START_PENDING, 0);
    fipsd_daemon_hooks hooks = {.ctx = NULL, .handle_signals = false,
                                .attach = on_attach, .running = on_running, .detach = on_detach};
    svc.exit_code = svc.body(svc.arg, &hooks);
    /* The SCM may end the process once it sees STOPPED: nothing after this. */
    report(FIPSD_SVC_STOPPED, svc.exit_code);
}

int fipsd_service_run(fipsd_service_body body, void *arg)
{
    svc.body = body;
    svc.arg = arg;
    SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)WIDE(FIPSD_SERVICE_NAME), service_main}, {NULL, NULL}};
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD e = GetLastError();
        if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            fputs("fipsd: --service is for the service control manager; run fipsd without it in a console,\n"
                  "or install the service with --install-service and start it with sc.exe start " FIPSD_SERVICE_NAME "\n", stderr);
        else
            fprintf(stderr, "fipsd: cannot start the service dispatcher (error %lu)\n", e);
        return 1;
    }
    return svc.exit_code;
}

/* ── install / uninstall (upstream install_service, uninstall_service) ─────── */

/* "<system message> (os error <n>)", as std::io::Error prints it. */
static void scm_error(FILE *err, const char *what, DWORD code)
{
    char text[512] = "";
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), text, (DWORD)sizeof text, NULL);
    while (n && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' ')) text[--n] = 0;
    fprintf(err, "%s: %s (os error %lu)\n", what, n ? text : "unknown error", code);
}

static void print_wide(FILE *out, const wchar_t *text)
{
    char utf8[4 * MAX_PATH];
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, (int)sizeof utf8, NULL, NULL) > 0) fputs(utf8, out);
}

int fipsd_service_install(FILE *out, FILE *err)
{
    wchar_t exe[32768];
    DWORD n = GetModuleFileNameW(NULL, exe, (DWORD)(sizeof exe / sizeof *exe));
    if (!n || n >= sizeof exe / sizeof *exe) { scm_error(err, "Failed to install service", GetLastError()); return 1; }
    static wchar_t command[32768 + 16];
    swprintf(command, sizeof command / sizeof *command, L"\"%ls\" --service", exe);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) { scm_error(err, "Failed to install service (Administrator required)", GetLastError()); return 1; }
    bool existed = false;
    SC_HANDLE service = CreateServiceW(scm, WIDE(FIPSD_SERVICE_NAME), WIDE(FIPSD_SERVICE_DISPLAY_NAME),
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, command, NULL, NULL, NULL, NULL /* LocalSystem */, NULL);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
        /* Repoint a registration made from another folder at this binary,
         * keeping the operator's start type and account (upstream repoint_service). */
        fprintf(out, "Service '%s' already exists, updating configuration...\n", FIPSD_SERVICE_NAME);
        existed = true;
        service = OpenServiceW(scm, WIDE(FIPSD_SERVICE_NAME), SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
        static union { QUERY_SERVICE_CONFIGW config; unsigned char bytes[8192]; } before;
        DWORD need = 0;
        QUERY_SERVICE_CONFIGW *old = &before.config;
        bool have_old = service && QueryServiceConfigW(service, old, (DWORD)sizeof before, &need);
        if (service && !ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_NO_CHANGE,
                SERVICE_ERROR_NORMAL, command, NULL, NULL, NULL, NULL, NULL, WIDE(FIPSD_SERVICE_DISPLAY_NAME))) {
            scm_error(err, "Failed to update the existing service", GetLastError());
            CloseServiceHandle(service); CloseServiceHandle(scm); return 1;
        }
        if (service && have_old && old->lpBinaryPathName && wcscmp(old->lpBinaryPathName, command)) {
            fputs("  Binary path was: ", out); print_wide(out, old->lpBinaryPathName); fputc('\n', out);
            fputs("  Binary path now: ", out); print_wide(out, command); fputc('\n', out);
        }
    }
    if (!service) {
        scm_error(err, "Failed to create service", GetLastError());
        CloseServiceHandle(scm); return 1;
    }
    SERVICE_DESCRIPTIONW description = {(LPWSTR)WIDE(FIPSD_SERVICE_DESCRIPTION)};
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description))
        scm_error(err, "Warning: could not set service description", GetLastError());
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    fprintf(out, existed ? "Service '%s' updated.\n" : "Service '%s' installed successfully.\n", FIPSD_SERVICE_NAME);
    fprintf(out, "Start it with: sc.exe start %s\n\n", FIPSD_SERVICE_NAME);
    fprintf(out, "Configuration: the service reads %s (or FIPS_CONFIG)\n", FIPSD_SERVICE_CONFIG);
    fputs("  keep hosts, peers.allow and peers.deny beside it.\n", out);
    fprintf(out, "Logs: the service writes %s\n", FIPSD_SERVICE_LOG);
    fputs("Control: fipsctl and fipstop reach it on TCP 127.0.0.1:21210.\n", out);
    return 0;
}

/* Callback for NotifyServiceStatusChangeW, run as an APC in the alertable
 * wait below; it marks its own notification (another APC may end the wait). */
static VOID CALLBACK status_changed(PVOID parameter)
{
    SERVICE_NOTIFYW *notify = parameter;
    *(volatile LONG *)notify->pContext = 1;
}

/* Wait for STOPPED on the SCM's own notification, bounded by the stop wait
 * hint the service reports: no polling. `notify` is the caller's and must
 * stay valid until the service handle is closed, which cancels a
 * notification still registered after a timeout. */
static bool wait_stopped(SC_HANDLE service, SERVICE_NOTIFYW *notify, volatile LONG *fired, DWORD timeout_ms)
{
    *fired = 0;
    *notify = (SERVICE_NOTIFYW){.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE,
                                .pfnNotifyCallback = status_changed, .pContext = (PVOID)fired};
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    if (NotifyServiceStatusChangeW(service, SERVICE_NOTIFY_STOPPED, notify) != ERROR_SUCCESS) return false;
    while (!*fired) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        (void)SleepEx((DWORD)(deadline - now), TRUE);
    }
    return notify->dwNotificationStatus == ERROR_SUCCESS &&
           notify->ServiceStatus.dwCurrentState == SERVICE_STOPPED;
}

int fipsd_service_uninstall(FILE *out, FILE *err)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { scm_error(err, "Failed to uninstall service (Administrator required)", GetLastError()); return 1; }
    SC_HANDLE service = OpenServiceW(scm, WIDE(FIPSD_SERVICE_NAME), SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) {
        scm_error(err, "Failed to uninstall service", GetLastError());
        CloseServiceHandle(scm); return 1;
    }
    SERVICE_STATUS status;
    SERVICE_NOTIFYW notify; /* outlives any registration: the handle closes below */
    volatile LONG fired = 0;
    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        fputs("Stopping service...\n", out);
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            scm_error(err, "Warning: could not stop the service", GetLastError());
        else if (!wait_stopped(service, &notify, &fired, FIPSD_RUNTIME_DEFAULT_GRACE_MS + FIPSD_SVC_STOP_MARGIN_MS))
            fputs("Warning: the service has not stopped yet; it is removed once it does.\n", err);
    }
    bool ok = DeleteService(service) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    if (!ok) scm_error(err, "Failed to uninstall service", GetLastError());
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    if (!ok) return 1;
    fprintf(out, "Service '%s' uninstalled.\n", FIPSD_SERVICE_NAME);
    return 0;
}
