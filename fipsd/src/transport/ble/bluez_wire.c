/* The pure half of the BlueZ backend (bluez.h): object paths, advert
 * properties, D-Bus property parsing, the sockaddr, and error mapping. No bus,
 * no socket, no state: the unit tests drive all of it with literals. */
#define _POSIX_C_SOURCE 200809L
#include "transport/ble/bluez.h"
#include <errno.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

bool fipsd_ble_bluez_adapter_valid(const char *a)
{
    size_t n = a ? strlen(a) : 0;
    if (!n || n >= FIPSD_BLE_ADAPTER_MAX) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = a[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}
static int hex(char c)
{ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* Six hex pairs separated by `sep`, exactly 17 characters. */
static bool parse_pairs(const char *s, char sep, uint8_t out[6])
{
    if (!s || strlen(s) != 17) return false;
    uint8_t v[6];
    for (size_t i = 0; i < 6; ++i) {
        int hi = hex(s[3 * i]), lo = hex(s[3 * i + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && s[3 * i + 2] != sep)) return false;
        v[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(out, v, 6);
    return true;
}
bool fipsd_ble_bluez_parse_address(const char *s, uint8_t out[6]) { return parse_pairs(s, ':', out); }
size_t fipsd_ble_bluez_device_path(const char *adapter, const uint8_t d[6], char *out, size_t cap)
{
    int n = snprintf(out, cap, "/org/bluez/%s/dev_%02X_%02X_%02X_%02X_%02X_%02X", adapter, d[0], d[1], d[2], d[3], d[4], d[5]);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}
bool fipsd_ble_bluez_device_from_path(const char *path, const char *adapter, uint8_t out[6])
{
    char prefix[FIPSD_BLE_ADAPTER_MAX + 20];
    int n = snprintf(prefix, sizeof prefix, "/org/bluez/%s/dev_", adapter);
    if (!path || n < 0 || (size_t)n >= sizeof prefix || strncmp(path, prefix, (size_t)n)) return false;
    return parse_pairs(path + n, '_', out); /* exactly 17: a GATT child path is longer */
}

/* ---- the advertisement -------------------------------------------------------- */
const char *const fipsd_ble_bluez_advert_properties[] = {"Type", "ServiceUUIDs", "ServiceData", "MinInterval",
    "MaxInterval", NULL};
GVariant *fipsd_ble_bluez_advert_property(const fipsd_ble_advert *ad, const char *name)
{
    if (!strcmp(name, "Type")) return g_variant_new_string("peripheral");
    if (!strcmp(name, "ServiceUUIDs")) {
        const char *uuids[] = {FIPSD_BLE_SERVICE_UUID};
        return g_variant_new_strv(uuids, 1);
    }
    if (!strcmp(name, "ServiceData")) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", FIPSD_BLE_BLUEZ_PSM_DATA_UUID,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, ad->service_data, sizeof ad->service_data, 1));
        return g_variant_builder_end(&b);
    }
    if (!strcmp(name, "MinInterval")) return g_variant_new_uint32(FIPSD_BLE_BLUEZ_ADV_MIN_MS);
    if (!strcmp(name, "MaxInterval")) return g_variant_new_uint32(FIPSD_BLE_BLUEZ_ADV_MAX_MS);
    return NULL;
}

/* ---- properties ------------------------------------------------------------------ */
static bool boolean(GVariant *v, bool *out)
{
    if (!g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) return false;
    *out = g_variant_get_boolean(v);
    return true;
}
static void service_data(GVariant *v, fipsd_ble_bluez_props *p)
{
    if (!g_variant_is_of_type(v, G_VARIANT_TYPE_VARDICT)) return;
    p->fields |= FIPSD_BLE_BLUEZ_SERVICE_DATA;
    p->psm_data_len = 0;
    GVariantIter it;
    const char *key;
    GVariant *value;
    g_variant_iter_init(&it, v);
    while (g_variant_iter_next(&it, "{&sv}", &key, &value)) {
        if (!strcasecmp(key, FIPSD_BLE_BLUEZ_PSM_DATA_UUID) && g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) {
            gsize n = 0;
            const uint8_t *bytes = g_variant_get_fixed_array(value, &n, 1);
            if (n > sizeof p->psm_data) n = sizeof p->psm_data; /* extra bytes ignored (backend.h) */
            if (n) memcpy(p->psm_data, bytes, n);
            p->psm_data_len = (uint8_t)n;
            if (n) p->fips = true; /* the FIPS PSM key: a FIPS advertiser, whatever UUIDs says */
        }
        g_variant_unref(value);
    }
}
void fipsd_ble_bluez_parse_props(GVariant *dict, fipsd_ble_bluez_props *p)
{
    memset(p, 0, sizeof *p);
    if (!dict || !g_variant_is_of_type(dict, G_VARIANT_TYPE_VARDICT)) return;
    GVariantIter it;
    const char *key;
    GVariant *v;
    g_variant_iter_init(&it, dict);
    while (g_variant_iter_next(&it, "{&sv}", &key, &v)) {
        bool flag;
        if (!strcmp(key, "Address") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
            if (fipsd_ble_bluez_parse_address(g_variant_get_string(v, NULL), p->address)) p->fields |= FIPSD_BLE_BLUEZ_ADDRESS;
        } else if (!strcmp(key, "AddressType") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
            p->random = !strcmp(g_variant_get_string(v, NULL), "random");
            p->fields |= FIPSD_BLE_BLUEZ_ADDRESS_TYPE;
        } else if (!strcmp(key, "UUIDs") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
            gsize n = 0;
            const gchar **uuids = g_variant_get_strv(v, &n);
            for (gsize i = 0; i < n; ++i) if (!strcasecmp(uuids[i], FIPSD_BLE_SERVICE_UUID)) p->fips = true;
            g_free(uuids);
            p->fields |= FIPSD_BLE_BLUEZ_UUIDS;
        } else if (!strcmp(key, "ServiceData")) {
            service_data(v, p);
        } else if (!strcmp(key, "RSSI") && g_variant_is_of_type(v, G_VARIANT_TYPE_INT16)) {
            int16_t rssi = g_variant_get_int16(v);
            if (rssi != 127) { p->rssi = rssi; p->fields |= FIPSD_BLE_BLUEZ_RSSI; }
        } else if (!strcmp(key, "Paired") && boolean(v, &flag)) { p->paired = flag; p->fields |= FIPSD_BLE_BLUEZ_PAIRED; }
        else if (!strcmp(key, "Trusted") && boolean(v, &flag)) { p->trusted = flag; p->fields |= FIPSD_BLE_BLUEZ_TRUSTED; }
        else if (!strcmp(key, "Connected") && boolean(v, &flag)) { p->connected = flag; p->fields |= FIPSD_BLE_BLUEZ_CONNECTED; }
        else if (!strcmp(key, "Blocked") && boolean(v, &flag)) { p->blocked = flag; p->fields |= FIPSD_BLE_BLUEZ_BLOCKED; }
        else if (!strcmp(key, "Powered") && boolean(v, &flag)) { p->powered = flag; p->fields |= FIPSD_BLE_BLUEZ_POWERED; }
        else if (!strcmp(key, "ServicesResolved") && boolean(v, &flag)) {
            p->services_resolved = flag; p->fields |= FIPSD_BLE_BLUEZ_SERVICES_RESOLVED;
        }
        g_variant_unref(v);
    }
}
void fipsd_ble_bluez_merge_props(fipsd_ble_bluez_props *into, const fipsd_ble_bluez_props *u)
{
    unsigned f = u->fields;
    if (f & FIPSD_BLE_BLUEZ_ADDRESS) memcpy(into->address, u->address, 6);
    if (f & FIPSD_BLE_BLUEZ_ADDRESS_TYPE) into->random = u->random;
    /* Sticky for the object's life: once its services resolve (BlueZ runs
     * a GATT client over every LE link, ours included), UUIDs lists the GATT
     * services and drops the advertised ones. */
    if (u->fips) into->fips = true;
    if (f & FIPSD_BLE_BLUEZ_RSSI) into->rssi = u->rssi;
    if (f & FIPSD_BLE_BLUEZ_SERVICE_DATA) {
        into->psm_data_len = u->psm_data_len;
        memcpy(into->psm_data, u->psm_data, sizeof into->psm_data);
    }
    if (f & FIPSD_BLE_BLUEZ_PAIRED) into->paired = u->paired;
    if (f & FIPSD_BLE_BLUEZ_TRUSTED) into->trusted = u->trusted;
    if (f & FIPSD_BLE_BLUEZ_CONNECTED) into->connected = u->connected;
    if (f & FIPSD_BLE_BLUEZ_BLOCKED) into->blocked = u->blocked;
    if (f & FIPSD_BLE_BLUEZ_POWERED) into->powered = u->powered;
    if (f & FIPSD_BLE_BLUEZ_SERVICES_RESOLVED) into->services_resolved = u->services_resolved;
    into->fields |= f;
}
static bool set_or_unknown(const fipsd_ble_bluez_props *p, unsigned field, bool value)
{ return !(p->fields & field) || value; }
bool fipsd_ble_bluez_clear_at_scan_start(const fipsd_ble_bluez_props *p)
{
    return !set_or_unknown(p, FIPSD_BLE_BLUEZ_PAIRED, p->paired) && !set_or_unknown(p, FIPSD_BLE_BLUEZ_TRUSTED, p->trusted) &&
        !set_or_unknown(p, FIPSD_BLE_BLUEZ_CONNECTED, p->connected) && !set_or_unknown(p, FIPSD_BLE_BLUEZ_BLOCKED, p->blocked);
}
uint8_t fipsd_ble_bluez_dial_type(const fipsd_ble_bluez_props *known)
{
    return known && (known->fields & FIPSD_BLE_BLUEZ_ADDRESS_TYPE) && known->random ? FIPSD_BT_ADDR_LE_RANDOM
                                                                                     : FIPSD_BT_ADDR_LE_PUBLIC;
}
void fipsd_ble_bluez_sockaddr(const uint8_t device[6], uint8_t type, uint16_t psm, fipsd_l2cap_sockaddr *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->l2_family = FIPSD_BT_AF_BLUETOOTH;
    uint8_t le[2] = {(uint8_t)psm, (uint8_t)(psm >> 8)};
    memcpy(&sa->l2_psm, le, 2);
    for (size_t i = 0; i < 6; ++i) sa->l2_bdaddr[i] = device[5 - i];
    sa->l2_bdaddr_type = type;
}
void fipsd_ble_bluez_sockaddr_device(const fipsd_l2cap_sockaddr *sa, uint8_t device[6])
{ for (size_t i = 0; i < 6; ++i) device[i] = sa->l2_bdaddr[5 - i]; }

/* ---- GATT ------------------------------------------------------------------------ */
static bool under(const char *path, const char *parent)
{
    size_t n = strlen(parent);
    return !strncmp(path, parent, n) && path[n] == '/';
}
static bool uuid_is(GVariant *props, const char *uuid)
{
    GVariant *v = props ? g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING) : NULL;
    bool match = v && !strcasecmp(g_variant_get_string(v, NULL), uuid);
    if (v) g_variant_unref(v);
    return match;
}
bool fipsd_ble_bluez_find_psm_characteristic(GVariant *objects, const char *device, char *out, size_t cap)
{
    if (!objects || !device || !g_variant_is_of_type(objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"))) return false;
    enum { SERVICES = 8 };
    char services[SERVICES][128];
    size_t n = 0;
    GVariantIter it;
    const char *path;
    GVariant *ifaces;
    g_variant_iter_init(&it, objects); /* the FIPS services of this device */
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *svc = g_variant_lookup_value(ifaces, "org.bluez.GattService1", G_VARIANT_TYPE_VARDICT);
        if (svc && n < SERVICES && under(path, device) && strlen(path) < sizeof services[0] &&
            uuid_is(svc, FIPSD_BLE_SERVICE_UUID)) strcpy(services[n++], path);
        if (svc) g_variant_unref(svc);
        g_variant_unref(ifaces);
    }
    bool found = false;
    g_variant_iter_init(&it, objects); /* and the PSM characteristic of one */
    while (!found && g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *chr = g_variant_lookup_value(ifaces, "org.bluez.GattCharacteristic1", G_VARIANT_TYPE_VARDICT);
        if (chr && under(path, device) && uuid_is(chr, FIPSD_BLE_PSM_CHARACTERISTIC_UUID)) {
            GVariant *svc = g_variant_lookup_value(chr, "Service", G_VARIANT_TYPE_OBJECT_PATH);
            const char *owner = svc ? g_variant_get_string(svc, NULL) : "";
            for (size_t i = 0; i < n && !found; ++i)
                if (!strcmp(owner, services[i]) && strlen(path) < cap) { strcpy(out, path); found = true; }
            if (svc) g_variant_unref(svc);
        }
        if (chr) g_variant_unref(chr);
        g_variant_unref(ifaces);
    }
    return found;
}

/* ---- errors -------------------------------------------------------------------- */
int fipsd_ble_bluez_dbus_errno(const char *name)
{
    static const struct { const char *name; int err; } map[] = {
        {"org.bluez.Error.NotReady", ENODEV},              /* adapter not powered */
        {"org.bluez.Error.NotPowered", ENODEV},
        {"org.bluez.Error.InProgress", EALREADY},
        {"org.bluez.Error.AlreadyConnected", EISCONN},
        {"org.bluez.Error.AlreadyExists", EEXIST},
        {"org.bluez.Error.DoesNotExist", ENOENT},
        {"org.bluez.Error.NotSupported", ENOTSUP},
        {"org.bluez.Error.NotAvailable", ENOTSUP},
        {"org.bluez.Error.InvalidArguments", EINVAL},
        {"org.bluez.Error.InvalidLength", EINVAL},
        {"org.bluez.Error.InvalidOffset", EINVAL},
        {"org.bluez.Error.NotPermitted", EPERM},
        {"org.bluez.Error.NotAuthorized", EACCES},
        {"org.bluez.Error.AuthenticationFailed", EACCES},
        {"org.bluez.Error.NotConnected", ENOTCONN},
        {"org.bluez.Error.Failed", EIO},
        {"org.freedesktop.DBus.Error.AccessDenied", EACCES},
        {"org.freedesktop.DBus.Error.AuthFailed", EACCES},
        {"org.freedesktop.DBus.Error.ServiceUnknown", ENODEV}, /* bluetoothd is not running */
        {"org.freedesktop.DBus.Error.NameHasNoOwner", ENODEV},
        {"org.freedesktop.DBus.Error.Disconnected", ENODEV},
        {"org.freedesktop.DBus.Error.UnknownObject", ENODEV},  /* the adapter object went away */
        {"org.freedesktop.DBus.Error.UnknownInterface", ENOTSUP}, /* e.g. no LEAdvertisingManager1 */
        {"org.freedesktop.DBus.Error.UnknownMethod", ENOTSUP},
        {"org.freedesktop.DBus.Error.NoReply", ETIMEDOUT},
        {"org.freedesktop.DBus.Error.Timeout", ETIMEDOUT},
        {"org.freedesktop.DBus.Error.TimedOut", ETIMEDOUT},
        {"org.freedesktop.DBus.Error.NoMemory", ENOMEM},
        {"org.freedesktop.DBus.Error.LimitsExceeded", ENOSPC},
        {"org.freedesktop.DBus.Error.InvalidArgs", EINVAL},
    };
    if (name) for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) if (!strcmp(name, map[i].name)) return -map[i].err;
    return -EIO;
}
int fipsd_ble_bluez_gerror_errno(const GError *e)
{
    if (!e) return 0;
    if (e->domain == G_IO_ERROR) {
        switch (e->code) {
        case G_IO_ERROR_CANCELLED: return -ECANCELED;
        case G_IO_ERROR_TIMED_OUT: return -ETIMEDOUT;
        case G_IO_ERROR_CLOSED: case G_IO_ERROR_CONNECTION_CLOSED: case G_IO_ERROR_NOT_CONNECTED: return -ENODEV;
        case G_IO_ERROR_PERMISSION_DENIED: return -EACCES;
        default: break;
        }
    }
    gchar *remote = g_dbus_error_get_remote_error(e);
    if (remote) { int rc = fipsd_ble_bluez_dbus_errno(remote); g_free(remote); return rc; }
    if (e->domain == G_DBUS_ERROR) {
        switch (e->code) {
        case G_DBUS_ERROR_SERVICE_UNKNOWN: case G_DBUS_ERROR_NAME_HAS_NO_OWNER: case G_DBUS_ERROR_DISCONNECTED:
        case G_DBUS_ERROR_UNKNOWN_OBJECT: return -ENODEV;
        case G_DBUS_ERROR_NO_REPLY: case G_DBUS_ERROR_TIMEOUT: case G_DBUS_ERROR_TIMED_OUT: return -ETIMEDOUT;
        case G_DBUS_ERROR_ACCESS_DENIED: case G_DBUS_ERROR_AUTH_FAILED: return -EACCES;
        case G_DBUS_ERROR_UNKNOWN_METHOD: case G_DBUS_ERROR_UNKNOWN_INTERFACE: return -ENOTSUP;
        case G_DBUS_ERROR_NO_MEMORY: return -ENOMEM;
        default: break;
        }
    }
    return -EIO;
}
uint64_t fipsd_ble_bluez_kernel_timer_ms(uint64_t remaining)
{
    const uint64_t cap = UINT64_C(86400000); /* a day: far past any configured connect_timeout_ms */
    return remaining >= cap - FIPSD_BLE_BLUEZ_KERNEL_TIMER_SLACK_MS ? cap : remaining + FIPSD_BLE_BLUEZ_KERNEL_TIMER_SLACK_MS;
}
int fipsd_ble_bluez_connect_errno(int err, bool stopped, bool adapter_up, uint64_t elapsed_ms, uint64_t timer_ms)
{
    if (stopped || err == ECANCELED) return -ECANCELED;
    if (!adapter_up) return -ENODEV; /* our radio went away: nothing learned about the peer */
    switch (err) {
    case ETIMEDOUT: /* the deadline, an HCI connection or LL response timeout */
    case EHOSTDOWN: /* page or advertising timeout */
        return -ETIMEDOUT;
    case ECONNREFUSED:
        /* The channel timer has a jiffy's granularity: anything within a
         * quarter second of it is the timer, not a refusal. */
        return timer_ms && elapsed_ms + 250 >= timer_ms ? -ETIMEDOUT : -ECONNREFUSED;
    case EHOSTUNREACH: /* hci_get_route found no powered LE adapter: ours is down */
    case ENETDOWN: case ENODEV: case EADDRNOTAVAIL:
        return -ENODEV;
    default:
        return err > 0 ? -err : -EIO;
    }
}
