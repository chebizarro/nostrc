/* Windows TUN backend (nostrc-iq1n9.6.2): a Wintun 0.14 adapter configured
 * through IP Helper, behind the fipsd_tun_platform/fipsd_tun_ops seam so the
 * IPv6 shim runs unchanged (upstream ipv6tun/tun.rs windows_tun, winnet.rs).
 *
 * Adapter. wintun.dll is loaded at run time from the executable's directory
 * or System32 only (LoadLibraryExW search flags: never the working directory
 * or PATH). The adapter is named tun.name with tunnel type "FIPS" and a
 * system-chosen GUID (upstream's random UuidCreate GUID), so each daemon on a
 * host has its own adapter and `tun_name` reports the alias the host shows.
 * A name an interface in use already carries, or another Wintun adapter's
 * name, is refused (EEXIST) rather than created: Wintun would rename the
 * foreign adapter to make room. (A just-closed adapter, whose IP interfaces
 * are gone while Plug and Play finishes removing it, is not in use.) Closing the
 * adapter handle at destroy removes the adapter and with it every row.
 *
 * Receive. Wintun has no descriptor to wait on. A dedicated thread runs
 * Wintun's own loop (WintunReceivePacket until ERROR_NO_MORE_ITEMS, then
 * WaitForMultipleObjects on the session's read event and a quit event), copies
 * each packet, releases its ring slot at once and sends the copy into a
 * bounded GoChannel; a full channel blocks the thread, so overload backs up
 * into the 2 MiB ring and Wintun drops there, as a NIC queue does. The fiber
 * takes packets from the channel without blocking and, when there is none,
 * parks reading t->fd: one end of a connected loopback UDP pair (the
 * doorbell). The thread rings it only when the reader has said it is about
 * to park (`waiting`, ordered against the channel with seq_cst fences), so a
 * busy reader drains the channel with no syscalls, and the shim's
 * gof_io_reset_fd/gof_io_cancel_fd act on a real socket the IOCP poller
 * knows: cancelling ends the parked read with ECANCELED. Nothing polls.
 *
 * Send. WintunAllocateSendPacket/WintunSendPacket from the writer fiber; they
 * never block, and a full ring is ENOBUFS (the packet is dropped, counted by
 * the shim as a write error).
 *
 * Configuration (winnet.rs): wait, on IP Helper's interface notification, for
 * the adapter's IPv6 interface (at most 5 s) and turn duplicate address
 * detection and router discovery off on it (beyond winnet.rs: otherwise the
 * /128 is tentative, unusable, for its first second), then the node's /128,
 * the IPv6 MTU and the fd00::/8 on-link route (metric 256), each by LUID. An address or
 * route already present counts as done; a route this device did not create is
 * not removed at stop. Teardown deletes the route and the address, stops the
 * thread, ends the session and closes the adapter. */
#if defined(_WIN32)
#include "tun/internal.h"
#include "tun/wintun.h"
#include "tun/winnet.h"
#include "transport/sockos.h"
#include "log.h"
#include <libgo/fiber.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define MOD "tun"
#define PACKET_QUEUE 256u /* packets between the receive thread and the reader */
#if defined(__aarch64__) || defined(_M_ARM64)
#define WINTUN_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define WINTUN_ARCH "amd64"
#else
#define WINTUN_ARCH "x86"
#endif

/* ---------------- wintun.dll (wintun.h 0.14) ---------------- */
typedef void *wt_adapter;
typedef void *wt_session;
typedef enum { WT_LOG_INFO, WT_LOG_WARN, WT_LOG_ERR } wt_level;
typedef VOID(CALLBACK *wt_logger)(wt_level, DWORD64, LPCWSTR);
static struct {
    SRWLOCK lock;
    HMODULE dll;
    wt_adapter(WINAPI *create)(LPCWSTR, LPCWSTR, const GUID *);
    wt_adapter(WINAPI *open)(LPCWSTR);
    VOID(WINAPI *close)(wt_adapter);
    VOID(WINAPI *luid)(wt_adapter, uint64_t *); /* NET_LUID: one ULONG64 */
    DWORD(WINAPI *version)(VOID);
    VOID(WINAPI *set_logger)(wt_logger);
    wt_session(WINAPI *start)(wt_adapter, DWORD);
    VOID(WINAPI *end)(wt_session);
    HANDLE(WINAPI *event)(wt_session);
    BYTE *(WINAPI *receive)(wt_session, DWORD *);
    VOID(WINAPI *release)(wt_session, const BYTE *);
    BYTE *(WINAPI *allocate)(wt_session, DWORD);
    VOID(WINAPI *send)(wt_session, const BYTE *);
} api = {.lock = SRWLOCK_INIT};

/* Wintun's own log lines go to debug: the backend logs what failed itself. */
static VOID CALLBACK wintun_log(wt_level level, DWORD64 timestamp, LPCWSTR message)
{
    (void)timestamp;
    char text[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, message, -1, text, (int)sizeof text, NULL, NULL);
    if (n <= 0) memcpy(text, "?", 2);
    FIPSD_DEBUG(MOD, "wintun%s: %s", level == WT_LOG_ERR ? " error" : level == WT_LOG_WARN ? " warning" : "", text);
}
/* GetProcAddress gives FARPROC; through void (*)(void) the conversion to the
 * export's real type is a plain function-pointer cast. */
typedef void (*any_fn)(void);
static any_fn resolve(HMODULE dll, const char *name) { return (any_fn)(void (*)(void))GetProcAddress(dll, name); }
static uint32_t sys_load(void *u)
{
    (void)u;
    uint32_t code = 0;
    AcquireSRWLockExclusive(&api.lock);
    if (!api.dll) { /* a missing DLL is looked for again at the next start */
        HMODULE dll = LoadLibraryExW(L"wintun.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) code = GetLastError() ? GetLastError() : ERROR_MOD_NOT_FOUND;
        else {
#define RESOLVE(field, name) (*(any_fn *)(void *)&api.field = resolve(dll, name))
            if (!RESOLVE(create, "WintunCreateAdapter") || !RESOLVE(open, "WintunOpenAdapter") ||
                !RESOLVE(close, "WintunCloseAdapter") || !RESOLVE(luid, "WintunGetAdapterLUID") ||
                !RESOLVE(version, "WintunGetRunningDriverVersion") || !RESOLVE(set_logger, "WintunSetLogger") ||
                !RESOLVE(start, "WintunStartSession") || !RESOLVE(end, "WintunEndSession") ||
                !RESOLVE(event, "WintunGetReadWaitEvent") || !RESOLVE(receive, "WintunReceivePacket") ||
                !RESOLVE(release, "WintunReleaseReceivePacket") || !RESOLVE(allocate, "WintunAllocateSendPacket") ||
                !RESOLVE(send, "WintunSendPacket")) {
                code = ERROR_PROC_NOT_FOUND;
                (void)FreeLibrary(dll);
            } else {
                api.dll = dll; /* kept for the process: sessions may outlive any one device */
                api.set_logger(wintun_log);
            }
#undef RESOLVE
        }
    }
    ReleaseSRWLockExclusive(&api.lock);
    return code;
}
static uint32_t last_error(void) { DWORD e = GetLastError(); return e ? e : ERROR_GEN_FAILURE; }
static uint32_t sys_open_adapter(void *u, const uint16_t *name, void **out)
{ (void)u; *out = api.open((LPCWSTR)name); return *out ? 0 : last_error(); }
static uint32_t sys_create_adapter(void *u, const uint16_t *name, const uint16_t *type, void **out)
{ (void)u; *out = api.create((LPCWSTR)name, (LPCWSTR)type, NULL); return *out ? 0 : last_error(); }
static void sys_close_adapter(void *u, void *a) { (void)u; api.close(a); }
static uint64_t sys_adapter_luid(void *u, void *a) { (void)u; uint64_t luid = 0; api.luid(a, &luid); return luid; }
static uint32_t sys_driver_version(void *u) { (void)u; return api.version(); }
static uint32_t sys_start_session(void *u, void *a, uint32_t capacity, void **out)
{ (void)u; *out = api.start(a, capacity); return *out ? 0 : last_error(); }
static void sys_end_session(void *u, void *s) { (void)u; api.end(s); }
static void *sys_read_event(void *u, void *s) { (void)u; return api.event(s); }
static uint32_t sys_receive(void *u, void *s, uint8_t **packet, uint32_t *size)
{
    (void)u; DWORD n = 0;
    *packet = api.receive(s, &n); *size = n;
    return *packet ? 0 : last_error();
}
static void sys_release(void *u, void *s, const uint8_t *p) { (void)u; api.release(s, p); }
static uint32_t sys_allocate(void *u, void *s, uint32_t size, uint8_t **out)
{ (void)u; *out = api.allocate(s, size); return *out ? 0 : last_error(); }
static void sys_send(void *u, void *s, const uint8_t *p) { (void)u; api.send(s, p); }
static uint32_t sys_alias_in_use(void *u, const uint16_t *alias, bool *in_use) { (void)u; return fipsd_winnet_alias_in_use(alias, in_use); }
static uint32_t sys_ipv6_interface(void *u, uint64_t luid, uint32_t ms) { (void)u; return fipsd_winnet_ipv6_interface(luid, ms); }
static uint32_t sys_address(void *u, const fipsd_winnet_address *a, bool add) { (void)u; return fipsd_winnet_set_address(a, add); }
static uint32_t sys_mtu(void *u, uint64_t luid, uint32_t mtu) { (void)u; return fipsd_winnet_set_mtu(luid, mtu); }
static uint32_t sys_route(void *u, const fipsd_winnet_route *r, bool add) { (void)u; return fipsd_winnet_set_route(r, add); }
static const fipsd_tun_wintun_ops system_wintun = {.load = sys_load, .open_adapter = sys_open_adapter,
    .create_adapter = sys_create_adapter, .close_adapter = sys_close_adapter, .adapter_luid = sys_adapter_luid,
    .driver_version = sys_driver_version, .start_session = sys_start_session, .end_session = sys_end_session,
    .read_event = sys_read_event, .receive = sys_receive, .release = sys_release, .allocate = sys_allocate,
    .send = sys_send, .alias_in_use = sys_alias_in_use, .ipv6_interface = sys_ipv6_interface, .address = sys_address,
    .mtu = sys_mtu, .route = sys_route};
const fipsd_tun_wintun_ops *fipsd_tun_wintun_system_ops(void) { return &system_wintun; }

/* ---------------- fipsd_tun_ops on Windows ---------------- */
static int unsupported(void) { errno = ENOTSUP; return -1; }
static int sys_open(void *u, const char *p, int f) { (void)u; (void)p; (void)f; return unsupported(); }
static int sys_socket(void *u, int d, int t, int p) { (void)u; (void)d; (void)t; (void)p; return unsupported(); }
static int sys_ioctl(void *u, int f, unsigned long q, void *v) { (void)u; (void)f; (void)q; (void)v; return unsupported(); }
static int sys_connect(void *u, int f, const struct sockaddr *a, socklen_t n) { (void)u; (void)f; (void)a; (void)n; return unsupported(); }
static int sys_getsockopt(void *u, int f, int l, int q, void *v, socklen_t *n)
{ (void)u; (void)f; (void)l; (void)q; (void)v; (void)n; return unsupported(); }
/* Descriptors here are Winsock SOCKETs; kernel handle values fit in 32 bits. */
static gof_socket_t sock_of(int fd) { return (gof_socket_t)(uintptr_t)(unsigned)fd; }
static int sys_prepare(void *u, int fd) { (void)u; return fipsd_socket_prepare(sock_of(fd)); }
static int sys_route_socket(void *u) { (void)u; return unsupported(); }
static int sys_close(void *u, int fd)
{
    (void)u;
    if (closesocket(sock_of(fd))) { errno = gof_socket_errno(); return -1; }
    return 0;
}
/* The interface index of the adapter whose alias is `name`. */
static unsigned sys_index(void *u, const char *name)
{
    (void)u;
    uint16_t wide[FIPSD_WINTUN_NAME_MAX + 1];
    uint64_t luid = 0; uint32_t index = 0;
    if (!fipsd_winnet_wide(name, wide, sizeof wide / sizeof *wide) || fipsd_winnet_alias_luid(wide, &luid) ||
        fipsd_winnet_luid_index(luid, &index)) return 0;
    return index;
}
static int sys_name(void *u, int f, char *n, size_t z) { (void)u; (void)f; (void)n; (void)z; return unsupported(); }
static int sys_pid(void *u) { (void)u; return (int)GetCurrentProcessId(); }
static int sys_wait(void *u, int f, int ms) { (void)u; (void)f; (void)ms; return unsupported(); }
static ssize_t sys_read(void *u, int f, void *b, size_t n) { (void)u; (void)f; (void)b; (void)n; return unsupported(); }
static ssize_t sys_write(void *u, int f, const void *b, size_t n) { (void)u; (void)f; (void)b; (void)n; return unsupported(); }
static ssize_t fiber_read(void *u, int f, void *b, size_t n) { (void)u; return gof_read(sock_of(f), b, n); }
static ssize_t fiber_write(void *u, int f, const void *b, size_t n) { (void)u; return gof_write(sock_of(f), b, n); }
const fipsd_tun_ops *fipsd_tun_system_ops(void)
{
    static const fipsd_tun_ops o = {.open = sys_open, .socket = sys_socket, .ioctl = sys_ioctl,
        .connect = sys_connect, .getsockopt = sys_getsockopt, .prepare = sys_prepare, .route_socket = sys_route_socket,
        .close = sys_close, .ifindex = sys_index, .device_name = sys_name, .pid = sys_pid, .wait_read = sys_wait,
        .read = sys_read, .write = sys_write, .frame_read = fiber_read, .frame_write = fiber_write,
        .wintun = &system_wintun};
    return &o;
}

/* ---------------- the device ---------------- */
typedef struct frame { uint32_t len; uint8_t bytes[]; } frame;
typedef struct wintun_device {
    const fipsd_tun_wintun_ops *w;
    void *adapter, *session;
    uint64_t luid;
    HANDLE quit, thread;
    GoChannel *packets;   /* frame *, receive thread -> reader */
    SOCKET bell_tx;       /* the thread's end; t->fd is the reader's */
    _Atomic int waiting;  /* the reader is about to park on the doorbell */
    _Atomic uint64_t received, dropped;
    _Atomic uint32_t end_code; /* what ended the receive thread (0: a stop) */
    bool winsock;         /* this device's WSAStartup reference, for the doorbell */
} wintun_device;

/* A fiber may resume on another worker: errno is read and written through
 * these, never a cached TLS address (as tun.c). */
__attribute__((noinline)) static int frame_error(void) { return errno; }
__attribute__((noinline)) static void frame_set_error(int error) { errno = error; }

static void ring(wintun_device *d) { (void)send(d->bell_tx, "", 1, 0); }
static DWORD WINAPI receive_thread(void *arg)
{
    wintun_device *d = arg;
    const fipsd_tun_wintun_ops *w = d->w;
    HANDLE waits[2] = {w->read_event(w->user, d->session), d->quit};
    uint32_t end = 0;
    for (;;) {
        uint8_t *packet = NULL; uint32_t size = 0;
        uint32_t code = w->receive(w->user, d->session, &packet, &size);
        if (!code) {
            frame *f = malloc(sizeof *f + size);
            if (f) { f->len = size; memcpy(f->bytes, packet, size); }
            w->release(w->user, d->session, packet); /* the ring slot is free at once */
            if (!f) { atomic_fetch_add_explicit(&d->dropped, 1, memory_order_relaxed); continue; }
            if (go_channel_send(d->packets, f) != 0) { free(f); break; } /* closed: the device is stopping */
            atomic_fetch_add_explicit(&d->received, 1, memory_order_relaxed);
            atomic_thread_fence(memory_order_seq_cst); /* the send before the reader's flag */
            if (atomic_exchange_explicit(&d->waiting, 0, memory_order_seq_cst)) ring(d);
            continue;
        }
        if (code == ERROR_NO_MORE_ITEMS) {
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) continue;
            if (r != WAIT_OBJECT_0 + 1) end = last_error();
            break;
        }
        end = code; /* ERROR_HANDLE_EOF: the session is ending; ERROR_INVALID_DATA: a corrupt ring */
        break;
    }
    if (end) FIPSD_WARN(MOD, "Wintun receive ended (Win32 error %lu)", (unsigned long)end);
    atomic_store_explicit(&d->end_code, end, memory_order_relaxed);
    /* The reader takes what is buffered, then sees the end. */
    go_channel_close(d->packets);
    atomic_store_explicit(&d->waiting, 0, memory_order_seq_cst);
    ring(d);
    return 0;
}
static ssize_t take(frame *f, uint8_t *b, size_t cap)
{
    size_t n = f->len < cap ? f->len : cap; /* as upstream: a short buffer truncates */
    memcpy(b, f->bytes, n);
    free(f);
    return (ssize_t)n;
}
static ssize_t wintun_read(fipsd_tun *t, uint8_t *b, size_t cap)
{
    wintun_device *d = t->backend;
    if (!d) { frame_set_error(ENODEV); return -1; }
    for (;;) {
        void *f = NULL;
        if (go_channel_try_receive(d->packets, &f) == 0 && f) return take(f, b, cap);
        if (go_channel_is_closed(d->packets)) { /* every buffered packet has been taken */
            uint32_t end = atomic_load_explicit(&d->end_code, memory_order_relaxed);
            frame_set_error(end ? fipsd_winnet_errno(end) : ENODEV);
            return -1;
        }
        atomic_store_explicit(&d->waiting, 1, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst); /* the flag before the second look */
        if (go_channel_try_receive(d->packets, &f) == 0 && f) {
            atomic_store_explicit(&d->waiting, 0, memory_order_relaxed);
            return take(f, b, cap);
        }
        if (go_channel_is_closed(d->packets)) continue;
        /* A doorbell already queued by a ring that crossed the second look
         * only costs one more pass. Cancellation ends this read. */
        uint8_t bell[16];
        if (t->ops.frame_read(t->ops.user, t->fd, bell, sizeof bell) < 0) {
            int error = frame_error();
            atomic_store_explicit(&d->waiting, 0, memory_order_relaxed);
            frame_set_error(error);
            return -1;
        }
    }
}
static ssize_t wintun_write(fipsd_tun *t, const uint8_t *b, size_t n)
{
    wintun_device *d = t->backend;
    if (!d) { frame_set_error(ENODEV); return -1; }
    if (n > 0xffffu) { frame_set_error(EMSGSIZE); return -1; } /* WINTUN_MAX_IP_PACKET_SIZE */
    uint8_t *packet = NULL;
    uint32_t code = d->w->allocate(d->w->user, d->session, (uint32_t)n, &packet);
    if (code) { frame_set_error(fipsd_winnet_errno(code)); return -1; }
    memcpy(packet, b, n);
    d->w->send(d->w->user, d->session, packet);
    return (ssize_t)n;
}
/* A connected loopback UDP pair: datagrams from anywhere else are refused. */
static int doorbell(SOCKET *rx, SOCKET *tx)
{
    SOCKET s[2] = {INVALID_SOCKET, INVALID_SOCKET};
    struct sockaddr_in at[2];
    for (unsigned i = 0; i < 2; ++i) {
        int len = (int)sizeof at[i], one = 1;
        memset(&at[i], 0, sizeof at[i]);
        at[i].sin_family = AF_INET; at[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if ((s[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET ||
            setsockopt(s[i], SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one) ||
            bind(s[i], (struct sockaddr *)&at[i], sizeof at[i]) ||
            getsockname(s[i], (struct sockaddr *)&at[i], &len) || fipsd_socket_prepare(s[i])) goto fail;
    }
    if (connect(s[0], (struct sockaddr *)&at[1], sizeof at[1]) || connect(s[1], (struct sockaddr *)&at[0], sizeof at[0]))
        goto fail;
    *rx = s[0]; *tx = s[1];
    return 0;
fail:
    errno = gof_socket_errno();
    fipsd_socket_close(s[0]); fipsd_socket_close(s[1]);
    return -1;
}
static void device_free(wintun_device *d)
{
    if (d->thread) {
        (void)SetEvent(d->quit);
        go_channel_close(d->packets); /* wakes a thread blocked on a full channel */
        (void)WaitForSingleObject(d->thread, INFINITE);
        CloseHandle(d->thread);
    }
    if (d->packets) {
        void *f;
        while (go_channel_try_receive(d->packets, &f) == 0) free(f);
        go_channel_unref(d->packets);
    }
    fipsd_socket_close(d->bell_tx);
    ULONGLONG started = GetTickCount64();
    if (d->session) d->w->end_session(d->w->user, d->session);
    ULONGLONG ended = GetTickCount64();
    if (d->adapter) d->w->close_adapter(d->w->user, d->adapter); /* removes the created adapter */
    if (d->session || d->adapter)
        FIPSD_DEBUG(MOD, "Wintun session ended in %llu ms, adapter closed in %llu ms", ended - started, GetTickCount64() - ended);
    if (d->quit) CloseHandle(d->quit);
    if (d->winsock) (void)WSACleanup();
    free(d);
}
static int fail(wintun_device *d, SOCKET rx, int error)
{
    fipsd_socket_close(rx);
    device_free(d);
    errno = error;
    return -1;
}
static int open_wintun(fipsd_tun *t)
{
    const fipsd_tun_wintun_ops *w = t->ops.wintun;
    if (!w) { errno = ENOTSUP; return -1; }
    uint16_t name[FIPSD_WINTUN_NAME_MAX + 1], type[FIPSD_WINTUN_NAME_MAX + 1];
    if (!fipsd_winnet_wide(t->configured, name, sizeof name / sizeof *name) ||
        !fipsd_winnet_wide(FIPSD_WINTUN_TUNNEL_TYPE, type, sizeof type / sizeof *type)) { errno = EINVAL; return -1; }
    uint32_t code = w->load(w->user);
    if (code) {
        if (code == ERROR_MOD_NOT_FOUND)
            FIPSD_ERROR(MOD, "wintun.dll not found: put the " WINTUN_ARCH " wintun.dll from https://www.wintun.net/ (0.14.x) "
                "beside fipsd.exe");
        else if (code == ERROR_PROC_NOT_FOUND) FIPSD_ERROR(MOD, "wintun.dll does not export the Wintun 0.14 API");
        else FIPSD_ERROR(MOD, "loading wintun.dll failed (Win32 error %lu)", (unsigned long)code);
        errno = fipsd_winnet_errno(code); return -1;
    }
    bool taken = false; void *other = NULL;
    if ((!w->alias_in_use(w->user, name, &taken) && taken) || !w->open_adapter(w->user, name, &other)) {
        if (other) w->close_adapter(w->user, other); /* an opened adapter is kept */
        FIPSD_ERROR(MOD, "an adapter named %s already exists; set tun.name to a name no interface uses", t->configured);
        errno = EEXIST; return -1;
    }
    wintun_device *d = calloc(1, sizeof *d);
    if (!d) return -1;
    d->w = w; d->bell_tx = INVALID_SOCKET;
    SOCKET rx = INVALID_SOCKET;
    if ((code = w->create_adapter(w->user, name, type, &d->adapter))) {
        if (code == ERROR_ACCESS_DENIED || code == ERROR_ELEVATION_REQUIRED)
            FIPSD_ERROR(MOD, "creating Wintun adapter %s needs Administrator: run fipsd elevated", t->configured);
        else FIPSD_ERROR(MOD, "creating Wintun adapter %s failed (Win32 error %lu)", t->configured, (unsigned long)code);
        d->adapter = NULL;
        return fail(d, rx, fipsd_winnet_errno(code));
    }
    d->luid = w->adapter_luid(w->user, d->adapter);
    if ((code = w->start_session(w->user, d->adapter, FIPSD_WINTUN_RING_CAPACITY, &d->session))) {
        FIPSD_ERROR(MOD, "starting the Wintun session on %s failed (Win32 error %lu)", t->configured, (unsigned long)code);
        d->session = NULL;
        return fail(d, rx, fipsd_winnet_errno(code));
    }
    if ((code = w->ipv6_interface(w->user, d->luid, FIPSD_WINNET_IPV6_WAIT_MS))) {
        if (code == ERROR_TIMEOUT)
            FIPSD_ERROR(MOD, "adapter %s has no IPv6 interface after %u ms; is IPv6 enabled on it?", t->configured,
                FIPSD_WINNET_IPV6_WAIT_MS);
        else FIPSD_ERROR(MOD, "configuring the IPv6 interface of %s failed (Win32 error %lu)", t->configured, (unsigned long)code);
        return fail(d, rx, fipsd_winnet_errno(code));
    }
    if (!(d->quit = CreateEventW(NULL, TRUE, FALSE, NULL)) || !(d->packets = go_channel_create(PACKET_QUEUE)))
        return fail(d, rx, ENOMEM);
    /* Counted, so the device does not lean on libgo's poller having started. */
    WSADATA winsock;
    int rc = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (rc) { FIPSD_ERROR(MOD, "WSAStartup failed (%d)", rc); return fail(d, rx, EIO); }
    d->winsock = true;
    if (doorbell(&rx, &d->bell_tx)) {
        int error = errno;
        FIPSD_ERROR(MOD, "the receive doorbell for %s failed: %s", t->configured, strerror(error));
        return fail(d, INVALID_SOCKET, error);
    }
    if (!(d->thread = CreateThread(NULL, 0, receive_thread, d, 0, NULL))) {
        FIPSD_ERROR(MOD, "the Wintun receive thread for %s failed (Win32 error %lu)", t->configured, (unsigned long)last_error());
        return fail(d, rx, EAGAIN);
    }
    uint32_t version = w->driver_version ? w->driver_version(w->user) : 0;
    FIPSD_INFO(MOD, "Wintun adapter %s (driver %lu.%lu, ring %u KiB)", t->configured,
        (unsigned long)(version >> 16), (unsigned long)(version & 0xffffu), FIPSD_WINTUN_RING_CAPACITY / 1024u);
    memcpy(t->name, t->configured, sizeof t->name);
    t->backend = d;
    return (int)(unsigned)(uintptr_t)rx;
}
static int setup_error(fipsd_tun *t, const char *what, uint32_t code)
{
    FIPSD_ERROR(MOD, "%s on %s failed (Win32 error %lu)", what, t->name, (unsigned long)code);
    errno = fipsd_winnet_errno(code);
    return -1;
}
static int address(fipsd_tun *t, bool add)
{
    wintun_device *d = t->backend; fipsd_winnet_address row;
    if (!d || !fipsd_winnet_node_address(d->luid, t->address.bytes, &row)) { errno = EINVAL; return -1; }
    uint32_t code = d->w->address(d->w->user, &row, add);
    if (fipsd_winnet_outcome_of(code, add) == FIPSD_WINNET_FAILED)
        return setup_error(t, add ? "adding the node address" : "removing the node address", code);
    return 0;
}
static int mtu(fipsd_tun *t)
{
    wintun_device *d = t->backend;
    if (!d) { errno = ENODEV; return -1; }
    uint32_t code = d->w->mtu(d->w->user, d->luid, t->mtu);
    return code ? setup_error(t, "setting the IPv6 MTU", code) : 0;
}
/* A Wintun adapter is up while its session runs. */
static int up(fipsd_tun *t, bool raise) { (void)t; (void)raise; return 0; }
static int route(fipsd_tun *t, bool add)
{
    wintun_device *d = t->backend; fipsd_winnet_route row;
    if (!d || !fipsd_winnet_mesh_route(d->luid, &row)) { errno = ENODEV; return -1; }
    uint32_t code = d->w->route(d->w->user, &row, add);
    fipsd_winnet_outcome outcome = fipsd_winnet_outcome_of(code, add);
    if (outcome == FIPSD_WINNET_FAILED)
        return setup_error(t, add ? "adding route fd00::/8" : "removing route fd00::/8", code);
    if (add && outcome == FIPSD_WINNET_DONE) t->routed = true; /* only a route this device made is removed */
    return 0;
}
static int destroy(fipsd_tun *t)
{
    if (t->backend) { device_free(t->backend); t->backend = NULL; }
    return 0;
}
const fipsd_tun_platform *fipsd_tun_native_platform(void)
{
    static const fipsd_tun_platform p = {.os = FIPSD_TUN_WINDOWS, .family_header = 0, .open = open_wintun,
        .address = address, .mtu = mtu, .up = up, .route = route, .destroy = destroy,
        .frame_read = wintun_read, .frame_write = wintun_write};
    return &p;
}
fipsd_tun_wintun_stats fipsd_tun_wintun_stats_read(const fipsd_tun *t)
{
    const wintun_device *d = t ? t->backend : NULL;
    if (!d) return (fipsd_tun_wintun_stats){0};
    return (fipsd_tun_wintun_stats){.received = atomic_load_explicit(&d->received, memory_order_relaxed),
        .dropped = atomic_load_explicit(&d->dropped, memory_order_relaxed),
        .end_code = atomic_load_explicit(&d->end_code, memory_order_relaxed)};
}
uint64_t fipsd_tun_wintun_luid(const fipsd_tun *t)
{
    const wintun_device *d = t ? t->backend : NULL;
    return d ? d->luid : 0;
}
#endif
