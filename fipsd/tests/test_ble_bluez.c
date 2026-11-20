/* The BlueZ backend's pure half (nostrc-gfi8h.6.2): the kernel ABI it
 * defines, object paths, the advertisement (upstream start_advertising and
 * io_linux tests), D-Bus property parsing, upstream clear_at_scan_start and
 * dial_addr_type, and error mapping, including the PSM rule of backend.h: an
 * unanswered dial is ETIMEDOUT and only a peer's refusal is ECONNREFUSED. No
 * bus, no radio; FipsdBleBluezSmokeTest runs the real thing. */
#define _POSIX_C_SOURCE 200809L
#include "transport/ble/bluez.h"
#include "transport/ble/internal.h"
#include <assert.h>
#include <errno.h>
#include <gio/gio.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#if defined(__has_include)
#if __has_include(<bluetooth/bluetooth.h>) && __has_include(<bluetooth/l2cap.h>)
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#define HAVE_BLUEZ_HEADERS 1
#endif
#endif

static void kernel_abi(void)
{
    assert(sizeof(fipsd_l2cap_sockaddr) == 14);
    assert(offsetof(fipsd_l2cap_sockaddr, l2_psm) == 2 && offsetof(fipsd_l2cap_sockaddr, l2_bdaddr) == 4);
    assert(offsetof(fipsd_l2cap_sockaddr, l2_cid) == 10 && offsetof(fipsd_l2cap_sockaddr, l2_bdaddr_type) == 12);
    assert(sizeof(fipsd_bt_security) == 2 && sizeof(fipsd_bt_power) == 1);
#ifdef AF_BLUETOOTH
    assert(AF_BLUETOOTH == FIPSD_BT_AF_BLUETOOTH);
#endif
#ifdef HAVE_BLUEZ_HEADERS
    assert(sizeof(struct sockaddr_l2) == sizeof(fipsd_l2cap_sockaddr));
    assert(offsetof(struct sockaddr_l2, l2_psm) == offsetof(fipsd_l2cap_sockaddr, l2_psm));
    assert(offsetof(struct sockaddr_l2, l2_bdaddr) == offsetof(fipsd_l2cap_sockaddr, l2_bdaddr));
    assert(offsetof(struct sockaddr_l2, l2_cid) == offsetof(fipsd_l2cap_sockaddr, l2_cid));
    assert(offsetof(struct sockaddr_l2, l2_bdaddr_type) == offsetof(fipsd_l2cap_sockaddr, l2_bdaddr_type));
    assert(BTPROTO_L2CAP == FIPSD_BT_PROTO_L2CAP && SOL_BLUETOOTH == FIPSD_BT_SOL_BLUETOOTH);
    assert(BT_SECURITY == FIPSD_BT_SECURITY && BT_SECURITY_LOW == FIPSD_BT_SECURITY_LOW && BT_POWER == FIPSD_BT_POWER);
    assert(BT_SNDMTU == FIPSD_BT_SNDMTU && BT_RCVMTU == FIPSD_BT_RCVMTU);
    assert(BDADDR_LE_PUBLIC == FIPSD_BT_ADDR_LE_PUBLIC && BDADDR_LE_RANDOM == FIPSD_BT_ADDR_LE_RANDOM);
    assert(sizeof(struct bt_security) == sizeof(fipsd_bt_security) && sizeof(struct bt_power) == sizeof(fipsd_bt_power));
    /* str2ba's byte order is the sockaddr's: least significant first. */
    bdaddr_t ba;
    memset(&ba, 0, sizeof ba);
    ba.b[0] = 0x66; ba.b[5] = 0x11;
    struct sockaddr_l2 k;
    memset(&k, 0, sizeof k);
    k.l2_family = AF_BLUETOOTH; k.l2_psm = htobs(0x0085); k.l2_bdaddr = ba; k.l2_bdaddr_type = BDADDR_LE_RANDOM;
    fipsd_l2cap_sockaddr m;
    fipsd_ble_bluez_sockaddr((const uint8_t[]){0x11, 0, 0, 0, 0, 0x66}, FIPSD_BT_ADDR_LE_RANDOM, 0x0085, &m);
    assert(!memcmp(&k, &m, sizeof m));
    puts("kernel ABI: sockaddr_l2 layout and every constant match <bluetooth/l2cap.h>: ok");
#else
    puts("kernel ABI: sockaddr_l2 layout (no <bluetooth/l2cap.h> here to compare): ok");
#endif
}
static void sockaddr_and_paths(void)
{
    const uint8_t dev[6] = {0x00, 0xAA, 0x01, 0x00, 0x00, 0x2F};
    fipsd_l2cap_sockaddr sa;
    fipsd_ble_bluez_sockaddr(dev, FIPSD_BT_ADDR_LE_PUBLIC, 0x1234, &sa);
    const uint8_t *psm = (const uint8_t *)&sa.l2_psm;
    assert(sa.l2_family == FIPSD_BT_AF_BLUETOOTH && psm[0] == 0x34 && psm[1] == 0x12 && sa.l2_cid == 0);
    assert(sa.l2_bdaddr[0] == 0x2F && sa.l2_bdaddr[5] == 0x00 && sa.l2_bdaddr[4] == 0xAA);
    assert(sa.l2_bdaddr_type == FIPSD_BT_ADDR_LE_PUBLIC);
    uint8_t back[6];
    fipsd_ble_bluez_sockaddr_device(&sa, back);
    assert(!memcmp(back, dev, 6));

    assert(fipsd_ble_bluez_adapter_valid("hci0") && fipsd_ble_bluez_adapter_valid("hci_12"));
    const char *bad_adapters[] = {"", "hci-0", "hci0/x", "hci 0", "averyveryverylong"};
    for (size_t i = 0; i < 5; ++i) assert(!fipsd_ble_bluez_adapter_valid(bad_adapters[i]));
    assert(!fipsd_ble_bluez_adapter_valid(NULL));

    uint8_t a[6];
    assert(fipsd_ble_bluez_parse_address("00:aa:01:00:00:2f", a) && !memcmp(a, dev, 6));
    assert(!fipsd_ble_bluez_parse_address("00:AA:01:00:00", a) && !fipsd_ble_bluez_parse_address("00-AA-01-00-00-2F", a));
    assert(!fipsd_ble_bluez_parse_address("00:AA:01:00:00:2G", a) && !fipsd_ble_bluez_parse_address(NULL, a));

    char path[64];
    assert(fipsd_ble_bluez_device_path("hci1", dev, path, sizeof path) == strlen("/org/bluez/hci1/dev_00_AA_01_00_00_2F"));
    assert(!strcmp(path, "/org/bluez/hci1/dev_00_AA_01_00_00_2F"));
    char small[20];
    assert(!fipsd_ble_bluez_device_path("hci1", dev, small, sizeof small)); /* does not fit */
    memset(a, 0, 6);
    assert(fipsd_ble_bluez_device_from_path(path, "hci1", a) && !memcmp(a, dev, 6));
    assert(!fipsd_ble_bluez_device_from_path(path, "hci0", a));                                 /* another adapter */
    assert(!fipsd_ble_bluez_device_from_path("/org/bluez/hci1/dev_00_AA_01_00_00_2F/service000a", "hci1", a));
    assert(!fipsd_ble_bluez_device_from_path("/org/bluez/hci1", "hci1", a) && !fipsd_ble_bluez_device_from_path(NULL, "hci1", a));
    assert(!fipsd_ble_bluez_device_from_path("/org/bluez/hci10/dev_00_AA_01_00_00_2F", "hci1", a));
    puts("sockaddr (LE psm, reversed bdaddr), adapter names, addresses, device object paths: ok");
}
static void advertisement(void)
{
    /* Upstream psm_service_data_uuid_expands_the_key_over_the_base_uuid: a
     * wrong expansion is a plausible UUID that silently never matches. */
    unsigned key = 0;
    assert(sscanf(FIPSD_BLE_BLUEZ_PSM_DATA_UUID, "0000%4x-", &key) == 1 && key == FIPSD_BLE_PSM_SERVICE_DATA_UUID16);
    assert(!strcmp(&FIPSD_BLE_BLUEZ_PSM_DATA_UUID[8], "-0000-1000-8000-00805f9b34fb")); /* the Bluetooth base UUID */

    fipsd_ble_advert ad = {.psm = 0x00C1};
    fipsd_ble_encode_psm(ad.psm, ad.service_data);
    GVariant *v = fipsd_ble_bluez_advert_property(&ad, "Type");
    assert(v && !strcmp(g_variant_get_string(v, NULL), "peripheral"));
    g_variant_unref(g_variant_ref_sink(v));
    v = g_variant_ref_sink(fipsd_ble_bluez_advert_property(&ad, "ServiceUUIDs"));
    gsize n = 0;
    const gchar **uuids = g_variant_get_strv(v, &n);
    assert(n == 1 && !strcmp(uuids[0], FIPSD_BLE_SERVICE_UUID));
    g_free(uuids);
    g_variant_unref(v);
    v = g_variant_ref_sink(fipsd_ble_bluez_advert_property(&ad, "ServiceData"));
    assert(g_variant_is_of_type(v, G_VARIANT_TYPE("a{sv}")) && g_variant_n_children(v) == 1);
    GVariant *data = g_variant_lookup_value(v, FIPSD_BLE_BLUEZ_PSM_DATA_UUID, G_VARIANT_TYPE_BYTESTRING);
    const uint8_t *bytes = g_variant_get_fixed_array(data, &n, 1);
    uint16_t psm = 0;
    assert(n == 2 && bytes[0] == 0xC1 && bytes[1] == 0x00 && fipsd_ble_decode_psm(bytes, n, &psm) && psm == 0x00C1);
    g_variant_unref(data);
    g_variant_unref(v);
    v = g_variant_ref_sink(fipsd_ble_bluez_advert_property(&ad, "MinInterval"));
    assert(g_variant_get_uint32(v) == 400);
    g_variant_unref(v);
    v = g_variant_ref_sink(fipsd_ble_bluez_advert_property(&ad, "MaxInterval"));
    assert(g_variant_get_uint32(v) == 600);
    g_variant_unref(v);
    /* No LocalName (it would not fit), no scan-response fields. */
    assert(!fipsd_ble_bluez_advert_property(&ad, "LocalName") && !fipsd_ble_bluez_advert_property(&ad, "Includes"));
    size_t count = 0;
    for (const char *const *p = fipsd_ble_bluez_advert_properties; *p; ++p, ++count) {
        GVariant *x = fipsd_ble_bluez_advert_property(&ad, *p);
        assert(x);
        g_variant_unref(g_variant_ref_sink(x));
    }
    assert(count == 5);
    /* On the air: flags 3 + 128-bit UUID list 18 + 16-bit service data 6. */
    assert(FIPSD_BLE_ADV_FLAGS_BYTES + (2 + 16) + (2 + 2 + sizeof ad.service_data) <= FIPSD_BLE_LEGACY_ADV_BYTES);
    puts("advertisement: peripheral, FIPS UUID, ServiceData {0x9C90 over the base UUID: psm LE}, 400-600 ms, no name: ok");
}
static GVariant *parsed(const char *text) { return g_variant_ref_sink(g_variant_new_parsed(text)); }
static void properties(void)
{
    GVariant *d = parsed("{'Address': <'00:AA:01:01:00:01'>, 'AddressType': <'random'>, "
        "'UUIDs': <['0000180a-0000-1000-8000-00805f9b34fb', '9C90B790-2CC5-42C0-9F87-C9CC40648F4C']>, "
        "'ServiceData': <{'00009C90-0000-1000-8000-00805F9B34FB': <[byte 0x85, 0x00]>, "
        "'0000feaa-0000-1000-8000-00805f9b34fb': <[byte 1, 2, 3]>}>, "
        "'RSSI': <int16 -61>, 'Paired': <false>, 'Trusted': <false>, 'Connected': <true>, 'Blocked': <false>, "
        "'Name': <'peer'>, 'ServicesResolved': <false>}");
    fipsd_ble_bluez_props p;
    fipsd_ble_bluez_parse_props(d, &p);
    const unsigned all = FIPSD_BLE_BLUEZ_ADDRESS | FIPSD_BLE_BLUEZ_ADDRESS_TYPE | FIPSD_BLE_BLUEZ_UUIDS |
        FIPSD_BLE_BLUEZ_SERVICE_DATA | FIPSD_BLE_BLUEZ_RSSI | FIPSD_BLE_BLUEZ_PAIRED | FIPSD_BLE_BLUEZ_TRUSTED |
        FIPSD_BLE_BLUEZ_CONNECTED | FIPSD_BLE_BLUEZ_BLOCKED | FIPSD_BLE_BLUEZ_SERVICES_RESOLVED;
    assert(p.fields == all);
    assert(p.address[0] == 0x00 && p.address[1] == 0xAA && p.address[5] == 0x01 && p.random && p.fips);
    assert(p.rssi == -61 && !p.paired && !p.trusted && p.connected && !p.blocked && !p.services_resolved);
    uint16_t psm = 0;
    assert(p.psm_data_len == 2 && fipsd_ble_decode_psm(p.psm_data, p.psm_data_len, &psm) && psm == 0x0085);
    g_variant_unref(d);

    /* PropertiesChanged carries only what changed; a merge keeps the rest. */
    fipsd_ble_bluez_props u;
    d = parsed("{'ServiceData': <{'00009c90-0000-1000-8000-00805f9b34fb': <[byte 0x91, 0x00, 0xEE]>}>, 'RSSI': <int16 127>}");
    fipsd_ble_bluez_parse_props(d, &u);
    assert(u.fields == FIPSD_BLE_BLUEZ_SERVICE_DATA && u.psm_data_len == 3); /* 127: RSSI unavailable */
    fipsd_ble_bluez_merge_props(&p, &u);
    assert(p.fips && p.random && p.rssi == -61 && p.connected && p.fields == all);
    assert(fipsd_ble_decode_psm(p.psm_data, p.psm_data_len, &psm) && psm == 0x0091); /* trailing byte ignored */
    g_variant_unref(d);

    /* A legacy UUID-only advertiser: service data without the key. */
    d = parsed("{'UUIDs': <['9c90b790-2cc5-42c0-9f87-c9cc40648f4c']>, 'ServiceData': <{'0000feaa-0000-1000-8000-00805f9b34fb': <[byte 1]>}>}");
    fipsd_ble_bluez_parse_props(d, &u);
    assert(u.fips && (u.fields & FIPSD_BLE_BLUEZ_SERVICE_DATA) && u.psm_data_len == 0);
    fipsd_ble_bluez_merge_props(&p, &u);
    assert(p.psm_data_len == 0 && !fipsd_ble_decode_psm(p.psm_data, p.psm_data_len, &psm));
    g_variant_unref(d);

    /* Oversized service data is clamped; wrong types are ignored, not trusted. */
    d = parsed("{'ServiceData': <{'00009c90-0000-1000-8000-00805f9b34fb': <[byte 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]>}>, "
        "'Address': <uint32 7>, 'Paired': <'no'>, 'UUIDs': <'9c90b790-2cc5-42c0-9f87-c9cc40648f4c'>, 'RSSI': <int32 -40>}");
    fipsd_ble_bluez_parse_props(d, &u);
    assert(u.fields == FIPSD_BLE_BLUEZ_SERVICE_DATA && u.psm_data_len == FIPSD_BLE_SERVICE_DATA_MAX && u.psm_data[15] == 16);
    g_variant_unref(d);
    /* Services resolved (BlueZ's GATT client ran over a link): UUIDs lists
     * the GATT services and no longer the advertised FIPS UUID, as measured
     * on BlueZ 5.72. The PSM key still marks it, and a merge keeps it. */
    d = parsed("{'UUIDs': <['00001800-0000-1000-8000-00805f9b34fb', '00001801-0000-1000-8000-00805f9b34fb']>, "
        "'ServiceData': <{'00009c90-0000-1000-8000-00805f9b34fb': <[byte 0x91, 0x00]>}>, 'ServicesResolved': <true>}");
    fipsd_ble_bluez_parse_props(d, &u);
    assert(u.fips && u.services_resolved && u.psm_data_len == 2);
    g_variant_unref(d);
    fipsd_ble_bluez_props seen = {0};
    d = parsed("{'UUIDs': <['9c90b790-2cc5-42c0-9f87-c9cc40648f4c']>}");
    fipsd_ble_bluez_parse_props(d, &u);
    fipsd_ble_bluez_merge_props(&seen, &u);
    g_variant_unref(d);
    d = parsed("{'UUIDs': <['00001800-0000-1000-8000-00805f9b34fb']>, 'ServicesResolved': <true>}");
    fipsd_ble_bluez_parse_props(d, &u);
    assert(!u.fips);
    fipsd_ble_bluez_merge_props(&seen, &u);
    assert(seen.fips && seen.services_resolved);
    g_variant_unref(d);
    d = parsed("{'UUIDs': <['0000180a-0000-1000-8000-00805f9b34fb']>, 'Address': <'not an address'>}");
    fipsd_ble_bluez_parse_props(d, &u);
    assert(u.fields == FIPSD_BLE_BLUEZ_UUIDS && !u.fips);
    g_variant_unref(d);
    d = parsed("('not', 'a dict')");
    fipsd_ble_bluez_parse_props(d, &u);
    assert(u.fields == 0);
    g_variant_unref(d);
    fipsd_ble_bluez_parse_props(NULL, &u);
    assert(u.fields == 0);

    /* Adapter1. */
    d = parsed("{'Address': <'00:AA:01:00:00:00'>, 'AddressType': <'public'>, 'Powered': <true>, 'Discovering': <false>}");
    fipsd_ble_bluez_parse_props(d, &u);
    assert((u.fields & FIPSD_BLE_BLUEZ_POWERED) && u.powered && !u.random && u.address[1] == 0xAA);
    g_variant_unref(d);
    puts("properties: Device1/Adapter1 a{sv}, FIPS by UUID or 0x9C90 key (sticky past GATT resolution), RSSI 127, partial merges, wrong types: ok");
}
/* read_psm: the PSM characteristic of the device's FIPS service, from a
 * GetManagedObjects reply shaped as BlueZ 5.72 sends it. */
static void gatt_lookup(void)
{
    GVariant *o = parsed("{objectpath '/org/bluez/hci0/dev_00_AA_01_01_00_01': {'org.bluez.Device1': {'Connected': <true>}}, "
        "'/org/bluez/hci0/dev_00_AA_01_01_00_01/service0008': {'org.bluez.GattService1': {'UUID': <'00001801-0000-1000-8000-00805f9b34fb'>}}, "
        "'/org/bluez/hci0/dev_00_AA_01_01_00_01/service0008/char0009': {'org.bluez.GattCharacteristic1': {'UUID': <'abdd3056-28fa-441d-a470-55a75a52553a'>, 'Service': <objectpath '/org/bluez/hci0/dev_00_AA_01_01_00_01/service0008'>}}, "
        "'/org/bluez/hci0/dev_00_AA_01_01_00_02/service0010': {'org.bluez.GattService1': {'UUID': <'9c90b790-2cc5-42c0-9f87-c9cc40648f4c'>}}, "
        "'/org/bluez/hci0/dev_00_AA_01_01_00_02/service0010/char0011': {'org.bluez.GattCharacteristic1': {'UUID': <'abdd3056-28fa-441d-a470-55a75a52553a'>, 'Service': <objectpath '/org/bluez/hci0/dev_00_AA_01_01_00_02/service0010'>}}, "
        "'/org/bluez/hci0/dev_00_AA_01_01_00_01/service0020': {'org.bluez.GattService1': {'UUID': <'9C90B790-2CC5-42C0-9F87-C9CC40648F4C'>, 'Primary': <true>}}, "
        "'/org/bluez/hci0/dev_00_AA_01_01_00_01/service0020/char0021': {'org.bluez.GattCharacteristic1': {'UUID': <'0000aaaa-0000-1000-8000-00805f9b34fb'>, 'Service': <objectpath '/org/bluez/hci0/dev_00_AA_01_01_00_01/service0020'>}}, "
        "'/org/bluez/hci0/dev_00_AA_01_01_00_01/service0020/char0023': {'org.bluez.GattCharacteristic1': {'UUID': <'ABDD3056-28FA-441D-A470-55A75A52553A'>, 'Service': <objectpath '/org/bluez/hci0/dev_00_AA_01_01_00_01/service0020'>}}}");
    char out[160];
    /* Only the characteristic under this device's FIPS service: not the same
     * UUID under another service, nor another device's. */
    assert(fipsd_ble_bluez_find_psm_characteristic(o, "/org/bluez/hci0/dev_00_AA_01_01_00_01", out, sizeof out));
    assert(!strcmp(out, "/org/bluez/hci0/dev_00_AA_01_01_00_01/service0020/char0023"));
    assert(fipsd_ble_bluez_find_psm_characteristic(o, "/org/bluez/hci0/dev_00_AA_01_01_00_02", out, sizeof out));
    assert(!strcmp(out, "/org/bluez/hci0/dev_00_AA_01_01_00_02/service0010/char0011"));
    assert(!fipsd_ble_bluez_find_psm_characteristic(o, "/org/bluez/hci0/dev_00_AA_01_01_00_0", out, sizeof out)); /* prefix only */
    assert(!fipsd_ble_bluez_find_psm_characteristic(o, "/org/bluez/hci0/dev_00_AA_01_01_00_03", out, sizeof out));
    assert(!fipsd_ble_bluez_find_psm_characteristic(o, "/org/bluez/hci0/dev_00_AA_01_01_00_01", out, 20)); /* does not fit */
    g_variant_unref(o);
    o = parsed("@a{oa{sa{sv}}} {}");
    assert(!fipsd_ble_bluez_find_psm_characteristic(o, "/org/bluez/hci0/dev_00_AA_01_01_00_01", out, sizeof out));
    g_variant_unref(o);
    o = parsed("('wrong', 'type')");
    assert(!fipsd_ble_bluez_find_psm_characteristic(o, "/org/bluez/hci0/dev_00_AA_01_01_00_01", out, sizeof out));
    g_variant_unref(o);
    puts("GATT: the PSM characteristic of the device's FIPS service, case-insensitive, never another service's or device's: ok");
}
static void scan_policy(void)
{
    fipsd_ble_bluez_props p = {.fields = FIPSD_BLE_BLUEZ_PAIRED | FIPSD_BLE_BLUEZ_TRUSTED | FIPSD_BLE_BLUEZ_CONNECTED |
        FIPSD_BLE_BLUEZ_BLOCKED};
    assert(fipsd_ble_bluez_clear_at_scan_start(&p)); /* holds no user state */
    bool *flags[] = {&p.paired, &p.trusted, &p.connected, &p.blocked};
    for (size_t i = 0; i < 4; ++i) { *flags[i] = true; assert(!fipsd_ble_bluez_clear_at_scan_start(&p)); *flags[i] = false; }
    unsigned each[] = {FIPSD_BLE_BLUEZ_PAIRED, FIPSD_BLE_BLUEZ_TRUSTED, FIPSD_BLE_BLUEZ_CONNECTED, FIPSD_BLE_BLUEZ_BLOCKED};
    for (size_t i = 0; i < 4; ++i) { /* unreadable: taken as set */
        fipsd_ble_bluez_props q = p;
        q.fields &= ~each[i];
        assert(!fipsd_ble_bluez_clear_at_scan_start(&q));
    }
    /* dial_addr_type: random only when reported so. */
    fipsd_ble_bluez_props r = {.fields = FIPSD_BLE_BLUEZ_ADDRESS_TYPE, .random = true};
    assert(fipsd_ble_bluez_dial_type(&r) == FIPSD_BT_ADDR_LE_RANDOM);
    r.random = false;
    assert(fipsd_ble_bluez_dial_type(&r) == FIPSD_BT_ADDR_LE_PUBLIC);
    r = (fipsd_ble_bluez_props){.random = true}; /* no AddressType read */
    assert(fipsd_ble_bluez_dial_type(&r) == FIPSD_BT_ADDR_LE_PUBLIC && fipsd_ble_bluez_dial_type(NULL) == FIPSD_BT_ADDR_LE_PUBLIC);
    puts("scan start clears only objects with no user state (unknown = set); dial type random only when reported: ok");
}
static void errors(void)
{
    assert(fipsd_ble_bluez_dbus_errno("org.bluez.Error.NotReady") == -ENODEV);
    assert(fipsd_ble_bluez_dbus_errno("org.bluez.Error.InProgress") == -EALREADY);
    assert(fipsd_ble_bluez_dbus_errno("org.bluez.Error.AlreadyExists") == -EEXIST);
    assert(fipsd_ble_bluez_dbus_errno("org.bluez.Error.NotSupported") == -ENOTSUP);
    assert(fipsd_ble_bluez_dbus_errno("org.bluez.Error.NotPermitted") == -EPERM);
    assert(fipsd_ble_bluez_dbus_errno("org.bluez.Error.Failed") == -EIO);
    assert(fipsd_ble_bluez_dbus_errno("org.freedesktop.DBus.Error.ServiceUnknown") == -ENODEV);
    assert(fipsd_ble_bluez_dbus_errno("org.freedesktop.DBus.Error.UnknownObject") == -ENODEV);
    assert(fipsd_ble_bluez_dbus_errno("org.freedesktop.DBus.Error.UnknownMethod") == -ENOTSUP);
    assert(fipsd_ble_bluez_dbus_errno("org.freedesktop.DBus.Error.NoReply") == -ETIMEDOUT);
    assert(fipsd_ble_bluez_dbus_errno("org.freedesktop.DBus.Error.AccessDenied") == -EACCES);
    assert(fipsd_ble_bluez_dbus_errno("com.example.Whatever") == -EIO && fipsd_ble_bluez_dbus_errno(NULL) == -EIO);

    assert(fipsd_ble_bluez_gerror_errno(NULL) == 0);
    GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "x");
    assert(fipsd_ble_bluez_gerror_errno(e) == -ECANCELED);
    g_error_free(e);
    e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "x");
    assert(fipsd_ble_bluez_gerror_errno(e) == -ETIMEDOUT);
    g_error_free(e);
    e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "x");
    assert(fipsd_ble_bluez_gerror_errno(e) == -ENODEV);
    g_error_free(e);
    /* As g_dbus_connection_call_finish returns them: registered and not. */
    e = g_dbus_error_new_for_dbus_error("org.bluez.Error.NotReady", "Resource Not Ready");
    assert(fipsd_ble_bluez_gerror_errno(e) == -ENODEV);
    g_error_free(e);
    e = g_dbus_error_new_for_dbus_error("org.freedesktop.DBus.Error.ServiceUnknown", "no bluetoothd");
    assert(e->domain == G_DBUS_ERROR && fipsd_ble_bluez_gerror_errno(e) == -ENODEV);
    g_error_free(e);
    e = g_dbus_error_new_for_dbus_error("org.freedesktop.DBus.Error.NoReply", "late");
    assert(fipsd_ble_bluez_gerror_errno(e) == -ETIMEDOUT);
    g_error_free(e);
    e = g_error_new_literal(G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "local");
    assert(fipsd_ble_bluez_gerror_errno(e) == -EACCES);
    g_error_free(e);
    puts("errors: org.bluez/org.freedesktop D-Bus names and GErrors as negative errno: ok");
}
/* backend.h: a timeout keeps a learned PSM and every other failure forgets
 * it, so an unanswered dial must surface as ETIMEDOUT and nothing else. */
static void connect_errors(void)
{
    const uint64_t timer = fipsd_ble_bluez_kernel_timer_ms(10000); /* connect_timeout_ms 10 s */
    assert(timer == 10000 + FIPSD_BLE_BLUEZ_KERNEL_TIMER_SLACK_MS);
    const uint64_t remaining[] = {1, 1000, 10000, 60000, UINT64_C(86400000), UINT64_MAX - 1};
    for (size_t i = 0; i < 6; ++i) { /* past the deadline, so the deadline ends an unanswered dial */
        uint64_t t = fipsd_ble_bluez_kernel_timer_ms(remaining[i]);
        assert(t > remaining[i] || t == UINT64_C(86400000));
        assert(t <= UINT64_C(86400000));
    }
    /* A refusal: the LE link formed and the peer's L2CAP said no, fast. */
    assert(fipsd_ble_bluez_connect_errno(ECONNREFUSED, false, true, 22, timer) == -ECONNREFUSED);
    assert(fipsd_ble_bluez_connect_errno(ECONNREFUSED, false, true, timer - 251, timer) == -ECONNREFUSED);
    /* The channel timer: nobody answered. Measured: an absent device fails
     * with ECONNREFUSED after 41.04 s under the default 40 s, and after
     * 5.07 s with SO_SNDTIMEO 5 s. */
    assert(fipsd_ble_bluez_connect_errno(ECONNREFUSED, false, true, 41043, FIPSD_BLE_BLUEZ_KERNEL_TIMER_DEFAULT_MS) == -ETIMEDOUT);
    assert(fipsd_ble_bluez_connect_errno(ECONNREFUSED, false, true, 5074, 5000) == -ETIMEDOUT);
    assert(fipsd_ble_bluez_connect_errno(ECONNREFUSED, false, true, timer - 250, timer) == -ETIMEDOUT);
    /* The deadline, and the HCI-level "nobody there" codes. */
    assert(fipsd_ble_bluez_connect_errno(ETIMEDOUT, false, true, 10000, timer) == -ETIMEDOUT);
    assert(fipsd_ble_bluez_connect_errno(EHOSTDOWN, false, true, 3000, timer) == -ETIMEDOUT);
    /* Our side, not the peer's: keep the PSM. */
    assert(fipsd_ble_bluez_connect_errno(ECONNREFUSED, true, true, 22, timer) == -ECANCELED);
    assert(fipsd_ble_bluez_connect_errno(ECANCELED, false, true, 22, timer) == -ECANCELED);
    assert(fipsd_ble_bluez_connect_errno(ECONNREFUSED, false, false, 22, timer) == -ENODEV);
    assert(fipsd_ble_bluez_connect_errno(EHOSTUNREACH, false, true, 1, timer) == -ENODEV);
    assert(fipsd_ble_bluez_connect_errno(EADDRNOTAVAIL, false, true, 1, timer) == -ENODEV);
    /* Anything else is a connect error, passed through. */
    assert(fipsd_ble_bluez_connect_errno(ECONNRESET, false, true, 900, timer) == -ECONNRESET);
    assert(fipsd_ble_bluez_connect_errno(EACCES, false, true, 900, timer) == -EACCES);
    assert(fipsd_ble_bluez_connect_errno(0, false, true, 900, timer) == -EIO);
    puts("connect errors: refusal before the kernel timer, ETIMEDOUT at it and at the deadline, ours keep the PSM: ok");
}
static void constructor(void)
{
    fipsd_ble_config c;
    fipsd_ble_config_default(&c);
    fipsd_ble_backend b;
    const char *reason = NULL;
    c.adapter = "hci-0";
    assert(!fipsd_ble_bluez_backend(&c, &b, &reason) && reason && strstr(reason, "hci0"));
    c.adapter = "hci1";
    reason = NULL;
    assert(fipsd_ble_bluez_backend(&c, &b, &reason) && !reason && fipsd_ble_backend_valid(&b));
    assert(!strcmp(b.ops->adapter_name(b.self), "hci1"));
    fipsd_ble_adapter a = b.ops->adapter(b.self);
    assert(!a.present && !a.powered); /* absent until BlueZ says otherwise */
    fipsd_ble_peer self;
    assert(!b.ops->local_addr(b.self, &self) && b.ops->read_psm);
    b.ops->stop(b.self);
    b.ops->stop(b.self); /* idempotent */
    b.ops->destroy(b.self); /* never started: no thread to join */
    /* The platform hook picks it. */
    reason = NULL;
    assert(fipsd_ble_platform_backend(&c, &b, &reason) && fipsd_ble_backend_valid(&b));
    b.ops->destroy(b.self);
    puts("constructor: adapter validation, ops table, absent before start, stop idempotent, destroy without start: ok");
}
int main(void)
{
    kernel_abi();
    sockaddr_and_paths();
    advertisement();
    properties();
    gatt_lookup();
    scan_policy();
    errors();
    connect_errors();
    constructor();
    puts("FipsdBleBluezTest: ok");
    return 0;
}
