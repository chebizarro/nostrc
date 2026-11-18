/* The BlueZ backend (nostrc-gfi8h.6.2): upstream transport/ble/io_linux.rs
 * over the contract in backend.h.
 *
 * Control plane: BlueZ over D-Bus through GDBus. The backend owns a private
 * system-bus connection, driven by a GLib main loop on its own thread. It is
 * private, so closing it releases what BlueZ holds for this client (the
 * advertisement and the discovery session), and losing the bus never exits
 * the daemon. Everything on the bus happens on that thread:
 *   - the adapter: the org.bluez name watch, ObjectManager InterfacesAdded/
 *     InterfacesRemoved and Adapter1 PropertiesChanged give presence, power
 *     and the address. The adapter is powered on whenever it appears
 *     (upstream BluerIo::new); a later power-off is the operator's and is
 *     reported, never undone;
 *   - advertising: an exported LEAdvertisement1 (FIPS UUID, ServiceData
 *     {0x9C90: psm LE}, 400-600 ms) registered with LEAdvertisingManager1;
 *   - discovery: SetDiscoveryFilter {le, FIPS UUID} + StartDiscovery. A FIPS
 *     device object (UUIDs with the FIPS UUID, or ServiceData with the 0x9C90
 *     key: once BlueZ's GATT client has resolved a device's services, UUIDs
 *     lists those and no longer the advertised ones) is PEER_DISCOVERED once
 *     per discovery session, and again when its PSM service data changes;
 *     its InterfacesRemoved is PEER_LOST.
 *     At scan start, cached objects holding no user state are removed first
 *     and the kept FIPS ones are reported once (upstream start_scanning and
 *     bluer's discover_devices).
 * Fiber-side operations hand a request to that thread with
 * g_main_context_invoke and park on the reply with their deadline and the
 * stop channel (radio.c's bridge). The thread reports through the sink only;
 * it never runs transport code.
 *
 * Data plane: kernel L2CAP CoC sockets (AF_BLUETOOTH, SOCK_SEQPACKET) on
 * fibers, through libgo I/O. listen binds the adapter's address at the PSM and
 * runs a tracked accept fiber; connect dials from the adapter's address with a
 * nonblocking connect bounded by the deadline; every channel is an fd stream
 * on the core's fast path, so its SDUs are the core's reads. A dead channel is
 * seen by the core's own reader, so this backend never needs STREAM_CLOSED.
 *
 * Errors keep backend.h's PSM rule. The kernel ends an unanswered LE dial with
 * ECONNREFUSED when the channel timer fires, the errno of a real refusal; the
 * timer is the socket's SO_SNDTIMEO, so a dial sets it past its deadline and
 * the deadline (ETIMEDOUT) always ends an unanswered dial first
 * (fipsd_ble_bluez_connect_errno). */
#define _POSIX_C_SOURCE 200809L
#include "transport/ble/bluez.h"
#include "transport/ble/internal.h"
#include "runtime.h"
#include "log.h"
#include <gio/gio.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#define MOD "ble"
#define BZ_DEVICES_MAX 1024u   /* device objects mirrored per adapter */
#define BZ_QUIET_CALL_MS 5000  /* best-effort calls nobody waits for */
#define BZ_RETRY_MIN_MS 1000u  /* system bus reconnect backoff */
#define BZ_RETRY_MAX_MS 30000u
#define BZ_BACKLOG 8           /* BLE_INBOUND_INFLIGHT */
#define BZ_ACCEPT_BACKOFF_MAX_MS 5000u
#define BZ_BLUEZ "org.bluez"
#define BZ_ADAPTER1 "org.bluez.Adapter1"
#define BZ_DEVICE1 "org.bluez.Device1"
#define BZ_ADV_MANAGER1 "org.bluez.LEAdvertisingManager1"
#define BZ_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define BZ_PROPERTIES "org.freedesktop.DBus.Properties"
static const char adv_xml[] =
    "<node><interface name='org.bluez.LEAdvertisement1'><method name='Release'/>"
    "<property name='Type' type='s' access='read'/><property name='ServiceUUIDs' type='as' access='read'/>"
    "<property name='ServiceData' type='a{sv}' access='read'/><property name='MinInterval' type='u' access='read'/>"
    "<property name='MaxInterval' type='u' access='read'/></interface></node>";

enum { REQ_ADVERTISE, REQ_ADVERTISE_STOP, REQ_SCAN_START, REQ_SCAN_STOP };
typedef struct bz bz;
typedef struct bz_req {
    _Atomic int refs;
    int kind, stage, rc;
    bz *b;
    GoChannel *reply;
    uint64_t deadline;
    fipsd_ble_advert advert;
} bz_req;
typedef struct bz_device {
    fipsd_ble_bluez_props props; /* merged from every update */
    bool reported, clearing;     /* reported this discovery session; RemoveDevice in flight */
    uint8_t reported_len, reported_data[FIPSD_BLE_SERVICE_DATA_MAX];
} bz_device;
typedef struct bz_listener { bz *b; int fd; GoChannel *done; } bz_listener;
typedef struct bz_dial { struct bz_dial *next; int fd; } bz_dial;
typedef struct bz_stream { fipsd_ble_stream base; int fd; uint16_t send_mtu, recv_mtu; } bz_stream;
struct bz {
    char adapter[FIPSD_BLE_ADAPTER_MAX], path[FIPSD_BLE_ADAPTER_MAX + 12], object[FIPSD_BLE_ADAPTER_MAX + 48];
    uint16_t mtu;
    fipsd_runtime *rt;
    GoChannel *stop_ch;
    _Atomic uint64_t next_id;
    bool thread_ok; /* written by start, before any caller */
    nsync_mu mu;    /* guards the block below; never held across a park */
    fipsd_ble_sink *sink;
    bool stopped, present, powered, have_address;
    uint8_t address[6];
    GHashTable *devices; /* device object path -> bz_device, written by the GLib thread */
    bz_listener *listener;
    bz_dial *dials;
    /* the GLib thread's own */
    GMainContext *ctx;
    GMainLoop *loop;
    pthread_t thread;
    GCancellable *cancel; /* cancelled at quit: every call still in flight */
    GDBusNodeInfo *info;
    GDBusConnection *bus;
    GSource *retry;
    char *owner; /* BlueZ's unique name: the only caller of our Release */
    guint watch, sub_added, sub_removed, sub_props, object_id;
    unsigned retry_ms, pending;
    bool quitting, power_requested, advertising, scanning, warned_bus, warned_devices;
    fipsd_ble_advert adv; /* what the advertisement object serves */
    GQueue adv_q, scan_q;
    bool adv_busy, scan_busy;
};
static uint64_t now_ms(void) { return ble_now(); }
/* Reacquire TLS errno after a migrating fiber resumes (stream_read.c). */
__attribute__((noinline)) static int io_errno(void) { return errno; }

/* ---- shared state ------------------------------------------------------------ */
static bool is_stopped(bz *b) { nsync_mu_lock(&b->mu); bool s = b->stopped; nsync_mu_unlock(&b->mu); return s; }
static bool adapter_up(bz *b)
{ nsync_mu_lock(&b->mu); bool u = b->present && b->powered && !b->stopped; nsync_mu_unlock(&b->mu); return u; }
/* GLib thread: the adapter facts to the core (coalesced by the sink). */
static void report_adapter(bz *b)
{
    nsync_mu_lock(&b->mu);
    fipsd_ble_sink *sink = b->stopped ? NULL : b->sink;
    bool present = b->present, powered = b->powered;
    nsync_mu_unlock(&b->mu);
    if (sink) fipsd_ble_sink_adapter(sink, present, powered);
}
/* A discovery session ended (scan stop, power-off, the adapter or BlueZ gone):
 * every device is new to the next one. GLib thread, mu held. */
static void forget_reports_locked(bz *b)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, b->devices);
    while (g_hash_table_iter_next(&it, &k, &v)) ((bz_device *)v)->reported = false;
}

/* ---- requests: fiber -> GLib thread ------------------------------------------------ */
static void req_unref(bz_req *q)
{
    if (atomic_fetch_sub(&q->refs, 1) != 1) return;
    if (q->reply) go_channel_unref(q->reply);
    free(q);
}
static bz_req *req_new(bz *b, int kind)
{
    bz_req *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->b = b; q->kind = kind; atomic_init(&q->refs, 2); /* the caller's and the GLib thread's */
    if (!(q->reply = go_channel_create(1))) { free(q); return NULL; }
    return q;
}
static void answer(bz_req *q, int rc)
{
    q->rc = rc;
    (void)go_channel_try_send(q->reply, q);
    req_unref(q);
}
/* A D-Bus timeout for work done on behalf of `q`: what its caller has left,
 * at least a second, so a late reply still settles the backend's state. */
static int call_timeout(const bz_req *q)
{
    if (!q || q->deadline == FIPSD_BLE_NO_DEADLINE) return -1; /* GDBus default */
    uint64_t now = now_ms(), left = q->deadline > now ? q->deadline - now : 0;
    if (left < 1000) left = 1000;
    return left > INT_MAX ? INT_MAX : (int)left;
}
static gboolean dispatch(gpointer data);
static int call(bz *b, bz_req *q, uint64_t deadline)
{
    if (!q) return -ENOMEM;
    q->deadline = deadline;
    if (!b->thread_ok || go_channel_is_closed(b->stop_ch)) { req_unref(q); return -ECANCELED; }
    g_main_context_invoke_full(b->ctx, G_PRIORITY_DEFAULT, dispatch, q, NULL);
    void *v = NULL;
    GoSelectCase cases[] = {{GO_SELECT_RECEIVE, q->reply, NULL, &v}, {GO_SELECT_RECEIVE, b->stop_ch, NULL, &v}};
    int selected;
    if (deadline == FIPSD_BLE_NO_DEADLINE) selected = go_select(cases, 2);
    else {
        uint64_t now = now_ms();
        selected = go_select_timeout(cases, 2, deadline > now ? deadline - now : 0).selected_case;
    }
    return selected == 0 ? q->rc : selected == 1 ? -ECANCELED : -ETIMEDOUT; /* the caller then drops its reference */
}
static int simple(bz *b, int kind, const fipsd_ble_advert *ad, uint64_t deadline)
{
    bz_req *q = req_new(b, kind);
    if (q && ad) q->advert = *ad;
    int rc = call(b, q, deadline);
    if (q) req_unref(q);
    return rc;
}

/* ---- D-Bus calls (GLib thread) ---------------------------------------------------------- */
typedef void (*bz_done)(bz *, bz_req *, GVariant *reply, const GError *);
typedef struct bz_call { bz *b; bz_req *q; bz_done done; } bz_call;
static void call_finished(GObject *src, GAsyncResult *res, gpointer data)
{
    bz_call *c = data;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    --c->b->pending;
    if (c->done) c->done(c->b, c->q, r, e);
    if (r) g_variant_unref(r);
    if (e) g_error_free(e);
    g_free(c);
}
/* Every call completes through `done` (also without a bus), in this thread;
 * `pending` counts them so teardown can wait for the cancelled ones. */
static void bluez_call(bz *b, const char *path, const char *iface, const char *method, GVariant *params,
    const GVariantType *reply_type, int timeout_ms, bz_req *q, bz_done done)
{
    if (!b->bus || b->quitting) {
        if (params) g_variant_unref(g_variant_ref_sink(params));
        GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "not connected to BlueZ");
        if (done) done(b, q, NULL, e);
        g_error_free(e);
        return;
    }
    bz_call *c = g_new0(bz_call, 1);
    c->b = b; c->q = q; c->done = done;
    ++b->pending;
    g_dbus_connection_call(b->bus, BZ_BLUEZ, path, iface, method, params, reply_type, G_DBUS_CALL_FLAGS_NONE,
        timeout_ms, b->cancel, call_finished, c);
}
static void log_failure(bz *b, const char *what, const GError *e)
{
    if (e && !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        FIPSD_WARN(MOD, "%s: BlueZ %s failed: %s", b->adapter, what, e->message);
}
static void quiet_done(bz *b, bz_req *q, GVariant *r, const GError *e) { (void)b; (void)q; (void)r; (void)e; }

/* ---- devices and discovery ---------------------------------------------------------- */
/* Merge an update; report the device when it is a FIPS peer not yet reported
 * this session, or its PSM service data changed (a restarted listener). */
static void device_update(bz *b, const char *path, GVariant *props)
{
    fipsd_ble_peer peer = {{0}, {0}};
    if (!fipsd_ble_bluez_device_from_path(path, b->adapter, peer.device)) return;
    memcpy(peer.adapter, b->adapter, sizeof peer.adapter);
    fipsd_ble_bluez_props u;
    fipsd_ble_bluez_parse_props(props, &u);
    nsync_mu_lock(&b->mu);
    bz_device *d = g_hash_table_lookup(b->devices, path);
    if (!d) {
        if (g_hash_table_size(b->devices) >= BZ_DEVICES_MAX) {
            bool warn = !b->warned_devices; b->warned_devices = true;
            nsync_mu_unlock(&b->mu);
            if (warn) FIPSD_WARN(MOD, "%s: BlueZ holds more than %u devices; ignoring the rest", b->adapter, BZ_DEVICES_MAX);
            return;
        }
        d = g_new0(bz_device, 1);
        memcpy(d->props.address, peer.device, 6);
        g_hash_table_insert(b->devices, g_strdup(path), d);
    }
    fipsd_ble_bluez_merge_props(&d->props, &u);
    const fipsd_ble_bluez_props *p = &d->props;
    if (b->scanning && !b->stopped && b->sink && p->fips && !d->clearing &&
        (!d->reported || d->reported_len != p->psm_data_len || memcmp(d->reported_data, p->psm_data, p->psm_data_len))) {
        d->reported = true; d->reported_len = p->psm_data_len;
        memcpy(d->reported_data, p->psm_data, sizeof d->reported_data);
        int16_t rssi = p->rssi;
        bool has_rssi = (p->fields & FIPSD_BLE_BLUEZ_RSSI) != 0;
        /* The sink never calls back into the backend: safe under mu. */
        (void)fipsd_ble_sink_discovered(b->sink, &peer, has_rssi ? &rssi : NULL, p->psm_data_len ? p->psm_data : NULL,
            p->psm_data_len);
    }
    nsync_mu_unlock(&b->mu);
}
static void device_removed(bz *b, const char *path)
{
    fipsd_ble_peer peer = {{0}, {0}};
    if (!fipsd_ble_bluez_device_from_path(path, b->adapter, peer.device)) return;
    memcpy(peer.adapter, b->adapter, sizeof peer.adapter);
    nsync_mu_lock(&b->mu);
    bz_device *d = g_hash_table_lookup(b->devices, path);
    if (d && d->reported && b->scanning && !b->stopped && b->sink) (void)fipsd_ble_sink_lost(b->sink, &peer);
    (void)g_hash_table_remove(b->devices, path);
    nsync_mu_unlock(&b->mu);
}
/* BlueZ (or the adapter) is gone: so is everything it held for us. */
static void forget_adapter(bz *b)
{
    nsync_mu_lock(&b->mu);
    b->present = b->powered = false;
    g_hash_table_remove_all(b->devices);
    nsync_mu_unlock(&b->mu);
    b->advertising = b->scanning = false;
    b->power_requested = false;
}
static void power_done(bz *b, bz_req *q, GVariant *r, const GError *e) { (void)q; (void)r; log_failure(b, "power-on", e); }
static void adapter_update(bz *b, GVariant *props, bool appeared)
{
    fipsd_ble_bluez_props p;
    fipsd_ble_bluez_parse_props(props, &p);
    nsync_mu_lock(&b->mu);
    if (appeared) b->present = true;
    if (p.fields & FIPSD_BLE_BLUEZ_POWERED) b->powered = p.powered;
    if (p.fields & FIPSD_BLE_BLUEZ_ADDRESS) { memcpy(b->address, p.address, 6); b->have_address = true; }
    bool up = b->present && b->powered;
    if (!up) forget_reports_locked(b);
    nsync_mu_unlock(&b->mu);
    if (!up) b->scanning = false; /* BlueZ ends discovery with the power */
    if (appeared && (p.fields & FIPSD_BLE_BLUEZ_POWERED) && !p.powered && !b->power_requested) {
        b->power_requested = true;
        FIPSD_INFO(MOD, "%s: powering the adapter on", b->adapter);
        bluez_call(b, b->path, BZ_PROPERTIES, "Set", g_variant_new("(ssv)", BZ_ADAPTER1, "Powered", g_variant_new_boolean(TRUE)),
            NULL, BZ_QUIET_CALL_MS, NULL, power_done);
    }
    report_adapter(b);
}
static bool strv_has(GVariant *as, const char *s)
{
    gsize n = 0;
    const gchar **v = g_variant_get_strv(as, &n);
    bool found = false;
    for (gsize i = 0; i < n && !found; ++i) found = !strcmp(v[i], s);
    g_free(v);
    return found;
}
static void interfaces_added(bz *b, const char *path, GVariant *ifaces)
{
    GVariant *props;
    if (!strcmp(path, b->path) && (props = g_variant_lookup_value(ifaces, BZ_ADAPTER1, G_VARIANT_TYPE_VARDICT))) {
        adapter_update(b, props, true);
        g_variant_unref(props);
    } else if ((props = g_variant_lookup_value(ifaces, BZ_DEVICE1, G_VARIANT_TYPE_VARDICT))) {
        device_update(b, path, props);
        g_variant_unref(props);
    }
}
static void on_added(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal,
    GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)signal;
    const char *object;
    GVariant *ifaces;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(oa{sa{sv}})"))) return;
    g_variant_get(params, "(&o@a{sa{sv}})", &object, &ifaces);
    interfaces_added(data, object, ifaces);
    g_variant_unref(ifaces);
}
static void on_removed(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal,
    GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)signal;
    bz *b = data;
    const char *object;
    GVariant *ifaces;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(oas)"))) return;
    g_variant_get(params, "(&o@as)", &object, &ifaces);
    if (!strcmp(object, b->path) && strv_has(ifaces, BZ_ADAPTER1)) {
        FIPSD_INFO(MOD, "%s: adapter removed", b->adapter);
        forget_adapter(b);
        report_adapter(b);
    } else if (strv_has(ifaces, BZ_DEVICE1)) device_removed(b, object);
    g_variant_unref(ifaces);
}
static void on_props(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *signal,
    GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)iface; (void)signal;
    bz *b = data;
    const char *which;
    GVariant *changed;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(sa{sv}as)"))) return;
    g_variant_get(params, "(&s@a{sv}@as)", &which, &changed, NULL);
    if (!strcmp(which, BZ_ADAPTER1) && !strcmp(path, b->path)) adapter_update(b, changed, false);
    else if (!strcmp(which, BZ_DEVICE1)) device_update(b, path, changed);
    g_variant_unref(changed);
}
static void objects_loaded(bz *b, bz_req *q, GVariant *r, const GError *e)
{
    (void)q;
    if (r) {
        GVariant *objects = g_variant_get_child_value(r, 0), *ifaces;
        GVariantIter it;
        const char *path;
        g_variant_iter_init(&it, objects);
        while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            interfaces_added(b, path, ifaces);
            g_variant_unref(ifaces);
        }
        g_variant_unref(objects);
    } else log_failure(b, "GetManagedObjects", e);
    nsync_mu_lock(&b->mu);
    bool present = b->present;
    nsync_mu_unlock(&b->mu);
    if (!present) FIPSD_INFO(MOD, "%s: BlueZ has no such adapter", b->adapter);
    report_adapter(b); /* known now, present or not */
}

/* ---- the advertisement object -------------------------------------------------------- */
static void adv_method(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *method,
    GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    (void)c; (void)path; (void)iface; (void)params;
    bz *b = data;
    if (!strcmp(method, "Release") && b->owner && sender && !strcmp(sender, b->owner)) {
        /* BlueZ dropped it on its own (adapter gone, daemon shutdown). */
        b->advertising = false;
        FIPSD_DEBUG(MOD, "%s: BlueZ released the advertisement", b->adapter);
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.AccessDenied", "not BlueZ");
}
static GVariant *adv_get(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *name,
    GError **error, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface;
    GVariant *v = fipsd_ble_bluez_advert_property(&((bz *)data)->adv, name);
    if (!v) g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", name);
    return v;
}
static const GDBusInterfaceVTable adv_vtable = {.method_call = adv_method, .get_property = adv_get};

/* ---- advertising: one operation at a time ----------------------------------------------- */
static void adv_kick(bz *b);
static void adv_finish(bz *b, int rc)
{
    bz_req *q = g_queue_pop_head(&b->adv_q);
    b->adv_busy = false;
    if (q) answer(q, rc);
    adv_kick(b);
}
static void adv_register(bz *b, bz_req *q);
static void adv_unregistered(bz *b, bz_req *q, GVariant *r, const GError *e)
{
    (void)r; (void)e; /* gone either way: removed now, or BlueZ never had it */
    b->advertising = false;
    if (q->kind == REQ_ADVERTISE) adv_register(b, q);
    else adv_finish(b, 0);
}
static void adv_unregister(bz *b, bz_req *q)
{
    bluez_call(b, b->path, BZ_ADV_MANAGER1, "UnregisterAdvertisement", g_variant_new("(o)", b->object), NULL,
        call_timeout(q), q, adv_unregistered);
}
static void adv_registered(bz *b, bz_req *q, GVariant *r, const GError *e)
{
    (void)r;
    int rc = fipsd_ble_bluez_gerror_errno(e);
    if (!rc) {
        b->advertising = true;
        FIPSD_DEBUG(MOD, "%s: advertising psm 0x%04x", b->adapter, b->adv.psm);
        if (is_stopped(b)) { /* stop ran while BlueZ was registering it */
            b->advertising = false;
            bluez_call(b, b->path, BZ_ADV_MANAGER1, "UnregisterAdvertisement", g_variant_new("(o)", b->object), NULL,
                BZ_QUIET_CALL_MS, NULL, quiet_done);
        }
    } else if (rc == -EEXIST && !q->stage) {
        q->stage = 1; /* BlueZ holds an older registration: replace it, once */
        b->advertising = true;
        adv_unregister(b, q);
        return;
    } else log_failure(b, "RegisterAdvertisement", e);
    adv_finish(b, rc);
}
static void adv_register(bz *b, bz_req *q)
{
    b->adv = q->advert;
    bluez_call(b, b->path, BZ_ADV_MANAGER1, "RegisterAdvertisement", g_variant_new("(oa{sv})", b->object, NULL), NULL,
        call_timeout(q), q, adv_registered);
}
static void adv_kick(bz *b)
{
    bz_req *q = b->adv_busy ? NULL : g_queue_peek_head(&b->adv_q);
    if (!q) return;
    b->adv_busy = true;
    if (is_stopped(b)) { adv_finish(b, -ECANCELED); return; }
    if (q->kind == REQ_ADVERTISE_STOP) {
        if (b->advertising) adv_unregister(b, q);
        else adv_finish(b, 0);
        return;
    }
    if (!adapter_up(b) || !b->bus) { adv_finish(b, -ENODEV); return; }
    /* The same PSM does not restart the advertiser. */
    if (b->advertising && b->adv.psm == q->advert.psm && !memcmp(b->adv.service_data, q->advert.service_data, 2)) {
        adv_finish(b, 0);
        return;
    }
    if (b->advertising) adv_unregister(b, q); /* then registers the new one */
    else adv_register(b, q);
}

/* ---- discovery: one operation at a time ---------------------------------------------------- */
static void scan_kick(bz *b);
static void scan_finish(bz *b, int rc)
{
    bz_req *q = g_queue_pop_head(&b->scan_q);
    b->scan_busy = false;
    if (q) answer(q, rc);
    scan_kick(b);
}
static void scan_stopped(bz *b, bz_req *q, GVariant *r, const GError *e) { (void)q; (void)r; (void)e; scan_finish(b, 0); }
static void scan_started(bz *b, bz_req *q, GVariant *r, const GError *e)
{
    (void)q; (void)r;
    int rc = fipsd_ble_bluez_gerror_errno(e);
    if (rc == -EALREADY) rc = 0; /* this client is already discovering */
    if (rc) { log_failure(b, "StartDiscovery", e); scan_finish(b, rc); return; }
    b->scanning = true;
    /* The kept FIPS objects once, as bluer's discover_devices does. */
    nsync_mu_lock(&b->mu);
    forget_reports_locked(b);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, b->devices);
    GPtrArray *kept = g_ptr_array_new_with_free_func(g_free);
    while (g_hash_table_iter_next(&it, &k, &v))
        if (((bz_device *)v)->props.fips && !((bz_device *)v)->clearing) g_ptr_array_add(kept, g_strdup(k));
    nsync_mu_unlock(&b->mu);
    for (guint i = 0; i < kept->len; ++i) device_update(b, g_ptr_array_index(kept, i), NULL);
    g_ptr_array_unref(kept);
    FIPSD_DEBUG(MOD, "%s: scanning for the FIPS service", b->adapter);
    scan_finish(b, 0);
}
static void scan_filtered(bz *b, bz_req *q, GVariant *r, const GError *e)
{
    (void)r;
    int rc = fipsd_ble_bluez_gerror_errno(e);
    if (rc) { log_failure(b, "SetDiscoveryFilter", e); scan_finish(b, rc); return; }
    bluez_call(b, b->path, BZ_ADAPTER1, "StartDiscovery", NULL, NULL, call_timeout(q), q, scan_started);
}
/* RemoveDevice for a cached object; on failure it stays, unmarked, and is
 * reported like a kept one. */
typedef struct bz_removal { bz *b; char *path; } bz_removal;
static void device_cleared(GObject *src, GAsyncResult *res, gpointer data)
{
    bz_removal *m = data;
    bz *b = m->b;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    --b->pending;
    if (r) g_variant_unref(r); /* removed: its InterfacesRemoved follows */
    if (e) {
        nsync_mu_lock(&b->mu);
        bz_device *d = g_hash_table_lookup(b->devices, m->path);
        if (d) d->clearing = false;
        nsync_mu_unlock(&b->mu);
        if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) device_update(b, m->path, NULL);
        g_error_free(e);
    }
    g_free(m->path);
    g_free(m);
}
static void remove_device(bz *b, const char *path)
{
    nsync_mu_lock(&b->mu);
    bz_device *d = g_hash_table_lookup(b->devices, path);
    if (d) d->clearing = true;
    nsync_mu_unlock(&b->mu);
    bz_removal *m = g_new0(bz_removal, 1);
    m->b = b; m->path = g_strdup(path);
    ++b->pending;
    g_dbus_connection_call(b->bus, BZ_BLUEZ, b->path, BZ_ADAPTER1, "RemoveDevice", g_variant_new("(o)", path), NULL,
        G_DBUS_CALL_FLAGS_NONE, BZ_QUIET_CALL_MS, b->cancel, device_cleared, m);
}
static void scan_objects(bz *b, bz_req *q, GVariant *r, const GError *e)
{
    if (!r) { int rc = fipsd_ble_bluez_gerror_errno(e); log_failure(b, "GetManagedObjects", e); scan_finish(b, rc); return; }
    /* Upstream start_scanning: drop cached objects that hold no user state,
     * so their next advert is a fresh InterfacesAdded with fresh data. */
    GVariant *objects = g_variant_get_child_value(r, 0), *ifaces, *props;
    GVariantIter it;
    const char *path;
    uint8_t device[6];
    g_variant_iter_init(&it, objects);
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        if (fipsd_ble_bluez_device_from_path(path, b->adapter, device) &&
            (props = g_variant_lookup_value(ifaces, BZ_DEVICE1, G_VARIANT_TYPE_VARDICT))) {
            device_update(b, path, props);
            fipsd_ble_bluez_props p;
            fipsd_ble_bluez_parse_props(props, &p);
            g_variant_unref(props);
            if (fipsd_ble_bluez_clear_at_scan_start(&p)) remove_device(b, path);
        }
        g_variant_unref(ifaces);
    }
    g_variant_unref(objects);
    GVariantBuilder f;
    g_variant_builder_init(&f, G_VARIANT_TYPE_VARDICT);
    const char *uuids[] = {FIPSD_BLE_SERVICE_UUID};
    g_variant_builder_add(&f, "{sv}", "Transport", g_variant_new_string("le"));
    g_variant_builder_add(&f, "{sv}", "UUIDs", g_variant_new_strv(uuids, 1));
    /* No PropertiesChanged for every repeat of unchanged service data. */
    g_variant_builder_add(&f, "{sv}", "DuplicateData", g_variant_new_boolean(FALSE));
    bluez_call(b, b->path, BZ_ADAPTER1, "SetDiscoveryFilter", g_variant_new("(a{sv})", &f), NULL, call_timeout(q), q,
        scan_filtered);
}
static void scan_kick(bz *b)
{
    bz_req *q = b->scan_busy ? NULL : g_queue_peek_head(&b->scan_q);
    if (!q) return;
    b->scan_busy = true;
    if (is_stopped(b)) { scan_finish(b, -ECANCELED); return; }
    if (q->kind == REQ_SCAN_STOP) {
        if (!b->scanning) { scan_finish(b, 0); return; }
        b->scanning = false;
        nsync_mu_lock(&b->mu); forget_reports_locked(b); nsync_mu_unlock(&b->mu);
        bluez_call(b, b->path, BZ_ADAPTER1, "StopDiscovery", NULL, NULL, BZ_QUIET_CALL_MS, q, scan_stopped);
        return;
    }
    if (!adapter_up(b) || !b->bus) { scan_finish(b, -ENODEV); return; }
    if (b->scanning) { scan_finish(b, 0); return; }
    bluez_call(b, "/", BZ_OBJECT_MANAGER, "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), call_timeout(q), q,
        scan_objects);
}

/* ---- the system bus ---------------------------------------------------------------- */
static void bus_open(bz *b);
static gboolean retry_fire(gpointer data)
{
    bz *b = data;
    g_source_unref(b->retry);
    b->retry = NULL;
    bus_open(b);
    return G_SOURCE_REMOVE;
}
static void schedule_retry(bz *b)
{
    if (b->quitting || b->retry) return;
    b->retry_ms = b->retry_ms ? (b->retry_ms * 2 > BZ_RETRY_MAX_MS ? BZ_RETRY_MAX_MS : b->retry_ms * 2) : BZ_RETRY_MIN_MS;
    b->retry = g_timeout_source_new(b->retry_ms);
    g_source_set_callback(b->retry, retry_fire, b, NULL);
    (void)g_source_attach(b->retry, b->ctx);
}
static void bluez_appeared(GDBusConnection *c, const gchar *name, const gchar *owner, gpointer data)
{
    (void)c; (void)name;
    bz *b = data;
    g_free(b->owner);
    b->owner = g_strdup(owner);
    FIPSD_DEBUG(MOD, "%s: BlueZ is %s", b->adapter, owner);
    bluez_call(b, "/", BZ_OBJECT_MANAGER, "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), -1, NULL,
        objects_loaded);
}
static void bluez_vanished(GDBusConnection *c, const gchar *name, gpointer data)
{
    (void)c; (void)name;
    bz *b = data;
    if (b->owner) FIPSD_WARN(MOD, "%s: bluetoothd left the system bus", b->adapter);
    else FIPSD_WARN(MOD, "%s: bluetoothd is not running", b->adapter);
    g_free(b->owner);
    b->owner = NULL;
    forget_adapter(b);
    report_adapter(b);
}
static void bus_release(bz *b)
{
    if (!b->bus) return;
    if (b->watch) g_bus_unwatch_name(b->watch);
    guint *subs[] = {&b->sub_added, &b->sub_removed, &b->sub_props};
    for (size_t i = 0; i < 3; ++i) if (*subs[i]) g_dbus_connection_signal_unsubscribe(b->bus, *subs[i]);
    if (b->object_id) (void)g_dbus_connection_unregister_object(b->bus, b->object_id);
    b->watch = b->sub_added = b->sub_removed = b->sub_props = b->object_id = 0;
    (void)g_signal_handlers_disconnect_matched(b->bus, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, b);
}
static void bus_closed(GDBusConnection *c, gboolean remote, GError *e, gpointer data)
{
    (void)c; (void)remote;
    bz *b = data;
    if (b->quitting) return;
    FIPSD_WARN(MOD, "%s: the system bus connection closed (%s); reconnecting", b->adapter, e ? e->message : "closed");
    bus_release(b);
    g_object_unref(b->bus);
    b->bus = NULL;
    g_free(b->owner);
    b->owner = NULL;
    forget_adapter(b);
    report_adapter(b);
    schedule_retry(b);
}
static void bus_opened(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    bz *b = data;
    GError *e = NULL;
    GDBusConnection *c = g_dbus_connection_new_for_address_finish(res, &e);
    --b->pending;
    if (!c) {
        if (!b->quitting) {
            if (!b->warned_bus) FIPSD_WARN(MOD, "%s: cannot reach the system bus (%s); retrying", b->adapter, e->message);
            b->warned_bus = true;
            report_adapter(b);
            schedule_retry(b);
        }
        g_error_free(e);
        return;
    }
    if (b->quitting) { (void)g_dbus_connection_close_sync(c, NULL, NULL); g_object_unref(c); return; }
    b->bus = c; b->retry_ms = 0; b->warned_bus = false;
    g_dbus_connection_set_exit_on_close(c, FALSE);
    (void)g_signal_connect(c, "closed", G_CALLBACK(bus_closed), b);
    /* Subscribed before the first GetManagedObjects, so nothing falls between. */
    b->sub_added = g_dbus_connection_signal_subscribe(c, BZ_BLUEZ, BZ_OBJECT_MANAGER, "InterfacesAdded", "/", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_added, b, NULL);
    b->sub_removed = g_dbus_connection_signal_subscribe(c, BZ_BLUEZ, BZ_OBJECT_MANAGER, "InterfacesRemoved", "/", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_removed, b, NULL);
    b->sub_props = g_dbus_connection_signal_subscribe(c, BZ_BLUEZ, BZ_PROPERTIES, "PropertiesChanged", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_props, b, NULL);
    b->object_id = g_dbus_connection_register_object(c, b->object, b->info->interfaces[0], &adv_vtable, b, NULL, &e);
    if (!b->object_id) {
        FIPSD_ERROR(MOD, "%s: cannot export the advertisement object: %s", b->adapter, e->message);
        g_clear_error(&e);
    }
    b->watch = g_bus_watch_name_on_connection(c, BZ_BLUEZ, G_BUS_NAME_WATCHER_FLAGS_NONE, bluez_appeared, bluez_vanished,
        b, NULL);
}
static void bus_open(bz *b)
{
    if (b->quitting) return;
    GError *e = NULL;
    gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, b->cancel, &e);
    if (!address) {
        if (!b->warned_bus) FIPSD_WARN(MOD, "%s: no system bus address (%s); retrying", b->adapter, e->message);
        b->warned_bus = true;
        g_error_free(e);
        report_adapter(b);
        schedule_retry(b);
        return;
    }
    ++b->pending;
    g_dbus_connection_new_for_address(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, b->cancel,
        bus_opened, b);
    g_free(address);
}

/* ---- the GLib thread ---------------------------------------------------------------- */
static gboolean dispatch(gpointer data)
{
    bz_req *q = data;
    bz *b = q->b;
    if (b->quitting) { answer(q, -ECANCELED); return G_SOURCE_REMOVE; }
    if (q->kind == REQ_ADVERTISE || q->kind == REQ_ADVERTISE_STOP) { g_queue_push_tail(&b->adv_q, q); adv_kick(b); }
    else { g_queue_push_tail(&b->scan_q, q); scan_kick(b); }
    return G_SOURCE_REMOVE;
}
/* stop(): withdraw the advertisement and end discovery, without waiting. */
static gboolean on_stop(gpointer data)
{
    bz *b = data;
    if (b->advertising) {
        b->advertising = false;
        bluez_call(b, b->path, BZ_ADV_MANAGER1, "UnregisterAdvertisement", g_variant_new("(o)", b->object), NULL,
            BZ_QUIET_CALL_MS, NULL, quiet_done);
    }
    if (b->scanning) {
        b->scanning = false;
        bluez_call(b, b->path, BZ_ADAPTER1, "StopDiscovery", NULL, NULL, BZ_QUIET_CALL_MS, NULL, quiet_done);
    }
    return G_SOURCE_REMOVE;
}
static gboolean on_quit(gpointer data)
{
    bz *b = data;
    b->quitting = true;
    g_main_loop_quit(b->loop);
    return G_SOURCE_REMOVE;
}
static void drain(GQueue *q) { bz_req *r; while ((r = g_queue_pop_head(q))) answer(r, -ECANCELED); }
static void *glib_thread(void *arg)
{
    bz *b = arg;
    g_main_context_push_thread_default(b->ctx);
    bus_open(b);
    g_main_loop_run(b->loop);
    /* Quit: let what stop sent reach the bus, then cancel the rest and wait
     * for every call to complete, so nothing outlives the backend. */
    if (b->bus) (void)g_dbus_connection_flush_sync(b->bus, NULL, NULL);
    g_cancellable_cancel(b->cancel);
    if (b->retry) { g_source_destroy(b->retry); g_source_unref(b->retry); b->retry = NULL; }
    bus_release(b);
    while (b->pending) (void)g_main_context_iteration(b->ctx, TRUE);
    drain(&b->adv_q); drain(&b->scan_q);
    if (b->bus) {
        (void)g_dbus_connection_close_sync(b->bus, NULL, NULL);
        g_object_unref(b->bus);
        b->bus = NULL;
    }
    while (g_main_context_iteration(b->ctx, FALSE)) {}
    g_main_context_pop_thread_default(b->ctx);
    return NULL;
}

/* ---- streams: one L2CAP CoC channel, always on the fd path ------------------------------ */
static ssize_t s_read(fipsd_ble_stream *s, uint8_t *buf, size_t cap, uint64_t deadline)
{ return ble_io_read(s, ((bz_stream *)s)->fd, buf, cap, deadline); }
static int s_write(fipsd_ble_stream *s, const uint8_t *buf, size_t len, uint64_t deadline)
{ return ble_io_write(s, ((bz_stream *)s)->fd, buf, len, deadline); }
static void s_cancel(fipsd_ble_stream *s) { (void)gof_io_cancel_fd(((bz_stream *)s)->fd); }
static void s_close(fipsd_ble_stream *s)
{
    bz_stream *z = (bz_stream *)s;
    (void)gof_io_reset_fd(z->fd); /* a cancelled latch must not outlive the descriptor */
    (void)close(z->fd);
    free(z);
}
static void s_mtu(const fipsd_ble_stream *s, uint16_t *send, uint16_t *recv)
{ const bz_stream *z = (const bz_stream *)s; *send = z->send_mtu; *recv = z->recv_mtu; }
static int s_fd(const fipsd_ble_stream *s) { return ((const bz_stream *)s)->fd; }
static const fipsd_ble_stream_ops stream_ops = {.read = s_read, .write = s_write, .cancel = s_cancel, .close = s_close,
    .mtu = s_mtu, .fd = s_fd};
static uint16_t sock_mtu(int fd, int opt)
{
    uint16_t v = 0;
    socklen_t n = sizeof v;
    return getsockopt(fd, FIPSD_BT_SOL_BLUETOOTH, opt, &v, &n) || n != sizeof v ? 0 : v;
}
static fipsd_ble_stream *stream_new(bz *b, int fd, const uint8_t device[6])
{
    bz_stream *z = calloc(1, sizeof *z);
    if (!z) return NULL;
    int fl = fcntl(fd, F_GETFL);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK)) { free(z); return NULL; }
    z->fd = fd;
    z->send_mtu = sock_mtu(fd, FIPSD_BT_SNDMTU); /* the peer's MTU */
    z->recv_mtu = sock_mtu(fd, FIPSD_BT_RCVMTU); /* ours (transports.ble mtu) */
    z->base.ops = &stream_ops;
    z->base.id = atomic_fetch_add(&b->next_id, 1) + 1;
    memcpy(z->base.peer.adapter, b->adapter, sizeof z->base.peer.adapter);
    memcpy(z->base.peer.device, device, 6);
    (void)gof_io_reset_fd(fd);
    return &z->base;
}
/* A SEQPACKET L2CAP socket bound to the adapter's address at `psm` (0 for a
 * dial), with this backend's options: security low (no pairing; FIPS
 * authenticates the peer itself), receive MTU, active mode. The kernel takes
 * BT_RCVMTU only once the socket is bound to an LE address. */
static int l2cap_open(bz *b, const uint8_t local[6], uint16_t psm)
{
    int fd = socket(FIPSD_BT_AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, FIPSD_BT_PROTO_L2CAP);
    if (fd < 0) return -io_errno();
    fipsd_l2cap_sockaddr sa;
    fipsd_ble_bluez_sockaddr(local, FIPSD_BT_ADDR_LE_PUBLIC, psm, &sa);
    fipsd_bt_security sec = {FIPSD_BT_SECURITY_LOW, 0};
    uint16_t mtu = b->mtu;
    int e = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) e = io_errno();
    else if (setsockopt(fd, FIPSD_BT_SOL_BLUETOOTH, FIPSD_BT_SECURITY, &sec, sizeof sec)) e = io_errno();
    else if (setsockopt(fd, FIPSD_BT_SOL_BLUETOOTH, FIPSD_BT_RCVMTU, &mtu, sizeof mtu)) e = io_errno();
    if (e) {
        (void)close(fd);
        return -e;
    }
    fipsd_bt_power power = {1}; /* upstream set_power_forced_active: no sniff mode (BR/EDR only) */
    (void)setsockopt(fd, FIPSD_BT_SOL_BLUETOOTH, FIPSD_BT_POWER, &power, sizeof power);
    return fd;
}
static int remaining_ms(uint64_t deadline)
{
    if (deadline == FIPSD_BLE_NO_DEADLINE) return -1;
    uint64_t now = now_ms(), left = deadline > now ? deadline - now : 1;
    return left > INT_MAX ? INT_MAX : (int)left;
}

/* ---- the listener --------------------------------------------------------------------------- */
static void accept_fiber(void *arg)
{
    bz_listener *l = arg;
    bz *b = l->b;
    unsigned backoff = 0;
    for (;;) {
        fipsd_l2cap_sockaddr sa;
        memset(&sa, 0, sizeof sa);
        socklen_t len = sizeof sa;
        int c = gof_accept(l->fd, (struct sockaddr *)&sa, &len, -1);
        int e = c < 0 ? io_errno() : 0;
        nsync_mu_lock(&b->mu);
        bool gone = b->stopped || b->listener != l;
        fipsd_ble_sink *sink = b->sink;
        nsync_mu_unlock(&b->mu);
        if (c >= 0 && gone) { (void)close(c); break; }
        if (c < 0) {
            if (e == ECANCELED || gone) break;
            if (e == EINTR || e == ECONNABORTED || e == EPROTO || e == EAGAIN) continue;
            if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM) {
                if (!backoff) FIPSD_WARN(MOD, "%s: L2CAP accept is out of resources (%d); backing off", b->adapter, -e);
                backoff = backoff ? (backoff * 2 > BZ_ACCEPT_BACKOFF_MAX_MS ? BZ_ACCEPT_BACKOFF_MAX_MS : backoff * 2) : 100;
                void *v;
                GoSelectCase cases[] = {{GO_SELECT_RECEIVE, b->stop_ch, NULL, &v}};
                if (go_select_timeout(cases, 1, backoff).selected_case == 0) break;
                continue;
            }
            FIPSD_ERROR(MOD, "%s: L2CAP accept failed (%d); the listener is closed", b->adapter, -e);
            break;
        }
        backoff = 0;
        uint8_t device[6];
        fipsd_ble_bluez_sockaddr_device(&sa, device);
        fipsd_ble_stream *s = stream_new(b, c, device);
        if (!s) { (void)close(c); continue; }
        (void)fipsd_ble_sink_inbound(sink, s); /* ownership passes, delivered or not */
    }
    nsync_mu_lock(&b->mu); /* the descriptor dies under mu: stop and listen cancel only live ones */
    (void)gof_io_reset_fd(l->fd);
    (void)close(l->fd);
    l->fd = -1;
    if (b->listener == l) b->listener = NULL;
    nsync_mu_unlock(&b->mu);
    go_channel_close(l->done);
    go_channel_unref(l->done);
    free(l);
}
static int b_listen(void *self, uint16_t requested, uint64_t deadline, uint16_t *bound)
{
    bz *b = self;
    if (!requested) return -EINVAL;
    /* A previous listener releases its PSM before the new bind. */
    nsync_mu_lock(&b->mu);
    bz_listener *old = b->listener;
    b->listener = NULL;
    GoChannel *old_done = old ? go_channel_ref(old->done) : NULL;
    if (old && old->fd >= 0) (void)gof_io_cancel_fd(old->fd);
    nsync_mu_unlock(&b->mu);
    if (old_done) {
        void *v;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, old_done, NULL, &v}, {GO_SELECT_RECEIVE, b->stop_ch, NULL, &v}};
        int selected;
        if (deadline == FIPSD_BLE_NO_DEADLINE) selected = go_select(cases, 2);
        else { uint64_t now = now_ms(); selected = go_select_timeout(cases, 2, deadline > now ? deadline - now : 0).selected_case; }
        go_channel_unref(old_done);
        if (selected != 0) return selected == 1 ? -ECANCELED : -ETIMEDOUT;
    }
    nsync_mu_lock(&b->mu);
    bool stopped = b->stopped, up = b->present && b->powered && b->have_address;
    uint8_t address[6];
    memcpy(address, b->address, 6);
    nsync_mu_unlock(&b->mu);
    if (stopped) return -ECANCELED;
    if (!up) return -ENODEV;
    int fd = l2cap_open(b, address, requested);
    if (fd >= 0 && listen(fd, BZ_BACKLOG)) { int e = io_errno(); (void)close(fd); fd = -e; }
    if (fd < 0) {
        FIPSD_DEBUG(MOD, "%s: L2CAP listen on psm 0x%04x failed (%d)", b->adapter, requested, fd);
        return fd == -EAFNOSUPPORT ? -ENODEV : fd;
    }
    bz_listener *l = calloc(1, sizeof *l);
    if (l && !(l->done = go_channel_create(1))) { free(l); l = NULL; }
    if (!l) { (void)close(fd); return -ENOMEM; }
    l->b = b; l->fd = fd;
    (void)gof_io_reset_fd(fd);
    nsync_mu_lock(&b->mu);
    stopped = b->stopped;
    if (!stopped) b->listener = l;
    nsync_mu_unlock(&b->mu);
    if (stopped || fipsd_runtime_spawn(b->rt, accept_fiber, l)) {
        nsync_mu_lock(&b->mu);
        if (b->listener == l) b->listener = NULL;
        nsync_mu_unlock(&b->mu);
        (void)gof_io_reset_fd(fd);
        (void)close(fd);
        go_channel_unref(l->done);
        free(l);
        return stopped ? -ECANCELED : -ENOMEM;
    }
    FIPSD_DEBUG(MOD, "%s: L2CAP listening on psm 0x%04x (mtu %u)", b->adapter, requested, b->mtu);
    *bound = requested; /* BlueZ binds what is asked */
    return 0;
}

/* ---- dialling ------------------------------------------------------------------------------- */
static int b_connect(void *self, const fipsd_ble_peer *p, uint16_t psm, uint64_t deadline, fipsd_ble_stream **out)
{
    bz *b = self;
    if (!psm) return -EINVAL;
    uint64_t start = now_ms();
    if (deadline != FIPSD_BLE_NO_DEADLINE && deadline <= start) return -ETIMEDOUT;
    /* Upstream dials the device address whatever the adapter part names. */
    char path[FIPSD_BLE_ADAPTER_MAX + 40];
    bool have_path = fipsd_ble_bluez_device_path(b->adapter, p->device, path, sizeof path) != 0;
    nsync_mu_lock(&b->mu);
    bool stopped = b->stopped, up = b->present && b->powered && b->have_address;
    uint8_t local[6];
    memcpy(local, b->address, 6);
    bz_device *known = have_path ? g_hash_table_lookup(b->devices, path) : NULL;
    uint8_t type = fipsd_ble_bluez_dial_type(known ? &known->props : NULL);
    nsync_mu_unlock(&b->mu);
    if (stopped) return -ECANCELED;
    if (!up) return -ENODEV;
    int fd = l2cap_open(b, local, 0);
    if (fd < 0) {
        FIPSD_DEBUG(MOD, "%s: L2CAP socket for a dial failed (%d)", b->adapter, fd);
        return fd == -EAFNOSUPPORT ? -ENODEV : fipsd_ble_bluez_connect_errno(-fd, false, true, 0, 0);
    }
    /* The kernel channel timer past the deadline: see bluez.h. */
    uint64_t timer = FIPSD_BLE_BLUEZ_KERNEL_TIMER_DEFAULT_MS;
    if (deadline != FIPSD_BLE_NO_DEADLINE) {
        uint64_t t = fipsd_ble_bluez_kernel_timer_ms(deadline - start);
        struct timeval tv = {.tv_sec = (time_t)(t / 1000), .tv_usec = (suseconds_t)(t % 1000 * 1000)};
        if (!setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv)) timer = t;
    }
    fipsd_l2cap_sockaddr dst;
    fipsd_ble_bluez_sockaddr(p->device, type, psm, &dst);
    int rc = -1, err = 0;
    bz_dial dial = {.next = NULL, .fd = fd};
    if (!err) {
        (void)gof_io_reset_fd(fd);
        nsync_mu_lock(&b->mu);
        stopped = b->stopped;
        if (!stopped) { dial.next = b->dials; b->dials = &dial; }
        nsync_mu_unlock(&b->mu);
        if (stopped) err = ECANCELED;
        else {
            rc = gof_connect(fd, (struct sockaddr *)&dst, sizeof dst, remaining_ms(deadline));
            if (rc) err = io_errno();
            nsync_mu_lock(&b->mu);
            for (bz_dial **q = &b->dials; *q; q = &(*q)->next) if (*q == &dial) { *q = dial.next; break; }
            nsync_mu_unlock(&b->mu);
        }
    }
    nsync_mu_lock(&b->mu);
    stopped = b->stopped;
    up = b->present && b->powered;
    nsync_mu_unlock(&b->mu);
    fipsd_ble_stream *s = NULL;
    if (!rc && !stopped && !(s = stream_new(b, fd, p->device))) err = ENOMEM;
    if (!s) {
        uint64_t elapsed = now_ms() - start;
        int mapped = err == ENOMEM ? -ENOMEM : fipsd_ble_bluez_connect_errno(rc ? err : ECANCELED, stopped, up, elapsed, timer);
        FIPSD_DEBUG(MOD, "%s: L2CAP connect to %02X:%02X:%02X:%02X:%02X:%02X psm 0x%04x failed after %llu ms (errno %d -> %d)",
            b->adapter, p->device[0], p->device[1], p->device[2], p->device[3], p->device[4], p->device[5], psm,
            (unsigned long long)elapsed, err, mapped);
        (void)gof_io_reset_fd(fd);
        (void)close(fd);
        return mapped;
    }
    s->peer = *p;
    *out = s;
    return 0;
}

/* ---- the backend vtable ------------------------------------------------------------------------ */
static int b_start(void *self, fipsd_runtime *rt, fipsd_ble_sink *sink)
{
    bz *b = self;
    nsync_mu_lock(&b->mu);
    b->rt = rt; b->sink = sink;
    nsync_mu_unlock(&b->mu);
    /* The GLib thread takes no signals: the runtime's signal fiber does. */
    sigset_t all, old;
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &old);
    int rc = pthread_create(&b->thread, NULL, glib_thread, b);
    (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc) { FIPSD_ERROR(MOD, "%s: cannot start the BlueZ thread (%d)", b->adapter, rc); return -1; }
    b->thread_ok = true;
    return 0;
}
static void b_stop(void *self)
{
    bz *b = self;
    nsync_mu_lock(&b->mu);
    bool first = !b->stopped;
    b->stopped = true;
    if (b->listener && b->listener->fd >= 0) (void)gof_io_cancel_fd(b->listener->fd);
    for (bz_dial *d = b->dials; d; d = d->next) (void)gof_io_cancel_fd(d->fd);
    nsync_mu_unlock(&b->mu);
    go_channel_close(b->stop_ch); /* every parked request: -ECANCELED */
    if (first && b->thread_ok) g_main_context_invoke_full(b->ctx, G_PRIORITY_DEFAULT, on_stop, b, NULL);
}
static void b_destroy(void *self)
{
    bz *b = self;
    b_stop(b);
    if (b->thread_ok) {
        g_main_context_invoke_full(b->ctx, G_PRIORITY_DEFAULT, on_quit, b, NULL);
        (void)pthread_join(b->thread, NULL);
    }
    nsync_mu_lock(&b->mu);
    bz_listener *l = b->listener; /* never started: nobody accepts on it */
    b->listener = NULL;
    nsync_mu_unlock(&b->mu);
    if (l) { if (l->fd >= 0) { (void)gof_io_reset_fd(l->fd); (void)close(l->fd); } go_channel_unref(l->done); free(l); }
    g_free(b->owner);
    g_hash_table_destroy(b->devices);
    g_dbus_node_info_unref(b->info);
    g_object_unref(b->cancel);
    g_main_loop_unref(b->loop);
    g_main_context_unref(b->ctx);
    go_channel_unref(b->stop_ch);
    free(b);
}
static fipsd_ble_adapter b_adapter(void *self)
{
    bz *b = self;
    nsync_mu_lock(&b->mu);
    fipsd_ble_adapter a = {b->present, b->powered};
    nsync_mu_unlock(&b->mu);
    return a;
}
static int b_advertise(void *self, const fipsd_ble_advert *ad, uint64_t deadline) { return simple(self, REQ_ADVERTISE, ad, deadline); }
static int b_advertise_stop(void *self) { return simple(self, REQ_ADVERTISE_STOP, NULL, FIPSD_BLE_NO_DEADLINE); }
static int b_scan_start(void *self, uint64_t deadline) { return simple(self, REQ_SCAN_START, NULL, deadline); }
static int b_scan_stop(void *self) { return simple(self, REQ_SCAN_STOP, NULL, FIPSD_BLE_NO_DEADLINE); }
static bool b_local_addr(void *self, fipsd_ble_peer *out)
{
    bz *b = self;
    nsync_mu_lock(&b->mu);
    bool have = b->have_address;
    if (have) { memset(out, 0, sizeof *out); memcpy(out->adapter, b->adapter, sizeof out->adapter); memcpy(out->device, b->address, 6); }
    nsync_mu_unlock(&b->mu);
    return have;
}
static const char *b_adapter_name(void *self) { return ((bz *)self)->adapter; }
static const fipsd_ble_backend_ops bz_ops = {.start = b_start, .stop = b_stop, .destroy = b_destroy, .adapter = b_adapter,
    .listen = b_listen, .advertise = b_advertise, .advertise_stop = b_advertise_stop, .scan_start = b_scan_start,
    .scan_stop = b_scan_stop, .read_psm = NULL, .connect = b_connect, .local_addr = b_local_addr,
    .adapter_name = b_adapter_name};

bool fipsd_ble_bluez_backend(const fipsd_ble_config *c, fipsd_ble_backend *out, const char **reason)
{
    if (!c || !fipsd_ble_bluez_adapter_valid(c->adapter)) {
        *reason = "the adapter is not a BlueZ controller name such as hci0";
        return false;
    }
    bz *b = calloc(1, sizeof *b);
    if (!b) { *reason = "out of memory"; return false; }
    nsync_mu_init(&b->mu);
    strcpy(b->adapter, c->adapter);
    (void)snprintf(b->path, sizeof b->path, "/org/bluez/%s", c->adapter);
    (void)snprintf(b->object, sizeof b->object, "/org/nostrc/fipsd/ble/%s/advertisement0", c->adapter);
    b->mtu = c->mtu;
    b->adv.psm = c->psm;
    fipsd_ble_encode_psm(c->psm, b->adv.service_data);
    g_queue_init(&b->adv_q);
    g_queue_init(&b->scan_q);
    b->stop_ch = go_channel_create(1);
    b->devices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    b->ctx = g_main_context_new();
    b->loop = g_main_loop_new(b->ctx, FALSE);
    b->cancel = g_cancellable_new();
    b->info = g_dbus_node_info_new_for_xml(adv_xml, NULL);
    if (!b->stop_ch || !b->info) {
        if (b->stop_ch) go_channel_unref(b->stop_ch);
        if (b->info) g_dbus_node_info_unref(b->info);
        g_hash_table_destroy(b->devices); g_object_unref(b->cancel); g_main_loop_unref(b->loop); g_main_context_unref(b->ctx);
        free(b);
        *reason = "out of memory";
        return false;
    }
    *out = (fipsd_ble_backend){&bz_ops, b};
    return true;
}
