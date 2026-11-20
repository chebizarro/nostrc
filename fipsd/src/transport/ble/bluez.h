#ifndef FIPSD_BLE_BLUEZ_H
#define FIPSD_BLE_BLUEZ_H
/* The BlueZ backend (nostrc-gfi8h.6.2, upstream transport/ble/io_linux.rs):
 * backend.h on Linux. The control plane (adapter, advertising, discovery,
 * GATT) is BlueZ over D-Bus through GDBus on the backend's own GLib thread;
 * the data plane (listen, accept, connect, streams) is kernel L2CAP CoC
 * sockets on fibers, every channel an fd stream on the core's fast path.
 * bluez.c is the backend; bluez_wire.c holds the pure pieces below, which the
 * unit tests drive without a bus or a radio. */
#include "transport/ble/ble.h"
#include <glib.h>
#include <sys/socket.h>

/* transports.ble over BlueZ: copies what it needs from `config`. False, with
 * a reason, for an adapter name BlueZ cannot have. */
bool fipsd_ble_bluez_backend(const fipsd_ble_config *config, fipsd_ble_backend *out, const char **reason);

/* ---- kernel ABI (include/net/bluetooth/{bluetooth,l2cap}.h) ----------------
 * Defined here rather than taken from BlueZ's GPL headers, as bluer does: the
 * backend needs only gio-2.0. The unit test checks every value and the
 * sockaddr layout against <bluetooth/l2cap.h> where that header exists. */
#define FIPSD_BT_AF_BLUETOOTH 31
#define FIPSD_BT_PROTO_L2CAP 0
#define FIPSD_BT_SOL_BLUETOOTH 274
#define FIPSD_BT_SECURITY 4
#define FIPSD_BT_SECURITY_LOW 1 /* no SMP pairing: FIPS authenticates above the link (Noise IK) */
#define FIPSD_BT_POWER 9
#define FIPSD_BT_SNDMTU 12
#define FIPSD_BT_RCVMTU 13
#define FIPSD_BT_ADDR_LE_PUBLIC 0x01
#define FIPSD_BT_ADDR_LE_RANDOM 0x02
typedef struct fipsd_bt_security { uint8_t level, key_size; } fipsd_bt_security;
typedef struct fipsd_bt_power { uint8_t force_active; } fipsd_bt_power;
typedef struct fipsd_l2cap_sockaddr {
    sa_family_t l2_family;
    uint16_t l2_psm;        /* little-endian */
    uint8_t l2_bdaddr[6];   /* bdaddr_t: least significant byte first */
    uint16_t l2_cid;
    uint8_t l2_bdaddr_type;
} fipsd_l2cap_sockaddr;

/* ---- pure helpers (bluez_wire.c) ------------------------------------------ */
/* The 0x9C90 service-data key as BlueZ spells it: the 16-bit UUID over the
 * Bluetooth base UUID. The controller puts it on the air as 16-bit Service
 * Data, the layout psm.c specifies. */
#define FIPSD_BLE_BLUEZ_PSM_DATA_UUID "00009c90-0000-1000-8000-00805f9b34fb"
#define FIPSD_BLE_BLUEZ_ADV_MIN_MS 400u
#define FIPSD_BLE_BLUEZ_ADV_MAX_MS 600u
/* A valid adapter name is also a D-Bus path element: [A-Za-z0-9_]+. */
bool fipsd_ble_bluez_adapter_valid(const char *adapter);
/* "AA:BB:CC:DD:EE:FF" <-> display-order bytes. */
bool fipsd_ble_bluez_parse_address(const char *, uint8_t out[6]);
/* /org/bluez/<adapter>/dev_AA_BB_CC_DD_EE_FF: 0 if it does not fit. The parse
 * accepts the device object itself only, never one of its GATT children. */
size_t fipsd_ble_bluez_device_path(const char *adapter, const uint8_t device[6], char *out, size_t capacity);
bool fipsd_ble_bluez_device_from_path(const char *path, const char *adapter, uint8_t out[6]);

/* LEAdvertisement1 property `name` for `advert` (upstream start_advertising:
 * peripheral, the FIPS UUID, ServiceData {0x9C90: psm LE}, 400-600 ms, no
 * name: 27 of the 31 legacy bytes). A floating reference, or NULL for a
 * property the advertisement does not have. */
GVariant *fipsd_ble_bluez_advert_property(const fipsd_ble_advert *advert, const char *name);
extern const char *const fipsd_ble_bluez_advert_properties[]; /* NULL-terminated */

/* An a{sv} of org.bluez.Device1 or Adapter1 properties, as GetManagedObjects
 * and PropertiesChanged carry them; `fields` says which were present (a
 * PropertiesChanged carries only what changed). */
enum {
    FIPSD_BLE_BLUEZ_ADDRESS = 1u << 0, FIPSD_BLE_BLUEZ_ADDRESS_TYPE = 1u << 1, FIPSD_BLE_BLUEZ_UUIDS = 1u << 2,
    FIPSD_BLE_BLUEZ_SERVICE_DATA = 1u << 3, FIPSD_BLE_BLUEZ_RSSI = 1u << 4, FIPSD_BLE_BLUEZ_PAIRED = 1u << 5,
    FIPSD_BLE_BLUEZ_TRUSTED = 1u << 6, FIPSD_BLE_BLUEZ_CONNECTED = 1u << 7, FIPSD_BLE_BLUEZ_BLOCKED = 1u << 8,
    FIPSD_BLE_BLUEZ_POWERED = 1u << 9, FIPSD_BLE_BLUEZ_SERVICES_RESOLVED = 1u << 10
};
typedef struct fipsd_ble_bluez_props {
    unsigned fields;
    uint8_t address[6];
    bool random;    /* AddressType "random" */
    bool fips;      /* UUIDs carries the FIPS service UUID, or ServiceData the 0x9C90 PSM key */
    int16_t rssi;   /* present only when known: BlueZ's 127 is "unavailable" */
    bool paired, trusted, connected, blocked, powered, services_resolved;
    uint8_t psm_data_len; /* ServiceData[0x9C90], raw; 0 when the dict lacks the key */
    uint8_t psm_data[FIPSD_BLE_SERVICE_DATA_MAX];
} fipsd_ble_bluez_props;
void fipsd_ble_bluez_parse_props(GVariant *dict, fipsd_ble_bluez_props *out);
/* Merge the fields `update` carries into `into`; `fips` is sticky. */
void fipsd_ble_bluez_merge_props(fipsd_ble_bluez_props *into, const fipsd_ble_bluez_props *update);
/* Upstream clear_at_scan_start: a cached device object holding no user state
 * (pairing, trust, a connection, a block) is removed when a scan starts, so
 * its next advert arrives as a fresh InterfacesAdded. An unknown property
 * counts as set. */
bool fipsd_ble_bluez_clear_at_scan_start(const fipsd_ble_bluez_props *);
/* Upstream dial_addr_type: random only when BlueZ reported it so. */
uint8_t fipsd_ble_bluez_dial_type(const fipsd_ble_bluez_props *known);
void fipsd_ble_bluez_sockaddr(const uint8_t device[6], uint8_t type, uint16_t psm, fipsd_l2cap_sockaddr *out);
void fipsd_ble_bluez_sockaddr_device(const fipsd_l2cap_sockaddr *, uint8_t device[6]);

/* read_psm: in a GetManagedObjects reply (a{oa{sa{sv}}}), the object path of
 * the PSM characteristic (FIPSD_BLE_PSM_CHARACTERISTIC_UUID) of a FIPS
 * service (FIPSD_BLE_SERVICE_UUID) under `device_path`. False when the
 * device serves none (or the path does not fit). */
bool fipsd_ble_bluez_find_psm_characteristic(GVariant *objects, const char *device_path, char *out, size_t capacity);

/* Errors as backend.h's negative errno. D-Bus error names (org.bluez.Error.*,
 * org.freedesktop.DBus.Error.*); a GError from a GDBus call (cancellation,
 * timeouts and closed connections included). */
int fipsd_ble_bluez_dbus_errno(const char *dbus_error_name);
int fipsd_ble_bluez_gerror_errno(const GError *);
/* The kernel channel timer (SO_SNDTIMEO) for a dial with `remaining_ms` left:
 * past the deadline, so the deadline is what ends an unanswered dial. */
#define FIPSD_BLE_BLUEZ_KERNEL_TIMER_SLACK_MS 2000u
#define FIPSD_BLE_BLUEZ_KERNEL_TIMER_DEFAULT_MS 40000u /* L2CAP_CONN_TIMEOUT */
uint64_t fipsd_ble_bluez_kernel_timer_ms(uint64_t remaining_ms);
/* A failed connect() as backend.h's errno. The kernel ends an unanswered LE
 * dial (no LE connection ever formed) with ECONNREFUSED when the channel
 * timer fires (l2cap_chan_timeout in BT_CONNECT), the same errno as a peer
 * whose L2CAP layer refused the PSM. Measured on Linux 7.0: an absent device
 * fails with ECONNREFUSED after exactly SO_SNDTIMEO (40 s by default). So an
 * ECONNREFUSED at the timer is a timeout; one before it is a refusal. */
int fipsd_ble_bluez_connect_errno(int err, bool stopped, bool adapter_up, uint64_t elapsed_ms, uint64_t kernel_timer_ms);
#endif
